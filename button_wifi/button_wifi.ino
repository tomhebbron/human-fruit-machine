/*
 * Human Fruit Machine — Booth Controller (button edition)
 * ==================================================================
 * Replaces lever_wifi (lever hardware retired). The ESP32:
 *   - hosts the web app: WiFi AP + LittleFS web server + WebSocket
 *   - reads the big spin button, coin sensor, and 3 easter-egg buttons
 *   - drives WS2812B cabinet strips + egg-button LEDs from real game
 *     events sent back by the page (per-reel locks, win/lose/jackpot)
 *   - SELF-SYNCS the web app from GitHub Pages: at boot it tries to
 *     join your home WiFi; if it succeeds it downloads index.html and
 *     the sound files listed in sync-manifest.txt into LittleFS, then
 *     switches to AP mode. Deploy = git push + power-cycle at home.
 *
 * ── Fete-day boot (no home WiFi in range) ───────────────────────
 *   Boot → ~10 s trying home WiFi → gives up → AP "FruitMachine"
 *   (password "fruitfair") → iPad joins → Safari http://192.168.4.1
 *   → tap the on-screen TAP TO START overlay once (unlocks audio).
 *
 * ── Wiring ───────────────────────────────────────────────────────
 *   Spin button        → GPIO 19 to GND   (INPUT_PULLUP, debounced)
 *   KY-010 coin OUT    → GPIO 32          (LOW = coin)
 *   Egg switches       → GPIO 33 / 21 / 22 to GND (INPUT_PULLUP)
 *       NOTE: moved off GPIO 35/36 — those pins are input-only with
 *       NO internal pull-ups; they float and fire at random.
 *   Egg LED anodes     → GPIO 25 / 26 / 27 via 330 Ω (LEDC PWM)
 *   Strip 1 data       → GPIO 13   Strip 2 → GPIO 4   Strip 3 → GPIO 16
 *       300-500 Ω in series on each data line; strips on their own
 *       5 V supply (50 LEDs full white ≈ 3 A), common GND with ESP32.
 *       GPIO 16 is unavailable on WROVER boards (PSRAM) — WROOM only.
 *   Onboard LED        → GPIO 2 (AP up / sync activity)
 *
 * ── Build (tested targets — pin these) ──────────────────────────
 *   Board: ESP32 Dev Module (classic WROOM), ESP32 Arduino core 3.x
 *   (this sketch uses the core-3 LEDC API: ledcAttach/ledcWrite).
 *   Libraries via Library Manager:
 *     - ESPAsyncWebServer + AsyncTCP  (the ESP32Async / mathieucarbou
 *       forks — the old me-no-dev/lacamera forks do NOT compile on
 *       core 3.x)
 *     - FastLED
 *     - ArduinoJson (v7)
 *   Partition scheme: default 4 MB with ~1.5 MB LittleFS is enough —
 *   the synced payload (index.html + 8 wired MP3s) is ~0.9 MB.
 *   No filesystem-upload plugin needed: the sketch downloads its own
 *   files. First flash on a blank board: flash, power it near your
 *   home WiFi once, watch Serial for "sync ok".
 *
 * ── WebSocket protocol ───────────────────────────────────────────
 *   ESP32 → page:  {"t":"coin"}  {"t":"button"}  {"t":"egg","n":0-2}
 *   page → ESP32:  {"t":"spin_start"}  {"t":"reel","n":0-2}
 *                  {"t":"result","type":"jackpot"|"three"|"two"|"none"}
 *                  {"t":"state","s":"..."} (informational; parsed, unused)
 *   (Legacy lever/release messages are gone — the page still accepts
 *   them for the old BLE firmware, but this sketch never sends them.)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <math.h>

// ── Sync config — EDIT THESE ─────────────────────────────────────
#define SYNC_ENABLED  true
const char* HOME_SSID = "YOUR_HOME_WIFI";      // <-- your home network
const char* HOME_PASS = "YOUR_HOME_PASSWORD";
const char* SYNC_BASE = "https://tomhebbron.github.io/human-fruit-machine/";
const unsigned long HOME_JOIN_TIMEOUT_MS = 10000;

// ── AP config ────────────────────────────────────────────────────
const char* AP_SSID = "FruitMachine";
const char* AP_PASS = "fruitfair";

// ── NeoPixel strips ──────────────────────────────────────────────
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

// ── Inputs / outputs ─────────────────────────────────────────────
const int BUTTON_PIN = 19;
const int COIN_PIN   = 32;
const int LED_PIN    = 2;
const int EGG_COUNT  = 3;
const int EGG_SW[]   = {33, 21, 22};
const int EGG_LED[]  = {25, 26, 27};

const unsigned long BTN_DEBOUNCE_MS  = 40;
const unsigned long COIN_DEBOUNCE_MS = 500;
const unsigned long EGG_DEBOUNCE_MS  = 200;

// ── Server ───────────────────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ── Light engine ─────────────────────────────────────────────────
enum LightMode {
  LM_IDLE,
  LM_COIN,       // orange flash on coin insert
  LM_PRESS,      // white pulse on button press (tactile ack)
  LM_NOCLIENT,   // red blips: button pressed but no page connected
  LM_SPINNING,   // chase + per-reel gold lock overlay
  LM_WIN,        // rainbow — pair or three-of-a-kind
  LM_JACKPOT,    // gold/white strobe
  LM_LOSE,       // red fade
  LM_EGG,
};

LightMode     lightMode  = LM_IDLE;
unsigned long lightTs    = 0;
int           eggFlash   = 0;
uint8_t       lockedMask = 0;

void setLight(LightMode m) { lightMode = m; lightTs = millis(); }

void btnBright(int idx, int b) { ledcWrite(EGG_LED[idx], constrain(b, 0, 255)); }
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

    case LM_PRESS: {
      uint8_t b = max(0, (int)(255.0f * (1.0f - el / 250.0f)));
      fill_solid(leds, TOTAL_LEDS, CRGB(b, b, b));
      allBtns(b);
      if (el > 250) setLight(LM_IDLE);   // page's spin_start overrides this
      break;
    }

    case LM_NOCLIENT: {
      bool on = (el / 160) % 2 == 0;
      fill_solid(leds, TOTAL_LEDS, on ? CRGB(180, 0, 0) : CRGB::Black);
      allBtns(0);
      if (el > 1000) setLight(LM_IDLE);
      break;
    }

    case LM_SPINNING: {
      if (el > 15000) { setLight(LM_IDLE); break; }   // safety timeout

      fadeToBlackBy(leds, TOTAL_LEDS, 55);
      int pos = (millis() / 18) % TOTAL_LEDS;
      leds[pos] = CRGB::White;
      leds[(pos + TOTAL_LEDS - 1) % TOTAL_LEDS] = CRGB(80, 120, 255);

      int sec = TOTAL_LEDS / 3;
      for (int r = 0; r < 3; r++)
        if (lockedMask & (1 << r))
          fill_solid(leds + r * sec, sec, CRGB(255, 150, 0));

      for (int i = 0; i < EGG_COUNT; i++)
        btnBright(i, (lockedMask & (1 << i)) ? 255 : ((millis() / 60) % 2 ? 160 : 10));
      break;
    }

    case LM_WIN:
      fill_rainbow(leds, TOTAL_LEDS, gHue * 2, 7);
      allBtns((millis() / 65) % 2 ? 255 : 0);
      if (el > 2500) setLight(LM_IDLE);
      break;

    case LM_JACKPOT: {
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

// ── WebSocket ─────────────────────────────────────────────────────
void broadcast(const char* json) { ws.textAll(json); }

void handlePageMessage(const uint8_t* data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, (const char*)data, len)) return;
  const char* t = doc["t"];
  if (!t) return;

  if (!strcmp(t, "spin_start")) {
    lockedMask = 0;
    setLight(LM_SPINNING);

  } else if (!strcmp(t, "reel")) {
    int n = doc["n"] | -1;
    if (n >= 0 && n < 3) lockedMask |= (1 << n);

  } else if (!strcmp(t, "result")) {
    lockedMask = 0;
    const char* type = doc["type"] | "";
    if      (!strcmp(type, "jackpot")) setLight(LM_JACKPOT);
    else if (!strcmp(type, "none"))    setLight(LM_LOSE);
    else                               setLight(LM_WIN);
  }
  // "state" messages are valid JSON we simply ignore — do NOT let them
  // alias other message types (the old substring parser bug).
}

void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
      handlePageMessage(data, len);
  }
}

// ── Sync from GitHub Pages ────────────────────────────────────────
bool fetchToFile(WiFiClientSecure& client, const String& url, const String& path) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("  GET %s -> %d\n", url.c_str(), code);
    http.end();
    return false;
  }
  // Ensure parent directory exists (LittleFS needs explicit mkdir).
  int slash = path.lastIndexOf('/');
  if (slash > 0) LittleFS.mkdir(path.substring(0, slash));

  String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) { http.end(); return false; }
  int written = http.writeToStream(&f);
  f.close();
  http.end();
  if (written <= 0) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(path);
  LittleFS.rename(tmp, path);
  Serial.printf("  %s  (%d bytes)\n", path.c_str(), written);
  return true;
}

void syncFromPages() {
  Serial.printf("Sync: joining %s ...\n", HOME_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(HOME_SSID, HOME_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < HOME_JOIN_TIMEOUT_MS) {
    digitalWrite(LED_PIN, (millis() / 120) % 2);
    fill_solid(leds, TOTAL_LEDS, ((millis() / 200) % 2) ? CRGB(0, 40, 120) : CRGB::Black);
    FastLED.show();
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sync: home WiFi not found — serving existing files.");
    WiFi.disconnect(true);
    return;
  }
  Serial.printf("Sync: connected, IP %s\n", WiFi.localIP().toString().c_str());

  WiFiClientSecure client;
  client.setInsecure();   // public static assets; integrity risk accepted

  // Manifest lists one repo-relative path per line, index.html first.
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String manifest;
  if (http.begin(client, String(SYNC_BASE) + "sync-manifest.txt") && http.GET() == HTTP_CODE_OK)
    manifest = http.getString();
  http.end();

  if (!manifest.length()) {
    Serial.println("Sync: no manifest — serving existing files.");
    WiFi.disconnect(true);
    return;
  }

  int ok = 0, fail = 0, from = 0;
  while (from < (int)manifest.length()) {
    int nl = manifest.indexOf('\n', from);
    if (nl < 0) nl = manifest.length();
    String line = manifest.substring(from, nl);
    from = nl + 1;
    line.trim();
    if (!line.length() || line.startsWith("#")) continue;
    if (fetchToFile(client, String(SYNC_BASE) + line, "/" + line)) ok++; else fail++;
  }
  Serial.printf("Sync %s: %d ok, %d failed\n", fail ? "PARTIAL" : "ok", ok, fail);
  WiFi.disconnect(true);
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(COIN_PIN,   INPUT_PULLUP);
  for (int i = 0; i < EGG_COUNT; i++) {
    pinMode(EGG_SW[i], INPUT_PULLUP);
    ledcAttach(EGG_LED[i], 1000, 8);
  }

  FastLED.addLeds<WS2812B, PIN_STRIP1, GRB>(leds, 0,                       NUM_STRIP1);
  FastLED.addLeds<WS2812B, PIN_STRIP2, GRB>(leds, NUM_STRIP1,              NUM_STRIP2);
  FastLED.addLeds<WS2812B, PIN_STRIP3, GRB>(leds, NUM_STRIP1 + NUM_STRIP2, NUM_STRIP3);
  FastLED.setBrightness(LED_BRIGHT);
  FastLED.clear(true);

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed!");

  if (SYNC_ENABLED && strcmp(HOME_SSID, "YOUR_HOME_WIFI") != 0) syncFromPages();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest* req) {
    if (req->url() == "/" || req->url() == "/index.html")
      req->send(200, "text/plain",
        "No index.html on this ESP32 yet.\n"
        "Set HOME_SSID/HOME_PASS in button_wifi.ino and power-cycle it "
        "in range of that network to sync from GitHub Pages.");
    else
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

  // Spin button (debounced falling edge)
  static bool          btnStable   = false;   // true = pressed
  static bool          btnLastRaw  = false;
  static unsigned long btnChangeMs = 0;
  bool raw = (digitalRead(BUTTON_PIN) == LOW);
  if (raw != btnLastRaw) { btnLastRaw = raw; btnChangeMs = now; }
  if (raw != btnStable && (now - btnChangeMs) >= BTN_DEBOUNCE_MS) {
    btnStable = raw;
    if (btnStable) {                          // press edge
      if (ws.count() > 0) {
        broadcast("{\"t\":\"button\"}");
        setLight(LM_PRESS);
        Serial.println("BUTTON -> {\"t\":\"button\"}");
      } else {
        setLight(LM_NOCLIENT);                // no page connected — show it
        Serial.println("BUTTON pressed but no WS client");
      }
    }
  }

  // Coin (page applies the coin/credit-mode gate)
  static unsigned long lastCoinMs = 0;
  if (digitalRead(COIN_PIN) == LOW && (now - lastCoinMs) > COIN_DEBOUNCE_MS) {
    lastCoinMs = now;
    broadcast("{\"t\":\"coin\"}");
    setLight(LM_COIN);
    Serial.println("COIN -> {\"t\":\"coin\"}");
  }

  // Easter-egg buttons
  static bool          eggHeld[EGG_COUNT]   = {};
  static unsigned long eggLastMs[EGG_COUNT] = {};
  for (int i = 0; i < EGG_COUNT; i++) {
    bool pressed = (digitalRead(EGG_SW[i]) == LOW);
    if (pressed && !eggHeld[i] && (now - eggLastMs[i]) > EGG_DEBOUNCE_MS) {
      eggHeld[i]   = true;
      eggLastMs[i] = now;
      eggFlash     = i;
      char buf[24];
      snprintf(buf, sizeof(buf), "{\"t\":\"egg\",\"n\":%d}", i);
      broadcast(buf);
      setLight(LM_EGG);
    } else if (!pressed) {
      eggHeld[i] = false;
    }
  }

  delay(16); // ~60 fps
}
