
#include <BLEDevice.h>
#include <BLEScan.h>
#include <SPI.h>
#include "dw3000.h"
#include "tag_config.h"
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// =============================================================================
// FreeRTOS IPC primitives
// =============================================================================

static EventGroupHandle_t sysEvents;
#define EVT_CONNECTED        (1 << 0)
#define EVT_AUTHED           (1 << 1)
#define EVT_ANCHOR_UWB_READY (1 << 2)
#define EVT_UWB_INIT         (1 << 3)
#define EVT_UWB_STOP         (1 << 4)
#define EVT_DEVICE_FOUND     (1 << 5)

// Queue: uwbTask gửi BLE write requests → bleTask (Core 0) thực hiện
struct BleWriteMsg { char data[32]; uint8_t len; };
static QueueHandle_t bleWriteQueue;

// =============================================================================
// State
// =============================================================================

static volatile bool connected       = false;
static volatile bool authenticated   = false;
static volatile bool uwbInitialized  = false;
static volatile bool tagInUnlockZone = false;
static volatile bool uwbStoppedFar   = false;
static          bool anchorUwbReady  = false;
static volatile int  currentRssi     = 0;

// ── Friend mode ───────────────────────────────────────────────────────────────
static volatile bool isFriendMode       = false;
static uint8_t       friendKey[16]      = {};
static uint8_t       s_bundleWire[BUNDLE_WIRE_MAX_LEN] = {};
static size_t        s_bundleWireLen    = 0;
// Anchor gửi 2-byte binary payload: [0]=0 accepted / 1 rejected, [1]=reason
static volatile bool s_friendAccepted   = false;
static volatile bool s_friendStatusRcvd = false;

// =============================================================================
// BLE handles
// =============================================================================

static BLEAdvertisedDevice*     myDevice              = nullptr;
static BLEClient*               pClient               = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static BLERemoteCharacteristic* pChallengeChar        = nullptr;
static BLERemoteCharacteristic* pAuthChar             = nullptr;
static BLERemoteCharacteristic* pFriendBundleChar     = nullptr;
static BLERemoteCharacteristic* pFriendStatusChar     = nullptr;

static uint8_t pairingKey[16];

// =============================================================================
// UWB frame buffers + config
// =============================================================================

// Channel 5 | 1024-symbol preamble | PAC 32 | code 9 | 850 kbps | STS mode 1
static dwt_config_t uwbConfig = {
    5, DWT_PLEN_1024, DWT_PAC32, 9, 9, 1,
    DWT_BR_850K, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
    1001, DWT_STS_MODE_1, DWT_STS_LEN_256, DWT_PDOA_M0
};

static dwt_sts_cp_key_t sts_key;
static dwt_sts_cp_iv_t  sts_iv;
static bool stsConfigured = false;
extern dwt_txconfig_t txconfig_options;

static uint8_t tx_poll_msg[] = {0x41U,0x88U,0U,0xCAU,0xDEU,'W','A','V','E',0xE0U,0U,0U};
static uint8_t rx_resp_msg[] = {0x41U,0x88U,0U,0xCAU,0xDEU,'V','E','W','A',0xE1U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U};
static uint8_t  frame_seq_nb = 0U;
static uint8_t  rx_buffer[MSG_BUFFER_SIZE];

// =============================================================================
// Distance filter (moving average)
// =============================================================================

// float thay double: ESP32-S3 FPU chỉ hỗ trợ single-precision hardware
static float   distBuf[DIST_FILTER_SIZE] = {};
static uint8_t distBufIdx  = 0;
static bool    distBufFull = false;

static float applyDistanceFilter(float raw) {
    distBuf[distBufIdx] = raw;
    distBufIdx = (distBufIdx + 1) % DIST_FILTER_SIZE;
    if (distBufIdx == 0) distBufFull = true;
    uint8_t count = distBufFull ? DIST_FILTER_SIZE : distBufIdx;
    float sum = 0.0f;
    for (uint8_t i = 0; i < count; i++) sum += distBuf[i];
    return sum / count;
}

static void resetDistanceFilter() {
    memset(distBuf, 0, sizeof(distBuf));
    distBufIdx  = 0;
    distBufFull = false;
}

// =============================================================================
// Crypto helpers
// =============================================================================

static void hexStringToBytes(const char* hex, uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; i++)
        sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
}

static bool computeHMAC(const uint8_t* key, size_t keyLen,
                        const uint8_t* data, size_t dataLen,
                        uint8_t* output) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return (mbedtls_md_hmac(md, key, keyLen, data, dataLen, output) == 0);
}

static void printHex(const char* label, const uint8_t* data, size_t length) {
    Serial.print(label);
    for (size_t i = 0; i < length; i++) {
        if (data[i] < 0x10) Serial.print("0");
        Serial.print(data[i], HEX);
    }
    Serial.println();
}

// =============================================================================
// NVS helpers — namespace "sca_tag"
// Key length ≤ 15 chars (Preferences library limit)
// =============================================================================

static void saveServerPubkeyToNvs(const uint8_t* der, size_t len) {
    Preferences prefs;
    prefs.begin("sca_tag", false);
    prefs.putBytes("srv_pub", der, len);
    prefs.end();
    Serial.printf("[NVS] server pubkey saved (%u bytes)\n", len);
}

static bool loadServerPubkeyFromNvs(uint8_t* buf, size_t bufLen, size_t* outLen) {
    Preferences prefs;
    prefs.begin("sca_tag", true);
    size_t stored = prefs.getBytesLength("srv_pub");
    bool ok = (stored > 0 && stored <= bufLen);
    if (ok) {
        prefs.getBytes("srv_pub", buf, stored);
        *outLen = stored;
    }
    prefs.end();
    return ok;
}

static void saveBundleWireToNvs() {
    Preferences prefs;
    prefs.begin("sca_tag", false);
    prefs.putBytes("friend_wire", s_bundleWire, s_bundleWireLen);
    prefs.putUInt("friend_len", (uint32_t)s_bundleWireLen);
    prefs.end();
}

static void loadFriendBundleFromNVS() {
    Preferences prefs;
    prefs.begin("sca_tag", true);
    uint32_t len = prefs.getUInt("friend_len", 0);
    if (len > 0 && len <= BUNDLE_WIRE_MAX_LEN) {
        prefs.getBytes("friend_wire", s_bundleWire, len);
        s_bundleWireLen = len;
        memcpy(friendKey, s_bundleWire + 41, FRIEND_KEY_LEN);  // offset 41 = friend_key
        isFriendMode = true;
        Serial.printf("[setup] Friend bundle loaded (%u bytes) — friend mode ON\n", len);
    }
    prefs.end();
}

// =============================================================================
// ECDSA verify — SHA256withECDSA, server pubkey DER từ NVS
// =============================================================================

static bool verifyBundleEcdsa(const char* msg, const uint8_t* sigDer, size_t sigLen) {
    uint8_t pubKeyDer[128];
    size_t  pubKeyLen = 0;
    if (!loadServerPubkeyFromNvs(pubKeyDer, sizeof(pubKeyDer), &pubKeyLen)) {
        Serial.println("[ecdsa] No server pubkey in NVS");
        return false;
    }

    uint8_t hash[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md(md, (const uint8_t*)msg, strlen(msg), hash);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;
    if (mbedtls_pk_parse_public_key(&pk, pubKeyDer, pubKeyLen) == 0) {
        ok = (mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sigDer, sigLen) == 0);
        if (!ok) Serial.println("[ecdsa] Signature mismatch");
    } else {
        Serial.println("[ecdsa] Failed to parse server pubkey DER");
    }
    mbedtls_pk_free(&pk);
    return ok;
}

// =============================================================================
// processSetBundle — decode base64 JSON, verify ECDSA, pack wire format, save NVS
// Gọi từ serialTask khi nhận lệnh SET_BUNDLE.
// =============================================================================

static bool processSetBundle(const char* bundleB64) {
    // 1. Base64-decode → JSON string
    static uint8_t jsonBuf[768];
    size_t jsonLen = 0;
    if (mbedtls_base64_decode(jsonBuf, sizeof(jsonBuf) - 1, &jsonLen,
                              (const unsigned char*)bundleB64, strlen(bundleB64)) != 0) {
        Serial.println("[serial] SET_BUNDLE: base64 decode failed");
        return false;
    }
    jsonBuf[jsonLen] = '\0';

    // 2. Parse JSON
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, (char*)jsonBuf);
    if (err) {
        Serial.printf("[serial] SET_BUNDLE: JSON parse error: %s\n", err.c_str());
        return false;
    }

    int         bundle_version = doc["bundle_version"] | 0;
    const char* friend_id      = doc["friend_id"]      | "";
    const char* vehicle_id     = doc["vehicle_id"]     | "";
    const char* friend_key_hex = doc["friend_key_hex"] | "";
    int         permissions    = doc["permissions"]    | 0;
    const char* issued_at      = doc["issued_at"]      | "";
    const char* expires_at     = doc["expires_at"]     | "";
    const char* issuer_sig_b64 = doc["issuer_sig_b64"] | "";

    if (strlen(friend_id)      != FRIEND_ID_LEN * 2 ||   // 16 hex chars = 8 bytes
        strlen(friend_key_hex) != FRIEND_KEY_LEN * 2 ||  // 32 hex chars = 16 bytes
        strlen(issuer_sig_b64) == 0) {
        Serial.println("[serial] SET_BUNDLE: invalid field lengths");
        return false;
    }

    // 3. Base64-decode ECDSA signature → DER bytes
    uint8_t sigDer[ECDSA_SIG_DER_MAX];
    size_t  sigLen = 0;
    if (mbedtls_base64_decode(sigDer, sizeof(sigDer), &sigLen,
                              (const unsigned char*)issuer_sig_b64,
                              strlen(issuer_sig_b64)) != 0) {
        Serial.println("[serial] SET_BUNDLE: sig base64 decode failed");
        return false;
    }

    // 4. Build canonical message và verify ECDSA
    // Format phải khớp server _friend_bundle_message() và Anchor friend_token.h
    static char msgBuf[256];
    snprintf(msgBuf, sizeof(msgBuf), "v%d|%s|%s|%s|%s|%s|%d",
             bundle_version, friend_id, vehicle_id, friend_key_hex,
             issued_at, expires_at, permissions);

    if (!verifyBundleEcdsa(msgBuf, sigDer, sigLen)) {
        Serial.println("[serial] SET_BUNDLE: ECDSA verify FAILED — rejected");
        return false;
    }

    // 5. Pack binary wire format — phải khớp ft_parse_bundle_wire() của Anchor
    // Byte map:
    //   [0]        version        (1 byte)
    //   [1..8]     friend_id      (8 raw bytes, hex-decoded)
    //   [9..40]    vehicle_id     (32 bytes, null-padded ASCII)
    //   [41..56]   friend_key     (16 raw bytes, hex-decoded)
    //   [57]       permissions    (1 byte)
    //   [58..89]   issued_at_iso  (32 bytes, null-padded ASCII)
    //   [90..121]  expires_at_iso (32 bytes, null-padded ASCII)
    //   [122]      sig_len        (1 byte)
    //   [123..]    sig DER        (up to 72 bytes)
    memset(s_bundleWire, 0, sizeof(s_bundleWire));
    s_bundleWire[0] = (uint8_t)bundle_version;
    hexStringToBytes(friend_id,      s_bundleWire + 1,  FRIEND_ID_LEN);
    strncpy((char*)(s_bundleWire + 9),  vehicle_id, VEHICLE_ID_MAX_LEN - 1);
    hexStringToBytes(friend_key_hex, s_bundleWire + 41, FRIEND_KEY_LEN);
    memcpy(friendKey, s_bundleWire + 41, FRIEND_KEY_LEN);
    s_bundleWire[57] = (uint8_t)permissions;
    strncpy((char*)(s_bundleWire + 58), issued_at,  31);
    strncpy((char*)(s_bundleWire + 90), expires_at, 31);
    s_bundleWire[122] = (uint8_t)sigLen;
    memcpy(s_bundleWire + 123, sigDer, sigLen);
    s_bundleWireLen = BUNDLE_WIRE_HEADER_LEN + sigLen;

    // 6. Persist và activate friend mode
    saveBundleWireToNvs();
    isFriendMode = true;

    Serial.printf("[serial] SET_BUNDLE: OK, wirelen=%u\n", s_bundleWireLen);
    return true;
}

// =============================================================================
// UWB init / deinit (chỉ dùng trong owner mode)
// =============================================================================

static bool initUWB() {
    if (uwbInitialized) return true;
    Serial.println("[uwbTask] UWB: initializing...");

    spiBegin(PIN_IRQ, PIN_RST);
    { extern uint8_t _ss; _ss = PIN_SS; }
    pinMode(PIN_SS, OUTPUT); digitalWrite(PIN_SS, HIGH);

    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(2));
    pinMode(PIN_RST, INPUT);
    vTaskDelay(pdMS_TO_TICKS(50));

    int retries = 500;
    while (!dwt_checkidlerc() && retries-- > 0) vTaskDelay(pdMS_TO_TICKS(1));
    if (retries <= 0) { Serial.println("UWB: IDLE_RC timeout"); goto fail; }
    if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) { Serial.println("UWB: init failed"); goto fail; }

    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
    if (dwt_configure(&uwbConfig) != 0) { Serial.println("UWB: configure failed"); goto fail; }

    dwt_configuretxrf(&txconfig_options);
    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);
    dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    // Derive STS key từ pairingKey — phải khớp với Anchor
    memcpy(&sts_key, pairingKey, sizeof(sts_key));
    sts_iv.iv0 = 0x00000001U;
    sts_iv.iv1 = 0x00000000U;
    sts_iv.iv2 = 0x00000000U;
    sts_iv.iv3 = 0x00000000U;
    dwt_configurestskey(&sts_key);
    dwt_configurestsiv(&sts_iv);
    dwt_configurestsloadiv();
    stsConfigured = true;

    uwbInitialized = true;
    Serial.println("[uwbTask] UWB: ready (STS mode 1)");
    return true;
fail:
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    return false;
}

static void deinitUWB() {
    if (!uwbInitialized) return;
    dwt_forcetrxoff();
    dwt_softreset();
    vTaskDelay(pdMS_TO_TICKS(2));
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    uwbInitialized  = false;
    stsConfigured   = false;
    tagInUnlockZone = false;
    resetDistanceFilter();
    Serial.println("[uwbTask] UWB: stopped");
}

// =============================================================================
// UWB initiator loop (SS-TWR) — chạy trong uwbTask
// =============================================================================

static bool uwbInitiatorLoop() {
    dwt_writetodevice(STS_IV0_ID, 0, 4, (uint8_t*)&sts_iv.iv0);
    dwt_configurestsloadiv();

    dwt_write32bitreg(SYS_STATUS_ID,
        SYS_STATUS_TXFRS_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);

    tx_poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
    dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0U);
    dwt_writetxfctrl(sizeof(tx_poll_msg), 0U, 1);

    if (dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED) != DWT_SUCCESS) {
        frame_seq_nb++; return false;
    }

    uint32_t status_reg = 0U;
    unsigned long t0 = millis();
    while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
             (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR))) {
        if ((millis() - t0) > 600UL) { dwt_forcetrxoff(); frame_seq_nb++; return false; }
        taskYIELD();
    }
    frame_seq_nb++;

    if (!(status_reg & SYS_STATUS_RXFCG_BIT_MASK)) {
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        return false;
    }

    // Kiểm tra STS quality — từ chối nếu STS không hợp lệ (relay attack)
    int16_t stsQual;
    if (dwt_readstsquality(&stsQual) < 0) {
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_GOOD);
        return false;
    }

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
    if (frame_len == 0 || frame_len > sizeof(rx_buffer)) return false;

    dwt_readrxdata(rx_buffer, frame_len, 0U);
    rx_buffer[ALL_MSG_SN_IDX] = 0U;
    if (memcmp(rx_buffer, rx_resp_msg, ALL_MSG_COMMON_LEN) != 0) return false;

    uint32_t poll_tx_ts = dwt_readtxtimestamplo32();
    uint32_t resp_rx_ts = dwt_readrxtimestamplo32();
    float clockOffsetRatio = (float)dwt_readclockoffset() / (float)(1UL << 26);

    uint32_t poll_rx_ts, resp_tx_ts;
    resp_msg_get_ts(&rx_buffer[RESP_MSG_POLL_RX_TS_IDX], &poll_rx_ts);
    resp_msg_get_ts(&rx_buffer[RESP_MSG_RESP_TX_TS_IDX], &resp_tx_ts);

    int32_t rtd_init = (int32_t)(resp_rx_ts - poll_tx_ts);
    int32_t rtd_resp = (int32_t)(resp_tx_ts - poll_rx_ts);
    float tof = (((float)rtd_init - ((float)rtd_resp * (1.0f - clockOffsetRatio))) / 2.0f)
                * (float)DWT_TIME_UNITS;
    float distance = tof * (float)SPEED_OF_LIGHT;

    if (distance < 0.0f || distance > 100.0f) return false;

    float filtDist = applyDistanceFilter(distance);

    if (filtDist > UWB_FAR_DISTANCE_M) {
        tagInUnlockZone = false;
        if (connected) {
            BleWriteMsg wm; wm.len = 8; memcpy(wm.data, "UWB_STOP", 8);
            xQueueSend(bleWriteQueue, &wm, 0);
        }
        Serial.printf("[uwbTask] avg=%.1f m — beyond 20m, stopping UWB\n", filtDist);
        return true;
    }

    bool shouldUnlock = (filtDist <= UWB_UNLOCK_DISTANCE_M);
    bool shouldLock   = (filtDist >  UWB_LOCK_DISTANCE_M);

    if (shouldUnlock && !tagInUnlockZone) {
        tagInUnlockZone = true;
        if (connected) {
            BleWriteMsg wm;
            wm.len = (uint8_t)snprintf(wm.data, sizeof(wm.data), "VERIFIED:%.1fm", filtDist);
            xQueueSend(bleWriteQueue, &wm, 0);
        }
        Serial.printf("[uwbTask] avg=%.1f m — UNLOCK\n", filtDist);
    } else if (shouldLock && tagInUnlockZone) {
        tagInUnlockZone = false;
        if (connected) {
            BleWriteMsg wm;
            wm.len = (uint8_t)snprintf(wm.data, sizeof(wm.data), "WARNING:%.1fm", filtDist);
            xQueueSend(bleWriteQueue, &wm, 0);
        }
        Serial.printf("[uwbTask] avg=%.1f m — LOCK\n", filtDist);
    }

    static unsigned long lastDistLog = 0;
    if (millis() - lastDistLog > 500) {
        lastDistLog = millis();
        Serial.printf("[uwbTask] raw=%.1f avg=%.1f m %s | RSSI=%d dBm\n",
                      distance, filtDist, tagInUnlockZone ? "[UNLOCKED]" : "[LOCKED]", currentRssi);
    }
    return false;
}

// =============================================================================
// BLE callbacks
// =============================================================================

// notifyCallback: chạy trong BLE stack task (Core 0) — zero allocation
static void notifyCallback(BLERemoteCharacteristic* pChar,
                           uint8_t* pData, size_t length, bool isNotify) {
    if (!pData || length == 0) return;

    if (pChar == pAuthChar) {
        if (length == 7 && memcmp(pData, "AUTH_OK", 7) == 0) {
            authenticated = true;
            xEventGroupSetBits(sysEvents, EVT_AUTHED);
            Serial.println("[BLE notify] AUTH_OK");
        } else if (length == 9 && memcmp(pData, "AUTH_FAIL", 9) == 0) {
            authenticated = false;
            Serial.println("[BLE notify] AUTH_FAIL");
        }
    } else if (pChar == pRemoteCharacteristic) {
        if (length >= 10 && memcmp(pData, "UWB_ACTIVE", 10) == 0) {
            anchorUwbReady = true;
            xEventGroupSetBits(sysEvents, EVT_ANCHOR_UWB_READY);
            Serial.println("[BLE notify] UWB_ACTIVE received");
        }
    } else if (pChar == pFriendStatusChar) {
        // Anchor gửi binary 2-byte: [0]=0 accepted / 1 rejected, [1]=reason code
        // Xem ble_notify_friend_status() trong Anchor firmware
        if (length >= 1 && pData[0] == 0) {
            s_friendAccepted   = true;
            s_friendStatusRcvd = true;
            Serial.println("[BLE notify] FRIEND_OK — access granted");
        } else {
            s_friendAccepted   = false;
            s_friendStatusRcvd = true;
            uint8_t reason = (length >= 2) ? pData[1] : 0xFF;
            Serial.printf("[BLE notify] FRIEND_FAIL reason=%u\n", reason);
        }
    }
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!advertisedDevice.haveServiceUUID() ||
            !advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID))) return;

        int rssi = advertisedDevice.getRSSI();
        Serial.printf("[BLE scan] Anchor: %s | RSSI: %d dBm\n",
                      advertisedDevice.toString().c_str(), rssi);

        if (rssi < RSSI_THRESHOLD_DBM) {
            Serial.printf("[BLE scan] RSSI %d < threshold %d — too far\n",
                          rssi, RSSI_THRESHOLD_DBM);
            BLEDevice::getScan()->stop();
            return;
        }

        delete myDevice;
        myDevice = new BLEAdvertisedDevice(advertisedDevice);
        BLEDevice::getScan()->stop();
        xEventGroupSetBits(sysEvents, EVT_DEVICE_FOUND);
    }
};

class MyClientCallback : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) override {
        connected = true;
        xEventGroupSetBits(sysEvents, EVT_CONNECTED);
        xEventGroupClearBits(sysEvents, EVT_UWB_STOP);
        Serial.println("[BLE] Connected to Anchor");
    }
    void onDisconnect(BLEClient* pclient) override {
        Serial.println("[BLE] Disconnected from Anchor");
        connected             = false;
        authenticated         = false;
        anchorUwbReady        = false;
        tagInUnlockZone       = false;
        uwbStoppedFar         = false;
        pRemoteCharacteristic = nullptr;
        pChallengeChar        = nullptr;
        pAuthChar             = nullptr;
        pFriendBundleChar     = nullptr;
        pFriendStatusChar     = nullptr;
        s_friendStatusRcvd    = true;  // unblock connectAsFriend wait loop

        xEventGroupClearBits(sysEvents, EVT_CONNECTED | EVT_AUTHED | EVT_ANCHOR_UWB_READY | EVT_UWB_INIT);
        xEventGroupSetBits(sysEvents, EVT_UWB_STOP);

        delete myDevice; myDevice = nullptr;
    }
};
static MyClientCallback clientCallback;

// =============================================================================
// connectToServer — owner mode: connect + HMAC challenge-response
// =============================================================================

static bool connectToServer() {
    if (!myDevice) return false;
    Serial.println("[bleTask] Connecting to Anchor...");

    if (pClient) { delete pClient; pClient = nullptr; }
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallback);

    bool connOk = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (pClient->connectTimeout(myDevice, 2000)) { connOk = true; break; }
        Serial.printf("[bleTask] Connection failed (%d/3)\n", attempt);
        if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (!connOk) { delete pClient; pClient = nullptr; return false; }

    pClient->setMTU(517);
    vTaskDelay(pdMS_TO_TICKS(100));

    BLERemoteService* pSvc = pClient->getService(SERVICE_UUID);
    if (!pSvc) { pClient->disconnect(); return false; }

    pChallengeChar        = pSvc->getCharacteristic(CHALLENGE_CHAR_UUID);
    pAuthChar             = pSvc->getCharacteristic(AUTH_CHAR_UUID);
    pRemoteCharacteristic = pSvc->getCharacteristic(CHARACTERISTIC_UUID);
    if (!pChallengeChar || !pAuthChar || !pRemoteCharacteristic) {
        Serial.println("[bleTask] Characteristic(s) missing");
        pClient->disconnect(); return false;
    }

    if (pAuthChar->canNotify())             pAuthChar->registerForNotify(notifyCallback);
    if (pRemoteCharacteristic->canNotify()) pRemoteCharacteristic->registerForNotify(notifyCallback);

    std::string challenge;
    for (int i = 0; i < 60; i++) {
        String raw = pChallengeChar->readValue();
        if (raw.length() == 16) { challenge = std::string(raw.c_str(), 16); break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (challenge.length() != 16) {
        Serial.printf("[bleTask] Challenge not ready (%d bytes)\n", challenge.length());
        pClient->disconnect(); return false;
    }

    uint8_t response[32];
    if (!computeHMAC(pairingKey, 16, (const uint8_t*)challenge.data(), 16, response)) {
        pClient->disconnect(); return false;
    }
    pAuthChar->writeValue(response, 32);
    Serial.println("[bleTask] HMAC response sent");

    xEventGroupWaitBits(sysEvents, EVT_AUTHED, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
    if (!authenticated) {
        if (!pAuthChar || !pClient || !pClient->isConnected()) return false;
        String authRaw = pAuthChar->readValue();
        if (authRaw == "AUTH_OK") {
            authenticated = true;
            xEventGroupSetBits(sysEvents, EVT_AUTHED);
            Serial.println("[bleTask] Auth OK (poll fallback)");
        } else {
            Serial.println("[bleTask] Auth FAIL");
            pClient->disconnect(); return false;
        }
    }
    return true;
}

// =============================================================================
// armUWB — gửi TAG_UWB_READY và retry cho đến khi Anchor xác nhận
// =============================================================================

static bool armUWB(const char* label) {
    anchorUwbReady = false;
    xEventGroupClearBits(sysEvents, EVT_ANCHOR_UWB_READY | EVT_UWB_INIT);
    while (connected) {
        if (pRemoteCharacteristic)
            pRemoteCharacteristic->writeValue("TAG_UWB_READY", 13U);
        Serial.printf("[bleTask] %s: Sent TAG_UWB_READY\n", label);
        EventBits_t bits = xEventGroupWaitBits(sysEvents, EVT_ANCHOR_UWB_READY,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(UWB_REQUEST_RETRY_MS));
        if (bits & EVT_ANCHOR_UWB_READY) {
            xEventGroupSetBits(sysEvents, EVT_UWB_INIT);
            Serial.printf("[bleTask] %s: UWB armed\n", label);
            return true;
        }
    }
    return false;
}

// =============================================================================
// connectAsFriend — friend mode: connect BLE → write bundle → wait for status
// Không cần HMAC auth. Anchor verify ECDSA bên trong bundle.
// =============================================================================

static bool connectAsFriend() {
    if (!myDevice || s_bundleWireLen == 0) {
        Serial.println("[bleTask] connectAsFriend: no device or no bundle");
        return false;
    }
    Serial.printf("[bleTask] Connecting as friend (bundle %u bytes)...\n", s_bundleWireLen);

    if (pClient) { delete pClient; pClient = nullptr; }
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallback);

    bool connOk = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (pClient->connectTimeout(myDevice, 2000)) { connOk = true; break; }
        Serial.printf("[bleTask] Friend connect failed (%d/3)\n", attempt);
        if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (!connOk) { delete pClient; pClient = nullptr; return false; }

    pClient->setMTU(517);
    vTaskDelay(pdMS_TO_TICKS(100));

    BLERemoteService* pFriendSvc = pClient->getService(FRIEND_SERVICE_UUID);
    if (!pFriendSvc) {
        Serial.println("[bleTask] Friend service not found on Anchor");
        pClient->disconnect(); return false;
    }

    pFriendBundleChar = pFriendSvc->getCharacteristic(FRIEND_BUNDLE_CHAR_UUID);
    pFriendStatusChar = pFriendSvc->getCharacteristic(FRIEND_STATUS_CHAR_UUID);
    if (!pFriendBundleChar || !pFriendStatusChar) {
        Serial.println("[bleTask] Friend characteristics missing");
        pClient->disconnect(); return false;
    }

    // Đăng ký notify trước khi gửi bundle
    s_friendAccepted   = false;
    s_friendStatusRcvd = false;
    if (pFriendStatusChar->canNotify())
        pFriendStatusChar->registerForNotify(notifyCallback);
    vTaskDelay(pdMS_TO_TICKS(200));  // chờ CCCD write hoàn thành

    // Ghi binary wire-format bundle lên Anchor
    pFriendBundleChar->writeValue(s_bundleWire, s_bundleWireLen, true);
    Serial.printf("[bleTask] Friend bundle written (%u bytes)\n", s_bundleWireLen);

    // Chờ notification status (tối đa 10s — Anchor có thể cần validate online)
    for (int i = 0; i < 100 && !s_friendStatusRcvd; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!s_friendStatusRcvd) {
        Serial.println("[bleTask] Friend status timeout");
        pClient->disconnect(); return false;
    }

    return s_friendAccepted;
}

// =============================================================================
// TASK: bleTask — Core 0, Priority 3
// =============================================================================

static void bleTask(void* param) {
    Serial.println("[bleTask] started on core " + String(xPortGetCoreID()));

    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100U);
    pBLEScan->setWindow(80U);

    for (;;) {
        // ── SCAN ──────────────────────────────────────────────────────────────
        Serial.printf("[bleTask] Scanning... (mode: %s)\n",
                      isFriendMode ? "FRIEND" : "OWNER");
        xEventGroupClearBits(sysEvents, EVT_DEVICE_FOUND);
        do {
            pBLEScan->clearResults();
            pBLEScan->start(10, false);
        } while (!(xEventGroupGetBits(sysEvents) & EVT_DEVICE_FOUND));

        if (isFriendMode) {
            // ── FRIEND MODE ────────────────────────────────────────────────────
            bool ok = connectAsFriend();
            Serial.printf("[bleTask] Friend access %s\n",
                          ok ? "GRANTED — Anchor unlocked" : "DENIED or connection failed");
            if (pClient && pClient->isConnected()) pClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(3000));  // debounce trước khi scan lại

        } else {
            // ── OWNER MODE ─────────────────────────────────────────────────────
            if (!connectToServer()) {
                Serial.println("[bleTask] Connect failed — retrying in 500ms");
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            armUWB("arm");

            while (connected) {
                BleWriteMsg wm;
                while (xQueueReceive(bleWriteQueue, &wm, 0) == pdTRUE) {
                    if (pRemoteCharacteristic && connected)
                        pRemoteCharacteristic->writeValue((uint8_t*)wm.data, wm.len);
                }

                if (pClient) currentRssi = pClient->getRssi();

                if (uwbStoppedFar) {
                    if (pClient) {
                        int rssi = pClient->getRssi();
                        Serial.printf("[bleTask] UWB stopped — RSSI=%d dBm (threshold=%d)\n",
                                      rssi, RSSI_THRESHOLD_DBM);
                        if (rssi > RSSI_THRESHOLD_DBM && rssi > -115 && rssi != 0) {
                            Serial.printf("[bleTask] RSSI=%d — re-arming\n", rssi);
                            uwbStoppedFar = false;
                            armUWB("re-arm");
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(RSSI_CHECK_INTERVAL_MS));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }

            xEventGroupClearBits(sysEvents, EVT_DEVICE_FOUND | EVT_ANCHOR_UWB_READY | EVT_UWB_INIT);
            Serial.println("[bleTask] Disconnected — scanning again in 1s");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// =============================================================================
// TASK: uwbTask — Core 1, Priority 4
// Không chạy trong friend mode
// =============================================================================

static void uwbTask(void* param) {
    Serial.println("[uwbTask] started on core " + String(xPortGetCoreID()));

    for (;;) {
        xEventGroupWaitBits(sysEvents, EVT_UWB_INIT, pdFALSE, pdFALSE, portMAX_DELAY);

        if (isFriendMode) {
            xEventGroupClearBits(sysEvents, EVT_UWB_INIT);
            continue;
        }

        if (!initUWB()) {
            Serial.println("[uwbTask] Init failed");
            xEventGroupClearBits(sysEvents, EVT_UWB_INIT);
            continue;
        }

        while (!(xEventGroupGetBits(sysEvents) & EVT_UWB_STOP)) {
            if (uwbInitiatorLoop()) {
                uwbStoppedFar = true;
                xEventGroupClearBits(sysEvents, EVT_UWB_INIT | EVT_ANCHOR_UWB_READY);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        deinitUWB();
        xEventGroupClearBits(sysEvents, EVT_UWB_INIT | EVT_UWB_STOP);
    }
}

// =============================================================================
// TASK: serialTask — Core 1, Priority 1
// Nhận lệnh từ Android qua USB Serial:
//   SET_SERVER_PUBKEY {base64_DER} → lưu NVS → "SERVER_PUBKEY_OK"
//   SET_TIME {epoch}               → acknowledged → "TIME_OK"
//   SET_BUNDLE {base64_JSON}       → verify + save → "BUNDLE_OK:{id}" / "BUNDLE_ERR:..."
// Stack 16KB: mbedtls pk_verify cần nhiều stack
// =============================================================================

static void serialTask(void* param) {
    Serial.println("[serialTask] started on core " + String(xPortGetCoreID()));

    static char lineBuf[1024];
    int pos = 0;

    for (;;) {
        if (!Serial.available()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        char c = (char)Serial.read();
        if (c == '\r') continue;

        if (c == '\n' || pos >= (int)sizeof(lineBuf) - 1) {
            lineBuf[pos] = '\0';
            pos = 0;
            if (strlen(lineBuf) == 0) continue;

            // ── SET_SERVER_PUBKEY ─────────────────────────────────────────────
            if (strncmp(lineBuf, "SET_SERVER_PUBKEY ", 18) == 0) {
                const char* b64 = lineBuf + 18;
                uint8_t derBuf[128];
                size_t  derLen = 0;
                if (mbedtls_base64_decode(derBuf, sizeof(derBuf), &derLen,
                                          (const unsigned char*)b64, strlen(b64)) == 0
                    && derLen > 0) {
                    saveServerPubkeyToNvs(derBuf, derLen);
                    Serial.println("SERVER_PUBKEY_OK");
                } else {
                    Serial.println("SERVER_PUBKEY_ERR:decode_failed");
                }

            // ── SET_TIME ──────────────────────────────────────────────────────
            } else if (strncmp(lineBuf, "SET_TIME ", 9) == 0) {
                // Time check thực sự nằm ở Anchor — chỉ acknowledge
                Serial.println("TIME_OK");

            // ── SET_BUNDLE ────────────────────────────────────────────────────
            } else if (strncmp(lineBuf, "SET_BUNDLE ", 11) == 0) {
                const char* b64 = lineBuf + 11;
                if (processSetBundle(b64)) {
                    char fidHex[17] = {};
                    for (int i = 0; i < 8; i++)
                        snprintf(fidHex + 2 * i, 3, "%02x", s_bundleWire[1 + i]);
                    Serial.printf("BUNDLE_OK:%s\n", fidHex);
                } else {
                    Serial.println("BUNDLE_ERR:verify_failed");
                }
            }
            // Bỏ qua các lệnh không nhận ra
        } else {
            lineBuf[pos++] = c;
        }
    }
}

// =============================================================================
// setup
// =============================================================================

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("\nSmart Car Tag [FreeRTOS] (reset: %d)\n", reason);
    if (reason == ESP_RST_PANIC)
        Serial.println("WARNING: previous reset was a CRASH");
    if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT)
        Serial.println("WARNING: previous reset was a WATCHDOG");

    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    pinMode(PIN_SS,  OUTPUT); digitalWrite(PIN_SS,  HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));

    hexStringToBytes(PAIRING_KEY_HEX, pairingKey, 16);
    printHex("Pairing key: ", pairingKey, 16);

    // Load friend bundle từ NVS — set isFriendMode nếu đã có bundle
    loadFriendBundleFromNVS();

    sysEvents     = xEventGroupCreate();
    bleWriteQueue = xQueueCreate(8, sizeof(BleWriteMsg));
    if (!sysEvents || !bleWriteQueue) {
        Serial.println("FreeRTOS alloc failed — halting"); while(1);
    }

    BLEDevice::init("UserTag_01");
    BLEDevice::setPower(ESP_PWR_LVL_P9);

    // Core 0: bleTask(P3)
    // Core 1: uwbTask(P4) — timing-critical
    //         serialTask(P1) — lowest, USB host commands
    xTaskCreatePinnedToCore(bleTask,    "BLE_Task",    10240, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(uwbTask,    "UWB_Task",     8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(serialTask, "Serial_Task", 16384, NULL, 1, NULL, 1);

    Serial.printf("Tasks created — mode: %s\n", isFriendMode ? "FRIEND" : "OWNER");
}

void loop() {
    vTaskDelete(NULL);
}
