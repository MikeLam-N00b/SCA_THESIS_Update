/**
 * ESP32 DevKit V1 — BLE Peripheral (Anchor)
 * ────────────────────────────────────────
 * Đơn giản: chỉ quảng bá BLE để S3 (Central) kết nối.
 *
 * S3 sẽ:
 * - Scan tìm device này
 * - Connect vào
 * - Relay key và distance
 *
 * Board: ESP32 Dev Module
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-1234-1234-abcdefabcdef"
#define DEVICE_NAME         "ESP32-DevKit"

BLEServer*         pServer         = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool deviceConnected    = false;
bool oldDeviceConnected = false;

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pSrv) override {
        deviceConnected = true;
        Serial.println("✓ S3 connected");
    }
    void onDisconnect(BLEServer* pSrv) override {
        deviceConnected = false;
        Serial.println("✓ S3 disconnected");
    }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) override {
        Serial.println("[onWrite] triggered");   // luôn in — debug xem callback có chạy không
        uint8_t* data   = pChar->getData();
        size_t   length = pChar->getLength();
        if (data == nullptr || length == 0) {
            Serial.println("[onWrite] empty data");
            return;
        }
        // data là ASCII hex string (ví dụ: "1b1e53c7..."), in thẳng ra
        String keyHex = String((char*)data, length);
        Serial.println("==============================");
        Serial.println("  KEY RECEIVED FROM TAG");
        Serial.println("  HEX : " + keyHex);
        Serial.println("  SIZE: " + String(length / 2) + " bytes");
        Serial.println("==============================");
    }
};

void setup() {
    Serial.begin(115200);
    delay(1000);

    BLEDevice::init(DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ     |
        BLECharacteristic::PROPERTY_WRITE    |
        BLECharacteristic::PROPERTY_WRITE_NR |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pCharacteristic->addDescriptor(new BLE2902());
    pCharacteristic->setCallbacks(new CharacteristicCallbacks());
    pCharacteristic->setValue("DevKit Ready");

    pService->start();

    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.println("BLE Advertising: " + String(DEVICE_NAME));
    Serial.print("MAC: ");
    Serial.println(BLEDevice::getAddress().toString().c_str());
}

void loop() {
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = true;
    }

    if (!deviceConnected && oldDeviceConnected) {
        oldDeviceConnected = false;
        delay(500);
        pServer->startAdvertising();
        BLEDevice::startAdvertising();
        Serial.println("Restarting BLE advertising...");
    }

    delay(1000);
}
