/*
 * SUPERSEDED by button_wifi/button_wifi.ino (lever hardware retired;
 * this variant also has floating egg pins on GPIO 35/36). Kept only as
 * an emergency BLE fallback — needs the button rework before use.
 *
 * Human Fruit Machine — BLE HID Keyboard Lever  (NeoPixel edition)
 * ==================================================================
 * Same BLE HID keyboard behaviour as v1, but LED output now uses
 * FastLED WS2812B strips instead of simple single-colour LEDs.
 *
 * ── NeoPixel strip wiring ────────────────────────────────────────
 *   Strip 1 data → GPIO 13   (e.g. top of cabinet, 20 LEDs)
 *   Strip 2 data → GPIO  4   (e.g. lever surround,  20 LEDs)
 *   Strip 3 data → GPIO 16   (e.g. coin slot area,  10 LEDs)
 *   All strips:   VCC → 5 V rail,  GND → common GND
 *   Use a 300–500 Ω resistor in series on each data line.
 *   Power: 50 LEDs at full white = ~3 A — use a dedicated 5 V supply.
 *
 *   Egg button LEDs (simple single-colour) remain on:
 *   GPIO 25, 26, 27 via 330 Ω resistors (PWM via LEDC)
 *
 * ── Other wiring (unchanged from v1) ────────────────────────────
 *   Potentiometer wiper → GPIO 34
 *   KY-010 coin OUT     → GPIO 32  (LOW = coin)
 *   Onboard LED         → GPIO 2
 *   Egg switches        → GPIO 33 / 35 / 36  (INPUT_PULLUP)
 *   Egg LED anodes      → GPIO 25 / 26 / 27  via 330 Ω
 *
 * ── Libraries ───────────────────────────────────────────────────
 *   • ESP32-BLE-Keyboard  by T-vK          (Library Manager)
 *   • FastLED             by Daniel Garcia  (Library Manager)
 *
 * ── Tuning ──────────────────────────────────────────────────────
 *   Adjust NUM_STRIPx / PIN_STRIPx below for your strip layout.
 *   If you have only 1–2 strips, remove the extra addLeds() calls
 *   in setup() and set the unused NUM_ defines to 0.
 */

#include <BleKeyboard.h>
#include <FastLED.h>
#include <math.h>

BleKeyboard kb("FruitMachine", "FruitMachine", 100);

// ── NeoPixel strip config ────────────────────────────────────────
#define PIN_STRIP1   13
#define PIN_STRIP2    4
#define PIN_STRIP3   16
#define NUM_STRIP1   20    // LEDs on strip 1  — edit to match
#define NUM_STRIP2   20    // LEDs on strip 2
#define NUM_STRIP3   10    // LEDs on strip 3
#define TOTAL_LEDS   (NUM_STRIP1 + NUM_STRIP2 + NUM_STRIP3)
#define LED_BRIGHT   180   // global brightness 0–255

CRGB leds[TOTAL_LEDS];
uint8_t gHue = 0;          // incremented every frame; drives rainbow sweeps

// ── Easter-egg button LEDs (simple PWM, separate from NeoPixels) ─
const int EGG_COUNT  = 3;
const int EGG_SW[]   = {33, 35, 36};
const int EGG_LED[]  = {25, 26, 27};
const char EGG_KEY[] = {'q', 'w', 'e'};

// ── Lever / coin pins ────────────────────────────────────────────
const int  POT_PIN    = 34;
const bool INVERT_POT = false;
const int  POT_MIN    = 0;
const int  POT_MAX    = 4095;
const int  COIN_PIN   = 32;
const unsigned long COIN_DEBOUNCE_MS = 500;
const int  LED_PIN    = 2;

// ── State ────────────────────────────────────────────────────────
bool          coinReady    = false;
bool          releaseArmed = false;
int           leverLevel   = 0;
unsigned long lastCoinMs   = 0;
bool          eggHeld[EGG_COUNT]   = {};
unsigned long eggLastMs[EGG_COUNT] = {};

// ── Light engine ─────────────────────────────────────────────────
// Times are approximated from default game settings (spin 3 s, gap 0.9 s)
const unsigned long SPIN_MS     = 3000;
const unsigned long REEL_GAP_MS = 900;

enum LightMode {
  LM_IDLE,
  LM_COIN,
  LM_PULLING,
  LM_SPINNING,
  LM_CELEBRATE,
  LM_EGG,
};

LightMode     lightMode = LM_IDLE;
unsigned long lightTs   = 0;
int           eggFlash  = 0;

void setLight(LightMode m) { lightMode = m; lightTs = millis(); }

// ── Button LED PWM helpers ────────────────────────────────────────
void btnBright(int idx, int b) { ledcWrite(idx, constrain(b, 0, 255)); }
void allBtns(int b)            { for (int i = 0; i < EGG_COUNT; i++) btnBright(i, b); }

int breathe(unsigned long off, unsigned long period, int maxB = 90) {
  float t = ((millis() + off) % period) / (float)period;
  float v = (sinf(t * TWO_PI - PI / 2) + 1.0f) / 2.0f;
  return (int)(v * v * maxB);
}

// ── NeoPixel + button LED update ─────────────────────────────────
void updateLights() {
  gHue++;   // ~60 Hz → full rainbow cycle ~4 s

  unsigned long el = millis() - lightTs;

  switch (lightMode) {

    case LM_IDLE:
      // Slow rainbow sweep across all strips
      fill_rainbow(leds, TOTAL_LEDS, gHue / 3, 256 / max(1, TOTAL_LEDS));
      nscale8(leds, TOTAL_LEDS, 100);  // keep it moody, not blinding
      // Button LEDs: staggered breathing
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
      // Gold progress bar proportional to lever level
      int filled = map(leverLevel, 0, 5, 0, TOTAL_LEDS);
      fill_solid(leds,          filled,             CRGB(255, 150, 0));
      fill_solid(leds + filled, TOTAL_LEDS - filled, CRGB(10, 5, 0));
      // Button LEDs: progressive fill
      for (int i = 0; i < EGG_COUNT; i++) {
        int thresh = map(i, 0, EGG_COUNT - 1, 1, 5);
        btnBright(i, leverLevel >= thresh ? 220 : 10);
      }
      if (leverLevel == 0) setLight(LM_IDLE);
      break;
    }

    case LM_SPINNING: {
      unsigned long elapsed = millis() - lightTs;
      if (elapsed > SPIN_MS + REEL_GAP_MS * 3 + 1000) { setLight(LM_IDLE); break; }

      if (elapsed < SPIN_MS) {
        // Blue-white shooting star
        fadeToBlackBy(leds, TOTAL_LEDS, 55);
        int pos = (millis() / 18) % TOTAL_LEDS;
        leds[pos] = CRGB::White;
        leds[(pos + TOTAL_LEDS - 1) % TOTAL_LEDS] = CRGB(80, 120, 255);
        allBtns((millis() / 55) % 2 ? 180 : 15);
      } else if (elapsed < SPIN_MS + REEL_GAP_MS) {
        // Reel 1 locked — first third of strip solid gold
        fill_solid(leds, TOTAL_LEDS / 3, CRGB(255, 150, 0));
        fadeToBlackBy(leds + TOTAL_LEDS / 3, TOTAL_LEDS * 2 / 3, 50);
        int pos = (millis() / 18) % (TOTAL_LEDS * 2 / 3);
        leds[TOTAL_LEDS / 3 + pos] = CRGB(80, 120, 255);
        btnBright(0, 255); btnBright(1, (millis()/55)%2 ? 180 : 15); btnBright(2, (millis()/55)%2 ? 180 : 15);
      } else if (elapsed < SPIN_MS + REEL_GAP_MS * 2) {
        // Reel 2 locked
        fill_solid(leds, TOTAL_LEDS * 2 / 3, CRGB(255, 150, 0));
        fadeToBlackBy(leds + TOTAL_LEDS * 2 / 3, TOTAL_LEDS / 3, 50);
        btnBright(0, 255); btnBright(1, 255); btnBright(2, (millis()/55)%2 ? 180 : 15);
      } else {
        // All locked
        fill_solid(leds, TOTAL_LEDS, CRGB(255, 150, 0));
        allBtns(255);
        setLight(LM_CELEBRATE);
      }
      break;
    }

    case LM_CELEBRATE:
      // Fast rainbow — outcome unknown in BLE mode so generic celebration
      fill_rainbow(leds, TOTAL_LEDS, gHue * 3, 7);
      allBtns((millis() / 70) % 2 ? 255 : 0);
      if (el > 2500) setLight(LM_IDLE);
      break;

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

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,  OUTPUT);
  pinMode(COIN_PIN, INPUT_PULLUP);

  for (int i = 0; i < EGG_COUNT; i++) {
    pinMode(EGG_SW[i], INPUT_PULLUP);
    ledcSetup(i, 1000, 8);
    ledcAttachPin(EGG_LED[i], i);
  }

  // NeoPixel strips
  FastLED.addLeds<WS2812B, PIN_STRIP1, GRB>(leds, 0,                      NUM_STRIP1);
  FastLED.addLeds<WS2812B, PIN_STRIP2, GRB>(leds, NUM_STRIP1,             NUM_STRIP2);
  FastLED.addLeds<WS2812B, PIN_STRIP3, GRB>(leds, NUM_STRIP1 + NUM_STRIP2, NUM_STRIP3);
  FastLED.setBrightness(LED_BRIGHT);
  FastLED.clear(true);

  kb.begin();
  Serial.println("FruitMachine BLE keyboard ready");
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
  bool connected = kb.isConnected();
  digitalWrite(LED_PIN, connected ? HIGH : LOW);
  updateLights();

  if (!connected) { delay(16); return; }

  unsigned long now = millis();

  // Coin
  if (digitalRead(COIN_PIN) == LOW && !coinReady && (now - lastCoinMs) > COIN_DEBOUNCE_MS) {
    coinReady  = true;
    lastCoinMs = now;
    kb.print('c');
    setLight(LM_COIN);
    Serial.println("COIN → 'c'");
  }

  // Lever
  int newLevel = readLeverLevel();
  if (newLevel != leverLevel) {
    leverLevel = newLevel;
    if (newLevel >= 1) {
      kb.print((char)('0' + newLevel));
      releaseArmed = true;
      setLight(LM_PULLING);
    } else if (releaseArmed) {
      releaseArmed = false;
      if (coinReady) {
        coinReady = false;
        kb.print(' ');
        setLight(LM_SPINNING);
        Serial.println("Release → ' ' (spin)");
      } else {
        kb.print('a');
        setLight(LM_IDLE);
        Serial.println("Release → 'a' (ahem)");
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
      kb.print(EGG_KEY[i]);
      setLight(LM_EGG);
    } else if (!pressed) {
      eggHeld[i] = false;
    }
  }

  delay(16); // ~60 fps
}
