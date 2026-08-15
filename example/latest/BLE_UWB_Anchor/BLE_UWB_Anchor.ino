/**
 * BLE_UWB_Anchor.ino — Smart Car Anchor (phía xe)
 *
 * Flow đơn giản:
 *   1. BLE advertise → Tag kết nối → Challenge-Response auth
 *   2. Auth OK → Tag gửi TAG_UWB_READY → Anchor init UWB → gửi UWB_ACTIVE
 *   3. UWB ranging: ≤ 3m → mở khóa, > 3m → khóa
 *   4. Tag ra > 20m → gửi UWB_STOP → Anchor tắt UWB + khóa xe
 *   5. BLE ngắt → khóa xe + tắt UWB → quảng bá lại
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <SPI.h>
#include "dw3000.h"
#include <mcp2515.h>
#include "can_commands.h"
#include <mbedtls/pk.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>


// ── Cấu hình WiFi & Server ───────────────────────────────────────────────────
const char* ssid          = "nubia Neo 2";
const char* password      = "29092004";
const char* vehicleId     = "1HGBH41JXMN109186";
static String serverBaseUrl = "http://10.0.7.178:8000"; // Tự động tìm qua mDNS

// ── BLE UUIDs ────────────────────────────────────────────────────────────────
#define DEVICE_NAME         "SmartCar_Vehicle"
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define CHARACTERISTIC_UUID "abcdef12-3456-7890-abcd-ef1234567890"
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"

// ── Timing ───────────────────────────────────────────────────────────────────
#define CHALLENGE_SEND_DELAY_MS  (200U)  // Chờ Tag subscribe notification

// ── Chân phần cứng ───────────────────────────────────────────────────────────
#define PIN_RST  (5)
#define PIN_IRQ  (4)
#define PIN_SS   (10)
#define CAN_CS   (9)
#define MCP_CLOCK MCP_8MHZ

// ── UWB: Antenna delay (calibrated) + Frame config ───────────────────────────
#define TX_ANT_DLY              (16385U)
#define RX_ANT_DLY              (16385U)
#define ALL_MSG_COMMON_LEN      (10U)
#define ALL_MSG_SN_IDX          (2U)
#define RESP_MSG_POLL_RX_TS_IDX (10U)
#define RESP_MSG_RESP_TX_TS_IDX (14U)
#define POLL_RX_TO_RESP_TX_DLY_UUS (800U)
#define MSG_BUFFER_SIZE         (20U)

// ── DW3000 RF config ─────────────────────────────────────────────────────────
static dwt_config_t uwbConfig = {
    5, DWT_PLEN_128, DWT_PAC8, 9, 9, 1,
    DWT_BR_6M8, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
    (129 + 8 - 8), DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0
};
extern dwt_txconfig_t txconfig_options;

// ── Trạng thái hệ thống ──────────────────────────────────────────────────────
// MUST be volatile: written on Core 0 (BLE callbacks), read on Core 1 (loop)
static volatile bool deviceConnected   = false;
static volatile bool prevConnected     = false;
static volatile bool authenticated     = false;
static volatile bool challengePending  = false;
static volatile bool uwbInitialized    = false;
static volatile bool carUnlocked       = false;
static volatile bool hasKey            = false;
static volatile bool bleStarted        = false;
static volatile unsigned long connectionTime = 0U;
// ── Deferred actions (BLE callbacks on Core 0 → loop on Core 1) ─────────
// MUST be volatile: written on Core 0 (BLE callbacks), read on Core 1 (loop)
static volatile bool pendingLock            = false;
static volatile bool pendingUnlock          = false;
static volatile bool pendingUwbInit         = false;
static volatile bool pendingUwbDeinit       = false;
static volatile bool pendingUwbActiveNotify = false;
static volatile bool pendingAuthVerify      = false;
// ── BLE Characteristics ──────────────────────────────────────────────────────
static BLECharacteristic *pCharacteristic          = nullptr;
static BLECharacteristic *pChallengeCharacteristic = nullptr;
static BLECharacteristic *pAuthCharacteristic      = nullptr;

// ── Auth buffers ─────────────────────────────────────────────────────────────
static uint8_t currentChallenge[16];
static uint8_t pairingKey[16];
static uint8_t responseBuffer[32];
static size_t  responseBufferLen = 0;

// ── NVS & Crypto ─────────────────────────────────────────────────────────────
static Preferences             preferences;
static String                  bleKeyHex = "";
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;

// ── CAN Bus ──────────────────────────────────────────────────────────────────
static MCP2515*    pMcp2515   = nullptr; // Init sau WiFi tránh SPI conflict
static CANCommands* pCanControl = nullptr;

// ── UWB Frame buffers ────────────────────────────────────────────────────────
static uint8_t rx_poll_msg[] = {0x41U, 0x88U, 0U, 0xCAU, 0xDEU, 'W','A','V','E', 0xE0U, 0U, 0U};
static uint8_t tx_resp_msg[] = {0x41U, 0x88U, 0U, 0xCAU, 0xDEU, 'V','E','W','A', 0xE1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
static uint8_t  rx_buffer[MSG_BUFFER_SIZE];
static uint8_t  frame_seq_nb       = 0U;
static uint32_t status_reg         = 0U;
static unsigned long lastPollReceivedTime = 0U;

// ── Khai báo hàm ─────────────────────────────────────────────────────────────
uint64_t get_rx_timestamp_u64(void);
void     resp_msg_set_ts(uint8_t *ts_field, uint64_t ts);
bool     initUWB(void);
void     deinitUWB(void);
void     uwbResponderLoop(void);

// ── Crypto helpers ───────────────────────────────────────────────────────────

void hexStringToBytes(const char* hex, uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
  }
}

// Dùng mbedtls CSPRNG thay vì random() để đảm bảo bảo mật
void generateChallenge(uint8_t* challenge, size_t length) {
  mbedtls_ctr_drbg_random(&ctr_drbg, challenge, length);
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

// HKDF-SHA256: derive key material từ shared secret
int hkdf_sha256(const unsigned char* salt, size_t salt_len,
                const unsigned char* ikm,  size_t ikm_len,
                const unsigned char* info, size_t info_len,
                unsigned char* okm, size_t okm_len) {
  unsigned char prk[32];
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  // Extract
  if (salt == NULL || salt_len == 0) {
    unsigned char zero[32] = {0};
    mbedtls_md_hmac(md, zero, 32, ikm, ikm_len, prk);
  } else {
    mbedtls_md_hmac(md, salt, salt_len, ikm, ikm_len, prk);
  }

  // Expand
  unsigned char t[32];
  unsigned char counter = 1;
  size_t t_len = 0, offset = 0;
  while (offset < okm_len) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md, 1);
    mbedtls_md_hmac_starts(&ctx, prk, 32);
    if (t_len > 0) mbedtls_md_hmac_update(&ctx, t, t_len);
    mbedtls_md_hmac_update(&ctx, info, info_len);
    mbedtls_md_hmac_update(&ctx, &counter, 1);
    mbedtls_md_hmac_finish(&ctx, t);
    mbedtls_md_free(&ctx);
    t_len = 32;
    size_t copy = (okm_len - offset < 32) ? (okm_len - offset) : 32;
    memcpy(okm + offset, t, copy);
    offset += copy;
    counter++;
  }
  return 0;
}

// ── NVS key storage ──────────────────────────────────────────────────────────

void checkStoredKey() {
  preferences.begin("ble-keys", true);
  if (preferences.isKey("bleKey")) {
    bleKeyHex = preferences.getString("bleKey", "");
    hasKey = (bleKeyHex.length() > 0);
  } else {
    hasKey = false;
  }
  preferences.end();
}

void saveKeyToMemory(String key) {
  preferences.begin("ble-keys", false);
  preferences.putString("bleKey", key);
  preferences.end();
  bleKeyHex = key;
  hasKey = true;
  Serial.println("Key saved to NVS");
}

void clearStoredKey() {
  preferences.begin("ble-keys", false);
  preferences.remove("bleKey");
  preferences.end();
  bleKeyHex = "";
  hasKey = false;
  Serial.println("Key cleared from NVS");
}

// ── WiFi ─────────────────────────────────────────────────────────────────────

void connectWiFi() {
  // Hard reset WiFi radio — needed after WDT/crash reboot
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(1000);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);

  for (int retry = 1; retry <= 3; retry++) {
    Serial.printf("WiFi attempt %d/3: %s\n", retry, ssid);
    WiFi.begin(ssid, password);

    for (int i = 0; i < 120 && WiFi.status() != WL_CONNECTED; i++) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi OK - IP: " + WiFi.localIP().toString());
      return;
    }

    Serial.printf("\nWiFi failed (status=%d)\n", WiFi.status());
    if (retry < 3) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(3000);
      WiFi.mode(WIFI_STA);
    }
  }
  Serial.println("WiFi connection failed after 3 attempts");
}

// ── mDNS: Tự tìm server trên mạng LAN ───────────────────────────────────────────

bool discoverServer() {
  if (!MDNS.begin("anchor")) {
    Serial.println("mDNS init failed");
    return false;
  }

  Serial.println("mDNS: searching for smartcar server...");

  for (int attempt = 1; attempt <= 5; attempt++) {
    int n = MDNS.queryService("http", "tcp");
    for (int i = 0; i < n; i++) {
      if (MDNS.hostname(i) == "smartcar") {
        serverBaseUrl = "http://" + MDNS.address(i).toString() + ":" + String(MDNS.port(i));
        Serial.println("mDNS: server found at " + serverBaseUrl);
        MDNS.end();
        return true;
      }
    }
    Serial.printf("mDNS: attempt %d/5 - not found, retrying...\n", attempt);
    delay(2000);
  }

  MDNS.end();
  Serial.println("mDNS: server not found after 5 attempts");
  return false;
}

// ── Server: Fetch pairing key via ECDH + AES-GCM ────────────────────────────

// Khai báo trước để fetchKeyFromServer() gọi được
String decryptResponse(mbedtls_pk_context* client_key, String server_pub_b64,
                       String encrypted_data_b64, String nonce_b64);

String fetchKeyFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return "";
  }

  // Tạo ephemeral EC key pair
  mbedtls_pk_context client_key;
  mbedtls_pk_init(&client_key);

  int ret = mbedtls_pk_setup(&client_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) { Serial.printf("Key setup failed: %d\n", ret); return ""; }

  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(client_key),
                             mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0) {
    Serial.printf("Key gen failed: %d\n", ret);
    mbedtls_pk_free(&client_key);
    return "";
  }

  // Export public key → Base64
  unsigned char pub_der[200];
  int pub_len = mbedtls_pk_write_pubkey_der(&client_key, pub_der, sizeof(pub_der));
  if (pub_len < 0) {
    Serial.printf("Export failed: %d\n", pub_len);
    mbedtls_pk_free(&client_key);
    return "";
  }

  size_t olen;
  unsigned char pub_b64[300];
  ret = mbedtls_base64_encode(pub_b64, sizeof(pub_b64), &olen,
                               pub_der + sizeof(pub_der) - pub_len, pub_len);
  if (ret != 0) {
    Serial.printf("Base64 failed: %d\n", ret);
    mbedtls_pk_free(&client_key);
    return "";
  }
  pub_b64[olen] = '\0';

  // HTTP POST
  HTTPClient http;
  String url = String(serverBaseUrl) + "/secure-check-pairing";
  http.begin(url);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<512> reqDoc;
  reqDoc["vehicle_id"] = vehicleId;
  reqDoc["client_public_key_b64"] = String((char*)pub_b64);
  String reqBody;
  serializeJson(reqDoc, reqBody);

  Serial.println("POST " + url);
  int httpCode = http.POST(reqBody);
  String key = "";

  if (httpCode == 200) {
    StaticJsonDocument<1024> respDoc;
    if (!deserializeJson(respDoc, http.getString())) {
      key = decryptResponse(&client_key,
                            respDoc["server_public_key_b64"].as<String>(),
                            respDoc["encrypted_data_b64"].as<String>(),
                            respDoc["nonce_b64"].as<String>());
    } else {
      Serial.println("JSON parse failed");
    }
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
  }

  http.end();
  mbedtls_pk_free(&client_key);
  return key;
}
// Macro giúp thoát sạch với cleanup ECDH + server_key
#define ECDH_CLEANUP(msg) do { \
  Serial.println(msg); \
  mbedtls_ecdh_free(&ecdh); \
  mbedtls_pk_free(&server_key); \
  return ""; \
} while(0)

String decryptResponse(mbedtls_pk_context* client_key, String server_pub_b64,
                       String encrypted_data_b64, String nonce_b64) {
  // Decode + parse server public key
  unsigned char server_pub_der[200];
  size_t server_pub_len;
  if (mbedtls_base64_decode(server_pub_der, sizeof(server_pub_der), &server_pub_len,
                             (const unsigned char*)server_pub_b64.c_str(),
                             server_pub_b64.length()) != 0) {
    Serial.println("Decode server key failed"); return "";
  }

  mbedtls_pk_context server_key;
  mbedtls_pk_init(&server_key);
  if (mbedtls_pk_parse_public_key(&server_key, server_pub_der, server_pub_len) != 0) {
    Serial.println("Parse server key failed");
    mbedtls_pk_free(&server_key); return "";
  }

  // ECDH shared secret
  mbedtls_ecdh_context ecdh;
  mbedtls_ecdh_init(&ecdh);

  if (mbedtls_ecdh_setup(&ecdh, MBEDTLS_ECP_DP_SECP256R1) != 0)
    ECDH_CLEANUP("ECDH setup failed");
  if (mbedtls_ecdh_get_params(&ecdh, (mbedtls_ecp_keypair*)mbedtls_pk_ec(*client_key), MBEDTLS_ECDH_OURS) != 0)
    ECDH_CLEANUP("ECDH client params failed");
  if (mbedtls_ecdh_get_params(&ecdh, (mbedtls_ecp_keypair*)mbedtls_pk_ec(server_key), MBEDTLS_ECDH_THEIRS) != 0)
    ECDH_CLEANUP("ECDH server params failed");

  unsigned char shared_secret[32];
  size_t olen;
  if (mbedtls_ecdh_calc_secret(&ecdh, &olen, shared_secret, sizeof(shared_secret),
                                mbedtls_ctr_drbg_random, &ctr_drbg) != 0 || olen != 32)
    ECDH_CLEANUP("Shared secret failed");

  // Derive KEK từ shared secret
  unsigned char kek[16];
  const unsigned char kdf_info[] = "secure-check-kek";
  if (hkdf_sha256(NULL, 0, shared_secret, 32, kdf_info, strlen((char*)kdf_info), kek, 16) != 0)
    ECDH_CLEANUP("HKDF failed");

  // Decode ciphertext + nonce
  unsigned char encrypted_data[200], nonce[12];
  size_t encrypted_len, nonce_len;
  if (mbedtls_base64_decode(encrypted_data, sizeof(encrypted_data), &encrypted_len,
                             (const unsigned char*)encrypted_data_b64.c_str(),
                             encrypted_data_b64.length()) != 0)
    ECDH_CLEANUP("Decode ciphertext failed");

  if (mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len,
                             (const unsigned char*)nonce_b64.c_str(),
                             nonce_b64.length()) != 0 || nonce_len != 12)
    ECDH_CLEANUP("Decode nonce failed");

  // AES-GCM decrypt
  unsigned char decrypted[200];
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128) != 0) {
    Serial.println("GCM setkey failed");
    mbedtls_gcm_free(&gcm);
    ECDH_CLEANUP("");
  }

  size_t cipher_len = encrypted_len - 16;
  unsigned char tag[16];
  memcpy(tag, encrypted_data + cipher_len, 16);

  int gcm_ret = mbedtls_gcm_auth_decrypt(&gcm, cipher_len, nonce, 12,
                                          NULL, 0, tag, 16, encrypted_data, decrypted);
  mbedtls_gcm_free(&gcm);

  if (gcm_ret != 0) ECDH_CLEANUP("Decryption failed (tag mismatch?)");

  decrypted[cipher_len] = '\0';

  // Parse JSON result
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, (char*)decrypted)) ECDH_CLEANUP("JSON parse failed");

  String key = "";
  if (doc["paired"].as<bool>()) {
    key = doc["pairing_key"].as<String>();
    Serial.println("Server: PAIRED - ID=" + doc["pairing_id"].as<String>());
  } else {
    Serial.println("Server: NOT PAIRED - " + doc["message"].as<String>());
  }

  mbedtls_ecdh_free(&ecdh);
  mbedtls_pk_free(&server_key);
  return key;
}

// ── BLE Callbacks ────────────────────────────────────────────────────────────

// Helper: khóa/mở khóa xe qua CAN (chỉ log khi thực sự chuyển trạng thái)
static void canLock() {
  if (!carUnlocked) return;
  if (pCanControl && pCanControl->lockCar()) {
    carUnlocked = false;
    Serial.println(">> Xe da KHOA");
  }
}

static void canUnlock() {
  if (carUnlocked) return;
  if (pCanControl && pCanControl->unlockCar()) {
    carUnlocked = true;
    Serial.println(">> Xe da MO KHOA");
  }
}

// AuthCharacteristicCallbacks: nhận HMAC response từ Tag
class AuthCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    String val = pChar->getValue();

    // Tích lũy vào buffer (response có thể đến nhiều chunk)
    for (size_t i = 0; i < val.length() && responseBufferLen < 32; i++) {
      responseBuffer[responseBufferLen++] = (uint8_t)val[i];
    }

    if (responseBufferLen < 32) return; // Chưa đủ, đợi chunk tiếp

    // Đủ 32 byte — defer xác thực sang loop() trên Core 1
    // (tránh gọi HMAC trên Core 0 BLE stack với stack nhỏ)
    pendingAuthVerify = true;
  }
};

// CharacteristicCallbacks: nhận lệnh từ Tag
// Chỉ set deferred flags khi trạng thái thực sự cần thay đổi → tránh spam serial + thao tác thừa
class CharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    if (!pChar) return;
    String value = pChar->getValue();
    if (value.length() == 0) return;

    if (value.startsWith("VERIFIED:")) {
      // ≤ 3m → mở khóa (chỉ khi đang khóa)
      if (!carUnlocked) {
        Serial.println("UWB: " + value.substring(9) + " -> Mo khoa");
        pendingUnlock = true;
      }

    } else if (value.startsWith("WARNING:")) {
      // > 3m, < 20m → khóa (chỉ khi đang mở)
      if (carUnlocked) {
        Serial.println("UWB: " + value.substring(8) + " -> Khoa");
        pendingLock = true;
      }

    } else if (value.startsWith("LOCK_CAR")) {
      if (carUnlocked) pendingLock = true;

    } else if (value.startsWith("UWB_STOP")) {
      // > 20m → khóa + tắt UWB
      if (carUnlocked) pendingLock = true;
      pendingUwbDeinit = true;
      Serial.println("UWB: Tag ngoai 20m, dung UWB");

    } else if (value.startsWith("TAG_UWB_READY")) {
      // Tag trong 20m, sẵn sàng UWB
      if (!authenticated) return;
      if (!uwbInitialized) {
        pendingUwbInit = true;
        pendingUwbActiveNotify = true;
      } else {
        pendingUwbActiveNotify = true;
      }

    } else if (value.startsWith("ALERT:RELAY_ATTACK")) {
      Serial.println("SECURITY: Relay attack detected!");
    }
  }
};

// MyServerCallbacks: kết nối / ngắt kết nối BLE
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    (void)pServer;
    deviceConnected   = true;
    authenticated     = false;
    responseBufferLen = 0;
    connectionTime    = millis();
    challengePending  = true;
    Serial.println("BLE: Tag connected");
  }

  void onDisconnect(BLEServer* pServer) {
    (void)pServer;
    deviceConnected   = false;
    authenticated     = false;
    responseBufferLen = 0;
    challengePending  = false;
    memset(currentChallenge, 0, sizeof(currentChallenge));

    // Defer SPI operations to loop() on Core 1 (BLE callback runs on Core 0)
    pendingUwbDeinit = true;
    pendingLock      = true;

    Serial.println("BLE: Tag disconnected");
    // Không gọi startAdvertising() ở đây — gọi từ BLE task có thể gây crash.
    // loop() xử lý việc này an toàn qua cờ prevConnected.
  }
};

// ── BLE Server init ───────────────────────────────────────────────────────────

void startBLE() {
  hexStringToBytes(bleKeyHex.c_str(), pairingKey, 16);

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setPower(ESP_PWR_LVL_P9);  // Max TX power (+9 dBm) for longer range
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

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

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);  // 7.5ms min connection interval
  pAdv->setMaxPreferred(0x12);  // 22.5ms max connection interval
  BLEDevice::startAdvertising();

  Serial.println("BLE started - Advertising as: " + String(DEVICE_NAME));
}

// ── Main flow: load key → start BLE ──────────────────────────────────────────

void executeMainFlow() {
  checkStoredKey();

  if (hasKey) {
    Serial.println("Key found in NVS: " + bleKeyHex);
  } else {
    Serial.println("No key in NVS, need WiFi to fetch from server...");

    // Chỉ bật WiFi khi thực sự cần lấy key — tránh xung đột SPI/RF khi không cần thiết
    connectWiFi();

    if (WiFi.status() == WL_CONNECTED) {
      if (serverBaseUrl.length() == 0 || serverBaseUrl == "") {
        discoverServer();
      }
      String serverKey = fetchKeyFromServer();
      if (serverKey.length() > 0) {
        saveKeyToMemory(serverKey);
      } else {
        Serial.println("Failed to get key. Please pair vehicle first, then restart.");
      }
    } else {
      Serial.println("WiFi failed - cannot fetch key. Restart and try again.");
    }

    // Tắt WiFi ngay sau khi dùng xong — WiFi và BLE dùng chung radio 2.4GHz
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    if (!hasKey) {
      Serial.println("\n=== Khong lay duoc key qua WiFi ===");
      Serial.println("Dung lenh Serial de nhap key thu cong:");
      Serial.println("  SETKEY <32-hex-chars>");
      Serial.println("  Vi du: SETKEY e9b8da4e60206bd04bd554c6a94e4e0e");
      Serial.println("  (Lay key tu server hoac tu thiet bi da pair)");
      Serial.println("  Hoac dung lenh WIFI de thu lai ket noi WiFi");
      Serial.println("======================================\n");
      return;
    }
  }

  startBLE();
  bleStarted = true;
}

// ── UWB Init / Deinit ─────────────────────────────────────────────────────────

bool initUWB(void) {
  if (uwbInitialized) return true;

  Serial.println("UWB: initializing...");

  // Deselect CAN CS to free SPI bus for DW3000
  digitalWrite(CAN_CS, HIGH);

  // Set up SPI peripheral (stores _irq, _rst, calls SPI.begin())
  spiBegin(PIN_IRQ, PIN_RST);

  // Set DW3000 CS pin only — DO NOT call spiSelect() which writes
  // DW1000-era register commands to the DW3000 (wrong SPI protocol,
  // wrong register addresses) and can corrupt chip state.
  {
    extern uint8_t _ss;
    _ss = PIN_SS;
  }
  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);

  // Hardware reset DW3000: drive RST LOW then float
  // (DW3000 spec: RST should not be driven HIGH, only floated)
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(2);
  pinMode(PIN_RST, INPUT);
  delay(50); // DW3000 cần thời gian ổn định thạch anh sau reset

  // Chờ DW3000 vào IDLE_RC (retry tối đa 500ms, tránh while(1) gây WDT reset)
  int retries = 500;
  while (!dwt_checkidlerc() && retries > 0) {
    delay(1);
    retries--;
  }
  if (retries == 0) {
    Serial.println("UWB: IDLE_RC failed - aborting (not halting)");
    // Giữ DW3000 trong reset để không chiếm SPI bus
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, LOW);
    return false;
  }

  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) {
    Serial.println("UWB: init failed - aborting");
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, LOW);
    return false;
  }

  dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

  if (dwt_configure(&uwbConfig) != 0) {
    Serial.println("UWB: configure failed - aborting");
    dwt_softreset();
    delay(2);
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, LOW);
    return false;
  }

  dwt_configuretxrf(&txconfig_options);
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

  uwbInitialized = true;
  lastPollReceivedTime = millis();
  Serial.println("UWB: ready");
  return true;
}

void deinitUWB(void) {
  if (!uwbInitialized) return;
  dwt_forcetrxoff();  // Stop any ongoing RX/TX before reset
  dwt_softreset();
  delay(2);
  // Hold DW3000 in reset to prevent it from driving SPI MISO
  // and interfering with MCP2515 (CAN) on the shared SPI bus
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  uwbInitialized = false;
  Serial.println("UWB: stopped");
}

// ── UWB Responder loop (TWR) ──────────────────────────────────────────────────

void uwbResponderLoop(void) {
  (void)dwt_rxenable(DWT_START_RX_IMMEDIATE);

  // Chờ RX OK hoặc lỗi (timeout 50ms)
  unsigned long t0 = millis();
  while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
           (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR))) {
    if ((millis() - t0) > 50UL) {
      dwt_forcetrxoff();
      return;
    }
  }

  if (!(status_reg & SYS_STATUS_RXFCG_BIT_MASK)) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
    dwt_forcetrxoff();  // Đưa DW3000 về IDLE, giải phóng SPI MISO
    return;
  }

  // Nhận frame
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
  uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
  if (frame_len > sizeof(rx_buffer)) return;

  dwt_readrxdata(rx_buffer, frame_len, 0U);
  rx_buffer[ALL_MSG_SN_IDX] = 0U; // Bỏ qua SN khi so sánh header

  if (memcmp(rx_buffer, rx_poll_msg, ALL_MSG_COMMON_LEN) != 0) return;

  // Tính thời điểm gửi response
  uint64_t poll_rx_ts = get_rx_timestamp_u64();
  uint32_t resp_tx_time = (uint32_t)((poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8);
  (void)dwt_setdelayedtrxtime(resp_tx_time);

  uint64_t resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

  // Nhúng timestamps vào response frame
  resp_msg_set_ts(&tx_resp_msg[RESP_MSG_POLL_RX_TS_IDX], poll_rx_ts);
  resp_msg_set_ts(&tx_resp_msg[RESP_MSG_RESP_TX_TS_IDX], resp_tx_ts);
  tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;

  dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0U);
  dwt_writetxfctrl(sizeof(tx_resp_msg), 0U, 1);

  if (dwt_starttx(DWT_START_TX_DELAYED) != DWT_SUCCESS) return;

  // Chờ TX hoàn thành (safety timeout)
  unsigned long tx_t0 = millis();
  while (!(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS_BIT_MASK)) {
    if ((millis() - tx_t0) > 10UL) {
      dwt_forcetrxoff();
      return;
    }
  }
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);

  frame_seq_nb++;
  lastPollReceivedTime = millis();
}

// ── setup ─────────────────────────────────────────────────────────────────────

void setup(void) {
  Serial.begin(115200);
  delay(1000);

  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("\nSmart Car Anchor - Vehicle ID: %s (reset: %d)\n", vehicleId, reason);
  if (reason == ESP_RST_PANIC)   Serial.println("WARNING: Last reset was CRASH!");
  if (reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT)
    Serial.println("WARNING: Last reset was WATCHDOG!");

  // Giữ DWM3000 trong reset trong khi WiFi init để tiết kiệm dòng trên rail 3.3V
  // (WiFi peak ~300mA, DWM3000 bình thường ~30-50mA)
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
  pinMode(PIN_SS,  OUTPUT); digitalWrite(PIN_SS,  HIGH);
  pinMode(CAN_CS,  OUTPUT); digitalWrite(CAN_CS,  HIGH);
  delay(100);

  // Init mbedTLS CSPRNG
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  const char* pers = "anchor_secure";
  if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char*)pers, strlen(pers)) != 0) {
    Serial.println("mbedtls init failed");
    return;
  }

  // executeMainFlow() trước CAN — WiFi cần chạy khi SPI bus chưa active
  // (MCP2515 constructor gọi SPI.begin(), gây nhiễu WiFi trên board)
  executeMainFlow();

  // Đảm bảo WiFi đã tắt (executeMainFlow tắt nếu bật, nhưng phòng trường hợp key có sẵn)
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }
  Serial.println("WiFi disabled - BLE only mode");

  // Init CAN Bus SAU WiFi — tránh SPI bus conflict khi kết nối WiFi
  pMcp2515  = new MCP2515(CAN_CS);
  pCanControl = new CANCommands(pMcp2515);
  if (!pCanControl->initialize(CAN_CS, CAN_100KBPS, MCP_CLOCK)) {
    Serial.println("CAN: init failed, continuing with BLE+UWB only");
  }
}

// ── loop ──────────────────────────────────────────────────────────────────────

void loop(void) {
  bool uwbJustInitialized = false;

  // ── Xử lý auth verify trên Core 1 (stack đủ lớn cho HMAC) ───────────────
  if (pendingAuthVerify) {
    pendingAuthVerify = false;

    uint8_t received[32];
    memcpy(received, responseBuffer, 32);
    responseBufferLen = 0;

    uint8_t expected[32];
    if (!computeHMAC(pairingKey, 16, currentChallenge, 16, expected)) {
      Serial.println("HMAC compute failed");
      BLEDevice::getServer()->disconnect(BLEDevice::getServer()->getConnId());
    } else {
      bool match = (memcmp(received, expected, 32) == 0);
      authenticated = match;

      if (match) {
        Serial.println("Auth OK - Tag duoc xac thuc");
        pAuthCharacteristic->setValue("AUTH_OK");
        pAuthCharacteristic->notify();
      } else {
        Serial.println("Auth FAIL - Sai key, ngat ket noi");
        pAuthCharacteristic->setValue("AUTH_FAIL");
        pAuthCharacteristic->notify();
        delay(100);
        BLEDevice::getServer()->disconnect(BLEDevice::getServer()->getConnId());
      }
    }
  }

  // ── Serial commands ──────────────────────────────────────────────────────
  handleSerialCommands();

  // ── BLE connection edge detection ────────────────────────────────────────
  if (deviceConnected && !prevConnected) prevConnected = true;

  // Gửi challenge sau khi Tag subscribe
  if (deviceConnected && challengePending &&
      (millis() - connectionTime) >= CHALLENGE_SEND_DELAY_MS) {
    challengePending = false;
    generateChallenge(currentChallenge, 16);
    pChallengeCharacteristic->setValue(currentChallenge, 16);
    pChallengeCharacteristic->notify();
    Serial.println("Challenge sent to Tag");
  }

  // ── Deferred SPI actions (BLE Core 0 → loop Core 1) ─────────────────────
  if (pendingUwbDeinit) { pendingUwbDeinit = false; deinitUWB(); }
  if (pendingUwbInit && authenticated) {
    pendingUwbInit = false;
    if (initUWB()) uwbJustInitialized = true;
  }
  // Tạm dừng UWB trước khi gửi CAN — DW3000 và MCP2515 chia sẻ SPI bus.
  // Nếu không forcetrxoff(), DW3000 có thể drive MISO gây lỗi CAN (ERROR_ALLTXBUSY).
  if ((pendingLock || pendingUnlock) && uwbInitialized) {
    dwt_forcetrxoff();
    digitalWrite(PIN_SS, HIGH); // Đảm bảo DW3000 CS deselect
  }
  if (pendingLock)   { pendingLock   = false; canLock();   }
  if (pendingUnlock) { pendingUnlock = false; canUnlock(); }
  if (pendingUwbActiveNotify && uwbInitialized && pCharacteristic) {
    pendingUwbActiveNotify = false;
    pCharacteristic->setValue("UWB_ACTIVE");
    pCharacteristic->notify();
  }

  // ── Restart advertising sau khi ngắt kết nối ────────────────────────────
  if (!deviceConnected && prevConnected) {
    prevConnected = false;
    delay(300);
    BLEDevice::startAdvertising();
    Serial.println("BLE: advertising restarted");
  }

  // ── Safety net: re-advertising nếu không có kết nối trong 10s ───────────
  // ESP32-S3 BLE stack có thể tự dừng advertising sau disconnect
  static unsigned long lastAdvRefresh = 0;
  if (!deviceConnected && (millis() - lastAdvRefresh > 10000)) {
    lastAdvRefresh = millis();
    BLEDevice::startAdvertising();
  }

  // ── UWB ranging (skip first cycle sau init để hardware ổn định) ──────────
  if (uwbInitialized && !uwbJustInitialized) uwbResponderLoop();

  // ── Status log: chỉ in khi có thay đổi hoặc mỗi 30s heartbeat ──────────
  printStatusIfChanged();

  delay(10);
}

// ── Serial command handler (tách ra cho gọn loop) ─────────────────────────

void handleSerialCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "CLEAR") {
    clearStoredKey();
    Serial.println("Key cleared. Restart ESP32.");

  } else if (cmd.startsWith("SETKEY ")) {
    Serial.readString(); // flush
    String keyHex = cmd.substring(7);
    keyHex.trim();
    if (keyHex.length() != 32) {
      Serial.printf("ERROR: Can 32 ky tu hex (hien tai: %d)\n", keyHex.length());
      return;
    }
    bool valid = true;
    for (size_t i = 0; i < keyHex.length(); i++) {
      if (!isxdigit(keyHex.charAt(i))) { valid = false; break; }
    }
    if (!valid) {
      Serial.println("ERROR: Chi dung 0-9, a-f, A-F");
      return;
    }
    saveKeyToMemory(keyHex);
    Serial.println("Key saved: " + keyHex);
    if (!bleStarted) { startBLE(); bleStarted = true; }

  } else if (cmd == "WIFI") {
    if (hasKey) {
      Serial.println("Da co key. Dung CLEAR truoc.");
      return;
    }
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      if (serverBaseUrl.length() == 0) discoverServer();
      String k = fetchKeyFromServer();
      if (k.length() > 0) {
        saveKeyToMemory(k);
        if (!bleStarted) { startBLE(); bleStarted = true; }
      }
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

  } else if (cmd == "STATUS") {
    Serial.printf("Key:%s BLE:%s Auth:%s UWB:%s Car:%s\n",
      hasKey ? "Y" : "N", deviceConnected ? "Y" : "N",
      authenticated ? "Y" : "N", uwbInitialized ? "Y" : "N",
      carUnlocked ? "UNLOCKED" : "LOCKED");

  } else if (cmd == "SHOW") {
    Serial.println(hasKey ? "Key: " + bleKeyHex : "No key stored");

  } else if (cmd == "HELP") {
    Serial.println("Commands: SETKEY <hex32>, WIFI, SHOW, STATUS, CLEAR, HELP");
  }
}

// ── Status: chỉ in khi trạng thái thay đổi hoặc heartbeat 30s ────────────

void printStatusIfChanged() {
  static bool lastBle = false, lastAuth = false, lastUwb = false, lastCar = false;
  static unsigned long lastHeartbeat = 0;

  bool ble = deviceConnected, auth = authenticated, uwb = uwbInitialized, car = carUnlocked;
  bool changed = (ble != lastBle || auth != lastAuth || uwb != lastUwb || car != lastCar);

  if (changed || (millis() - lastHeartbeat > 30000)) {
    Serial.printf("[%s] BLE:%s Auth:%s UWB:%s Car:%s\n",
      changed ? "Change" : "Status",
      ble ? "ON" : "OFF", auth ? "OK" : "NO",
      uwb ? "ON" : "OFF", car ? "OPEN" : "LOCKED");
    lastBle = ble; lastAuth = auth; lastUwb = uwb; lastCar = car;
    lastHeartbeat = millis();
  }
}