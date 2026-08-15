/**
 * BLE_UWB_Tag.ino — Smart Car Tag (phía người dùng)
 *
 * Flow đơn giản:
 *   1. BLE scan + kết nối Anchor
 *   2. Challenge-Response auth (HMAC-SHA256)
 *   3. Auth OK → khởi tạo UWB ngay
 *   4. UWB ranging: ≤ 3m → mở khóa, > 3.5m → khóa
 *   5. UWB > 10m → tắt UWB + gửi UWB_STOP (BLE vẫn giữ)
 *   6. Quay lại gần (RSSI) → bật lại UWB
 *   7. BLE ngắt → tắt UWB + quét lại
 */

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLE2902.h>
#include <SPI.h>
#include "dw3000.h"
#include <mbedtls/md.h>

// ── BLE UUIDs ────────────────────────────────────────────────────────────────
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "abcdef12-3456-7890-abcd-ef1234567890"
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"

// ── Pairing key (test) ────────────────────────────────────────────────────────
const char* CORRECT_KEY_HEX = "e9b8da4e60206bd04bd554c6a94e4e0e";
const char* WRONG_KEY_HEX   = "0000000000000000000000000000000";
bool useCorrectKey = true;

// ── Thresholds ────────────────────────────────────────────────────────────────
#define UWB_UNLOCK_DISTANCE_M    (3.0)   // ≤ 3m → mở khóa
#define UWB_LOCK_DISTANCE_M      (3.5)   // > 3.5m → khóa (khoảng chết 0.5m tránh nhấp nháy)
#define UWB_FAR_DISTANCE_M       (10.0)  // > 10m → tắt UWB
#define RSSI_THRESHOLD_DBM       (-80)   // ~10m BLE, dùng để restart UWB sau khi ra xa
#define RSSI_CONNECT_MIN_DBM     (-100)   // RSSI tối thiểu để thử kết nối (~12-13m)
#define RSSI_CHECK_INTERVAL_MS   (1000U) // Check RSSI mỗi 1s khi chờ restart UWB
#define UWB_REQUEST_RETRY_MS     (5000U) // Retry TAG_UWB_READY sau 5s nếu Anchor chưa phản hồi

// ── Chân phần cứng UWB ───────────────────────────────────────────────────────
#define PIN_RST (5)
#define PIN_IRQ (4)
#define PIN_SS  (10)

// ── UWB frame constants ───────────────────────────────────────────────────────
#define TX_ANT_DLY                  (16385U)
#define RX_ANT_DLY                  (16385U)
#define ALL_MSG_COMMON_LEN          (10U)
#define ALL_MSG_SN_IDX              (2U)
#define RESP_MSG_POLL_RX_TS_IDX     (10U)
#define RESP_MSG_RESP_TX_TS_IDX     (14U)
#define RESP_MSG_TS_LEN             (4U)
#define POLL_TX_TO_RESP_RX_DLY_UUS  (500U)
#define RESP_RX_TIMEOUT_UUS         (10000U)
#define MSG_BUFFER_SIZE             (20U)

// ── DW3000 RF config ─────────────────────────────────────────────────────────
static dwt_config_t config = {
    5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1,
    DWT_BR_6M8, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
    (129 + 8 - 8), DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0
};
extern dwt_txconfig_t txconfig_options;

// ── BLE state ────────────────────────────────────────────────────────────────
static BLEAdvertisedDevice*     myDevice               = nullptr;
static BLEClient*               pClient                = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristic  = nullptr;
static BLERemoteCharacteristic* pChallengeCharacteristic = nullptr;
static BLERemoteCharacteristic* pAuthCharacteristic    = nullptr;

static bool          doConnect      = false;
static bool          connected      = false;
static bool          doScan         = false;
static bool          isReconnecting = false;
static bool          authenticated  = false;
static unsigned long nextScanTime   = 0U;

// ── Auth ─────────────────────────────────────────────────────────────────────
static uint8_t pairingKey[16];

// ── Deferred processing (BLE callback Core 0 → loop Core 1) ──────────────────
static volatile bool pendingChallengeProcess = false;
static volatile bool pendingUwbDeinit       = false;
static uint8_t challengeBuffer[16];

// ── UWB state ────────────────────────────────────────────────────────────────
static bool     uwbInitialized  = false;
static bool     anchorUwbReady  = false;
static bool     uwbRequested    = false;
static unsigned long uwbRequestTime = 0U;
static bool     uwbStoppedFar   = false;      // UWB tắt vì >10m, cần RSSI để restart
static unsigned long lastRssiCheck = 0U;

static uint8_t  tx_poll_msg[]  = {0x41U,0x88U,0U,0xCAU,0xDEU,'W','A','V','E',0xE0U,0U,0U};
static uint8_t  rx_resp_msg[]  = {0x41U,0x88U,0U,0xCAU,0xDEU,'V','E','W','A',0xE1U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U};
static uint8_t  frame_seq_nb   = 0U;
static uint8_t  rx_buffer[MSG_BUFFER_SIZE];
static uint32_t status_reg     = 0U;
static double   tof            = 0.0;
static double   distance       = 0.0;
static bool     tagInUnlockZone = false;

// ── Moving average distance filter ───────────────────────────────────────────
#define UWB_DIST_FILTER_N       (5U)   // Trung bình 5 mẫu (~500ms ở 10Hz)
static double   distBuf[UWB_DIST_FILTER_N] = {};
static uint8_t  distBufIdx  = 0U;
static bool     distBufFull = false;

// ── Khai báo hàm ─────────────────────────────────────────────────────────────
uint64_t get_tx_timestamp_u64(void);
uint64_t get_rx_timestamp_u64(void);
void resp_msg_get_ts(const uint8_t *ts_field, uint32_t *ts);
bool initUWB(void);
void deinitUWB(void);
void uwbInitiatorLoop(void);
bool connectToServer(void);

// ── Crypto helpers ───────────────────────────────────────────────────────────

void hexStringToBytes(const char* hex, uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
  }
}

bool computeHMAC(const uint8_t* key, size_t keyLen,
                 const uint8_t* data, size_t dataLen,
                 uint8_t* output) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return (mbedtls_md_hmac(md, key, keyLen, data, dataLen, output) == 0);
}

void printHex(const char* label, const uint8_t* data, size_t length) {
  Serial.print(label);
  for (size_t i = 0; i < length; i++) {
    if (data[i] < 0x10) Serial.print("0");
    Serial.print(data[i], HEX);
  }
  Serial.println();
}

// ── BLE Callbacks ─────────────────────────────────────────────────────────────

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (!advertisedDevice.haveServiceUUID() ||
        !advertisedDevice.isAdvertisingService(BLEUUID(SERVICE_UUID))) return;

    int rssi = advertisedDevice.getRSSI();
    Serial.printf("Anchor found: %s | RSSI: %d dBm\n",
                  advertisedDevice.toString().c_str(), rssi);

    if (rssi < RSSI_CONNECT_MIN_DBM) {
      Serial.printf("RSSI %d dBm qua yeu (min %d), bo qua\n", rssi, RSSI_CONNECT_MIN_DBM);
      return;
    }

    delete myDevice;
    myDevice = new BLEAdvertisedDevice(advertisedDevice);
    doConnect = true;
    doScan = false;
    BLEDevice::getScan()->stop();
  }
};

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                           uint8_t* pData, size_t length, bool isNotify) {
  (void)isNotify;
  if (!pData || length == 0) return;

  String charUUID = pBLERemoteCharacteristic->getUUID().toString().c_str();

  if (charUUID == CHALLENGE_CHAR_UUID) {
    if (length != 16) return;
    memcpy(challengeBuffer, pData, 16);
    pendingChallengeProcess = true;
    Serial.println("Challenge received, processing in main loop...");

  } else if (charUUID == AUTH_CHAR_UUID) {
    std::string value((char*)pData, length);
    if (value == "AUTH_OK") {
      authenticated = true;
      Serial.println("Auth OK");
    } else if (value == "AUTH_FAIL") {
      authenticated = false;
      Serial.println("Auth FAIL");
    }

  } else if (charUUID == CHARACTERISTIC_UUID) {
    char buf[64];
    size_t len = (length < sizeof(buf) - 1) ? length : sizeof(buf) - 1;
    memcpy(buf, pData, len);
    buf[len] = '\0';
    String msg(buf);
    Serial.println("Anchor: " + msg);
    if (msg.indexOf("UWB_ACTIVE") >= 0) {
      anchorUwbReady = true;
      Serial.println("Anchor UWB ready");
    }
  }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    (void)pclient;
    connected = true;
    authenticated = false;
    isReconnecting = false;
    anchorUwbReady = false;
    uwbRequested = false;
    uwbStoppedFar = false;
    Serial.println("BLE: Connected to Anchor");
  }

  void onDisconnect(BLEClient* pclient) {
    (void)pclient;
    Serial.println("BLE: Disconnected from Anchor");
    pendingUwbDeinit = true;

    connected = false;
    authenticated = false;
    anchorUwbReady = false;
    uwbRequested = false;
    uwbStoppedFar = false;
    tagInUnlockZone = false;
    pRemoteCharacteristic = nullptr;
    pChallengeCharacteristic = nullptr;
    pAuthCharacteristic = nullptr;

    delete myDevice;
    myDevice = nullptr;

    isReconnecting = true;
    doScan = true;
    doConnect = false;
    nextScanTime = millis() + 1000;
  }
};

static MyClientCallback clientCallback;

// ── connectToServer ───────────────────────────────────────────────────────────

bool connectToServer(void) {
  Serial.println("Connecting to Anchor...");

  if (!myDevice) { Serial.println("No device to connect!"); return false; }

  if (pClient) {
    delete pClient;
    pClient = nullptr;
  }

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(&clientCallback);

  bool connOk = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    // connectTimeout(device, ms): timeout 5s per attempt (default portMAX_DELAY quá lâu khi tín hiệu yếu)
    if (pClient->connectTimeout(myDevice, 5000)) {
      connOk = true;
      break;
    }
    Serial.printf("Connection failed (%d/3)\n", attempt);
    if (attempt < 3) delay(300);
  }
  if (!connOk) {
    delete pClient;
    pClient = nullptr;
    return false;
  }

  pClient->setMTU(517);
  delay(100);

  BLERemoteService* pSvc = pClient->getService(SERVICE_UUID);
  if (!pSvc) {
    Serial.println("Service not found");
    pClient->disconnect(); return false;
  }

  pChallengeCharacteristic = pSvc->getCharacteristic(CHALLENGE_CHAR_UUID);
  pAuthCharacteristic       = pSvc->getCharacteristic(AUTH_CHAR_UUID);
  pRemoteCharacteristic     = pSvc->getCharacteristic(CHARACTERISTIC_UUID);

  if (!pChallengeCharacteristic || !pAuthCharacteristic || !pRemoteCharacteristic) {
    Serial.println("Characteristic(s) not found");
    pClient->disconnect(); return false;
  }

  if (pChallengeCharacteristic->canNotify())
    pChallengeCharacteristic->registerForNotify(notifyCallback);
  if (pAuthCharacteristic->canNotify())
    pAuthCharacteristic->registerForNotify(notifyCallback);
  if (pRemoteCharacteristic->canNotify())
    pRemoteCharacteristic->registerForNotify(notifyCallback);

  Serial.println("Subscribed. Waiting for challenge...");

  // Chờ notification tối đa 2s
  for (int i = 0; i < 40 && !authenticated; i++) delay(50);

  // Polling fallback nếu notification chưa đến
  if (!authenticated) {
    String raw = pChallengeCharacteristic->readValue();
    std::string challengeVal(raw.c_str(), raw.length());

    if (challengeVal.length() == 16) {
      uint8_t response[32];
      if (!computeHMAC(pairingKey, 16, (uint8_t*)challengeVal.data(), 16, response)) {
        Serial.println("HMAC failed"); pClient->disconnect(); return false;
      }
      pAuthCharacteristic->writeValue(response, 32);

      for (int i = 0; i < 20 && !authenticated; i++) delay(50);

      if (!authenticated) {
        String authRaw = pAuthCharacteristic->readValue();
        std::string authResult(authRaw.c_str(), authRaw.length());

        if (authResult == "AUTH_OK") {
          authenticated = true;
          Serial.println("Auth OK (polling)");
        } else if (authResult == "AUTH_FAIL") {
          Serial.println("Auth FAIL - disconnecting");
          pClient->disconnect(); return false;
        } else {
          Serial.println("No auth result yet");
        }
      }
    } else {
      Serial.printf("Challenge not ready (%d bytes)\n", challengeVal.length());
    }
  }

  connected = true;
  return true;
}

// ── UWB Init / Deinit ─────────────────────────────────────────────────────────

bool initUWB(void) {
  if (uwbInitialized) return true;
  Serial.println("UWB: initializing (Tag/Initiator)...");

  spiBegin(PIN_IRQ, PIN_RST);

  {
    extern uint8_t _ss;
    _ss = PIN_SS;
  }
  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);

  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(2);
  pinMode(PIN_RST, INPUT);
  delay(50);

  int retries = 500;
  while (!dwt_checkidlerc() && retries > 0) { delay(1); retries--; }
  if (retries == 0) {
    Serial.println("UWB: IDLE_RC failed");
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    return false;
  }

  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) {
    Serial.println("UWB: init failed");
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    return false;
  }

  dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

  if (dwt_configure(&config) != 0) {
    Serial.println("UWB: config failed");
    dwt_softreset(); delay(2);
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    return false;
  }

  dwt_configuretxrf(&txconfig_options);
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);
  dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
  dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

  uwbInitialized = true;
  Serial.println("UWB: ready");
  return true;
}

void deinitUWB(void) {
  if (!uwbInitialized) return;
  dwt_forcetrxoff();
  dwt_softreset();
  delay(2);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  uwbInitialized = false;
  tagInUnlockZone = false;
  distBufIdx  = 0U;
  distBufFull = false;
  Serial.println("UWB: stopped");
}

// ── UWB Initiator loop (TWR) ──────────────────────────────────────────────────

void uwbInitiatorLoop(void) {
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);

  tx_poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
  dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0U);
  dwt_writetxfctrl(sizeof(tx_poll_msg), 0U, 1);

  if (dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED) != DWT_SUCCESS) {
    dwt_forcetrxoff();
    frame_seq_nb++;
    return;
  }

  unsigned long uwb_wait_start = millis();
  while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
           (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR))) {
    if (millis() - uwb_wait_start > 50UL) {
      dwt_forcetrxoff();
      frame_seq_nb++;
      return;
    }
  }

  frame_seq_nb++;

  if (!(status_reg & SYS_STATUS_RXFCG_BIT_MASK)) {
    dwt_forcetrxoff();
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
    return;
  }

  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
  uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
  if (frame_len > sizeof(rx_buffer)) return;

  dwt_readrxdata(rx_buffer, frame_len, 0U);
  rx_buffer[ALL_MSG_SN_IDX] = 0U;
  if (memcmp(rx_buffer, rx_resp_msg, ALL_MSG_COMMON_LEN) != 0) return;

  // Tính khoảng cách TWR
  uint32_t poll_tx_ts = dwt_readtxtimestamplo32();
  uint32_t resp_rx_ts = dwt_readrxtimestamplo32();
  float clockOffsetRatio = (float)dwt_readclockoffset() / (float)(1UL << 26);

  uint32_t poll_rx_ts, resp_tx_ts;
  resp_msg_get_ts(&rx_buffer[RESP_MSG_POLL_RX_TS_IDX], &poll_rx_ts);
  resp_msg_get_ts(&rx_buffer[RESP_MSG_RESP_TX_TS_IDX], &resp_tx_ts);

  int32_t rtd_init = (int32_t)(resp_rx_ts - poll_tx_ts);
  int32_t rtd_resp = (int32_t)(resp_tx_ts - poll_rx_ts);
  tof = (((double)rtd_init - ((double)rtd_resp * (1.0 - (double)clockOffsetRatio))) / 2.0) * DWT_TIME_UNITS;
  distance = tof * SPEED_OF_LIGHT;

  if (distance < 0 || distance > 100) return;

  // Moving average filter: tích lũy mẫu rồi dùng trung bình để quyết định
  distBuf[distBufIdx] = distance;
  distBufIdx = (distBufIdx + 1U) % UWB_DIST_FILTER_N;
  if (distBufIdx == 0U) distBufFull = true;
  uint8_t nSamples = distBufFull ? UWB_DIST_FILTER_N : distBufIdx;
  double filteredDist = 0.0;
  for (uint8_t i = 0U; i < nSamples; i++) filteredDist += distBuf[i];
  filteredDist /= (double)nSamples;

  // > 10m: tắt UWB, gửi UWB_STOP cho Anchor
  if (filteredDist > UWB_FAR_DISTANCE_M) {
    tagInUnlockZone = false;
    if (connected && pRemoteCharacteristic)
      pRemoteCharacteristic->writeValue("UWB_STOP", 8U);
    Serial.printf("UWB: raw=%.1fm avg=%.1fm -> ngoai 10m, tat UWB\n", distance, filteredDist);
    deinitUWB();
    uwbStoppedFar = true;
    uwbRequested = false;
    anchorUwbReady = false;
    return;
  }

  // Lock/Unlock (chỉ gửi khi chuyển trạng thái, dùng khoảng cách đã lọc)
  bool shouldUnlock = (filteredDist <= UWB_UNLOCK_DISTANCE_M);
  bool shouldLock   = (filteredDist > UWB_LOCK_DISTANCE_M);

  if (shouldUnlock && !tagInUnlockZone) {
    tagInUnlockZone = true;
    if (connected && pRemoteCharacteristic) {
      char msg[32];
      snprintf(msg, sizeof(msg), "VERIFIED:%.1fm", filteredDist);
      pRemoteCharacteristic->writeValue(msg, strlen(msg));
    }
    Serial.printf("UWB: avg=%.1fm -> MO KHOA\n", filteredDist);

  } else if (shouldLock && tagInUnlockZone) {
    tagInUnlockZone = false;
    if (connected && pRemoteCharacteristic) {
      char msg[32];
      snprintf(msg, sizeof(msg), "WARNING:%.1fm", filteredDist);
      pRemoteCharacteristic->writeValue(msg, strlen(msg));
    }
    Serial.printf("UWB: avg=%.1fm -> KHOA\n", filteredDist);
  }

  // Log khoảng cách định kỳ (2s)
  static unsigned long lastDistLog = 0;
  if (millis() - lastDistLog > 2000) {
    lastDistLog = millis();
    Serial.printf("UWB: raw=%.1fm avg=%.1fm %s\n", distance, filteredDist, tagInUnlockZone ? "[MO]" : "[KHOA]");
  }
}

// ── setup ─────────────────────────────────────────────────────────────────────

void setup(void) {
  Serial.begin(115200);
  delay(1000);

  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("\nSmart Car Tag - BLE+UWB (reset reason: %d)\n", reason);
  if (reason == ESP_RST_PANIC)   Serial.println("WARNING: Last reset was PANIC!");
  if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT)
    Serial.println("WARNING: Last reset was WATCHDOG!");

  // Giữ DW3000 trong reset khi boot
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
  pinMode(PIN_SS,  OUTPUT); digitalWrite(PIN_SS,  HIGH);
  delay(100);

  const char* keyToUse = useCorrectKey ? CORRECT_KEY_HEX : WRONG_KEY_HEX;
  hexStringToBytes(keyToUse, pairingKey, 16);
  Serial.println("Key: " + String(useCorrectKey ? "CORRECT" : "WRONG"));
  printHex("Pairing key: ", pairingKey, 16);

  BLEDevice::init("UserTag_01");
  BLEDevice::setPower(ESP_PWR_LVL_P9);  // Max TX power (+9 dBm) for longer range
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100U);   // 100 * 0.625ms = 62.5ms
  pBLEScan->setWindow(80U);      // 80 * 0.625ms = 50ms (80% duty cycle → bắt tín hiệu yếu tốt hơn)

  Serial.println("Scanning for Anchor...");
  pBLEScan->start(5, false);
  doScan = true;
}

// ── loop ──────────────────────────────────────────────────────────────────────

void loop(void) {
  // Xử lý challenge auth trên Core 1
  if (pendingChallengeProcess && pAuthCharacteristic) {
    pendingChallengeProcess = false;
    printHex("Challenge: ", challengeBuffer, 16);
    uint8_t response[32];
    if (computeHMAC(pairingKey, 16, challengeBuffer, 16, response)) {
      printHex("Response: ", response, 32);
      pAuthCharacteristic->writeValue(response, 32);
      Serial.println("Response sent");
    } else {
      Serial.println("HMAC compute failed");
    }
  }

  if (doConnect) {
    if (!connectToServer()) {
      isReconnecting = true;
      doScan = true;
      nextScanTime = millis() + 500;
    }
    doConnect = false;
  }

  // Deferred UWB deinit (BLE callback Core 0 → loop Core 1)
  if (pendingUwbDeinit) {
    pendingUwbDeinit = false;
    deinitUWB();
  }

  if (doScan && !connected && millis() >= nextScanTime) {
    Serial.printf("%sScanning...\n", isReconnecting ? "[Reconnect] " : "");
    BLEDevice::getScan()->clearResults();
    BLEDevice::getScan()->start(3, false);
    nextScanTime = millis() + 4000;
  }

  // ── UWB activation: sau auth → khởi tạo UWB ngay ──────────────────────────
  if (connected && authenticated && !uwbInitialized) {
    if (uwbStoppedFar) {
       // UWB đã tắt vì >20m → dùng RSSI để detect khi quay lại gần
      if ((millis() - lastRssiCheck) > RSSI_CHECK_INTERVAL_MS) {
        lastRssiCheck = millis();
        int rssi = pClient->getRssi();
        if (rssi > RSSI_THRESHOLD_DBM && rssi > -115 && rssi != 0) {
          Serial.printf("RSSI: %ddBm -> trong 20m, restart UWB\n", rssi);
          uwbStoppedFar = false;
          // uwbRequested & anchorUwbReady đã reset khi UWB stop
        }
      }
    } else {
      // Gửi TAG_UWB_READY (retry sau 5s nếu Anchor chưa phản hồi)
      if (!uwbRequested || (!anchorUwbReady && (millis() - uwbRequestTime > UWB_REQUEST_RETRY_MS))) {
        if (pRemoteCharacteristic) {
          pRemoteCharacteristic->writeValue("TAG_UWB_READY", 13U);
          uwbRequested = true;
          uwbRequestTime = millis();
          Serial.println("Sent TAG_UWB_READY");
        }
      }

      // Init UWB khi Anchor sẵn sàng
      if (anchorUwbReady) {
        Serial.println("Anchor ready -> init UWB");
        if (!initUWB()) {
          anchorUwbReady = false;
          uwbRequested = false;
        }
      }
    }
  }

  // ── UWB ranging ────────────────────────────────────────────────────────────
  if (connected && authenticated && uwbInitialized && anchorUwbReady && !pendingChallengeProcess) {
    uwbInitiatorLoop();
    delay(100);
  }

  delay(10);
}
