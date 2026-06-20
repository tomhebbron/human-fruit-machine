/*
 * Human Fruit Machine — BLE HID Keyboard Lever
 * =============================================
 * Reads a potentiometer (lever pivot) and a KY-010 IR break-beam coin
 * sensor, then emulates a Bluetooth keyboard so an iPad can receive
 * game events without any app installation.
 *
 * Key scheme (matches index.html keydown handler):
 *   c        — coin inserted
 *   1 – 5    — lever position (1 = just started, 5 = fully pulled)
 *   Space    — lever released WITH coin (triggers spin)
 *   a        — lever released WITHOUT coin (plays "ahem")
 *
 * ── Wiring ──────────────────────────────────────────────────────
 *   Potentiometer:   Left leg  → GND
 *                    Right leg → 3.3V
 *                    Wiper     → GPIO 34
 *
 *   KY-010 coin IR:  VCC  → 3.3V
 *                    GND  → GND
 *                    OUT  → GPIO 32   (LOW when beam broken = coin)
 *
 *   Onboard LED:     GPIO 2  (on = BLE paired, off = waiting)
 *
 * ── Arduino IDE setup ───────────────────────────────────────────
 *   Board:   "DOIT ESP32 DevKit V1"
 *   Library: ESP32-BLE-Keyboard by T-vK (install via Library Manager)
 *
 * ── Tuning ──────────────────────────────────────────────────────
 *   Pull lever fully and watch Serial (115200) to see raw ADC values.
 *   Set POT_MIN / POT_MAX to the values observed at rest and full pull.
 *   If pulling gives DECREASING values, set INVERT_POT true.
 */

#include <BleKeyboard.h>

// ── Config ──────────────────────────────────────────────────────
#define DEVICE_NAME  "FruitMachine"

const int  POT_PIN    = 34;     // ADC1 only (GPIO 32–39)
const int  COIN_PIN   = 32;     // KY-010 OUT — LOW when coin breaks beam
const int  LED_PIN    = 2;      // onboard LED

const bool INVERT_POT = false;  // flip if lever reads backwards
const int  POT_MIN    = 0;      // raw ADC at lever rest  (calibrate via Serial)
const int  POT_MAX    = 4095;   // raw ADC at full pull   (calibrate via Serial)

const unsigned long COIN_DEBOUNCE_MS = 500; // ignore re-triggers for this long
// ────────────────────────────────────────────────────────────────

BleKeyboard kb(DEVICE_NAME, "FruitMachine", 100);

// ── State ────────────────────────────────────────────────────────
bool          coinReady    = false;  // coin has been inserted this play
bool          releaseArmed = false;  // lever was pulled at least once
int           leverLevel   = 0;      // 0 = at rest, 1-5 = positions
unsigned long lastCoinTime = 0;      // debounce timestamp

// 8-sample moving average — smooths ESP32 ADC noise
int smooth(int newVal) {
  static int buf[8] = {};
  static int idx    = 0;
  buf[idx] = newVal;
  idx      = (idx + 1) % 8;
  long sum = 0;
  for (int v : buf) sum += v;
  return (int)(sum / 8);
}

// Map smoothed ADC value to discrete level 0–5
int readLeverLevel() {
  int raw      = analogRead(POT_PIN);
  int smoothed = smooth(raw);
  int mapped   = map(smoothed, POT_MIN, POT_MAX, 0, 255);
  int value    = constrain(mapped, 0, 255);
  if (INVERT_POT) value = 255 - value;

  // Divide 0-255 into 6 bands: 0 = rest, 1-5 = pull levels
  if (value < 20)  return 0;
  if (value < 70)  return 1;
  if (value < 120) return 2;
  if (value < 165) return 3;
  if (value < 210) return 4;
  return 5;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN,  OUTPUT);
  pinMode(COIN_PIN, INPUT_PULLUP);

  kb.begin();
  Serial.println("FruitMachine BLE keyboard starting — pair from iPad Settings");
}

void loop() {
  bool isConnected = kb.isConnected();
  digitalWrite(LED_PIN, isConnected ? HIGH : LOW);

  if (!isConnected) {
    delay(500);
    return;
  }

  // ── Coin sensor ─────────────────────────────────────────────
  bool coinBeam = (digitalRead(COIN_PIN) == LOW); // LOW = beam broken = coin
  unsigned long now = millis();

  if (coinBeam && !coinReady && (now - lastCoinTime) > COIN_DEBOUNCE_MS) {
    coinReady    = true;
    lastCoinTime = now;
    kb.print('c');
    Serial.println("COIN inserted → sent 'c'");
  }

  // ── Lever position ──────────────────────────────────────────
  int newLevel = readLeverLevel();

  if (newLevel != leverLevel) {
    int prev   = leverLevel;
    leverLevel = newLevel;

    if (newLevel >= 1) {
      // Lever being pulled — send position key '1'–'5'
      char key = '0' + newLevel;
      kb.print(key);
      releaseArmed = true;
      Serial.printf("Lever level %d → sent '%c'\n", newLevel, key);
    } else {
      // Lever returned to rest (level 0)
      if (releaseArmed) {
        releaseArmed = false;
        if (coinReady) {
          kb.print(' ');  // Space = spin!
          coinReady = false;
          Serial.println("Lever released (coin) → sent ' ' (spin)");
        } else {
          kb.print('a');  // no coin = ahem
          Serial.println("Lever released (no coin) → sent 'a' (ahem)");
        }
      }
    }
  }

  delay(20); // 50 Hz is plenty for smooth lever tracking
}
