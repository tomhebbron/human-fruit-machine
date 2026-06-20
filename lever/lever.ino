/*
 * Human Fruit Machine — Bluetooth Lever
 * ======================================
 * Reads a potentiometer on the lever pivot and broadcasts
 * position (0–255) over BLE GATT with notify.
 *
 * The web app (Chrome) connects via Web Bluetooth, receives live
 * position data, plays ratchet sounds as the lever is pulled, and
 * triggers a spin when the lever releases and springs back.
 *
 * ── Wiring ────────────────────────────────────────────────────
 *   Potentiometer:   Left leg  → GND
 *                    Right leg → 3.3V
 *                    Wiper     → GPIO 34
 *
 *   Onboard LED:     GPIO 2  (solid = BLE connected, off = waiting)
 *
 * ── Arduino IDE setup ─────────────────────────────────────────
 *   Board:   "DOIT ESP32 DevKit V1"
 *   Library: ESP32 BLE Arduino (included with ESP32 board package)
 *
 * ── Tuning ────────────────────────────────────────────────────
 *   If pulling the lever gives DECREASING values → set INVERT_POT true
 *   If lever only spans part of the ADC range  → adjust POT_MIN / POT_MAX
 *   Watch Serial Monitor at 115200 baud while pulling to see raw values.
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ── Config — edit these if needed ───────────────────────────────────
#define DEVICE_NAME   "FruitMachineLever"
#define SERVICE_UUID  "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26a8"

const int  POT_PIN    = 34;    // must be an ADC1 pin (32–39 on DevKit)
const int  LED_PIN    = 2;     // onboard LED
const bool INVERT_POT = false; // flip if lever reads backwards
const int  POT_MIN    = 0;     // raw ADC at lever rest — read from Serial, then set
const int  POT_MAX    = 4095;  // raw ADC at full pull  — read from Serial, then set
// ────────────────────────────────────────────────────────────────────

BLEServer*         pServer   = nullptr;
BLECharacteristic* pChar     = nullptr;
bool               connected = false;

class ConnectCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    connected = true;
    Serial.println("✓ App connected");
  }
  void onDisconnect(BLEServer*) override {
    connected = false;
    Serial.println("App disconnected — restarting advertising");
    BLEDevice::startAdvertising();
  }
};

// 8-sample moving average — smooths out ESP32 ADC noise
int smooth(int newVal) {
  static int buf[8] = {};
  static int idx    = 0;
  buf[idx] = newVal;
  idx      = (idx + 1) % 8;
  long sum = 0;
  for (int v : buf) sum += v;
  return (int)(sum / 8);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ConnectCB());

  BLEService* svc = pServer->createService(SERVICE_UUID);
  pChar = svc->createCharacteristic(
    CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pChar->addDescriptor(new BLE2902());
  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();

  Serial.println("FruitMachineLever ready — open Chrome and click 📡");
}

int lastSent = -99;

void loop() {
  int raw      = analogRead(POT_PIN);
  int smoothed = smooth(raw);
  int mapped   = map(smoothed, POT_MIN, POT_MAX, 0, 255);
  int value    = constrain(mapped, 0, 255);
  if (INVERT_POT) value = 255 - value;

  digitalWrite(LED_PIN, connected ? HIGH : LOW);

  if (connected && abs(value - lastSent) > 2) {
    uint8_t v = (uint8_t)value;
    pChar->setValue(&v, 1);
    pChar->notify();
    lastSent = value;
    Serial.printf("Lever: %3d  (raw: %4d)\n", value, raw);
  }

  delay(20); // 50 updates/second is plenty for smooth sound
}
