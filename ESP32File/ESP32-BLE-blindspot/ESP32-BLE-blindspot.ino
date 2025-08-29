// ESP32 + HC-SR04 -> BLE notify (distance + alert)
// Works with ESP32 Arduino core v2/v3 and either classic BLE or NimBLE.
//
// If NimBLE is installed, this will use it; otherwise uses classic BLE (nkolban).
// No explicit security is configured (not needed for notify-only).

// ---------- BLE includes (auto-select) ----------
#if __has_include(<NimBLEDevice.h>)
  #define USE_NIMBLE 1
  #include <NimBLEDevice.h>
#else
  #include <BLEDevice.h>
  #include <BLEUtils.h>
  #include <BLEServer.h>
  #include <BLE2902.h>
#endif

// ---------- Ultrasonic pins ----------
const int TRIG_PIN = 5;
const int ECHO_PIN = 18;

// ---------- Constants ----------
#define SOUND_SPEED_CM_PER_US 0.034f
const uint16_t MAX_CM = 500;              // clamp >5m to MAX_CM
const uint16_t ALERT_THRESHOLD_CM = 300;  // alert when <=3m
const uint32_t SAMPLE_INTERVAL_MS = 100;  // ~10Hz
const char* DEVICE_NAME = "BikeBlindspot";

// ---------- UUIDs ----------
#define SERVICE_UUID        "0000ffe5-0000-1000-8000-00805f9b34fb"
#define CHARACTERISTIC_UUID "0000ffe6-0000-1000-8000-00805f9b34fb"

// ---------- Globals ----------
#if defined(USE_NIMBLE)
  NimBLECharacteristic* gCharacteristic = nullptr;
  NimBLEAdvertising* gAdvertising = nullptr;
  bool gConnected = false;

  class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* /*pServer*/) override { gConnected = true;  Serial.println("BLE: Connected"); }
    void onDisconnect(NimBLEServer* /*pServer*/) override {
      gConnected = false; Serial.println("BLE: Disconnected, advertising...");
      NimBLEDevice::startAdvertising();
    }
  };
  class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override {
      std::string v = pCharacteristic->getValue();
      if (!v.empty()) { Serial.print("GOT WRITE: "); Serial.println(v.c_str()); }
    }
  };
#else
  BLECharacteristic* gCharacteristic = nullptr;
  BLEAdvertising* gAdvertising = nullptr;
  bool gConnected = false;

  class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* /*pServer*/) override { gConnected = true;  Serial.println("BLE: Connected"); }
    void onDisconnect(BLEServer* /*pServer*/) override {
      gConnected = false; Serial.println("BLE: Disconnected, advertising...");
      gAdvertising->start();
    }
  };
  class CharCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) override {
      String v = pCharacteristic->getValue();
      if (v.length()) { Serial.print("GOT WRITE: "); Serial.println(v); }
    }
  };
#endif

// ---------- Ultrasonic helpers ----------
float readDistanceCmOnce() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000UL); // 30ms timeout ~5m
  if (duration == 0) return NAN;

  float cm = (duration * SOUND_SPEED_CM_PER_US) / 2.0f;
  if (cm < 0) cm = NAN;
  return cm;
}

float readDistanceCmMedian3() {
  float a = readDistanceCmOnce(); delay(5);
  float b = readDistanceCmOnce(); delay(5);
  float c = readDistanceCmOnce();

  float arr[3] = {a, b, c};
  for (int i=0;i<3;i++) if (isnan(arr[i])) arr[i] = 99999.0f;
  for (int i=0;i<3;i++) for (int j=i+1;j<3;j++) if (arr[j] < arr[i]) { float t=arr[i]; arr[i]=arr[j]; arr[j]=t; }
  if (arr[1] >= 99999.0f) return NAN;
  return arr[1];
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

#if defined(USE_NIMBLE)
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(128);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

  gCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
  );
  gCharacteristic->setCallbacks(new CharCallbacks());

  // CCCD auto-created in NimBLE when NOTIFY is set

  pService->start();

  gAdvertising = NimBLEDevice::getAdvertising();
  gAdvertising->addServiceUUID(SERVICE_UUID);
  gAdvertising->setName(DEVICE_NAME);
  NimBLEDevice::startAdvertising();

#else
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(128);

  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  gCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_READ   |
    BLECharacteristic::PROPERTY_WRITE
  );
  gCharacteristic->setCallbacks(new CharCallbacks());
  gCharacteristic->addDescriptor(new BLE2902()); // allow central to enable notify

  pService->start();

  BLEAdvertisementData adv;
  adv.setName(DEVICE_NAME);
  adv.setCompleteServices(BLEUUID(SERVICE_UUID));

  gAdvertising = BLEDevice::getAdvertising();
  gAdvertising->setAdvertisementData(adv);
  gAdvertising->addServiceUUID(SERVICE_UUID);
  gAdvertising->setScanResponse(false);
  gAdvertising->start();
#endif

  Serial.println("BLE advertising started");
}

// ---------- Loop ----------
uint32_t lastSampleMs = 0;
uint16_t lastSentCm = 0xFFFF;
uint8_t lastSentAlert = 255;
uint32_t lastForceMs = 0;

void loop() {
  uint32_t now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  float cmF = readDistanceCmMedian3();
  if (isnan(cmF)) return;

  uint16_t cm = (uint16_t)roundf(constrain(cmF, 0.0f, (float)MAX_CM));
  uint8_t alert = (cm <= ALERT_THRESHOLD_CM) ? 1 : 0;

  bool changed = (cm != lastSentCm) || (alert != lastSentAlert);
  bool force = (now - lastForceMs) > 1000;

  if (gConnected && (changed || force)) {
    uint8_t payload[3];
    payload[0] = (uint8_t)(cm & 0xFF);
    payload[1] = (uint8_t)((cm >> 8) & 0xFF);
    payload[2] = alert;

#if defined(USE_NIMBLE)
    gCharacteristic->setValue((uint8_t*)payload, sizeof(payload));
    gCharacteristic->notify();
#else
    gCharacteristic->setValue(payload, sizeof(payload));
    gCharacteristic->notify();
#endif

    lastSentCm = cm;
    lastSentAlert = alert;
    if (force) lastForceMs = now;

    Serial.print("Notify -> cm="); Serial.print(cm);
    Serial.print(" alert="); Serial.println(alert);
  }
}
