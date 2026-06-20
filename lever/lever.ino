/*
 * Human Fruit Machine — BLE HID Keyboard Lever v2
 * =================================================
 * Sends keyboard events over BLE HID so an iPad/laptop treats the
 * hardware as a Bluetooth keyboard. All game logic lives in index.html.
 *
 * NEW in v2:
 *   • 3 Easter-egg illuminated buttons → keys q / w / e
 *   • LED light patterns driven autonomously from known game state
 *     (idle breathe → coin flash → pull fill → spin chase → celebrate)
 *
 * Key scheme (matches index.html):
 *   c        — coin inserted
 *   1–5      — lever position
 *   Space    — lever released WITH coin  → spin
 *   a        — lever released WITHOUT coin → ahem
 *   q/w/e    — Easter-egg buttons 1/2/3
 *
 * ── Wiring ──────────────────────────────────────────────────────
 *   Potentiometer:   wiper → GPIO 34
 *   KY-010 coin:     OUT   → GPIO 32  (LOW = coin)
 *   Onboard LED:     GPIO 2
 *
 *   Egg button 1:    switch → GPIO 33 (INPUT_PULLUP)
 *                    LED    → GPIO 25 via 330 Ω (or illum. button LED)
 *   Egg button 2:    switch → GPIO 35,  LED → GPIO 26
 *   Egg button 3:    switch → GPIO 36,  LED → GPIO 27
 *
 *   If using WS2812B NeoPixels instead of plain LEDs, replace the
 *   ledcXxx() calls with Adafruit_NeoPixel calls on a single data pin.
 *
 * ── Library ─────────────────────────────────────────────────────
 *   ESP32-BLE-Keyboard by T-vK (Arduino Library Manager)
 *
 * ── Tuning ──────────────────────────────────────────────────────
 *   Pull lever fully, watch Serial @ 115200, then set POT_MIN/POT_MAX.
 *   Set INVERT_POT true if values decrease when you pull.
 *   Adjust EGG_SW[] / EGG_LED[] / EGG_COUNT to match your button count.
 */

#include <BleKeyboard.h>
#include <math.h>

// ── BLE keyboard ────────────────────────────────────────────────
BleKeyboard kb("FruitMachine", "FruitMachine", 100);

// ── Potentiometer ───────────────────────────────────────────────
const int  POT_PIN    = 34;
const bool INVERT_POT = false;
const int  POT_MIN    = 0;     // raw ADC at lever rest   — calibrate via Serial
const int  POT_MAX    = 4095;  // raw ADC at full pull    — calibrate via Serial

// ── Coin sensor ─────────────────────────────────────────────────
const int  COIN_PIN          = 32;
const unsigned long COIN_DEBOUNCE_MS = 500;

// ── Onboard LED (BLE status) ─────────────────────────────────────
const int LED_PIN = 2;

// ── Easter-egg buttons + LEDs ────────────────────────────────────
// Adjust EGG_COUNT and pin arrays to your build (3–5 buttons).
// Each LED uses one LEDC PWM channel (channels 0, 1, 2 …).
const int  EGG_COUNT  = 3;
const int  EGG_SW[]   = {33, 35, 36};  // switch pins (INPUT_PULLUP, LOW = pressed)
const int  EGG_LED[]  = {25, 26, 27};  // LED anode pins (OUTPUT via 330 Ω resistor)
const char EGG_KEY[]  = {'q', 'w', 'e'};

// ── State ────────────────────────────────────────────────────────
bool          coinReady    = false;
bool          releaseArmed = false;
int           leverLevel   = 0;
unsigned long lastCoinMs   = 0;
bool          eggHeld[EGG_COUNT]  = {};
unsigned long eggLastMs[EGG_COUNT] = {};

// ── Light pattern engine ─────────────────────────────────────────
// The ESP32 knows its own sent events, so we approximate game timing.
// Default spin = 3 s, reel gap = 0.9 s → total ~5.7 s before result.
// Adjust SPIN_MS / REEL_GAP_MS if you change the Settings in the app.
const unsigned long SPIN_MS     = 3000;
const unsigned long REEL_GAP_MS = 900;

enum LightMode {
  LM_IDLE,      // staggered breathing
  LM_COIN,      // rapid flash all LEDs
  LM_PULLING,   // progressive fill with lever level
  LM_SPINNING,  // chase then cascade-lock per reel timing
  LM_CELEBRATE, // alternating strobe after spin resolves
  LM_EGG,       // flash the pressed egg button
};

LightMode     lightMode = LM_IDLE;
unsigned long lightTs   = 0;
int           eggFlash  = 0;   // which egg LED to flash in LM_EGG

void setLight(LightMode m) { lightMode = m; lightTs = millis(); }

void ledBright(int idx, int b) {
  ledcWrite(idx, constrain(b, 0, 255));
}

void allLeds(int b) {
  for (int i = 0; i < EGG_COUNT; i++) ledBright(i, b);
}

// Smooth sinusoidal breath value 0–maxB
int breathe(unsigned long ms_offset, unsigned long period_ms, int maxB = 90) {
  float t = ((millis() + ms_offset) % period_ms) / (float)period_ms;
  float v = (sinf(t * TWO_PI - PI / 2) + 1.0f) / 2.0f;
  return (int)(v * v * maxB);
}

void updateLights() {
  unsigned long el = millis() - lightTs;

  switch (lightMode) {

    case LM_IDLE:
      // Staggered gentle breathing
      for (int i = 0; i < EGG_COUNT; i++)
        ledBright(i, breathe(i * 700UL, 2400));
      break;

    case LM_COIN: {
      bool on = (millis() / 45) % 2;
      allLeds(on ? 220 : 0);
      if (el > 700) setLight(LM_IDLE);
      break;
    }

    case LM_PULLING:
      // Progressive fill: LEDs light up as lever level increases
      for (int i = 0; i < EGG_COUNT; i++) {
        int threshold = map(i, 0, EGG_COUNT - 1, 1, 5);
        ledBright(i, leverLevel >= threshold ? 255 : 10);
      }
      if (leverLevel == 0) setLight(LM_IDLE);
      break;

    case LM_SPINNING: {
      unsigned long spinEnd = SPIN_MS;
      if (el < spinEnd) {
        // Rapid chase
        int active = (millis() / 55) % EGG_COUNT;
        for (int i = 0; i < EGG_COUNT; i++)
          ledBright(i, i == active ? 255 : 12);
      } else if (el < spinEnd + REEL_GAP_MS) {
        // Reel 1 locked
        ledBright(0, 255);
        for (int i = 1; i < EGG_COUNT; i++) ledBright(i, 12);
      } else if (el < spinEnd + REEL_GAP_MS * 2) {
        // Reel 2 locked
        ledBright(0, 255); ledBright(1, 255);
        for (int i = 2; i < EGG_COUNT; i++) ledBright(i, 12);
      } else if (el < spinEnd + REEL_GAP_MS * 3) {
        // All locked
        allLeds(255);
      } else {
        setLight(LM_CELEBRATE);
      }
      break;
    }

    case LM_CELEBRATE: {
      bool on = (millis() / 75) % 2;
      allLeds(on ? 255 : 0);
      if (el > 2000) setLight(LM_IDLE);
      break;
    }

    case LM_EGG: {
      bool on = (millis() / 38) % 2;
      for (int i = 0; i < EGG_COUNT; i++)
        ledBright(i, (i == eggFlash && on) ? 255 : 0);
      if (el > 400) setLight(LM_IDLE);
      break;
    }
  }
}

// ── Potentiometer smoothing ──────────────────────────────────────
int smooth(int v) {
  static int buf[8] = {};
  static int idx    = 0;
  buf[idx] = v;
  idx      = (idx + 1) % 8;
  long sum = 0;
  for (int x : buf) sum += x;
  return (int)(sum / 8);
}

int readLeverLevel() {
  int raw = analogRead(POT_PIN);
  int s   = smooth(raw);
  int v   = constrain(map(s, POT_MIN, POT_MAX, 0, 255), 0, 255);
  if (INVERT_POT) v = 255 - v;
  if (v <  20) return 0;
  if (v <  70) return 1;
  if (v < 120) return 2;
  if (v < 165) return 3;
  if (v < 210) return 4;
  return 5;
}

// ── Setup ────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,  OUTPUT);
  pinMode(COIN_PIN, INPUT_PULLUP);

  for (int i = 0; i < EGG_COUNT; i++) {
    pinMode(EGG_SW[i], INPUT_PULLUP);
    ledcSetup(i, 1000, 8);          // channel i, 1 kHz, 8-bit
    ledcAttachPin(EGG_LED[i], i);
  }

  kb.begin();
  Serial.println("FruitMachine BLE keyboard ready — pair from iPad Settings");
}

// ── Loop ─────────────────────────────────────────────────────────
void loop() {
  bool connected = kb.isConnected();
  digitalWrite(LED_PIN, connected ? HIGH : LOW);
  updateLights();

  if (!connected) { delay(200); return; }

  unsigned long now = millis();

  // ── Coin ──────────────────────────────────────────────────────
  if (digitalRead(COIN_PIN) == LOW && !coinReady && (now - lastCoinMs) > COIN_DEBOUNCE_MS) {
    coinReady  = true;
    lastCoinMs = now;
    kb.print('c');
    setLight(LM_COIN);
    Serial.println("COIN → 'c'");
  }

  // ── Lever ─────────────────────────────────────────────────────
  int newLevel = readLeverLevel();
  if (newLevel != leverLevel) {
    leverLevel = newLevel;
    if (newLevel >= 1) {
      kb.print((char)('0' + newLevel));
      releaseArmed = true;
      setLight(LM_PULLING);
      Serial.printf("Lever %d → '%c'\n", newLevel, '0' + newLevel);
    } else if (releaseArmed) {
      releaseArmed = false;
      if (coinReady) {
        kb.print(' ');
        coinReady = false;
        setLight(LM_SPINNING);
        Serial.println("Release (coin) → ' ' (spin)");
      } else {
        kb.print('a');
        setLight(LM_IDLE);
        Serial.println("Release (no coin) → 'a' (ahem)");
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
      kb.print(EGG_KEY[i]);
      setLight(LM_EGG);
      Serial.printf("Egg %d → '%c'\n", i, EGG_KEY[i]);
    } else if (!pressed) {
      eggHeld[i] = false;
    }
  }

  delay(20);
}
