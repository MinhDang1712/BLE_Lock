/* ============================================================
 * Lock_Units_nimble.cpp
 * ------------------------------------------------------------
 * Chuyen sang NimBLE-Arduino. Luu y: NimBLE TU DONG them CCCD
 * descriptor khi characteristic co PROPERTY NOTIFY -- KHONG can
 * include/goi BLE2902 nhu ban Bluedroid cu.
 *
 * CAP NHAT: bo toan bo EvalLogger (Firebase/HTTPS). Board nay
 * khong con dung WiFi (WiFi chi phuc vu logging cu, khong lien
 * quan logic khoa/mo). Log thu cong qua Serial Monitor, dinh
 * dang CSV don gian de copy tay ve file .csv roi xu ly.
 * ============================================================ */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ESP32Servo.h>

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UNLOCK_UUID "0000ff02-0000-1000-8000-00805f9b34fb"
#define CHAR_STATUS_UUID "0000ff03-0000-1000-8000-00805f9b34fb"

#define FIXED_TOKEN 0xA5C31F09UL
#define SERVO_PIN 13
#define LED_RED_PIN 2
#define LED_GREEN_PIN 4
#define SERVO_LOCKED_ANGLE 0
#define SERVO_UNLOCK_ANGLE 90
#define UNLOCK_HOLD_MS 5000  // tang len 5s (thay vi 3s goc) -- de trinh dien on dinh hon

typedef struct __attribute__((packed)){ uint32_t token; } unlock_req_pkt_t;
typedef struct __attribute__((packed)){ uint8_t result; } unlock_status_pkt_t;

NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pUnlockChar = nullptr;
NimBLECharacteristic *pStatusChar = nullptr;
Servo lockServo;

bool deviceConnected = false;
bool oldDeviceConnected = false;
bool isUnlocked = false;
unsigned long unlockedAt = 0;

static volatile int pendingConnEvent = -1;
static volatile int pendingUnlockResult = -1;

void sendStatus(uint8_t result);
void doUnlock();
void doLock();

// ---- Log thu cong qua Serial, dinh dang CSV co dinh --
// [LOG] millis,device,event,int,str,f1,f2,result
// De copy tay tu Serial Monitor ra file .csv roi doi chieu voi
// analyze.py hoac mo bang Excel. ----
void logEvent(const char* event, int32_t intVal = INT32_MIN, const char* strVal = "",
              float floatVal1 = NAN, float floatVal2 = NAN, const char* resultVal = "") {
    Serial.printf("[LOG] %lu,lock_unit,%s,%d,%s,%.2f,%.2f,%s\n",
                  millis(), event, intVal, strVal, floatVal1, floatVal2, resultVal);
}

class MyServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *pServer) override {
        deviceConnected = true;
        Serial.println("[LOCK] Key Unit da ket noi");
        pendingConnEvent = 0;
    }
    void onDisconnect(NimBLEServer *pServer) override {
        deviceConnected = false;
        Serial.println("[LOCK] Key Unit ngat ket noi");
        pendingConnEvent = 1;
    }
};

class UnlockCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar) override {
        std::string value = pChar->getValue();

        if (value.length() != sizeof(unlock_req_pkt_t)) {
            Serial.println("[LOCK] Goi tin sai kich thuoc, bo qua");
            sendStatus(1);
            return;
        }

        unlock_req_pkt_t req;
        memcpy(&req, value.data(), sizeof(req));
        Serial.printf("[LOCK] Nhan UNLOCK_REQUEST, token=0x%08X\n", req.token);

        if (req.token == FIXED_TOKEN) {
            Serial.println("[LOCK] Token hop le -> MO KHOA");
            doUnlock();
            sendStatus(0);
            pendingUnlockResult = 0;
        } else {
            Serial.println("[LOCK] Token SAI -> tu choi");
            sendStatus(1);
            pendingUnlockResult = 1;
        }
    }
};

void sendStatus(uint8_t result) {
    unlock_status_pkt_t pkt = { result };
    pStatusChar->setValue((uint8_t*)&pkt, sizeof(pkt));
    pStatusChar->notify();
}

void doUnlock() {
    lockServo.write(SERVO_UNLOCK_ANGLE);
    digitalWrite(LED_GREEN_PIN, HIGH);
    digitalWrite(LED_RED_PIN, LOW);
    isUnlocked = true;
    unlockedAt = millis();
    Serial.println("[LOCK] Da mo khoa (LED XANH)");
}

void doLock() {
    lockServo.write(SERVO_LOCKED_ANGLE);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, HIGH);
    isUnlocked = false;
    Serial.println("[LOCK] Da tu khoa lai (LED DO)");
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[LOCK] Khoi dong (che do log thu cong qua Serial Monitor)");
    Serial.println("[LOG] millis,device,event,int,str,f1,f2,result");

    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_GREEN_PIN, OUTPUT);

    // Cap phat rieng 1 timer LEDC cho Servo TRUOC khi attach --
    // ESP32Servo va WiFi deu can dung timer phan cung de tao PWM,
    // neu khong chi dinh ro se bi xung dot, khien servo NHAN LENH
    // nhung KHONG DI CHUYEN (trong khi LED van sang binh thuong
    // vi LED khong dung timer PWM).
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
    lockServo.setPeriodHertz(50);  // chuan servo 50Hz
    lockServo.attach(SERVO_PIN, 500, 2400);  // chi ro khoang xung (us) chuan cho SG90
    lockServo.write(SERVO_LOCKED_ANGLE);
    digitalWrite(LED_RED_PIN, HIGH);
    digitalWrite(LED_GREEN_PIN, LOW);

    NimBLEDevice::init("LockUnit_ESP32");

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    pUnlockChar = pService->createCharacteristic(
        CHAR_UNLOCK_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pUnlockChar->setCallbacks(new UnlockCallbacks());

    // Khong can addDescriptor(new BLE2902()) -- NimBLE tu dong them CCCD
    pStatusChar = pService->createCharacteristic(CHAR_STATUS_UUID, NIMBLE_PROPERTY::NOTIFY);

    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.println("[LOCK] BLE Server started, advertising...");
}

void loop() {
    if (pendingConnEvent != -1) {
        int ev = pendingConnEvent;
        pendingConnEvent = -1;
        logEvent(ev == 0 ? "ble_connected" : "ble_disconnected");
    }
    if (pendingUnlockResult != -1) {
        int result = pendingUnlockResult;
        pendingUnlockResult = -1;
        logEvent("unlock_result", INT32_MIN, "", NAN, NAN, result == 0 ? "OK" : "REJECTED");
    }

    if (isUnlocked && millis() - unlockedAt > UNLOCK_HOLD_MS) {
        doLock();
    }

    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        pServer->startAdvertising();
        Serial.println("[LOCK] Restart advertising");
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }
}
