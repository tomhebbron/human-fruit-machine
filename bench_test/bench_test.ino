/*
 * Human Fruit Machine — BENCH TEST
 * ==================================================================
 * Flash THIS while you wire the cabinet. It has no WiFi, no web app,
 * no self-sync — just the hardware, so you can prove each connection
 * in isolation and see exactly which pin reacts.
 *
 * What it does:
 *   - Every LED strip shows a slow moving rainbow. If a whole zone is
 *     dark, that strip's 5V / GND / DATA (or the series resistor) is
 *     wrong. If it lights but garbles past a certain point, your
 *     NUM_ count for that zone is too high, or a joint is bad.
 *   - Press any button and you get a line in Serial Monitor naming it
 *     AND its lamp/LED lights while held — so one press tests the
 *     switch (input) and the lamp (output) together.
 *   - The onboard blue LED (GPIO 2) also mirrors the big spin button,
 *     so you can test that switch before any lamp is even wired.
 *
 * Wire ONE thing, watch it work here, move on. Nothing here has to
 * match your final LED counts to be useful — but set the NUM_ defines
 * to your real segment lengths once cut, so the rainbow reaches the
 * end of each strip.
 *
 * ── Only ONE library needed ─────────────────────────────────────
 *   FastLED  (Library Manager → search "FastLED" → by Daniel Garcia)
 *   You do NOT need the async web-server libraries for this sketch —
 *   save that hurdle for the real button_wifi.ino.
 *
 * ── Board settings (same as the real firmware) ──────────────────
 *   Board: ESP32 Dev Module   Port: /dev/cu.usbserial-0001
 *   Serial Monitor: 115200 baud
 *
 * Pinout is identical to button_wifi.ino — verify here, then flash
 * the real firmware with confidence.
 *
 * SAFETY: keep LED_BRIGHT modest and power the strips from your buck
 * converter, NOT the ESP32's 5V pin — 126 LEDs can pull several amps.
 * Common GND between ESP32, buck converter and strips or nothing reads.
 */

#include <FastLED.h>

// ── LED zones — EDIT COUNTS to your real segments ───────────────
#define PIN_TOP       13
#define PIN_MAIN       4
#define PIN_BTNPANEL  16
#define PIN_REELS     18
#define NUM_TOP       40   // square around top cabinet area
#define NUM_MAIN      30   // around the main panel
#define NUM_BTNPANEL  20   // around the buttons panel
#define NUM_REEL      12   // PER reel window (3 windows chained on GPIO 18)
#define NUM_REELS_ALL (NUM_REEL * 3)
#define TOTAL_LEDS    (NUM_TOP + NUM_MAIN + NUM_BTNPANEL + NUM_REELS_ALL)
#define LED_BRIGHT    120   // keep modest on the bench

CRGB leds[TOTAL_LEDS];
uint8_t hue = 0;

// ── Buttons / sensors (identical to button_wifi.ino) ────────────
const int BUTTON_PIN   = 19;          // big spin button  -> GND
const int SPIN_LED_PIN = 23;          // spin button lamp
const int COIN_PIN     = 32;          // KY-010 coin OUT (LOW = coin)
const int ONBOARD_LED  = 2;           // onboard blue LED
const int EGG_SW[3]    = {33, 21, 22};// egg switches under windows 0/1/2 -> GND
const int EGG_LED[3]   = {25, 26, 27};// egg button lamps

int lastButton = HIGH, lastCoin = HIGH, lastEgg[3] = {HIGH, HIGH, HIGH};

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Human Fruit Machine — BENCH TEST ===");
  Serial.println("All LED strips should show a moving rainbow.");
  Serial.println("Press each button: you'll see a line here and its lamp will light.");
  Serial.println();

  pinMode(BUTTON_PIN,  INPUT_PULLUP);
  pinMode(COIN_PIN,    INPUT_PULLUP);
  pinMode(ONBOARD_LED, OUTPUT);
  pinMode(SPIN_LED_PIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    pinMode(EGG_SW[i],  INPUT_PULLUP);
    pinMode(EGG_LED[i], OUTPUT);
  }

  FastLED.addLeds<WS2812B, PIN_TOP,      GRB>(leds, 0,                                NUM_TOP);
  FastLED.addLeds<WS2812B, PIN_MAIN,     GRB>(leds, NUM_TOP,                          NUM_MAIN);
  FastLED.addLeds<WS2812B, PIN_BTNPANEL, GRB>(leds, NUM_TOP + NUM_MAIN,               NUM_BTNPANEL);
  FastLED.addLeds<WS2812B, PIN_REELS,    GRB>(leds, NUM_TOP + NUM_MAIN + NUM_BTNPANEL, NUM_REELS_ALL);
  FastLED.setBrightness(LED_BRIGHT);
}

void loop() {
  // Moving rainbow across every zone — proves each addressable strip.
  fill_rainbow(leds, TOTAL_LEDS, hue++, 5);
  FastLED.show();

  // Big spin button: print on press, mirror to its lamp + onboard LED while held.
  int b = digitalRead(BUTTON_PIN);
  if (b == LOW && lastButton == HIGH) Serial.println("SPIN button   (GPIO 19)");
  lastButton = b;
  digitalWrite(SPIN_LED_PIN, b == LOW ? HIGH : LOW);
  digitalWrite(ONBOARD_LED,  b == LOW ? HIGH : LOW);

  // Egg buttons: print on press, light that egg's lamp while held.
  for (int i = 0; i < 3; i++) {
    int e = digitalRead(EGG_SW[i]);
    if (e == LOW && lastEgg[i] == HIGH) {
      Serial.print("EGG button ");  Serial.print(i);
      Serial.print("   (GPIO ");    Serial.print(EGG_SW[i]);  Serial.println(")");
    }
    lastEgg[i] = e;
    digitalWrite(EGG_LED[i], e == LOW ? HIGH : LOW);
  }

  // Coin sensor: KY-010 pulls LOW when a coin passes.
  int c = digitalRead(COIN_PIN);
  if (c == LOW && lastCoin == HIGH) Serial.println("COIN detected (GPIO 32)");
  lastCoin = c;

  delay(16);   // ~60 fps; plenty responsive for button presses
}
