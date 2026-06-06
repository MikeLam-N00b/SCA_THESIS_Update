#pragma once
// anchor_ble.h — CAN helpers, BLE callback classes, and BLE server init.

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include "anchor_ipc.h"
#include "anchor_crypto.h"
#include "anchor_config.h"
#include "friend_token.h"

// =============================================================================
// CAN helpers
// =============================================================================

static void canLock() {
    if (!pCanControl) return;
    bool ok = pCanControl->lockCar();
    carUnlocked = false;  // force locked regardless of CAN result
    Serial.printf(">> Car LOCKED %s\n", ok ? "(CAN OK)" : "(CAN FAILED — state forced)");
}

static void canUnlock() {
    if (carUnlocked || !pCanControl) return;
    if (pCanControl->unlockCar()) {
        carUnlocked = true;
        Serial.println(">> Car UNLOCKED");
    }
}

// =============================================================================
// Friend sharing helpers
// =============================================================================

static bool uwb_check_proximity_ok(void) {
    return (xEventGroupGetBits(sysEvents) & EVT_UWB_ACTIVE) != 0;
}

// Send 2-byte [accepted, reason] notification to guest via friend status characteristic.
static void ble_notify_friend_status(uint8_t accepted, token_verify_result_t reason) {
    if (!pFriendStatusChar) return;
    uint8_t payload[2] = { accepted, (uint8_t)reason };
    pFriendStatusChar->setValue(payload, 2);
    pFriendStatusChar->notify();
}

// =============================================================================
// BLE callbacks — only queue messages, no heavy work inside callbacks
// =============================================================================

class AuthCharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        uint8_t *pData = pChar->getData();
        size_t   len   = pChar->getLength();
        if (!pData || len == 0) return;

        size_t toCopy = len;
        if (toCopy > 32 - responseBufferLen) toCopy = 32 - responseBufferLen;
        memcpy(responseBuffer + responseBufferLen, pData, toCopy);
        responseBufferLen += toCopy;

        if (responseBufferLen >= 32) {
            BleCmdMsg msg;
            msg.type    = BLE_AUTH_VERIFY;
            msg.dataLen = 32;
            memcpy(msg.data, responseBuffer, 32);
            responseBufferLen = 0;
            xQueueSend(bleQueue, &msg, pdMS_TO_TICKS(10));
        }
    }
};

class CharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        if (!pChar) return;
        const uint8_t *pData = pChar->getData();
        size_t         len   = pChar->getLength();
        if (!pData || len == 0) return;

        #define STARTS(lit) (len >= sizeof(lit)-1 && memcmp(pData, lit, sizeof(lit)-1) == 0)

        uint8_t cmd;
        if (STARTS("VERIFIED:")) {
            Serial.printf("[BLE] VERIFIED received (carUnlocked=%d)\n", (int)carUnlocked);
            if (!carUnlocked) {
                cmd = CAN_CMD_UNLOCK;
                BaseType_t sent = xQueueSend(canQueue, &cmd, pdMS_TO_TICKS(10));
                Serial.printf("[BLE] canQueue send=%d\n", (int)sent);
            }
        } else if (STARTS("WARNING:") || STARTS("LOCK_CAR")) {
            if (carUnlocked) {
                cmd = CAN_CMD_LOCK;
                xQueueSend(canQueue, &cmd, pdMS_TO_TICKS(10));
            }
        } else if (STARTS("UWB_STOP")) {
            if (carUnlocked) {
                cmd = CAN_CMD_LOCK;
                xQueueSend(canQueue, &cmd, pdMS_TO_TICKS(10));
            }
            cmd = UWB_CMD_DEINIT;
            xQueueSend(uwbQueue, &cmd, pdMS_TO_TICKS(10));
            Serial.println("UWB: Tag beyond 20m");
        } else if (STARTS("TAG_UWB_READY")) {
            bool authed   = (xEventGroupGetBits(sysEvents) & EVT_AUTHED) != 0;
            bool friendOk = s_friendBundleVerified;
            Serial.printf("[BLE] TAG_UWB_READY received — authed=%d friend=%d\n",
                          (int)authed, (int)friendOk);
            if (authed || friendOk) {
                cmd = UWB_CMD_INIT;
                BaseType_t sent = xQueueSend(uwbQueue, &cmd, pdMS_TO_TICKS(10));
                Serial.printf("[BLE] UWB_CMD_INIT queued=%d\n", (int)(sent == pdTRUE));
            }
        } else if (STARTS("ALERT:RELAY_ATTACK")) {
            Serial.println("SECURITY ALERT: Relay attack detected!");
        }

        #undef STARTS
    }
};

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) override {
        deviceConnected        = true;
        authenticated          = false;
        responseBufferLen      = 0;
        s_bundleBufLen         = 0;
        s_friendBundleVerified = false;
        connectionGen++;
        xEventGroupSetBits(sysEvents, EVT_CONNECTED);
        xEventGroupClearBits(sysEvents, EVT_AUTHED | EVT_UWB_ACTIVE);
        Serial.printf("BLE: Tag connected (gen=%u)\n", (unsigned)connectionGen);

        // Queue key check first — key is loaded (or fetched from server) before challenge is sent.
        BleCmdMsg msg = {};
        msg.type    = BLE_KEY_CHECK;
        msg.data[0] = connectionGen;
        xQueueSend(bleQueue, &msg, pdMS_TO_TICKS(10));
    }

    void onDisconnect(BLEServer *pServer) override {
        deviceConnected = false;
        authenticated   = false;
        xEventGroupClearBits(sysEvents, EVT_CONNECTED | EVT_AUTHED | EVT_UWB_ACTIVE);
        Serial.println("BLE: Tag disconnected");

        uint8_t cmd;
        cmd = UWB_CMD_DEINIT; xQueueSend(uwbQueue, &cmd, pdMS_TO_TICKS(10));
        s_friendBundleVerified = false;
        s_uwbFriendMode = false;
        memset(s_activeFriendKey, 0, sizeof(s_activeFriendKey));

        if (carUnlocked) {
            cmd = CAN_CMD_LOCK; xQueueSend(canQueue, &cmd, pdMS_TO_TICKS(10));
        } else {
            Serial.println("[onDisconnect] Car not unlocked — skipping auto-lock");
        }

        BleCmdMsg msg = {}; msg.type = BLE_RESTART_ADV;
        xQueueSend(bleQueue, &msg, pdMS_TO_TICKS(10));
    }
};

// Accumulates fragmented BLE writes and pushes a parsed friend_bundle_t to bundleQueue.
class BundleSubmitCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        const uint8_t *data = pChar->getData();
        size_t len = pChar->getLength();
        if (!data || len == 0) return;

        size_t space  = sizeof(s_bundleBuf) - s_bundleBufLen;
        size_t toCopy = (len < space) ? len : space;
        memcpy(s_bundleBuf + s_bundleBufLen, data, toCopy);
        s_bundleBufLen += toCopy;

        if (s_bundleBufLen < BUNDLE_BIN_SIZE) return;

        friend_bundle_t bundle;
        if (ft_parse_bundle_wire(s_bundleBuf, BUNDLE_BIN_SIZE, &bundle)) {
            BaseType_t sent = xQueueSend(bundleQueue, &bundle, pdMS_TO_TICKS(10));
            Serial.printf("[BLE] Bundle queued=%d id=%02x%02x...\n",
                          (int)sent, bundle.friend_id[0], bundle.friend_id[1]);
        } else {
            Serial.println("[BLE] Bundle parse failed — discarding");
        }
        s_bundleBufLen = 0;
    }
};

// =============================================================================
// BLE server init
// =============================================================================

static void startBLE() {
    // pairingKey is loaded lazily in BLE_KEY_CHECK (at connection time).
    // Do NOT load it here — bleKeyHex may be empty when BLE starts without a stored key.

    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setPower(ESP_PWR_LVL_P9);
    pBleServer = BLEDevice::createServer();
    pBleServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pBleServer->createService(SERVICE_UUID);

    pChallengeCharacteristic = pService->createCharacteristic(
        CHALLENGE_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pChallengeCharacteristic->addDescriptor(new BLE2902());

    pAuthCharacteristic = pService->createCharacteristic(
        AUTH_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    pAuthCharacteristic->setCallbacks(new AuthCharacteristicCallbacks());
    pAuthCharacteristic->addDescriptor(new BLE2902());

    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
    pCharacteristic->setCallbacks(new CharacteristicCallbacks());
    pCharacteristic->addDescriptor(new BLE2902());
    pCharacteristic->setValue("ANCHOR_READY");

    pService->start();

    // Friend sharing GATT service (0xFACE)
    BLEService *pFriendService = pBleServer->createService(FRIEND_SERVICE_UUID);

    pBundleSubmitChar = pFriendService->createCharacteristic(
        FRIEND_BUNDLE_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    pBundleSubmitChar->setCallbacks(new BundleSubmitCallbacks());

    pFriendStatusChar = pFriendService->createCharacteristic(
        FRIEND_STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    pFriendStatusChar->addDescriptor(new BLE2902());

    pFriendService->start();
    Serial.println("BLE: Friend service started (0xFACE)");

    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    pAdv->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();
    Serial.println("BLE advertising: " + String(DEVICE_NAME));
}
