/*
 * Human Fruit Machine — WiFi Hotspot + WebSocket + NeoPixel edition
 * ==================================================================
 * ESP32 runs as a WiFi access point, serves index.html from LittleFS,
 * and uses a WebSocket for full two-way communication with the page.
 * NeoPixel (WS2812B) strips are driven with accurate game-event data:
 * - jackpot vs regular win are visually distinct (gold strobe vs rainbow)
 * - each reel lock is shown in real-time as the page sends reel events
 * - lose is a slow red fade — no guessing from timing
 *
 * ── Page → ESP32 messages ────────────────────────────────────────
 *   {"t":"spin_start"}
 *   {"t":"reel","n":0|1|2}
 *   {"t":"result","type":"jackpot"|"three"|"two"|"none"}
 *
 * ── ESP32 → page messages ────────────────────────────────────────
 *   {"t":"coin"}
 *   {"t":"lever","n":1-5}
 *   {"t":"release","coin":true|false}
 *   {"t":"egg","n":0-2}
 *
 * ── NeoPixel wiring (same data pins as BLE version) ─────────────
 *   Strip 1 → GPIO 13  (e.g. top of cabinet)
 *   Strip 2 → GPIO  4  (e.g. lever surround)
 *   Strip 3 → GPIO 16  (e.g. coin slot area)
 *   Power: 5 V dedicated supply (do NOT power from ESP32 3.3 V pin)
 *
 * ── Egg button LEDs (simple PWM) ─────────────────────────────────
 *   GPIO 25 / 26 / 27 via 330 Ω resistors
 *
 * ── Other wiring ─────────────────────────────────────────────────
 *   Pot wiper → GPIO 34,  KY-010 OUT → GPIO 32,  LED → GPIO 2
 *   Egg switches → GPIO 33 / 35 / 36
 *
 * ── Libraries ───────────────────────────────────────────────────
 *   • ESPAsyncWebServer  by lacamera     (Library Manager)
 *   • AsyncTCP           (installed with above)
 *   • FastLED            by Daniel Garcia (Library Manager)
 *
 * ── One-time setup steps ────────────────────────────────────────
 *   1. Copy index.html to  lever_wifi/data/index.html
 *   2. Flash this sketch
 *   3. Tools → ESP32 Sketch Data Upload  (uploads data/ to LittleFS)
 *   4. iPad: join WiFi "FruitMachine"  password "fruitfair"
 *   5. Safari: http://192.168.4.1
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <FastLED.h>
#include <math.h>

// ── WiFi ─────────────────────────────────────────────────────────
const char* AP_SSID = "FruitMachine";
const char* AP_PASS = "fruitfair";

// ── NeoPixel strip config ────────────────────────────────────────
#define PIN_STRIP1   13
#define PIN_STRIP2    4
#define PIN_STRIP3   16
#define NUM_STRIP1   20
#define NUM_STRIP2   20
#define NUM_STRIP3   10
#define TOTAL_LEDS   (NUM_STRIP1 + NUM_STRIP2 + NUM_STRIP3)
#define LED_BRIGHT   180

CRGB    leds[TOTAL_LEDS];
uint8_t gHue = 0;

// ── Egg button LEDs ───────────────────────────────────────────────
const int EGG_COUNT  = 3;
const int EGG_SW[]   = {33, 35, 36};
const int EGG_LED[]  = {25, 26, 27};

// ── Lever / coin pins ────────────────────────────────────────────
const int  POT_PIN    = 34;
const bool INVERT_POT = false;
const int  POT_MIN    = 0;
const int  POT_MAX    = 4095;
const int  COIN_PIN   = 32;
const unsigned long COIN_DEBOUNCE_MS = 500;
const int  LED_PIN    = 2;

// ── Server ───────────────────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ── Game state ───────────────────────────────────────────────────
bool          coinReady    = false;
bool          releaseArmed = false;
int           leverLevel   = 0;
unsigned long lastCoinMs   = 0;
bool          eggHeld[EGG_COUNT]   = {};
unsigned long eggLastMs[EGG_COUNT] = {};

// ── Light engine ─────────────────────────────────────────────────
enum LightMode {
  LM_IDLE,
  LM_COIN,
  LM_PULLING,
  LM_SPINNING,    // chase, with per-reel lock overlay
  LM_WIN,         // rainbow celebration — regular win
  LM_JACKPOT,     // gold strobe — jackpot only
  LM_LOSE,        // red fade
  LM_EGG,
};

LightMode     lightMode  = LM_IDLE;
unsigned long lightTs    = 0;
int           eggFlash   = 0;
uint8_t       lockedMask = 0;  // bitmask: bit i set when reel i locked

void setLight(LightMode m) { lightMode = m; lightTs = millis(); }

void btnBright(int idx, int b) { ledcWrite(idx, constrain(b, 0, 255)); }
void allBtns(int b)            { for (int i = 0; i < EGG_COUNT; i++) btnBright(i, b); }

int breathe(unsigned long off, unsigned long period, int maxB = 90) {
  float t = ((millis() + off) % period) / (float)period;
  float v = (sinf(t * TWO_PI - PI / 2) + 1.0f) / 2.0f;
  return (int)(v * v * maxB);
}

void updateLights() {
  gHue++;
  unsigned long el = millis() - lightTs;

  switch (lightMode) {

    case LM_IDLE:
      fill_rainbow(leds, TOTAL_LEDS, gHue / 3, 256 / max(1, TOTAL_LEDS));
      nscale8(leds, TOTAL_LEDS, 100);
      for (int i = 0; i < EGG_COUNT; i++)
        btnBright(i, breathe(i * 700UL, 2400));
      break;

    case LM_COIN: {
      bool on = (millis() / 70) % 2;
      fill_solid(leds, TOTAL_LEDS, on ? CRGB(255, 160, 0) : CRGB::Black);
      allBtns(on ? 220 : 0);
      if (el > 700) setLight(LM_IDLE);
      break;
    }

    case LM_PULLING: {
      int filled = map(leverLevel, 0, 5, 0, TOTAL_LEDS);
      fill_solid(leds,          filled,             CRGB(255, 150, 0));
      fill_solid(leds + filled, TOTAL_LEDS - filled, CRGB(10, 5, 0));
      for (int i = 0; i < EGG_COUNT; i++) {
        int thresh = map(i, 0, EGG_COUNT - 1, 1, 5);
        btnBright(i, leverLevel >= thresh ? 220 : 10);
      }
      if (leverLevel == 0) setLight(LM_IDLE);
      break;
    }

    case LM_SPINNING: {
      if (el > 10000) { setLight(LM_IDLE); break; }   // safety timeout

      // Blue-white shooting star
      fadeToBlackBy(leds, TOTAL_LEDS, 55);
      int pos = (millis() / 18) % TOTAL_LEDS;
      leds[pos] = CRGB::White;
      leds[(pos + TOTAL_LEDS - 1) % TOTAL_LEDS] = CRGB(80, 120, 255);

      // Overlay locked reel sections with solid gold
      // Divide strip evenly into thirds for the 3 reels
      int sec = TOTAL_LEDS / 3;
      for (int r = 0; r < 3; r++) {
        if (lockedMask & (1 << r))
          fill_solid(leds + r * sec, sec, CRGB(255, 150, 0));
      }

      // Button LEDs: chase or solid based on lock
      for (int i = 0; i < EGG_COUNT; i++)
        btnBright(i, (lockedMask & (1 << i)) ? 255 : ((millis() / 60) % 2 ? 160 : 10));
      break;
    }

    case LM_WIN:
      // Fast rainbow sweep — pair or three-of-a-kind
      fill_rainbow(leds, TOTAL_LEDS, gHue * 2, 7);
      allBtns((millis() / 65) % 2 ? 255 : 0);
      if (el > 2500) setLight(LM_IDLE);
      break;

    case LM_JACKPOT: {
      // Gold strobe — unmistakably jackpot
      bool on = (millis() / 50) % 2;
      fill_solid(leds, TOTAL_LEDS, on ? CRGB(255, 215, 0) : CRGB::White);
      allBtns(on ? 255 : 80);
      if (el > 3000) setLight(LM_IDLE);
      break;
    }

    case LM_LOSE: {
      uint8_t bright = max(0, (int)(200.0f * (1.0f - el / 2000.0f)));
      fill_solid(leds, TOTAL_LEDS, CRGB(bright, 0, 0));
      allBtns(bright / 2);
      if (el > 2000) setLight(LM_IDLE);
      break;
    }

    case LM_EGG: {
      static const CRGB eggC[] = { CRGB(255, 80, 0), CRGB(255, 215, 0), CRGB(0, 230, 120) };
      bool on = (millis() / 38) % 2;
      fill_solid(leds, TOTAL_LEDS, (eggFlash < EGG_COUNT && on) ? eggC[eggFlash] : CRGB::Black);
      for (int i = 0; i < EGG_COUNT; i++)
        btnBright(i, (i == eggFlash && on) ? 255 : 0);
      if (el > 400) setLight(LM_IDLE);
      break;
    }
  }

  FastLED.show();
}

// ── Pot smoothing ─────────────────────────────────────────────────
int smooth(int v) {
  static int buf[8] = {};
  static int idx    = 0;
  buf[idx] = v; idx = (idx + 1) % 8;
  long s = 0; for (int x : buf) s += x;
  return (int)(s / 8);
}

int readLeverLevel() {
  int s = smooth(analogRead(POT_PIN));
  int v = constrain(map(s, POT_MIN, POT_MAX, 0, 255), 0, 255);
  if (INVERT_POT) v = 255 - v;
  if (v <  20) return 0;
  if (v <  70) return 1;
  if (v < 120) return 2;
  if (v < 165) return 3;
  if (v < 210) return 4;
  return 5;
}

// ── WebSocket ─────────────────────────────────────────────────────
void broadcast(const String& json) { ws.textAll(json); }

bool strhas(const String& s, const char* t) { return s.indexOf(t) >= 0; }

void handlePageMessage(const String& msg) {
  if (strhas(msg, "spin_start")) {
    lockedMask = 0;
    setLight(LM_SPINNING);

  } else if (strhas(msg, "\"reel\"")) {
    int idx = msg.indexOf("\"n\":");
    if (idx >= 0) {
      int n = msg.charAt(idx + 4) - '0';
      if (n >= 0 && n < 3) lockedMask |= (1 << n);
    }

  } else if (strhas(msg, "\"result\"")) {
    lockedMask = 0;
    if      (strhas(msg, "jackpot")) setLight(LM_JACKPOT);
    else if (strhas(msg, "\"none\"")) setLight(LM_LOSE);
    else                             setLight(LM_WIN);
  }
}

void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      String msg = String((char*)data, len);
      handlePageMessage(msg);
    }
  }
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(COIN_PIN, INPUT_PULLUP);

  for (int i = 0; i < EGG_COUNT; i++) {
    pinMode(EGG_SW[i], INPUT_PULLUP);
    ledcSetup(i, 1000, 8);
    ledcAttachPin(EGG_LED[i], i);
  }

  FastLED.addLeds<WS2812B, PIN_STRIP1, GRB>(leds, 0,                      NUM_STRIP1);
  FastLED.addLeds<WS2812B, PIN_STRIP2, GRB>(leds, NUM_STRIP1,             NUM_STRIP2);
  FastLED.addLeds<WS2812B, PIN_STRIP3, GRB>(leds, NUM_STRIP1 + NUM_STRIP2, NUM_STRIP3);
  FastLED.setBrightness(LED_BRIGHT);
  FastLED.clear(true);

  if (!LittleFS.begin(true))
    Serial.println("LittleFS failed — upload data/ via Sketch Data Upload");

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(LittleFS, "/index.html", "text/html");
  });
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });
  server.begin();
  Serial.println("HTTP server started — http://192.168.4.1");

  digitalWrite(LED_PIN, HIGH);
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
  ws.cleanupClients();
  updateLights();

  unsigned long now = millis();

  // Coin
  if (digitalRead(COIN_PIN) == LOW && !coinReady && (now - lastCoinMs) > COIN_DEBOUNCE_MS) {
    coinReady  = true;
    lastCoinMs = now;
    broadcast("{\"t\":\"coin\"}");
    setLight(LM_COIN);
  }

  // Lever
  int newLevel = readLeverLevel();
  if (newLevel != leverLevel) {
    leverLevel = newLevel;
    if (newLevel >= 1) {
      broadcast("{\"t\":\"lever\",\"n\":" + String(newLevel) + "}");
      releaseArmed = true;
      setLight(LM_PULLING);
    } else if (releaseArmed) {
      releaseArmed = false;
      if (coinReady) {
        coinReady = false;
        broadcast("{\"t\":\"release\",\"coin\":true}");
        // LM_SPINNING set when page confirms spin_start
      } else {
        broadcast("{\"t\":\"release\",\"coin\":false}");
        setLight(LM_IDLE);
      }
    }
  }

  // Easter-egg buttons
  for (int i = 0; i < EGG_COUNT; i++) {
    bool pressed = (digitalRead(EGG_SW[i]) == LOW);
    if (pressed && !eggHeld[i] && (now - eggLastMs[i]) > 200) {
      eggHeld[i]   = true;
      eggLastMs[i] = now;
      eggFlash     = i;
      broadcast("{\"t\":\"egg\",\"n\":" + String(i) + "}");
      setLight(LM_EGG);
    } else if (!pressed) {
      eggHeld[i] = false;
    }
  }

  delay(16); // ~60 fps
}
