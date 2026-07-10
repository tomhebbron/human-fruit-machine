/*
 * Human Fruit Machine — Booth Controller (button edition)
 * ==================================================================
 * Replaces lever_wifi (lever hardware retired). The ESP32:
 *   - hosts the web app: WiFi AP + LittleFS web server + WebSocket
 *   - reads the big spin button, coin sensor, and 3 light-up buttons
 *   - drives six WS2812B zones + button lamps from real game events,
 *     designed so the LIGHTS ALONE communicate the outcome (win /
 *     pair / jackpot / lose) if the iPad display or sound ever fails
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
 * ── LED zones (segments cut from one WS2812B reel) ──────────────
 *   Zone            Data pin   Count define    Effect role
 *   Top cabinet sq.  GPIO 13   NUM_TOP         ambient / celebration
 *   Main panel sq.   GPIO  4   NUM_MAIN        ambient / celebration
 *   Buttons panel    GPIO 16   NUM_BTNPANEL    ambient / celebration
 *   Reel borders ×3  GPIO 18   NUM_REEL each   per-window state: chase
 *                                              while spinning, gold on
 *                                              lock, green on match,
 *                                              red on lose, gold strobe
 *                                              on jackpot
 *   The three reel-window borders are ONE chain on GPIO 18: exit of
 *   window 1 feeds DIN of window 2, then window 3 — left to right.
 *   Every segment joint: 3 wires (5V, GND, DATA), DOUT→DIN direction.
 *   Set the NUM_ defines below to your real segment counts.
 *
 *   Power: data daisy-chains, power must NOT — run 5V/GND from the
 *   buck converter to each zone in parallel (default 126 LEDs ≈ 7.5 A
 *   theoretical full-white; real usage far less, but budget ≥4 A and
 *   keep LED_BRIGHT ≤ 160). 300-500 Ω series resistor on each of the
 *   4 data lines, 1000 µF cap across 5V/GND at the supply, common GND
 *   with the ESP32. GPIO 16 is unavailable on WROVER — WROOM only.
 *
 * ── Buttons (5 in a row: blue white green yellow red) ────────────
 *   Every switch wires GPIO -> GND (INPUT_PULLUP); pressed = LOW. The
 *   switches need NO power — two wires each. Lamps are 12 V via a
 *   driver (ULN2803, or a transistor per lamp) and are OPTIONAL; the
 *   buttons work unlit. Minimal build = wire the switches + red lamp.
 *     RED    (spin):  switch GPIO 19    lamp GPIO 23  (favoured flash)
 *     BLUE   (egg):   switch GPIO 33    lamp GPIO 25
 *     WHITE  (egg):   switch GPIO 21    lamp GPIO 26
 *     GREEN  (egg):   switch GPIO 22    lamp GPIO 27
 *     YELLOW (egg):   switch GPIO 32    lamp GPIO 17
 *   Red starts the spin (its lamp breathes "press me"); the four
 *   colours play funny sounds. The force-jackpot cheat is deliberately
 *   NOT on any public button (gamemaster keeps it on the keyboard).
 *   Coin slot abandoned. Keep switches off GPIO 34-39 (input-only, no
 *   pull-ups — they float). Onboard LED GPIO 2 = AP up / sync activity.
 *   On power-up, a self-test chases the button lamps then races a
 *   rainbow round the reel windows so you can see everything is alive.
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
 *   home WiFi once, watch Serial for "Sync ok".
 *
 * ── WebSocket protocol ───────────────────────────────────────────
 *   ESP32 → page:  {"t":"button"}  {"t":"egg","n":0|2}
 *   page → ESP32:  {"t":"spin_start"}  {"t":"reel","n":0-2}
 *                  {"t":"result","type":"jackpot"|"three"|"two"|"none",
 *                   "m":[matched reel indices]}
 *                  {"t":"state","s":"..."} (drives the spin-button lamp)
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

// ── LED zone config — EDIT COUNTS to your real segments ─────────
#define PIN_TOP       13
#define PIN_MAIN       4
#define PIN_BTNPANEL  16
#define PIN_REELS     18
#define NUM_TOP       40   // square around top cabinet area
#define NUM_MAIN      30   // around the main panel
#define NUM_BTNPANEL  20   // around the buttons panel
#define NUM_REEL      12   // PER reel-window border (3 windows, one chain)
#define NUM_REELS_ALL (NUM_REEL * 3)
#define TOTAL_LEDS    (NUM_TOP + NUM_MAIN + NUM_BTNPANEL + NUM_REELS_ALL)
#define LED_BRIGHT    160

// Zone offsets inside the one leds[] array.
const int Z_TOP   = 0;
const int Z_MAIN  = NUM_TOP;
const int Z_BTNP  = NUM_TOP + NUM_MAIN;
const int Z_REELS = NUM_TOP + NUM_MAIN + NUM_BTNPANEL;
const int CABINET_LEDS = Z_REELS;   // the three ambient zones are contiguous

CRGB    leds[TOTAL_LEDS];
uint8_t gHue = 0;

// ── Buttons (5 in a row: blue white green yellow red) ────────────
const int BUTTON_PIN   = 19;   // RED spin button switch
const int SPIN_LED_PIN = 23;   // RED button lamp (favoured "press me" flash)
const int LED_PIN      = 2;    // onboard LED (AP up / sync activity)
const int EGG_COUNT    = 4;
const int EGG_SW[]     = {33, 21, 22, 32};   // blue / white / green / yellow switches
const int EGG_LED[]    = {25, 26, 27, 17};   // their lamps (optional, 12 V via driver)
// Which web-app egg each colour button fires: 0 = party horn, 2 = crowd
// "ooh". NEVER 1 — that's the force-jackpot cheat, kept off public buttons.
const int EGG_SEND[]   = {0, 2, 0, 2};

const unsigned long BTN_DEBOUNCE_MS = 40;
const unsigned long EGG_DEBOUNCE_MS = 200;

// ── Server ───────────────────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ── Light engine ─────────────────────────────────────────────────
enum LightMode {
  LM_IDLE,
  LM_COIN,       // gold flash on coin insert
  LM_PRESS,      // white pulse on button press (tactile ack)
  LM_NOCLIENT,   // red blips: button pressed but no page connected
  LM_SPINNING,   // per-window chase; gold on each lock
  LM_WIN,        // pair (matched windows green) or triple (all green)
  LM_JACKPOT,    // gold/white strobe everywhere
  LM_LOSE,       // red fade everywhere
  LM_EGG,
};

LightMode     lightMode  = LM_IDLE;
unsigned long lightTs    = 0;
int           eggFlash   = 0;
uint8_t       lockedMask = 0;        // reels locked during spin
uint8_t       winMask    = 0;        // matched reels for LM_WIN
bool          winTriple  = false;
char          pageState[12] = "ready";   // drives the spin-button lamp

void setLight(LightMode m) { lightMode = m; lightTs = millis(); }

void btnBright(int idx, int b) { ledcWrite(EGG_LED[idx], constrain(b, 0, 255)); }
void allBtns(int b)            { for (int i = 0; i < EGG_COUNT; i++) btnBright(i, b); }

int breathe(unsigned long off, unsigned long period, int maxB = 90) {
  float t = ((millis() + off) % period) / (float)period;
  float v = (sinf(t * TWO_PI - PI / 2) + 1.0f) / 2.0f;
  return (int)(v * v * maxB);
}

void fillCabinet(CRGB c)        { fill_solid(leds, CABINET_LEDS, c); }
void reelBorder(int r, CRGB c)  { fill_solid(leds + Z_REELS + r * NUM_REEL, NUM_REEL, c); }
void reelScale(int r, uint8_t s){ nscale8(leds + Z_REELS + r * NUM_REEL, NUM_REEL, s); }

// Rotating comet inside one reel-window border (offset per window so the
// three don't move in lockstep).
void reelChase(int r) {
  int start = Z_REELS + r * NUM_REEL;
  fadeToBlackBy(leds + start, NUM_REEL, 60);
  int pos = ((int)(millis() / 28) + r * (NUM_REEL / 3)) % NUM_REEL;
  leds[start + pos] = CRGB::White;
  leds[start + (pos + NUM_REEL - 1) % NUM_REEL] = CRGB(80, 120, 255);
}

void updateLights() {
  gHue++;
  unsigned long el = millis() - lightTs;

  switch (lightMode) {

    case LM_IDLE:
      // Cabinet zones: slow rainbow. Reel borders: staggered gold breathing.
      fill_rainbow(leds, CABINET_LEDS, gHue / 3, max(1, 256 / CABINET_LEDS));
      nscale8(leds, CABINET_LEDS, 90);
      for (int r = 0; r < 3; r++) {
        reelBorder(r, CRGB(255, 150, 0));
        reelScale(r, 30 + breathe(r * 500UL, 3000, 90));
        btnBright(r, breathe(r * 700UL, 2400));
      }
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

      // Cabinet: dim blue pulse so the reel windows carry the drama.
      fillCabinet(CRGB(10, 25, 70));
      nscale8(leds, CABINET_LEDS, 90 + breathe(0, 800, 100));

      for (int r = 0; r < 3; r++) {
        if (lockedMask & (1 << r)) {
          reelBorder(r, CRGB(255, 150, 0));           // locked: solid gold
          btnBright(r, 255);                          // its button lights too
        } else {
          reelChase(r);
          btnBright(r, (millis() / 60) % 2 ? 160 : 10);
        }
      }
      break;
    }

    case LM_WIN: {
      bool on = (millis() / 130) % 2;
      if (winTriple) {
        // Three of a kind: all windows green, cabinet party rainbow.
        fill_rainbow(leds, CABINET_LEDS, gHue * 2, 7);
        for (int r = 0; r < 3; r++) {
          reelBorder(r, on ? CRGB(0, 230, 90) : CRGB(0, 90, 30));
          btnBright(r, on ? 255 : 40);
        }
      } else {
        // Pair: ONLY the matched windows flash green — readable outcome
        // even with no screen and no sound. Odd one out stays dim.
        fillCabinet(CRGB(0, 60, 25));
        for (int r = 0; r < 3; r++) {
          if (winMask & (1 << r)) {
            reelBorder(r, on ? CRGB(0, 230, 90) : CRGB(0, 110, 40));
            btnBright(r, on ? 255 : 60);
          } else {
            reelBorder(r, CRGB(20, 20, 20));
            btnBright(r, 0);
          }
        }
      }
      if (el > 3000) setLight(LM_IDLE);
      break;
    }

    case LM_JACKPOT: {
      // Unmissable: whole cabinet + all windows strobing gold/white.
      bool on = (millis() / 50) % 2;
      fill_solid(leds, TOTAL_LEDS, on ? CRGB(255, 215, 0) : CRGB::White);
      allBtns(on ? 255 : 80);
      if (el > 4000) setLight(LM_IDLE);
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
      static const CRGB eggC[] = { CRGB(0, 120, 255), CRGB(255, 255, 255), CRGB(0, 230, 120), CRGB(255, 200, 0) };
      bool on = (millis() / 38) % 2;
      fill_solid(leds, TOTAL_LEDS, (eggFlash < EGG_COUNT && on) ? eggC[eggFlash] : CRGB::Black);
      for (int i = 0; i < EGG_COUNT; i++)
        btnBright(i, (i == eggFlash && on) ? 255 : 0);
      if (el > 400) setLight(LM_IDLE);
      break;
    }
  }

  // Big red button lamp — driven by the page's reported state, independent
  // of the zone animation: breathe = press me, off = spinning, blink = reset.
  int sb;
  if      (lightMode == LM_SPINNING)         sb = 0;
  else if (!strcmp(pageState, "result"))     sb = ((millis() / 300) % 2) ? 220 : 25;
  else if (!strcmp(pageState, "nocoin"))     sb = breathe(0, 2600, 50);
  else                                       sb = breathe(0, 1100, 255);  // ready
  ledcWrite(SPIN_LED_PIN, sb);

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
    winMask    = 0;
    JsonArray m = doc["m"];
    if (!m.isNull())
      for (JsonVariant v : m) { int i = v | -1; if (i >= 0 && i < 3) winMask |= (1 << i); }
    const char* type = doc["type"] | "";
    if      (!strcmp(type, "jackpot")) setLight(LM_JACKPOT);
    else if (!strcmp(type, "none"))    setLight(LM_LOSE);
    else { winTriple = !strcmp(type, "three"); setLight(LM_WIN); }

  } else if (!strcmp(t, "state")) {
    const char* s = doc["s"] | "";
    strlcpy(pageState, s, sizeof(pageState));
  }
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

// ── Power-on self-test ────────────────────────────────────────────
// Chase the button lamps blue->white->green->yellow->red, then race a
// pair of colour comets round every LED. Purely visual — proves each
// lamp wire and every strip pixel at switch-on, before the game starts.
void bootSelfTest() {
  Serial.println("Self-test: button lamp chase + reel rainbow race");
  const int lamps[5] = { EGG_LED[0], EGG_LED[1], EGG_LED[2], EGG_LED[3], SPIN_LED_PIN };
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < 5; i++) {
      for (int j = 0; j < 5; j++) ledcWrite(lamps[j], j == i ? 255 : 0);
      delay(130);
    }
  }
  for (int j = 0; j < 5; j++) ledcWrite(lamps[j], 0);

  for (unsigned long t0 = millis(); millis() - t0 < 2200; ) {
    fadeToBlackBy(leds, TOTAL_LEDS, 48);
    int p = (millis() / 6) % TOTAL_LEDS;
    uint8_t h = (uint8_t)(millis() / 8);
    leds[p] = CHSV(h, 255, 255);
    leds[(p + TOTAL_LEDS / 2) % TOTAL_LEDS] = CHSV(h + 128, 255, 255);
    FastLED.show();
    delay(6);
  }
  FastLED.clear(true);
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  ledcAttach(SPIN_LED_PIN, 1000, 8);
  for (int i = 0; i < EGG_COUNT; i++) {
    pinMode(EGG_SW[i], INPUT_PULLUP);
    ledcAttach(EGG_LED[i], 1000, 8);
  }

  FastLED.addLeds<WS2812B, PIN_TOP,      GRB>(leds, Z_TOP,   NUM_TOP);
  FastLED.addLeds<WS2812B, PIN_MAIN,     GRB>(leds, Z_MAIN,  NUM_MAIN);
  FastLED.addLeds<WS2812B, PIN_BTNPANEL, GRB>(leds, Z_BTNP,  NUM_BTNPANEL);
  FastLED.addLeds<WS2812B, PIN_REELS,    GRB>(leds, Z_REELS, NUM_REELS_ALL);
  FastLED.setBrightness(LED_BRIGHT);
  FastLED.clear(true);
  bootSelfTest();

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

  // Colour buttons (blue/white/green/yellow) — play easter-egg sounds.
  static bool          eggHeld[EGG_COUNT]   = {};
  static unsigned long eggLastMs[EGG_COUNT] = {};
  for (int i = 0; i < EGG_COUNT; i++) {
    bool pressed = (digitalRead(EGG_SW[i]) == LOW);
    if (pressed && !eggHeld[i] && (now - eggLastMs[i]) > EGG_DEBOUNCE_MS) {
      eggHeld[i]   = true;
      eggLastMs[i] = now;
      eggFlash     = i;
      char buf[24];
      snprintf(buf, sizeof(buf), "{\"t\":\"egg\",\"n\":%d}", EGG_SEND[i]);
      broadcast(buf);
      setLight(LM_EGG);
    } else if (!pressed) {
      eggHeld[i] = false;
    }
  }

  delay(16); // ~60 fps
}
