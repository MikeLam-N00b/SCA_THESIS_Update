
#include <BLEDevice.h>
#include <BLEScan.h>
#include <SPI.h>
#include "dw3000.h"
#include "tag_config.h"
#include <mbedtls/md.h>

// =============================================================================
// FreeRTOS IPC primitives
// =============================================================================

static EventGroupHandle_t sysEvents;
// Bits của sysEvents:
#define EVT_CONNECTED        (1 << 0)  // BLE kết nối Anchor thành công
#define EVT_AUTHED           (1 << 1)  // Auth HMAC-SHA256 OK
#define EVT_ANCHOR_UWB_READY (1 << 2)  // Nhận "UWB_ACTIVE" notification từ Anchor
#define EVT_UWB_INIT         (1 << 3)  // Signal cho uwbTask: bắt đầu DW3000
#define EVT_UWB_STOP         (1 << 4)  // Signal cho uwbTask: dừng DW3000
#define EVT_DEVICE_FOUND     (1 << 5)  // BLE scan tìm thấy Anchor

// Queue: uwbTask gửi BLE write requests → bleTask (Core 0) thực hiện
// Tránh cross-core BLE writeValue từ uwbTask (Core 1) gây disconnect
struct BleWriteMsg { char data[32]; uint8_t len; };
static QueueHandle_t bleWriteQueue;

// =============================================================================
// State
// =============================================================================

static volatile bool connected      = false;
static volatile bool authenticated  = false;
static volatile bool uwbInitialized = false;
static volatile bool tagInUnlockZone = false;
static volatile bool uwbStoppedFar  = false;
static          bool anchorUwbReady = false;  // set bởi notify callback
static volatile int  currentRssi    = 0;      // cập nhật bởi bleTask, đọc bởi uwbTask

// =============================================================================
// BLE handles
// =============================================================================

static BLEAdvertisedDevice*     myDevice              = nullptr;
static BLEClient*               pClient               = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static BLERemoteCharacteristic* pChallengeChar        = nullptr;
static BLERemoteCharacteristic* pAuthChar             = nullptr;

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

// STS key/IV — derived từ pairingKey, phải khớp với Anchor
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
// double trên ESP32-S3 chạy bằng software emulation (~10x chậm hơn float)
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
// UWB init / deinit
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
// Logic giống BLE_UWB_Tag, dùng vTaskDelay thay delay()
// =============================================================================

// Returns true nếu cần dừng UWB (vượt 20m)
static bool uwbInitiatorLoop() {
    // Reload STS IV counter trước mỗi TX để sync với Anchor
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

    // Kiểm tra STS quality — từ chối nếu STS không hợp lệ (Anchor dùng key khác = relay attack)
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

    // SS-TWR distance calculation (float: đủ precision cho ±8cm, dùng hardware FPU)
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

    // Vượt 20m — dừng UWB, báo Anchor, chuyển sang RSSI monitor
    if (filtDist > UWB_FAR_DISTANCE_M) {
        tagInUnlockZone = false;
        if (connected) {
            BleWriteMsg wm; wm.len = 8; memcpy(wm.data, "UWB_STOP", 8);
            xQueueSend(bleWriteQueue, &wm, 0);
        }
        Serial.printf("[uwbTask] avg=%.1f m — beyond 20m, stopping UWB\n", filtDist);
        return true; // caller sẽ deinit UWB
    }

    // State transitions: chỉ gửi BLE khi zone thay đổi (tránh spam)
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
// BLE scan + connection callbacks
// =============================================================================

// Notification callback: chạy trong BLE stack task (Core 0)
// So sánh bằng con trỏ characteristic thay vì String UUID — không heap allocate
static void notifyCallback(BLERemoteCharacteristic* pChar,
                           uint8_t* pData, size_t length, bool isNotify) {
    if (!pData || length == 0) return;

    if (pChar == pAuthChar) {
        // memcmp thay std::string — zero allocation
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
    }
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!advertisedDevice.haveServiceUUID() ||
            !advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID))) return;

        int rssi = advertisedDevice.getRSSI();
        Serial.printf("[BLE scan] Anchor: %s | RSSI: %d dBm\n",
                      advertisedDevice.toString().c_str(), rssi);

        // Chỉ kết nối khi đủ gần — tránh timeout 30s ở xa (RSSI quá yếu)
        if (rssi < RSSI_THRESHOLD_DBM) {
            Serial.printf("[BLE scan] RSSI %d dBm < threshold %d — too far, skipping\n",
                          rssi, RSSI_THRESHOLD_DBM);
            // Stop scan ngay để loop restart scan mới — tránh chờ hết 10s do BLE dedup
            BLEDevice::getScan()->stop();
            return;
        }

        delete myDevice;
        myDevice = new BLEAdvertisedDevice(advertisedDevice);
        BLEDevice::getScan()->stop();
        // Báo bleTask đã tìm thấy Anchor
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

        xEventGroupClearBits(sysEvents, EVT_CONNECTED | EVT_AUTHED | EVT_ANCHOR_UWB_READY | EVT_UWB_INIT);
        // Signal uwbTask dừng UWB — không còn cần pendingUwbDeinit defer nữa
        xEventGroupSetBits(sysEvents, EVT_UWB_STOP);

        delete myDevice; myDevice = nullptr;
    }
};
static MyClientCallback clientCallback;

// =============================================================================
// BLE connection + challenge-response auth
// Chạy trực tiếp trong bleTask (không phải callback) — có thể dùng blocking delay
// =============================================================================

static bool connectToServer() {
    if (!myDevice) return false;
    Serial.println("[bleTask] Connecting to Anchor...");

    if (pClient) { delete pClient; pClient = nullptr; }
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallback);

    bool connOk = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (pClient->connectTimeout(myDevice, 500)) { connOk = true; break; }
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
        Serial.println("[bleTask] Characteristic(s) missing"); pClient->disconnect(); return false;
    }

    if (pAuthChar->canNotify())             pAuthChar->registerForNotify(notifyCallback);
    if (pRemoteCharacteristic->canNotify()) pRemoteCharacteristic->registerForNotify(notifyCallback);

    // Poll challenge characteristic (up to 3s)
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

    // Đợi AUTH_OK notification (tối đa 2s), fallback poll
    xEventGroupWaitBits(sysEvents, EVT_AUTHED, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
    if (!authenticated) {
        if (!pAuthChar || !pClient || !pClient->isConnected()) return false;
        String authRaw = pAuthChar->readValue();
        if (authRaw == "AUTH_OK") {
            authenticated = true;
            xEventGroupSetBits(sysEvents, EVT_AUTHED);
            Serial.println("[bleTask] Auth OK (poll fallback)");
        } else {
            Serial.println("[bleTask] Auth FAIL"); pClient->disconnect(); return false;
        }
    }
    return true;
}

// =============================================================================
// armUWB — helper dùng chung cho lần arm đầu và re-arm sau RSSI recovery
// Gửi TAG_UWB_READY, retry mỗi UWB_REQUEST_RETRY_MS cho đến khi Anchor xác nhận.
// Trả về true nếu arm thành công (EVT_UWB_INIT đã được set).
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
    return false;  // disconnected while waiting
}

// =============================================================================
// TASK: bleTask — Core 0, Priority 3
//
// Toàn bộ flow BLE chạy trong task này:
//   scan → đợi device found → connect → auth → arm UWB → monitor
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
        Serial.println("[bleTask] Scanning for Anchor...");
        xEventGroupClearBits(sysEvents, EVT_DEVICE_FOUND);
        // start(10, false) là blocking — trả về khi hết 10s hoặc stop() được gọi sớm
        // Sau khi trả về, check bit ngay — không cần wait thêm
        do {
            pBLEScan->clearResults();
            pBLEScan->start(10, false);
        } while (!(xEventGroupGetBits(sysEvents) & EVT_DEVICE_FOUND));

        // ── CONNECT + AUTH ─────────────────────────────────────────────────────
        if (!connectToServer()) {
            Serial.println("[bleTask] Connect failed — retrying in 500ms");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // ── ARM UWB ────────────────────────────────────────────────────────────
        armUWB("arm");

        // ── MONITOR LOOP ───────────────────────────────────────────────────────
        while (connected) {
            // Xử lý BLE write requests từ uwbTask — Core 0 an toàn để gọi BLE
            BleWriteMsg wm;
            while (xQueueReceive(bleWriteQueue, &wm, 0) == pdTRUE) {
                if (pRemoteCharacteristic && connected)
                    pRemoteCharacteristic->writeValue((uint8_t*)wm.data, wm.len);
            }

            // Cập nhật RSSI để uwbTask log
            if (pClient) currentRssi = pClient->getRssi();

            if (uwbStoppedFar) {
                // UWB dừng ở > 20m — dùng RSSI detect khi Tag quay lại
                if (pClient) {
                    int rssi = pClient->getRssi();
                    Serial.printf("[bleTask] UWB stopped — BLE connected, RSSI=%d dBm (threshold=%d)\n",
                                  rssi, RSSI_THRESHOLD_DBM);
                    if (rssi > RSSI_THRESHOLD_DBM && rssi > -115 && rssi != 0) {
                        Serial.printf("[bleTask] RSSI=%d dBm — within range, re-arming\n", rssi);
                        uwbStoppedFar = false;
                        armUWB("re-arm");
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(RSSI_CHECK_INTERVAL_MS));
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        // ── DISCONNECT cleanup ─────────────────────────────────────────────────
        xEventGroupClearBits(sysEvents, EVT_DEVICE_FOUND | EVT_ANCHOR_UWB_READY | EVT_UWB_INIT);
        Serial.println("[bleTask] Disconnected — scanning again in 1s");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// =============================================================================
// TASK: uwbTask — Core 1, Priority 4
//
// Đợi EVT_UWB_INIT, init DW3000, ranging loop liên tục.
// Dừng khi nhận EVT_UWB_STOP (BLE disconnect / vượt 20m).
//
// Trước: chạy lẫn trong loop() — BLE callbacks có thể preempt
// Sau:   pin cứng Core 1, priority 4 → không bị BLE task preempt
// =============================================================================

static void uwbTask(void* param) {
    Serial.println("[uwbTask] started on core " + String(xPortGetCoreID()));

    for (;;) {
        // Đợi signal init từ bleTask
        xEventGroupWaitBits(sysEvents, EVT_UWB_INIT, pdFALSE, pdFALSE, portMAX_DELAY);

        if (!initUWB()) {
            Serial.println("[uwbTask] Init failed — waiting for next signal");
            xEventGroupClearBits(sysEvents, EVT_UWB_INIT);
            continue;
        }

        // Ranging loop — chạy cho đến khi có EVT_UWB_STOP
        while (!(xEventGroupGetBits(sysEvents) & EVT_UWB_STOP)) {
            bool stopRequested = uwbInitiatorLoop();
            if (stopRequested) {
                // uwbInitiatorLoop trả true khi tag > 20m
                uwbStoppedFar = true;
                xEventGroupClearBits(sysEvents, EVT_UWB_INIT | EVT_ANCHOR_UWB_READY);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // Deinit DW3000
        deinitUWB();
        xEventGroupClearBits(sysEvents, EVT_UWB_INIT | EVT_UWB_STOP);
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
    if (reason == ESP_RST_PANIC) Serial.println("WARNING: previous reset was a CRASH");
    if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT)
        Serial.println("WARNING: previous reset was a WATCHDOG");

    // Hold DW3000 in reset during BLE init
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    pinMode(PIN_SS,  OUTPUT); digitalWrite(PIN_SS,  HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));

    hexStringToBytes(PAIRING_KEY_HEX, pairingKey, 16);
    printHex("Pairing key: ", pairingKey, 16);

    // Khởi tạo FreeRTOS primitives
    sysEvents     = xEventGroupCreate();
    bleWriteQueue = xQueueCreate(8, sizeof(BleWriteMsg));
    if (!sysEvents || !bleWriteQueue) {
        Serial.println("FreeRTOS alloc failed — halting"); while(1);
    }

    BLEDevice::init("UserTag_01");
    BLEDevice::setPower(ESP_PWR_LVL_P9);

    // Tạo tasks và pin vào đúng core
    xTaskCreatePinnedToCore(bleTask, "BLE_Task", 10240, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(uwbTask, "UWB_Task", 8192,  NULL, 4, NULL, 1);

    Serial.println("Tasks created — FreeRTOS scheduler running");
    Serial.println("Core 0: bleTask(P3) | Core 1: uwbTask(P4)");
}

// loop() không còn cần thiết
void loop() {
    vTaskDelete(NULL);
}
