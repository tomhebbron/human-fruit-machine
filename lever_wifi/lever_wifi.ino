/*
 * Human Fruit Machine — WiFi Hotspot + WebSocket version
 * =======================================================
 * The ESP32 becomes a WiFi access point AND serves index.html from
 * its own flash (LittleFS).  The iPad joins the hotspot, opens
 * http://192.168.4.1 in Safari, and the page communicates back over
 * a WebSocket — giving the ESP32 accurate game events so the LEDs
 * can react to the ACTUAL result rather than guessing from timing.
 *
 * Use alongside lever/lever.ino (BLE HID mode) — pick whichever
 * suits the day:
 *   BLE mode  → GitHub Pages URL, no WiFi needed
 *   WiFi mode → iPad joins "FruitMachine" hotspot, opens 192.168.4.1
 *
 * ── Page → ESP32 WebSocket messages ─────────────────────────────
 *   {"t":"spin_start"}           spin has begun
 *   {"t":"reel","n":0}           reel 0 stopped
 *   {"t":"reel","n":1}           reel 1 stopped
 *   {"t":"reel","n":2}           reel 2 stopped
 *   {"t":"result","type":"jackpot"|"three"|"two"|"none"}
 *
 * ── ESP32 → page WebSocket messages ─────────────────────────────
 *   {"t":"coin"}                 coin detected
 *   {"t":"lever","n":1-5}        lever position
 *   {"t":"release","coin":true}  lever released with coin (spin)
 *   {"t":"release","coin":false} lever released without coin (ahem)
 *   {"t":"egg","n":0-2}          Easter-egg button pressed
 *
 * ── Wiring (same as lever.ino) ───────────────────────────────────
 *   Potentiometer:   wiper → GPIO 34
 *   KY-010 coin:     OUT   → GPIO 32  (LOW = coin)
 *   Onboard LED:     GPIO 2  (on = WiFi AP running)
 *   Egg switch 1/2/3 → GPIO 33 / 35 / 36  (INPUT_PULLUP)
 *   Egg LED 1/2/3    → GPIO 25 / 26 / 27  (330 Ω to GND)
 *
 * ── Setup ────────────────────────────────────────────────────────
 *   1. Install libraries via Arduino Library Manager:
 *        • ESPAsyncWebServer  (by lacamera — ESP32 fork)
 *        • AsyncTCP           (dependency, usually installed with above)
 *        • LittleFS is built into the ESP32 Arduino core
 *
 *   2. Copy index.html into lever_wifi/data/index.html
 *      (same file as the project root — just a copy for LittleFS)
 *
 *   3. Flash sketch normally, then in Arduino IDE:
 *        Tools → ESP32 Sketch Data Upload (LittleFS)
 *      This uploads the data/ folder to ESP32 flash.
 *
 *   4. iPad: Settings → Wi-Fi → FruitMachine (password: fruitfair)
 *      Safari: http://192.168.4.1
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <math.h>

// ── WiFi hotspot ─────────────────────────────────────────────────
const char* AP_SSID = "FruitMachine";
const char* AP_PASS = "fruitfair";     // must be 8+ chars for WPA2

// ── Pins ─────────────────────────────────────────────────────────
const int  POT_PIN    = 34;
const bool INVERT_POT = false;
const int  POT_MIN    = 0;
const int  POT_MAX    = 4095;

const int  COIN_PIN          = 32;
const unsigned long COIN_DEBOUNCE_MS = 500;
const int  LED_PIN           = 2;

const int  EGG_COUNT = 3;
const int  EGG_SW[]  = {33, 35, 36};
const int  EGG_LED[] = {25, 26, 27};

// ── Server + WebSocket ───────────────────────────────────────────
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
  LM_SPINNING,   // chase + per-reel solid as locks come in
  LM_WIN,        // rapid celebration — triggered by actual result
  LM_LOSE,       // slow dim fade   — triggered by actual result
  LM_EGG,
};

LightMode     lightMode  = LM_IDLE;
unsigned long lightTs    = 0;
int           eggFlash   = 0;
uint8_t       lockedMask = 0;   // bitmask: bit i set when reel i locked

void setLight(LightMode m) { lightMode = m; lightTs = millis(); }

void ledBright(int idx, int b) { ledcWrite(idx, constrain(b, 0, 255)); }
void allLeds(int b)            { for (int i = 0; i < EGG_COUNT; i++) ledBright(i, b); }

int breathe(unsigned long offset, unsigned long period, int maxB = 90) {
  float t = ((millis() + offset) % period) / (float)period;
  float v = (sinf(t * TWO_PI - PI / 2) + 1.0f) / 2.0f;
  return (int)(v * v * maxB);
}

void updateLights() {
  unsigned long el = millis() - lightTs;

  switch (lightMode) {
    case LM_IDLE:
      for (int i = 0; i < EGG_COUNT; i++) ledBright(i, breathe(i * 700UL, 2400));
      break;

    case LM_COIN:
      allLeds((millis() / 45) % 2 ? 220 : 0);
      if (el > 700) setLight(LM_IDLE);
      break;

    case LM_PULLING:
      for (int i = 0; i < EGG_COUNT; i++) {
        int threshold = map(i, 0, EGG_COUNT - 1, 1, 5);
        ledBright(i, leverLevel >= threshold ? 255 : 10);
      }
      if (leverLevel == 0) setLight(LM_IDLE);
      break;

    case LM_SPINNING: {
      // 10s safety timeout if page never sends result
      if (el > 10000) { setLight(LM_IDLE); break; }
      int chase = (millis() / 55) % EGG_COUNT;
      for (int i = 0; i < EGG_COUNT; i++) {
        if (lockedMask & (1 << i)) ledBright(i, 255);        // locked = solid
        else ledBright(i, i == chase ? 200 : 10);             // still spinning
      }
      break;
    }

    case LM_WIN:
      allLeds((millis() / 65) % 2 ? 255 : 0);
      if (el > 2500) setLight(LM_IDLE);
      break;

    case LM_LOSE: {
      int b = max(0, (int)(180.0f * (1.0f - el / 2000.0f)));
      allLeds(b);
      if (el > 2000) setLight(LM_IDLE);
      break;
    }

    case LM_EGG:
      for (int i = 0; i < EGG_COUNT; i++)
        ledBright(i, (i == eggFlash && (millis() / 38) % 2) ? 255 : 0);
      if (el > 400) setLight(LM_IDLE);
      break;
  }
}

// ── Potentiometer smoothing ──────────────────────────────────────
int smooth(int v) {
  static int buf[8] = {};
  static int idx    = 0;
  buf[idx] = v; idx = (idx + 1) % 8;
  long sum = 0; for (int x : buf) sum += x;
  return (int)(sum / 8);
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

// ── WebSocket messaging ──────────────────────────────────────────
void broadcast(const String& json) {
  ws.textAll(json);
}

// Simple substring search — avoids ArduinoJson dependency
bool msgContains(const String& msg, const char* token) {
  return msg.indexOf(token) >= 0;
}

void handlePageMessage(const String& msg) {
  if (msgContains(msg, "spin_start")) {
    lockedMask = 0;
    setLight(LM_SPINNING);

  } else if (msgContains(msg, "\"reel\"")) {
    // {"t":"reel","n":0}  — find the n digit
    int idx = msg.indexOf("\"n\":");
    if (idx >= 0) {
      int n = msg.charAt(idx + 4) - '0';
      if (n >= 0 && n < EGG_COUNT) lockedMask |= (1 << n);
    }

  } else if (msgContains(msg, "\"result\"")) {
    bool won = !msgContains(msg, "\"none\"");
    setLight(won ? LM_WIN : LM_LOSE);
    lockedMask = 0;
  }
}

void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS client #%u connected\n", client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      String msg = String((char*)data, len);
      Serial.printf("WS ← %s\n", msg.c_str());
      handlePageMessage(msg);
    }
  }
}

// ── Setup ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(COIN_PIN, INPUT_PULLUP);

  for (int i = 0; i < EGG_COUNT; i++) {
    pinMode(EGG_SW[i], INPUT_PULLUP);
    ledcSetup(i, 1000, 8);
    ledcAttachPin(EGG_LED[i], i);
  }

  // LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — did you upload data/ via Sketch Data Upload?");
  } else {
    Serial.println("LittleFS OK");
  }

  // WiFi AP
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  // WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // Serve game page from LittleFS
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(LittleFS, "/index.html", "text/html");
  });

  // 404 handler
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("HTTP server started — open http://192.168.4.1 on iPad");

  digitalWrite(LED_PIN, HIGH);  // on = AP running
}

// ── Loop ─────────────────────────────────────────────────────────
void loop() {
  ws.cleanupClients();
  updateLights();

  unsigned long now = millis();

  // ── Coin ──────────────────────────────────────────────────────
  if (digitalRead(COIN_PIN) == LOW && !coinReady && (now - lastCoinMs) > COIN_DEBOUNCE_MS) {
    coinReady  = true;
    lastCoinMs = now;
    broadcast("{\"t\":\"coin\"}");
    setLight(LM_COIN);
    Serial.println("COIN → WS");
  }

  // ── Lever ─────────────────────────────────────────────────────
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
        // LM_SPINNING will be set when page sends spin_start
      } else {
        broadcast("{\"t\":\"release\",\"coin\":false}");
        setLight(LM_IDLE);
      }
    }
  }

  // ── Easter-egg buttons ────────────────────────────────────────
  for (int i = 0; i < EGG_COUNT; i++) {
    bool pressed = (digitalRead(EGG_SW[i]) == LOW);
    if (pressed && !eggHeld[i] && (now - eggLastMs[i]) > 200) {
      eggHeld[i]   = true;
      eggLastMs[i] = now;
      eggFlash     = i;
      broadcast("{\"t\":\"egg\",\"n\":" + String(i) + "}");
      setLight(LM_EGG);
      Serial.printf("Egg %d → WS\n", i);
    } else if (!pressed) {
      eggHeld[i] = false;
    }
  }

  delay(20);
}
