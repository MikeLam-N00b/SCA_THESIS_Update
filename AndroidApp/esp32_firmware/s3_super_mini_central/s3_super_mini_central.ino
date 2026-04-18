/**
 * ESP32-S3 Super Mini — BLE Central + UWB Tag  (FreeRTOS)
 * ─────────────────────────────────────────────────────────
 * Nguồn gốc: FreeRTOS_Tag.ino (merged vào s3_super_mini_central)
 *
 * Nhận lệnh từ Android app qua USB Serial (115200 baud):
 *   "SET_KEY:<32 hex chars>\n"  → lưu pairing key, bắt đầu scan BLE
 *   "DISCONNECT\n"              → ngắt BLE
 *
 * Gửi trạng thái về app:
 *   "READY"                          → khởi động xong, chờ key
 *   "KEY_OK:<hex>"                   → key hợp lệ, bắt đầu scan
 *   "KEY_INVALID"                    → key sai format
 *   "SCANNING..."                    → đang quét BLE tìm Anchor
 *   "FOUND:<mac>"                    → tìm thấy Anchor
 *   "CONNECTED:<mac>"                → kết nối + auth thành công
 *   "KEY_FORWARDED_TO_ANCHOR:<hex>"  → key đã gửi tới Anchor
 *   "DISCONNECTED"                   → mất kết nối BLE
 *   "DISTANCE:<value>"               → khoảng cách UWB (m)
 *   "UNLOCK:<dist>m"                 → tag vào vùng mở khóa
 *   "LOCK:<dist>m"                   → tag ra khỏi vùng mở khóa
 *
 * Board   : ESP32-S3 Super Mini
 * LED     : GPIO 48
 * USB Mode: "Hardware CDC and JTAG"
 *
 * Thư viện cần cài thêm:
 *   - DW3000 Arduino library (cho UWB — bỏ qua nếu chưa có phần cứng)
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLEAdvertisedDevice.h>
#include <mbedtls/md.h>
#include "tag_config.h"

#include <SPI.h>
#include "dw3000.h"
extern dwt_txconfig_t txconfig_options;

#define LED_PIN 48

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
#define EVT_KEY_SET          (1 << 6)   // app đã gửi SET_KEY thành công
#define EVT_CHALLENGE_RCVD   (1 << 7)   // Anchor gửi challenge mới qua notify

// Buffer nhận challenge qua notify — chỉ viết trong notifyCallback (Core 0)
static uint8_t receivedChallenge[16];

// Queue: uwbTask gửi BLE write requests → bleTask (Core 0) thực hiện
struct BleWriteMsg { char data[64]; uint8_t len; };
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
// Khi Anchor trả AUTH_FAIL → bleTask dừng scan, chờ SET_KEY mới từ app
static volatile bool authFailed      = false;

// =============================================================================
// BLE handles
// =============================================================================

static BLEAdvertisedDevice*     myDevice              = nullptr;
static BLEClient*               pClient               = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static BLERemoteCharacteristic* pChallengeChar        = nullptr;
static BLERemoteCharacteristic* pAuthChar             = nullptr;

static uint8_t pairingKey[16];
static char    pairingKeyHex[33];  // hex string để báo về app

// =============================================================================
// UWB frame buffers (chỉ dùng khi UWB_ENABLED)
// =============================================================================

#if UWB_ENABLED
static dwt_config_t uwbConfig = {
    5, DWT_PLEN_1024, DWT_PAC32, 9, 9, 1,
    DWT_BR_850K, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
    1001, DWT_STS_MODE_1, DWT_STS_LEN_256, DWT_PDOA_M0
};
static dwt_sts_cp_key_t sts_key;
static dwt_sts_cp_iv_t  sts_iv;
static bool stsConfigured = false;

static uint8_t tx_poll_msg[] = {0x41U,0x88U,0U,0xCAU,0xDEU,'W','A','V','E',0xE0U,0U,0U};
static uint8_t rx_resp_msg[] = {0x41U,0x88U,0U,0xCAU,0xDEU,'V','E','W','A',0xE1U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U};
static uint8_t frame_seq_nb  = 0U;
static uint8_t rx_buffer[MSG_BUFFER_SIZE];
#endif

// =============================================================================
// Distance filter (moving average)
// =============================================================================

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
// UWB init / deinit (no-op khi UWB_ENABLED = 0)
// =============================================================================

#if UWB_ENABLED
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

    memcpy(&sts_key, pairingKey, sizeof(sts_key));
    sts_iv = { 0x00000001U, 0x00000000U, 0x00000000U, 0x00000000U };
    dwt_configurestskey(&sts_key);
    dwt_configurestsiv(&sts_iv);
    dwt_configurestsloadiv();
    stsConfigured  = true;
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

    uint32_t poll_tx_ts  = dwt_readtxtimestamplo32();
    uint32_t resp_rx_ts  = dwt_readrxtimestamplo32();
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
        Serial.printf("[uwbTask] avg=%.1f m — beyond far threshold, stopping UWB\n", filtDist);
        return true;
    }

    bool shouldUnlock = (filtDist <= UWB_UNLOCK_DISTANCE_M);
    bool shouldLock   = (filtDist >  UWB_LOCK_DISTANCE_M);

    if (shouldUnlock && !tagInUnlockZone) {
        tagInUnlockZone = true;
        BleWriteMsg wm;
        wm.len = (uint8_t)snprintf(wm.data, sizeof(wm.data), "VERIFIED:%.1fm", filtDist);
        if (connected) xQueueSend(bleWriteQueue, &wm, 0);
        Serial.printf("UNLOCK:%.1fm\n", filtDist);     // báo app
    } else if (shouldLock && tagInUnlockZone) {
        tagInUnlockZone = false;
        BleWriteMsg wm;
        wm.len = (uint8_t)snprintf(wm.data, sizeof(wm.data), "WARNING:%.1fm", filtDist);
        if (connected) xQueueSend(bleWriteQueue, &wm, 0);
        Serial.printf("LOCK:%.1fm\n", filtDist);       // báo app
    }

    static unsigned long lastDistLog = 0;
    if (millis() - lastDistLog > 500) {
        lastDistLog = millis();
        Serial.printf("DISTANCE:%.1f\n", filtDist);    // báo app mỗi 500ms
    }
    return false;
}
#endif // UWB_ENABLED

// =============================================================================
// BLE notification + scan callbacks
// =============================================================================

static void notifyCallback(BLERemoteCharacteristic* pChar,
                           uint8_t* pData, size_t length, bool isNotify) {
    if (!pData || length == 0) return;

    if (pChar == pChallengeChar) {
        // Challenge mới từ Anchor — lưu vào buffer và báo connectToServer()
        if (length == 16) {
            memcpy(receivedChallenge, pData, 16);
            xEventGroupSetBits(sysEvents, EVT_CHALLENGE_RCVD);
            Serial.println("[BLE notify] Challenge received (16 bytes)");
        }
    } else if (pChar == pAuthChar) {
        if (length == 7 && memcmp(pData, "AUTH_OK", 7) == 0) {
            authenticated = true;
            xEventGroupSetBits(sysEvents, EVT_AUTHED);
            Serial.println("[BLE notify] AUTH_OK");
            Serial.printf("KEY_FORWARDED_TO_ANCHOR:%s\n", pairingKeyHex);
        } else if (length == 9 && memcmp(pData, "AUTH_FAIL", 9) == 0) {
            authenticated = false;
            authFailed    = true;
            // Xóa EVT_KEY_SET → bleTask sẽ chờ SET_KEY mới thay vì retry vô hạn
            xEventGroupClearBits(sysEvents, EVT_KEY_SET);
            Serial.println("[BLE notify] AUTH_FAIL");
            Serial.println("AUTH_FAIL_BAD_KEY");  // báo app
        }
    } else if (pChar == pRemoteCharacteristic) {
        if (length >= 10 && memcmp(pData, "UWB_ACTIVE", 10) == 0) {
            anchorUwbReady = true;
            xEventGroupSetBits(sysEvents, EVT_ANCHOR_UWB_READY);
            Serial.println("[BLE notify] UWB_ACTIVE");
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
        connected             = false;
        authenticated         = false;
        anchorUwbReady        = false;
        tagInUnlockZone       = false;
        uwbStoppedFar         = false;
        pRemoteCharacteristic = nullptr;
        pChallengeChar        = nullptr;
        pAuthChar             = nullptr;
        xEventGroupClearBits(sysEvents, EVT_CONNECTED | EVT_AUTHED | EVT_ANCHOR_UWB_READY | EVT_UWB_INIT);
        xEventGroupSetBits(sysEvents, EVT_UWB_STOP);
        digitalWrite(LED_PIN, LOW);
        Serial.println("DISCONNECTED");
    }
};
static MyClientCallback clientCallback;

// =============================================================================
// connectToServer — BLE connect + HMAC challenge-response
// =============================================================================

static bool connectToServer() {
    if (!myDevice) return false;
    Serial.printf("FOUND:%s\n", myDevice->getAddress().toString().c_str());

    if (pClient) { delete pClient; pClient = nullptr; }
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallback);
    pClient->setMTU(517);

    bool connOk = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (pClient->connect(myDevice)) { connOk = true; break; }
        Serial.printf("[bleTask] Connect attempt %d/3 failed\n", attempt);
        if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (!connOk) { delete pClient; pClient = nullptr; return false; }

    vTaskDelay(pdMS_TO_TICKS(100));
    BLERemoteService* pSvc = pClient->getService(SERVICE_UUID);
    if (!pSvc) { pClient->disconnect(); return false; }

    pRemoteCharacteristic = pSvc->getCharacteristic(CHARACTERISTIC_UUID);
    pChallengeChar        = pSvc->getCharacteristic(CHALLENGE_CHAR_UUID);
    pAuthChar             = pSvc->getCharacteristic(AUTH_CHAR_UUID);

    if (!pRemoteCharacteristic) {
        Serial.println("[bleTask] Main characteristic not found");
        pClient->disconnect(); return false;
    }

    if (pRemoteCharacteristic->canNotify())
        pRemoteCharacteristic->registerForNotify(notifyCallback);

    // ── HMAC challenge-response (bỏ qua nếu Anchor không có auth chars) ────
    bool hasAuth = (pChallengeChar != nullptr && pAuthChar != nullptr);
    if (hasAuth) {
        if (pAuthChar->canNotify())      pAuthChar->registerForNotify(notifyCallback);
        // Đăng ký notify cho challenge: Anchor gọi notify() khi set challenge mới.
        // KHÔNG dùng readValue() vì sẽ đọc challenge CŨ từ session trước → AUTH_FAIL.
        if (pChallengeChar->canNotify()) pChallengeChar->registerForNotify(notifyCallback);

        // Chờ challenge qua notify (timeout 3s).
        // Notify có thể bị miss nếu Tag đăng ký xong sau khi Anchor đã gửi notify.
        // Fallback: đọc readValue() — an toàn vì Anchor đã generate challenge mới
        // trong session này (setValue + notify trong loop()), không phải giá trị session cũ.
        xEventGroupClearBits(sysEvents, EVT_CHALLENGE_RCVD);
        EventBits_t bits = xEventGroupWaitBits(sysEvents, EVT_CHALLENGE_RCVD,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
        if (!(bits & EVT_CHALLENGE_RCVD)) {
            Serial.println("[bleTask] Challenge notify missed — fallback to readValue()");
            std::string fallback = pChallengeChar->readValue();
            if (fallback.length() == 16) {
                memcpy(receivedChallenge, fallback.data(), 16);
                Serial.println("[bleTask] Challenge read via fallback OK");
            } else {
                Serial.printf("[bleTask] Challenge not available (%d bytes) — disconnect\n",
                              fallback.length());
                pClient->disconnect(); return false;
            }
        }

        uint8_t response[32];
        printHex("[AUTH] Key:       ", pairingKey,       16);
        printHex("[AUTH] Challenge:  ", receivedChallenge, 16);
        if (!computeHMAC(pairingKey, 16, receivedChallenge, 16, response)) {
            pClient->disconnect(); return false;
        }
        printHex("[AUTH] HMAC resp:  ", response, 32);
        pAuthChar->writeValue(response, 32, true);  // true = WRITE_WITH_RESPONSE, anchor dùng PROPERTY_WRITE
        Serial.println("[bleTask] HMAC response sent");

        xEventGroupWaitBits(sysEvents, EVT_AUTHED, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        if (!authenticated) {
            std::string authRaw = pAuthChar->readValue();
            if (authRaw == "AUTH_OK") {
                authenticated = true;
                xEventGroupSetBits(sysEvents, EVT_AUTHED);
            } else {
                // Notify AUTH_FAIL có thể không tới kịp trước khi anchor disconnect.
                // Đọc trực tiếp giá trị characteristic để xác nhận lý do thất bại.
                if (authRaw == "AUTH_FAIL") {
                    authFailed = true;
                    xEventGroupClearBits(sysEvents, EVT_KEY_SET);
                    Serial.println("AUTH_FAIL_BAD_KEY");
                }
                Serial.println("[bleTask] Auth FAIL");
                pClient->disconnect(); return false;
            }
        }
    } else {
        // Anchor không có HMAC — gửi key trực tiếp (backward compat)
        pRemoteCharacteristic->writeValue((uint8_t*)pairingKeyHex, 32, true);
        Serial.printf("KEY_FORWARDED_TO_ANCHOR:%s\n", pairingKeyHex);
        authenticated = true;
        xEventGroupSetBits(sysEvents, EVT_AUTHED);
    }

    Serial.printf("CONNECTED:%s\n", myDevice->getAddress().toString().c_str());
    digitalWrite(LED_PIN, HIGH);   // LED sáng khi đã kết nối + auth
    return true;
}

// =============================================================================
// armUWB — gửi TAG_UWB_READY, chờ Anchor xác nhận
// =============================================================================

static bool armUWB(const char* label) {
#if !UWB_ENABLED
    return true;  // bỏ qua khi không có phần cứng UWB
#endif
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
// TASK: usbSerialTask — Core 0, Priority 2
// Đọc lệnh từ Android app qua USB Serial
// =============================================================================

static void usbSerialTask(void* param) {
    String buf = "";
    for (;;) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n') {
                buf.trim();
                if (buf.length() == 0) { buf = ""; continue; }

                if (buf.startsWith("SET_KEY:")) {
                    String hex = buf.substring(8);
                    Serial.printf("DBG_KEY:[%s] len=%d\n", hex.c_str(), hex.length());
                    if (hex.length() == 32) {
                        hexStringToBytes(hex.c_str(), pairingKey, 16);
                        memcpy(pairingKeyHex, hex.c_str(), 32);
                        pairingKeyHex[32] = '\0';
                        authFailed = false;  // reset: key mới → cho phép scan lại
                        printHex("[USB] Key: ", pairingKey, 16);
                        Serial.printf("KEY_OK:%s\n", pairingKeyHex);
                        xEventGroupSetBits(sysEvents, EVT_KEY_SET);
                        // Nếu đang kết nối mà chưa forward key, gửi ngay
                        if (connected && pRemoteCharacteristic && !authenticated) {
                            pRemoteCharacteristic->writeValue(
                                (uint8_t*)pairingKeyHex, 32, true);
                            Serial.printf("KEY_FORWARDED_TO_ANCHOR:%s\n", pairingKeyHex);
                        }
                    } else {
                        Serial.printf("KEY_INVALID:len=%d\n", hex.length());
                    }

                } else if (buf == "DISCONNECT") {
                    if (pClient && connected) {
                        pClient->disconnect();
                        Serial.println("DISCONNECTED");
                    }
                }
                buf = "";
            } else {
                if (buf.length() < 64) buf += c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =============================================================================
// TASK: bleTask — Core 0, Priority 3
// =============================================================================

static void bleTask(void* param) {
    Serial.printf("[bleTask] started on core %d\n", xPortGetCoreID());

    // Chờ app gửi SET_KEY trước khi scan
    Serial.println("[bleTask] Waiting for SET_KEY from app...");
    xEventGroupWaitBits(sysEvents, EVT_KEY_SET, pdFALSE, pdFALSE, portMAX_DELAY);
    Serial.println("[bleTask] Key ready — starting BLE scan");

    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100U);
    pBLEScan->setWindow(80U);

    for (;;) {
        // ── SCAN ──────────────────────────────────────────────────────────────
        Serial.println("SCANNING...");
        xEventGroupClearBits(sysEvents, EVT_DEVICE_FOUND);
        do {
            pBLEScan->clearResults();
            pBLEScan->start(10, false);
        } while (!(xEventGroupGetBits(sysEvents) & EVT_DEVICE_FOUND));

        // ── CONNECT + AUTH ─────────────────────────────────────────────────────
        if (!connectToServer()) {
            if (authFailed) {
                // Key sai → không retry, chờ SET_KEY mới từ app
                authFailed = false;
                Serial.println("[bleTask] Bad key — waiting for new SET_KEY from app...");
                xEventGroupWaitBits(sysEvents, EVT_KEY_SET, pdFALSE, pdFALSE, portMAX_DELAY);
                Serial.println("[bleTask] New key received — restarting scan");
            } else {
                Serial.println("[bleTask] Connect failed — retrying in 500ms");
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        // ── ARM UWB ────────────────────────────────────────────────────────────
        armUWB("arm");

        // ── MONITOR LOOP ───────────────────────────────────────────────────────
        while (connected) {
            BleWriteMsg wm;
            while (xQueueReceive(bleWriteQueue, &wm, 0) == pdTRUE) {
                if (pRemoteCharacteristic && connected)
                    pRemoteCharacteristic->writeValue((uint8_t*)wm.data, wm.len);
            }
            if (pClient) currentRssi = pClient->getRssi();

            if (uwbStoppedFar) {
                int rssi = pClient ? pClient->getRssi() : -127;
                Serial.printf("[bleTask] UWB stopped — RSSI=%d dBm\n", rssi);
                if (rssi > RSSI_THRESHOLD_DBM && rssi > -115 && rssi != 0) {
                    Serial.printf("[bleTask] RSSI=%d — re-arming UWB\n", rssi);
                    uwbStoppedFar = false;
                    armUWB("re-arm");
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

// =============================================================================
// TASK: uwbTask — Core 1, Priority 4  (no-op khi UWB_ENABLED = 0)
// =============================================================================

static void uwbTask(void* param) {
    Serial.printf("[uwbTask] started on core %d\n", xPortGetCoreID());

#if !UWB_ENABLED
    Serial.println("[uwbTask] UWB disabled — task idle");
    vTaskDelete(NULL);
    return;
#else
    for (;;) {
        xEventGroupWaitBits(sysEvents, EVT_UWB_INIT, pdFALSE, pdFALSE, portMAX_DELAY);

        if (!initUWB()) {
            xEventGroupClearBits(sysEvents, EVT_UWB_INIT);
            continue;
        }

        while (!(xEventGroupGetBits(sysEvents) & EVT_UWB_STOP)) {
            bool stopReq = uwbInitiatorLoop();
            if (stopReq) {
                uwbStoppedFar = true;
                xEventGroupClearBits(sysEvents, EVT_UWB_INIT | EVT_ANCHOR_UWB_READY);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        deinitUWB();
        xEventGroupClearBits(sysEvents, EVT_UWB_INIT | EVT_UWB_STOP);
    }
#endif
}

// =============================================================================
// setup
// =============================================================================

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("\nSmart Car Tag (reset: %d)\n", reason);
    if (reason == ESP_RST_PANIC)
        Serial.println("WARNING: previous reset was a CRASH");

    // Hold DW3000 in reset (nếu có) trong lúc BLE init
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    pinMode(PIN_SS,  OUTPUT); digitalWrite(PIN_SS,  HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));

    // LED init SAU BLE để BLE không reconfigure GPIO 48
    BLEDevice::init("UserTag_01");
    BLEDevice::setPower(ESP_PWR_LVL_P9);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // FreeRTOS primitives
    sysEvents     = xEventGroupCreate();
    bleWriteQueue = xQueueCreate(8, sizeof(BleWriteMsg));
    if (!sysEvents || !bleWriteQueue) {
        Serial.println("FreeRTOS alloc failed — halting");
        while (1);
    }

    // Tạo tasks, pin vào đúng core
    xTaskCreatePinnedToCore(usbSerialTask, "USB_Task", 4096,  NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(bleTask,       "BLE_Task", 10240, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(uwbTask,       "UWB_Task", 8192,  NULL, 4, NULL, 1);

    Serial.println("READY");
    Serial.println("Core 0: usbSerialTask(P2) bleTask(P3) | Core 1: uwbTask(P4)");
}

void loop() {
    vTaskDelete(NULL);  // loop() không dùng trong FreeRTOS mode
}
