/**
 * @file BLE_UWB_Anchor.ino
 * @brief Module Anchor BLE + UWB (Phía Xe)
 * 
 * @details Module này triển khai hệ thống xác thực khoảng cách bảo mật phía xe,
 *          kết hợp Bluetooth Low Energy (BLE) để kết nối ban đầu và 
 *          Ultra-Wideband (UWB) để xác minh khoảng cách chính xác.
 * 
 * @features
 * - BLE Server: Phát và chấp nhận kết nối từ thiết bị Tag
 * - UWB Responder: Phản hồi yêu cầu đo khoảng cách
 * - Bảo mật: Xác thực đo khoảng cách để ngăn tấn công relay
 * - Tiết kiệm điện: Chỉ kích hoạt UWB sau khi Tag xác minh RSSI
 * 
 * @operation_flow
 * 1. Bắt đầu phát BLE
 * 2. Chờ Tag kết nối qua BLE
 * 3. Trễ 2 giây để Tag xác minh RSSI gần
 * 4. Khởi tạo module UWB
 * 5. Phản hồi các yêu cầu đo khoảng cách UWB
 * 6. Nhận và xác thực kết quả xác minh khoảng cách từ Tag
 * 
 * @author Smart Car Access System
 * @version 2.0
 * @date 2026-03-02
 * 
 * @compliance Tuân thủ MISRA C:2012
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
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

/* ========================================================================
 * Các Hằng Số Cấu Hình BLE
 * ======================================================================== */
/** @brief UUID của BLE Service cho hệ thống truy cập xe */
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"

/** @brief UUID của BLE Characteristic để trao đổi dữ liệu */
#define CHARACTERISTIC_UUID "abcdef12-3456-7890-abcd-ef1234567890"

/** @brief UUID cho Challenge-Response Authentication */
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // Phone gửi response
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"  // Vehicle gửi challenge

/** @brief Pairing key (32 hex chars = 16 bytes) - trong thực tế lấy từ Preferences */
const char* PAIRING_KEY_HEX = "e9b8da4e60206bd04bd554c6a94e4e0e";

/* ========================================================================
 * WiFi & Server Configuration
 * ======================================================================== */
/** @brief WiFi SSID */
const char* ssid = "Student";

/** @brief WiFi password */
const char* password = "";

/** @brief Server base URL - tự động cập nhật qua mDNS sau khi WiFi kết nối */
String serverBaseUrl = "http://10.0.7.178:8000";

/** @brief Vehicle ID */
const char* vehicleId = "1HGBH41JXMN109186";

/** @brief Device name for BLE */
#define DEVICE_NAME "SmartCar_Vehicle"

/* ========================================================================
 * Cấu Hình Bảo Mật
 * ======================================================================== */
/** 
 * @brief Độ trễ trước khi kích hoạt UWB tính bằng mili giây
 * @details Cho phép Tag có thời gian xác minh RSSI trước khi bắt đầu đo khoảng cách UWB
 */
#define UWB_ACTIVATION_DELAY_MS (2000U)

/* ========================================================================
 * Các Biến Trạng Thái BLE
 * ======================================================================== */
/** @brief Cờ: Thiết bị Tag hiện đang kết nối */
static bool deviceConnected = false;

/** @brief Cờ: Trạng thái kết nối trước đó (phát hiện cạnh) */
static bool prevConnected = false;

/** @brief Con trỏ đến BLE characteristic để trao đổi dữ liệu */
static BLECharacteristic *pCharacteristic = nullptr;

/** @brief Mốc thời gian khi thiết lập kết nối (mili giây) */
static unsigned long connectionTime = 0U;

/** @brief Cờ: Đã yêu cầu kích hoạt UWB */
static bool uwbActivationRequested = false;

/* ========================================================================
 * Biến Challenge-Response Authentication
 * ======================================================================== */
/** @brief Con trỏ đến BLE characteristic cho challenge */
static BLECharacteristic *pChallengeCharacteristic = nullptr;

/** @brief Con trỏ đến BLE characteristic cho auth response */
static BLECharacteristic *pAuthCharacteristic = nullptr;

/** @brief Cờ: Tag đã được xác thực */
static bool authenticated = false;

/** @brief Challenge hiện tại (16 bytes) */
static uint8_t currentChallenge[16];

/** @brief Pairing key (16 bytes) */
static uint8_t pairingKey[16];

/** @brief Buffer cho response nhận được (có thể nhận theo nhiều packet) */
static uint8_t responseBuffer[32];
static size_t responseBufferLen = 0;

/** @brief Thời gian write cuối cùng */
static unsigned long lastWriteTime = 0U;

/* ========================================================================
 * NVS Storage & Key Management
 * ======================================================================== */
/** @brief NVS Preferences instance */
static Preferences preferences;

/** @brief BLE pairing key in hex string format */
static String bleKeyHex = "";

/** @brief Flag: có key trong bộ nhớ */
static bool hasKey = false;

/* ========================================================================
 * mbedTLS Crypto Context
 * ======================================================================== */
/** @brief mbedTLS entropy context */
static mbedtls_entropy_context entropy;

/** @brief mbedTLS random number generator context */
static mbedtls_ctr_drbg_context ctr_drbg;

/* ========================================================================
 * Cấu Hình CAN Bus
 * ======================================================================== */
/** @brief Chân CS cho MCP2515 CAN controller */
#define CAN_CS (9)

/** @brief Tần số thạch anh MCP2515 */
#define MCP_CLOCK MCP_8MHZ

/** @brief Con trỏ đến CAN controller */
static MCP2515 mcp2515(CAN_CS);
static CANCommands* pCanControl = nullptr;

/** @brief Cờ: Xe đã được mở khóa */
static bool carUnlocked = false;

/* ========================================================================
 * Cấu Hình Chân Phần Cứng UWB
 * ======================================================================== */
/** @brief Chân Reset của DW3000 */
#define PIN_RST (5)

/** @brief Chân Interrupt của DW3000 */
#define PIN_IRQ (4)

/** @brief Chân SPI Chip Select */
#define PIN_SS (10)

/* ========================================================================
 * Cấu Hình Đo Khoảng Cách UWB
 * ======================================================================== */
/** @brief Giá trị trễ ăng-ten phát (hiệu chuẩn theo thiết bị) */
#define TX_ANT_DLY (16385U)

/** @brief Giá trị trễ ăng-ten thu (hiệu chuẩn theo thiết bị) */
#define RX_ANT_DLY (16385U)

/* ========================================================================
 * Các Hằng Số Khung Tin Nhắn UWB
 * ======================================================================== */
/** @brief Độ dài header tin nhắn chung tính bằng byte */
#define ALL_MSG_COMMON_LEN (10U)

/** @brief Chỉ số số thứ tự trong khung tin nhắn */
#define ALL_MSG_SN_IDX (2U)

/** @brief Chỉ số timestamp RX Poll trong tin nhắn phản hồi */
#define RESP_MSG_POLL_RX_TS_IDX (10U)

/** @brief Chỉ số timestamp TX Response trong tin nhắn phản hồi */
#define RESP_MSG_RESP_TX_TS_IDX (14U)

/** @brief Độ dài trường timestamp tính bằng byte */
#define RESP_MSG_TS_LEN (4U)

/** @brief Độ trễ từ Poll RX đến Response TX tính bằng micro giây */
#define POLL_RX_TO_RESP_TX_DLY_UUS (800U) /* Tăng để đảm bảo đủ thời gian xử lý */

/** @brief Kích thước buffer tin nhắn tính bằng byte */
#define MSG_BUFFER_SIZE (20U)

/* DW3000 Configuration */
static dwt_config_t config = {
    5,                /* Channel number. */
    DWT_PLEN_128,     /* Preamble length. Used in TX only. */
    DWT_PAC8,         /* Preamble acquisition chunk size. Used in RX only. */
    9,                /* TX preamble code. Used in TX only. */
    9,                /* RX preamble code. Used in RX only. */
    1,                /* 0 to use standard 8 symbol SFD, 1 to use non-standard 8 symbol, 2 for non-standard 16 symbol SFD and 3 for 4z 8 symbol SDF type */
    DWT_BR_6M8,       /* Data rate. */
    DWT_PHRMODE_STD,  /* PHY header mode. */
    DWT_PHRRATE_STD,  /* PHY header rate. */
    (129 + 8 - 8),    /* SFD timeout (preamble length + 1 + SFD length - PAC size). Used in RX only. */
    DWT_STS_MODE_OFF, /* STS disabled */
    DWT_STS_LEN_64,   /* STS length see allowed values in Enum dwt_sts_lengths_e */
    DWT_PDOA_M0       /* PDOA mode off */
};

/* ========================================================================
 * Các Biến Trạng Thái UWB
 * ======================================================================== */
/** @brief Cờ: Module UWB đã được khởi tạo và sẵn sàng */
static bool uwbInitialized = false;

/** @brief Mẫu tin nhắn Poll mong đợi (Initiator -> Responder) */
static uint8_t rx_poll_msg[] = {0x41U, 0x88U, 0U, 0xCAU, 0xDEU, 'W', 'A', 'V', 'E', 0xE0U, 0U, 0U};

/** @brief Mẫu tin nhắn phản hồi (Responder -> Initiator) */
static uint8_t tx_resp_msg[] = {0x41U, 0x88U, 0U, 0xCAU, 0xDEU, 'V', 'E', 'W', 'A', 0xE1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};

/** @brief Số thứ tự khung (0-255, quay vòng) */
static uint8_t frame_seq_nb = 0U;

/** @brief Thời gian chờ giữa các lần đo khoảng cách tính bằng mili giây */
#define RANGING_DELAY_MS (100U)

/** @brief Thời gian timeout không nhận được POLL trước khi tắt UWB (ms) */
#define UWB_IDLE_TIMEOUT_MS (10000U)

/** @brief Mốc thời gian nhận POLL cuối cùng */
static unsigned long lastPollReceivedTime = 0U;

/** @brief Buffer cho các khung UWB nhận được */
static uint8_t rx_buffer[MSG_BUFFER_SIZE];

/** @brief Giá trị thanh ghi trạng thái DW3000 */
static uint32_t status_reg = 0U;

/** @brief Timestamp nhận tin nhắn Poll */
static uint64_t poll_rx_ts = 0U;

/** @brief Timestamp truyền tin nhắn Response */
static uint64_t resp_tx_ts = 0U;

extern dwt_txconfig_t txconfig_options;

/* ========================================================================
 * Khai Báo Nguyên Mẫu Hàm
 * ======================================================================== */  
uint64_t get_rx_timestamp_u64(void);
void resp_msg_set_ts(uint8_t *ts_field, uint64_t ts);
void initUWB(void);
void deinitUWB(void);
void uwbResponderLoop(void);

/* ========================================================================
 * Helper Functions cho Challenge-Response Authentication
 * ======================================================================== */

/**
 * @brief Chuyển đổi chuỗi hex thành mảng bytes
 */
void hexStringToBytes(const char* hexString, uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    sscanf(hexString + 2*i, "%2hhx", &bytes[i]);
  }
}

/**
 * @brief Tạo challenge ngẫu nhiên
 */
void generateChallenge(uint8_t* challenge, size_t length) {
  for (size_t i = 0; i < length; i++) {
    challenge[i] = random(0, 256);
  }
}

/**
 * @brief Tính HMAC-SHA256
 */
bool computeHMAC(const uint8_t* key, size_t keyLen,
                 const uint8_t* data, size_t dataLen,
                 uint8_t* output) {
  
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  
  int ret = mbedtls_md_hmac(md_info, key, keyLen, data, dataLen, output);
  
  return (ret == 0);
}

/**
 * @brief In bytes dưới dạng hex
 */
void printHex(const char* label, const uint8_t* data, size_t length) {
  Serial.print(label);
  for (size_t i = 0; i < length; i++) {
    if (data[i] < 16) Serial.print("0");
    Serial.print(data[i], HEX);
  }
  Serial.println();
}

/**
 * @brief HKDF-SHA256 key derivation function
 */
int hkdf_sha256(const unsigned char* salt, size_t salt_len,
                const unsigned char* ikm, size_t ikm_len,
                const unsigned char* info, size_t info_len,
                unsigned char* okm, size_t okm_len) {
  unsigned char prk[32];
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  
  if (salt == NULL || salt_len == 0) {
    unsigned char zero_salt[32] = {0};
    mbedtls_md_hmac(md_info, zero_salt, 32, ikm, ikm_len, prk);
  } else {
    mbedtls_md_hmac(md_info, salt, salt_len, ikm, ikm_len, prk);
  }
  
  unsigned char t[32];
  unsigned char counter = 1;
  size_t t_len = 0;
  size_t offset = 0;
  
  while (offset < okm_len) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md_info, 1);
    mbedtls_md_hmac_starts(&ctx, prk, 32);
    
    if (t_len > 0) {
      mbedtls_md_hmac_update(&ctx, t, t_len);
    }
    mbedtls_md_hmac_update(&ctx, info, info_len);
    mbedtls_md_hmac_update(&ctx, &counter, 1);
    mbedtls_md_hmac_finish(&ctx, t);
    mbedtls_md_free(&ctx);
    
    t_len = 32;
    size_t copy_len = (okm_len - offset < 32) ? (okm_len - offset) : 32;
    memcpy(okm + offset, t, copy_len);
    offset += copy_len;
    counter++;
  }
  
  return 0;
}

/* ========================================================================
 * NVS Storage Functions
 * ======================================================================== */

/**
 * @brief Kiểm tra key đã lưu trong NVS
 */
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

/**
 * @brief Lưu key vào NVS
 */
void saveKeyToMemory(String key) {
  Serial.println("Saving key to memory...");
  
  preferences.begin("ble-keys", false);
  preferences.putString("bleKey", key);
  preferences.end();
  
  Serial.println("✓ Key saved to memory!\n");
  
  bleKeyHex = key;
  hasKey = true;
}

/**
 * @brief Xóa key khỏi NVS
 */
void clearStoredKey() {
  Serial.println("Clearing stored key...");
  
  preferences.begin("ble-keys", false);
  preferences.remove("bleKey");
  preferences.end();
  
  bleKeyHex = "";
  hasKey = false;
  
  Serial.println("✓ Key cleared!\n");
}

/* ========================================================================
 * WiFi & Server Functions
 * ======================================================================== */

/**
 * @brief Kết nối WiFi
 */
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected!");
    Serial.println("IP: " + WiFi.localIP().toString() + "\n");
  } else {
    Serial.println("\n❌ WiFi connection failed!\n");
  }
}

void discoverServer() {
  Serial.println("Discovering server via mDNS...");

  if (!MDNS.begin("esp32-anchor")) {
    Serial.println("mDNS init failed, using fallback: " + serverBaseUrl);
    return;
  }

  int n = MDNS.queryService("http", "tcp");
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      if (MDNS.port(i) == 8000) {
        serverBaseUrl = "http://" + MDNS.address(i).toString() + ":8000";
        Serial.println("✓ Server found via mDNS: " + serverBaseUrl);
        return;
      }
    }
  }
  Serial.println("Server not found via mDNS, using fallback: " + serverBaseUrl);
}

/**
 * @brief Lấy key từ server qua ECDH + AES-GCM
 */
String fetchKeyFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected!");
    return "";
  }
  
  Serial.println("Fetching key from server...");
  Serial.println("Server: " + String(serverBaseUrl));
  Serial.println("Local IP: " + WiFi.localIP().toString());
  
  // Generate ephemeral EC key pair
  mbedtls_pk_context client_key;
  mbedtls_pk_init(&client_key);
  
  int ret = mbedtls_pk_setup(&client_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) {
    Serial.printf("❌ Key setup failed: %d\n", ret);
    return "";
  }
  
  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                             mbedtls_pk_ec(client_key),
                             mbedtls_ctr_drbg_random,
                             &ctr_drbg);
  if (ret != 0) {
    Serial.printf("❌ Key generation failed: %d\n", ret);
    mbedtls_pk_free(&client_key);
    return "";
  }
  
  // Export public key
  unsigned char client_pub_der[200];
  int pub_len = mbedtls_pk_write_pubkey_der(&client_key, client_pub_der, sizeof(client_pub_der));
  if (pub_len < 0) {
    Serial.printf("❌ Export failed: %d\n", pub_len);
    mbedtls_pk_free(&client_key);
    return "";
  }
  
  unsigned char* pub_start = client_pub_der + sizeof(client_pub_der) - pub_len;
  
  // Base64 encode
  size_t olen;
  unsigned char client_pub_b64[300];
  ret = mbedtls_base64_encode(client_pub_b64, sizeof(client_pub_b64), &olen, pub_start, pub_len);
  if (ret != 0) {
    Serial.printf("❌ Base64 failed: %d\n", ret);
    mbedtls_pk_free(&client_key);
    return "";
  }
  client_pub_b64[olen] = '\0';
  
  // Send request
  HTTPClient http;
  String url = String(serverBaseUrl) + "/secure-check-pairing";
  
  Serial.println("Connecting to: " + url);
  http.begin(url);
  http.setTimeout(10000); // 10 second timeout
  http.addHeader("Content-Type", "application/json");
  
  StaticJsonDocument<512> requestDoc;
  requestDoc["vehicle_id"] = vehicleId;
  requestDoc["client_public_key_b64"] = String((char*)client_pub_b64);
  
  String requestBody;
  serializeJson(requestDoc, requestBody);
  
  Serial.println("Sending secure request...");
  int httpCode = http.POST(requestBody);
  
  String key = "";
  
  if (httpCode == 200) {
    String response = http.getString();
    
    StaticJsonDocument<1024> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, response);
    
    if (!error) {
      String server_pub_b64 = responseDoc["server_public_key_b64"];
      String encrypted_data_b64 = responseDoc["encrypted_data_b64"];
      String nonce_b64 = responseDoc["nonce_b64"];
      
      key = decryptResponse(&client_key, server_pub_b64, encrypted_data_b64, nonce_b64);
    } else {
      Serial.println("❌ JSON parse failed");
    }
  } else {
    Serial.printf("❌ HTTP failed: %d\n", httpCode);
    
    // Detailed error messages
    if (httpCode == -1) {
      Serial.println("HTTPC_ERROR_CONNECTION_FAILED");
      Serial.println("Giai phap:");
      Serial.println("1. Kiem tra WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
      Serial.println("2. Kiem tra server dang chay tai: " + String(serverBaseUrl));
      Serial.println("3. Kiem tra ESP32 va server cung mang");
      Serial.println("4. Ping server tu may tinh: ping 10.0.7.178");
    } else if (httpCode == -11) {
      Serial.println("HTTPC_ERROR_READ_TIMEOUT");
      Serial.println("Server khong phan hoi - tang timeout hoac kiem tra server");
    } else if (httpCode > 0) {
      Serial.printf("HTTP Response Code: %d\n", httpCode);
      if (httpCode == 404) Serial.println("Endpoint '/secure-check-pairing' khong ton tai");
      else if (httpCode == 500) Serial.println("Server internal error");
    }
  }
  
  http.end();
  mbedtls_pk_free(&client_key);
  
  return key;
}

/**
 * @brief Giải mã response từ server
 */
String decryptResponse(mbedtls_pk_context* client_key, String server_pub_b64, 
                       String encrypted_data_b64, String nonce_b64) {
  
  // Decode server public key
  unsigned char server_pub_der[200];
  size_t server_pub_len;
  int ret = mbedtls_base64_decode(server_pub_der, sizeof(server_pub_der), &server_pub_len,
                                   (const unsigned char*)server_pub_b64.c_str(),
                                   server_pub_b64.length());
  if (ret != 0) {
    Serial.println("❌ Decode server key failed");
    return "";
  }
  
  // Parse server public key
  mbedtls_pk_context server_key;
  mbedtls_pk_init(&server_key);
  ret = mbedtls_pk_parse_public_key(&server_key, server_pub_der, server_pub_len);
  if (ret != 0) {
    Serial.println("❌ Parse server key failed");
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // ECDH - Compatible with mbedTLS 3.x
  mbedtls_ecdh_context ecdh;
  mbedtls_ecdh_init(&ecdh);
  
  // Setup ECDH context
  ret = mbedtls_ecdh_setup(&ecdh, MBEDTLS_ECP_DP_SECP256R1);
  if (ret != 0) {
    Serial.println("❌ ECDH setup failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Get our private key
  mbedtls_ecp_keypair *client_ec = (mbedtls_ecp_keypair*)mbedtls_pk_ec(*client_key);
  ret = mbedtls_ecdh_get_params(&ecdh, client_ec, MBEDTLS_ECDH_OURS);
  if (ret != 0) {
    Serial.println("❌ ECDH get client params failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Get peer public key
  mbedtls_ecp_keypair *server_ec = (mbedtls_ecp_keypair*)mbedtls_pk_ec(server_key);
  ret = mbedtls_ecdh_get_params(&ecdh, server_ec, MBEDTLS_ECDH_THEIRS);
  if (ret != 0) {
    Serial.println("❌ ECDH get server params failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Compute shared secret
  unsigned char shared_secret[32];
  size_t olen;
  ret = mbedtls_ecdh_calc_secret(&ecdh, &olen, shared_secret, sizeof(shared_secret),
                                  mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0 || olen != 32) {
    Serial.println("❌ Shared secret failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Derive KEK
  unsigned char kek[16];
  const unsigned char info[] = "secure-check-kek";
  ret = hkdf_sha256(NULL, 0, shared_secret, 32, info, strlen((char*)info), kek, 16);
  if (ret != 0) {
    Serial.println("❌ HKDF failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Decode encrypted data
  unsigned char encrypted_data[200];
  unsigned char nonce[12];
  size_t encrypted_len, nonce_len;
  
  ret = mbedtls_base64_decode(encrypted_data, sizeof(encrypted_data), &encrypted_len,
                               (const unsigned char*)encrypted_data_b64.c_str(),
                               encrypted_data_b64.length());
  if (ret != 0) {
    Serial.println("❌ Decode data failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  ret = mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len,
                               (const unsigned char*)nonce_b64.c_str(),
                               nonce_b64.length());
  if (ret != 0 || nonce_len != 12) {
    Serial.println("❌ Decode nonce failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Decrypt
  unsigned char decrypted[200];
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  
  ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128);
  if (ret != 0) {
    Serial.println("❌ GCM setkey failed");
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  size_t ciphertext_len = encrypted_len - 16;
  unsigned char tag[16];
  memcpy(tag, encrypted_data + ciphertext_len, 16);
  
  ret = mbedtls_gcm_auth_decrypt(&gcm, ciphertext_len, nonce, 12, NULL, 0,
                                  tag, 16, encrypted_data, decrypted);
  
  if (ret != 0) {
    Serial.println("❌ Decryption failed");
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  decrypted[ciphertext_len] = '\0';
  
  // Parse JSON
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, (char*)decrypted);
  
  if (error) {
    Serial.println("❌ Parse failed");
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  bool isPaired = doc["paired"];
  String key = "";
  
  if (isPaired) {
    key = doc["pairing_key"].as<String>();
    
    Serial.println("\n--- Server Response ---");
    Serial.println("Status: PAIRED");
    Serial.println("Pairing ID: " + doc["pairing_id"].as<String>());
    Serial.println("Paired At: " + doc["paired_at"].as<String>());
    Serial.println("Pairing Key: " + key);
    Serial.println("-----------------------\n");
  } else {
    Serial.println("\n--- Server Response ---");
    Serial.println("Status: NOT PAIRED");
    Serial.println("Message: " + doc["message"].as<String>());
    Serial.println("-----------------------\n");
  }
  
  // Cleanup
  mbedtls_gcm_free(&gcm);
  mbedtls_ecdh_free(&ecdh);
  mbedtls_pk_free(&server_key);
  
  return key;
}

/* ========================================================================
 * Các Class Callback BLE
 * ======================================================================== */

/**
 vb    * @brief Xử lý callback cho Auth Response từ Tag
 */
class AuthCharacteristicCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    Serial.println("\n📥 Nhan duoc write tren Auth characteristic!");
    
    // Lấy raw binary data - getValue() trả về Arduino String
    String arduinoValue = pCharacteristic->getValue();
    std::string value(arduinoValue.c_str(), arduinoValue.length());
    
    Serial.println("Do dai chunk nhan duoc: " + String(value.length()));
    
    // Debug: in ra vài byte đầu tiên
    if (value.length() > 0) {
      Serial.print("Cac byte dau tien: ");
      for (size_t i = 0; i < min(8, (int)value.length()); i++) {
        if ((uint8_t)value[i] < 16) Serial.print("0");
        Serial.print((uint8_t)value[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }
    
    // Buffer dữ liệu - copy binary data vào buffer
    for (size_t i = 0; i < value.length() && responseBufferLen < 32; i++) {
      responseBuffer[responseBufferLen++] = (uint8_t)value[i];
    }
    lastWriteTime = millis();
    
    Serial.println("Tong do dai buffer: " + String(responseBufferLen));
    
    // Chờ nhận đủ 32-byte response
    if (responseBufferLen >= 32) {
      Serial.println("\n✓ Nhan du response!");
      
      uint8_t receivedResponse[32];
      memcpy(receivedResponse, responseBuffer, 32);
      responseBufferLen = 0; // Xóa buffer
      
      printHex("Response nhan duoc: ", receivedResponse, 32);
      
      // Tính expected response: HMAC(key, challenge)
      uint8_t expectedResponse[32];
      bool success = computeHMAC(pairingKey, 16, currentChallenge, 16, expectedResponse);
      
      if (!success) {
        Serial.println("❌ Tinh HMAC that bai");
        responseBufferLen = 0;
        BLEDevice::getServer()->disconnect(BLEDevice::getServer()->getConnId());
        return;
      }
      
      printHex("Response mong doi: ", expectedResponse, 32);
      
      // So sánh responses
      bool match = (memcmp(receivedResponse, expectedResponse, 32) == 0);
      
      Serial.println();
      if (match) {
        authenticated = true;
        
        Serial.println("==================================================");
        Serial.println("✅ XAC THUC THANH CONG!");
        Serial.println("==================================================");
        Serial.println("Tag duoc uy quyen truy cap xe");
        Serial.println("==================================================\n");
        
        // Gửi xác nhận thành công
        pAuthCharacteristic->setValue("AUTH_OK");
        pAuthCharacteristic->notify();
        
      } else {
        authenticated = false;
        
        Serial.println("==================================================");
        Serial.println("❌ XAC THUC THAT BAI!");
        Serial.println("==================================================");
        Serial.println("Sai key - Dang ngat ket noi...");
        Serial.println("==================================================\n");
        
        // Gửi thông báo thất bại và ngắt kết nối
        pAuthCharacteristic->setValue("AUTH_FAIL");
        pAuthCharacteristic->notify();
        
        delay(100);
        BLEDevice::getServer()->disconnect(BLEDevice::getServer()->getConnId());
      }
      
      responseBufferLen = 0; // Xóa buffer sau khi xử lý
    }
  }
};

/**
 * @class CharacteristicCallbacks
 * @brief Bộ xử lý callback cho các thao tác ghi BLE characteristic
 * 
 * @details Xử lý tin nhắn nhận được từ thiết bị Tag qua BLE.
 *          Xử lý kết quả xác minh khoảng cách và cảnh báo bảo mật.
 */
class CharacteristicCallbacks : public BLECharacteristicCallbacks {
  /**
   * @brief Được gọi khi Tag ghi dữ liệu vào characteristic
   * 
   * @param[in] pCharacteristic Con trỏ đến characteristic đang được ghi
   * 
   * @return Không có
   * 
   * @note Xử lý:
   * - "VERIFIED:" - Khoảng cách đã xác thực, an toàn để mở khóa
   * - "ALERT:RELAY_ATTACK" - Phát hiện mối đe dọa bảo mật
   */
  void onWrite(BLECharacteristic *pCharacteristic) {
    if (pCharacteristic == nullptr) {
      return; /* MISRA C: Kiểm tra con trỏ null */
    }
    
    String value = pCharacteristic->getValue();
    if (value.length() > 0U) {
      Serial.print("Nhan tu Tag: ");
      Serial.println(value.c_str());
      
      /* Kiểm tra xác nhận khoảng cách đã xác minh */
      if (value.startsWith("VERIFIED:")) {
        Serial.println("Khoang cach da xac minh boi Tag - An toan de mo khoa");
        
        /* In thông báo mở khóa (TEST MODE - không gửi CAN) */
        if (!carUnlocked) {
          Serial.println("\n>>> CHO PHEP MO KHOA XE <<<");
          Serial.println(">>> XE DUOC MO KHOA (TEST MODE) <<<\n");
          carUnlocked = true;
          Commented out CAN command for testing without car model:
          if (pCanControl != nullptr) {
            pCanControl->unlockCar();
          }
        }
      }
      /* Kiểm tra cảnh báo khoảng cách (> 3m) */
      else if (value.startsWith("WARNING:")) {
        Serial.println("CANH BAO: Tag vuot nguong 3m - Dang khoa xe");
        Serial.println("(UWB van tiep tuc do khoang cach)");
        
        /* Khóa xe nhưng KHÔNG tắt UWB */
        if (carUnlocked) {
          Serial.println("\n>>> DANG KHOA XE <<<");
          Serial.println(">>> XE DA DUOC KHOA (TEST MODE) <<<\n");
          carUnlocked = false;
          // Commented out CAN command for testing without car model:
          // if (pCanControl != nullptr) {
          //   pCanControl->lockCar();
          // }
        }
      }
      /* Kiểm tra yêu cầu khóa xe (KHÔNG tắt UWB) */
      else if (value.startsWith("LOCK_CAR")) {
        Serial.println("Tag yeu cau khoa xe (khoang cach > 3m)");
        
        /* Khóa xe nhưng KHÔNG tắt UWB - Tag vẫn đang trong vùng 20m */
        if (carUnlocked) {
          Serial.println("\n>>> DANG KHOA XE <<<");
          Serial.println(">>> XE DA DUOC KHOA (TEST MODE) <<<");
          Serial.println(">>> UWB VAN HOAT DONG - TIEP TUC DO <<<\n");
          carUnlocked = false;
          // Commented out CAN command for testing without car model:
          // if (pCanControl != nullptr) {
          //   pCanControl->lockCar();
          // }
        }
      }
      /* Kiểm tra yêu cầu TẮT UWB (Tag ra khỏi 20m) */
      else if (value.startsWith("UWB_STOP")) {
        Serial.println("\n>>> NHAN LENH UWB_STOP TU TAG <<<");
        Serial.println("Tag da ra khoi 20m - Dang tat UWB...");
        
        /* Khóa xe nếu chưa khóa */
        if (carUnlocked) {
          Serial.println(">>> DANG KHOA XE <<<");
          Serial.println(">>> XE DA DUOC KHOA (TEST MODE) <<<");
          carUnlocked = false;
        }
        
        /* Tắt UWB để tiết kiệm năng lượng */
        deinitUWB();
        
        /* Reset cờ và thời gian để cho phép tự động bật lại UWB khi Tag quay về */
        uwbActivationRequested = false;
        connectionTime = millis(); /* Khởi động lại timer để đợi Tag xác minh RSSI */
        
        Serial.println("UWB da tat - Se kich hoat lai khi Tag quay ve trong 20m\n");
      }
      /* Kiểm tra cảnh báo bảo mật */
      else if (value.startsWith("ALERT:RELAY_ATTACK")) {
        Serial.println("CANH BAO BAO MAT: Phat hien tan cong relay!");
        Serial.println("Xe giu nguyen khoa");
        /* TODO: Ghi lại sự kiện bảo mật, có thể kích hoạt báo động */
      }
      /* Kiểm tra yêu cầu khởi tạo UWB từ Tag */
      else if (value.startsWith("TAG_UWB_READY")) {
        Serial.println("\n>>> NHAN TIN HIEU TAG_UWB_READY <<<");
        Serial.println("Tag da san sang - Khoi tao UWB ngay lap tuc...");
        
        if (!uwbInitialized) {
          initUWB();
          
          /* Gửi xác nhận UWB đã sẵn sàng */
          if (pCharacteristic != nullptr) {
            pCharacteristic->setValue("UWB_ACTIVE");
            pCharacteristic->notify();
            Serial.println(">>> Da gui notification UWB_ACTIVE den Tag!");
          }
        } else {
          Serial.println("UWB da duoc khoi tao truoc do");
        }
      }
      else {
        /* Tin nhắn không xác định - bỏ qua */
      }
    }
  }
};

/**
 * @class MyServerCallbacks
 * @brief Bộ xử lý callback cho các sự kiện kết nối BLE server
 * 
 * @details Quản lý kết nối/ngắt kết nối Tag và thời gian kích hoạt UWB
 */
class MyServerCallbacks : public BLEServerCallbacks {
  /**
   * @brief Được gọi khi Tag kết nối qua BLE
   * 
   * @param[in] pServer Con trỏ đến BLE server instance
   * 
   * @return Không có
   * 
   * @note Ghi lại thời gian kết nối, gửi challenge để xác thực Tag
   */
  void onConnect(BLEServer* pServer) {
    (void)pServer; /* Tham số không sử dụng */
    
    deviceConnected = true;
    authenticated = false; // Reset authentication status
    responseBufferLen = 0; // Reset response buffer
    connectionTime = millis();
    uwbActivationRequested = false;
    
    Serial.println("\n==================================================");
    Serial.println("📱 Tag da ket noi!");
    Serial.println("==================================================");
    
    // Đợi Tag subscribe notifications
    Serial.println("⏳ Cho 2 giay de Tag subscribe notifications...");
    delay(2000);
    
    // Tạo và gửi challenge
    generateChallenge(currentChallenge, 16);
    
    Serial.println("\n🔐 Bat dau Challenge-Response Authentication:");
    printHex("Challenge: ", currentChallenge, 16);
    
    // Gửi challenge đến Tag
    pChallengeCharacteristic->setValue(currentChallenge, 16);
    pChallengeCharacteristic->notify();
    
    Serial.println("✓ Da gui challenge den Tag");
    Serial.println("⏳ Dang cho response tu Tag...");
    Serial.println();
  };

  /**
   * @brief Được gọi khi Tag ngắt kết nối BLE
   * 
   * @param[in] pServer Con trỏ đến BLE server instance
   * 
   * @return Không có
   * 
   * @note Reset trạng thái kích hoạt UWB
   */
  void onDisconnect(BLEServer* pServer) {
    (void)pServer; /* Tham số không sử dụng */
    
    deviceConnected = false;
    authenticated = false; // Reset authentication
    responseBufferLen = 0; // Reset response buffer
    uwbActivationRequested = false;
    
    Serial.println("\n========================================");
    Serial.println("BLE: Tag da ngat ket noi!");
    
    /* In thông báo khóa xe khi Tag ngắt kết nối (TEST MODE) */
    if (carUnlocked) {
      Serial.println(">>> TAG DA RA KHOI KHOANG CACH <<<");
      Serial.println(">>> DANG KHOA XE <<<");
      Serial.println(">>> XE DA DUOC KHOA (TEST MODE) <<<");
      carUnlocked = false;
      // Commented out CAN command for testing without car model:
      // if (pCanControl != nullptr) {
      //   pCanControl->lockCar();
      // }
    }
    
    /* Tắt UWB khi mất kết nối để tiết kiệm năng lượng */
    deinitUWB();
    
    Serial.println("Dang cho ket noi lai...");
    Serial.println("Anchor dang phat lai BLE...");
    Serial.println("========================================\n");
    
    /* Restart advertising ngay lập tức */
    delay(100); /* Để BLE stack ổn định */
    BLEDevice::startAdvertising();
    Serial.println("BLE: Da bat dau phat lai - San sang cho ket noi moi");
  }
};

/* ========================================================================
 * BLE Server Initialization
 * ======================================================================== */

/**
 * @brief Khởi tạo BLE Server với key đã có
 */
void startBLE() {
  Serial.println("\n🔐 Starting BLE Server...");
  
  // Convert hex key to bytes
  hexStringToBytes(bleKeyHex.c_str(), pairingKey, 16);
  printHex("Pairing Key: ", pairingKey, 16);
  
  BLEDevice::init(DEVICE_NAME);
  
  BLEServer* pServer = BLEDevice::createServer();
  if (pServer != nullptr) {
    pServer->setCallbacks(new MyServerCallbacks());
  }
  
  BLEService* pService = pServer->createService(SERVICE_UUID);
  
  pChallengeCharacteristic = pService->createCharacteristic(
    CHALLENGE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pChallengeCharacteristic->addDescriptor(new BLE2902());
  
  pAuthCharacteristic = pService->createCharacteristic(
    AUTH_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  pAuthCharacteristic->setCallbacks(new AuthCharacteristicCallbacks());
  pAuthCharacteristic->addDescriptor(new BLE2902());
  
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  
  if (pCharacteristic != nullptr) {
    pCharacteristic->setCallbacks(new CharacteristicCallbacks());
    pCharacteristic->setValue("ANCHOR_READY");
  }
  
  pService->start();
  
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  if (pAdvertising != nullptr) {
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
  }
  BLEDevice::startAdvertising();
  
  Serial.println("✓ BLE Server Started");
  Serial.println("📡 Advertising as: " + String(DEVICE_NAME));
  Serial.println("⏳ Waiting for phone connection...\n");
}

/**
 * @brief Main flow: Check stored key or fetch from server, then start BLE
 */
void executeMainFlow() {
  Serial.println("\n=== MAIN FLOW START ===\n");
  
  // STEP 1: Check stored key
  Serial.println("STEP 1: Checking for stored key in Preferences...");
  checkStoredKey();
  
  if (hasKey) {
    // Key exists - Start BLE
    Serial.println("✓ Key found in memory!");
    Serial.println("Key: " + bleKeyHex);
    Serial.println("\nSTEP 2: Starting BLE with stored key...");
    startBLE();
    
  } else {
    // No key - Fetch from server
    Serial.println("✗ No key in memory");
    Serial.println("\nSTEP 2: Fetching key from server...");
    
    String serverKey = fetchKeyFromServer();
    
    if (serverKey.length() > 0) {
      Serial.println("✓ Key received from server!");
      Serial.println("Key: " + serverKey);
      
      // Save to memory
      saveKeyToMemory(serverKey);
      
      Serial.println("\nSTEP 3: Starting BLE with server key...");
      startBLE();
      
    } else {
      Serial.println("\n❌ Failed to get key from server");
      Serial.println("Vehicle not paired - Cannot start BLE");
      Serial.println("\nPlease pair the vehicle first, then restart.\n");
    }
  }
  
  Serial.println("=== MAIN FLOW END ===\n");
}

/* ========================================================================
 * Các Hàm Khởi Tạo Và Đo Khoảng Cách UWB
 * ======================================================================== */

/**
 * @brief Khởi tạo module UWB DW3000 để đo khoảng cách làm Responder
 * 
 * @param Không có
 * 
 * @return Không có
 * 
 * @details Trình tự khởi tạo:
 * 1. Khởi tạo giao diện SPI
 * 2. Reset chip DW3000
 * 3. Đợi trạng thái IDLE
 * 4. Cấu hình tham số RF
 * 5. Thiết lập trễ ăng-ten
 * 6. Bật LNA/PA
 * 
 * @note Chỉ khởi tạo một lần (kiểm tra cờ uwbInitialized)
 * @note Dừng lại nếu có lỗi nghiêm trọng (IDLE, INIT, CONFIG thất bại)
 */
void initUWB(void) {
  /* MISRA C: Kiểm tra cờ khởi tạo */
  if (uwbInitialized) {
    return;
  }
  
  Serial.println("Dang khoi tao module UWB (Anchor/Responder)...");
  
  /* Khởi tạo SPI và reset DW3000 */
  spiBegin(PIN_IRQ, PIN_RST);
  spiSelect(PIN_SS);
  
  delay(2); /* Thời gian cần thiết để DW3000 khởi động (tối thiểu 2ms) */
  
  /* MISRA C: Kiểm tra trạng thái IDLE với timeout */
  while (!dwt_checkidlerc()) {
    Serial.println("LOI IDLE");
    while (1) { /* Dừng lại khi gặp lỗi nghiêm trọng */ }
  }
  
  /* Khởi tạo driver DW3000 */
  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) {
    Serial.println("LOI KHOI TAO");
    while (1) { /* Dừng lại khi gặp lỗi nghiêm trọng */ }
  }
  
  /* Bật LED để hiển thị trạng thái */
  dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
  
  /* Cấu hình DW3000 với kênh, tốc độ dữ liệu, preamble, v.v. */
  if (dwt_configure(&config) != 0) {
    Serial.println("LOI CAU HINH");
    while (1) { /* Dừng lại khi gặp lỗi nghiêm trọng */ }
  }
  
  /* Cấu hình công suất RF và thiết lập PG */
  dwt_configuretxrf(&txconfig_options);
  
  /* Áp dụng giá trị hiệu chuẩn trễ ăng-ten */
  dwt_setrxantennadelay(RX_ANT_DLY);
  dwt_settxantennadelay(TX_ANT_DLY);
  
  /* Bật LNA (Bộ khuếch đại nhiễu thấp) và PA (Bộ khuếch đại công suất) */
  dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
  
  uwbInitialized = true;
  Serial.println("UWB: Da khoi tao va san sang do khoang cach!");
}

/**
 * @brief Tắt module UWB để tiết kiệm năng lượng
 * 
 * @param Không có
 * 
 * @return Không có
 * 
 * @note Được gọi khi Tag ra khỏi ngưỡng hoặc ngắt kết nối BLE
 */
void deinitUWB(void) {
  if (uwbInitialized) {
    Serial.println("\n>>> TAT UWB DE TIET KIEM NANG LUONG <<<");
    
    /* Reset chip DW3000 */
    dwt_softreset();
    delay(2);
    
    /* Reset cờ */
    uwbInitialized = false;
    
    Serial.println("UWB: Da tat\n");
  }
}

/**
 * @brief Thực hiện một chu kỳ phản hồi UWB làm Responder (Anchor)
 * 
 * @param Không có
 * 
 * @return Không có
 * 
 * @details Trình tự phản hồi Two-Way Ranging (TWR):
 * 1. Bật RX và chờ tin nhắn POLL từ Initiator
 * 2. Lấy timestamp nhận poll
 * 3. Tính thời gian truyền phản hồi có trễ
 * 4. Nhúng các timestamp vào tin nhắn RESPONSE
 * 5. Gửi tin nhắn RESPONSE tại thời gian đã lên lịch
 * 
 * @note Gọi liên tục để uy trì sẵn sàng đo khoảng cách
 * @note Các timestamp cho phép Initiator tính toán khoảng cách
 */
void uwbResponderLoop(void) {
  /* Kích hoạt nhận ngay lập tức */
  (void)dwt_rxenable(DWT_START_RX_IMMEDIATE);
  
  /* Poll chờ nhận khung hoặc lỗi */
  while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) & 
           (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR))) {
    /* Đợi thay đổi trạng thái */
  }
  
  /* Kiểm tra xem đã nhận khung thành công chưa */
  if ((status_reg & SYS_STATUS_RXFCG_BIT_MASK) != 0U) {
    uint32_t frame_len;
    
    /* Xóa sự kiện nhận khung tốt */
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    
    /* Đọc độ dài khung từ thanh ghi thông tin RX */
    frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
    /* MISRA C: Kiểm tra giới hạn trước khi đọc buffer */
    if (frame_len <= sizeof(rx_buffer)) {
      dwt_readrxdata(rx_buffer, frame_len, 0U);
      
      /* Xác thực khung bằng cách so sánh header (bỏ qua số thứ tự) */
      rx_buffer[ALL_MSG_SN_IDX] = 0U;
      /* Xác thực header tin nhắn poll */
      if (memcmp(rx_buffer, rx_poll_msg, ALL_MSG_COMMON_LEN) == 0) {
        uint32_t resp_tx_time;
        int ret;
        
        /* Lấy timestamp nhận poll */
        poll_rx_ts = get_rx_timestamp_u64();
        
        /* Tính thời gian truyền phản hồi có trễ */
        resp_tx_time = (uint32_t)((poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8);
        (void)dwt_setdelayedtrxtime(resp_tx_time);
        
        /* Tính timestamp TX phản hồi (thời gian lập trình + trễ ăng-ten) */
        resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;
        
        /* Nhúng các timestamp vào payload tin nhắn phản hồi */
        resp_msg_set_ts(&tx_resp_msg[RESP_MSG_POLL_RX_TS_IDX], poll_rx_ts);
        resp_msg_set_ts(&tx_resp_msg[RESP_MSG_RESP_TX_TS_IDX], resp_tx_ts);
        
        /* Ghi và gửi tin nhắn phản hồi */
        tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
        dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0U); /* Offset không trong buffer TX */
        dwt_writetxfctrl(sizeof(tx_resp_msg), 0U, 1);           /* Offset không, bit ranging được thiết lập */
        ret = dwt_starttx(DWT_START_TX_DELAYED);
        
        /* Kiểm tra xem TX có trễ bắt đầu thành công không */
        if (ret == DWT_SUCCESS) {
          /* Poll chờ sự kiện gửi khung TX */
          while ((dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS_BIT_MASK) == 0U) {
            /* Đợi TX hoàn thành */
          }
          
          /* Xóa sự kiện TXFRS */
          dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
          
          /* Tăng số thứ tự (0-255, quay vòng) */
          frame_seq_nb++;
          
          Serial.println("Phan hoi da gui");
        } else {
          /* TX có trễ thất bại - không đáp ứng được điều kiện thời gian */
        }
      }
    }
  } else {
    /* Xóa các sự kiện lỗi RX */
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
  }
}

/* ========================================================================
 * Thiết Lập Arduino Và Vòng Lặp Chính
 * ======================================================================== */

/**
 * @brief Hàm setup Arduino - khởi tạo BLE server
 * 
 * @param Không có
 * 
 * @return Không có
 * 
 * @details Trình tự khởi tạo:
 * 1. Khởi tạo giao tiếp serial
 * 2. Khởi tạo thiết bị BLE
 * 3. Tạo BLE server với callback
 * 4. Tạo service và characteristic
 * 5. Bắt đầu phát
 * 
 * @note Khởi tạo UWB bị hoãn cho đến khi Tag kết nối
 */
void setup(void) {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==================================================");
  Serial.println("🚗 Smart Car - Complete Anchor System");
  Serial.println("==================================================");
  Serial.println("Vehicle ID: " + String(vehicleId));
  Serial.println("==================================================\n");
  
  /* Initialize mbedTLS */
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  
  const char* pers = "anchor_secure";
  int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)pers, strlen(pers));
  if (ret != 0) {
    Serial.printf("❌ mbedtls init failed: %d\n", ret);
    return;
  }
  
  /* Khởi tạo CAN Bus System */
  Serial.println("--- Khoi tao CAN Bus ---");
  pCanControl = new CANCommands(&mcp2515);
  if (pCanControl != nullptr) {
    if (pCanControl->initialize(CAN_CS, CAN_100KBPS, MCP_CLOCK)) {
      Serial.println("CAN: Da khoi tao thanh cong!");
    } else {
      Serial.println("CAN: CANH BAO - Khong the khoi tao!");
      Serial.println("Tiep tuc khoi dong voi BLE+UWB...");
    }
  }
  Serial.println("--- Ket thuc khoi tao CAN ---\n");
  
  /* Connect to WiFi */
  connectWiFi();

  /* Tự động tìm server qua mDNS */
  discoverServer();
  Serial.println("Server URL: " + serverBaseUrl);
  
  /* Execute main flow: Check key or fetch from server, then start BLE */
  executeMainFlow();
}

/**
 * @brief Vòng lặp chính Arduino - quản lý kết nối BLE và đo khoảng cách UWB
 * 
 * @param Không có
 * 
 * @return Không có (chạy liên tục)
 * 
 * @details Máy trạng thái:
 * 1. Phát hiện kết nối BLE mới
 * 2. Đợi UWB_ACTIVATION_DELAY_MS sau khi kết nối
 * 3. Khởi tạo module UWB
 * 4. Xử lý các yêu cầu đo khoảng cách UWB
 * 5. Xử lý ngắt kết nối và khởi động lại phát
 * 6. Xử lý serial commands (CLEAR, SHOW, STATUS)
 * 
 * @note Vòng lặp chạy ở ~100Hz (trễ 10ms)
 */
void loop(void) {
  /* Handle serial commands */
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd == "CLEAR" || cmd == "clear") {
      clearStoredKey();
      Serial.println("✓ Key cleared! Please restart ESP32.\n");
      return;
    }
    
    if (cmd == "SHOW" || cmd == "show") {
      checkStoredKey();
      if (hasKey) {
        Serial.println("Stored key: " + bleKeyHex);
      } else {
        Serial.println("No key stored");
      }
      return;
    }
    
    if (cmd == "STATUS" || cmd == "status") {
      Serial.println("\n--- System Status ---");
      Serial.println("Has Key: " + String(hasKey ? "Yes" : "No"));
      Serial.println("BLE Connected: " + String(deviceConnected ? "Yes" : "No"));
      Serial.println("Authenticated: " + String(authenticated ? "Yes" : "No"));
      Serial.println("UWB Initialized: " + String(uwbInitialized ? "Yes" : "No"));
      Serial.println("--------------------\n");
      return;
    }
  }
  
  /* Xử lý kết nối BLE mới */
  if (deviceConnected && (!prevConnected)) {
    prevConnected = true;
  }
  
  /* Kích hoạt UWB có trễ - CHỈ sau khi Tag đã authenticated và xác minh RSSI */
  if (deviceConnected && authenticated && (!uwbActivationRequested) && 
      ((millis() - connectionTime) > UWB_ACTIVATION_DELAY_MS)) {
    uwbActivationRequested = true;
    
    /* Đánh thức UWB - Tag đã được xác thực và xác minh RSSI */
    if (!uwbInitialized) {
      Serial.println("\n==================================================");
      Serial.println("✅ Tag da duoc xac thuc");
      Serial.println("==================================================");
      Serial.println("Hoan thanh giai doan xac minh RSSI cua Tag");
      Serial.println("Dang kich hoat UWB de xac thuc khoang cach...");
      initUWB();
      
      /* Thông báo cho Tag rằng UWB đang hoạt động */
      if (pCharacteristic != nullptr) {
        Serial.println(">>> Dang gui notification UWB_ACTIVE den Tag...");
        pCharacteristic->setValue("UWB_ACTIVE");
        pCharacteristic->notify();
        Serial.println(">>> Da gui notification UWB_ACTIVE!");
        Serial.println("==================================================\n");
      } else {
        Serial.println("!!! LOI: pCharacteristic la nullptr - khong gui duoc notification!");
      }
    }
  }
  
  /* Xử lý ngắt kết nối BLE */
  if ((!deviceConnected) && prevConnected) {
    prevConnected = false;
    Serial.println("[Auto-Reconnect] Dang cho Tag ket noi lai...");
    delay(300); /* Đợi BLE stack ổn định */
    BLEDevice::startAdvertising();
    Serial.println("[Auto-Reconnect] BLE da san sang cho ket noi moi");
  }
  
  /* Chạy đo khoảng cách UWB chỉ khi đã authenticated, kết nối và khởi tạo UWB */
  if (deviceConnected && authenticated && uwbInitialized) {
    uwbResponderLoop();
  }
  
  /* Periodic status monitoring */
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 30000) {
    lastPrint = millis();
    
    Serial.println("\n--- Status ---");
    Serial.println("Connected: " + String(deviceConnected ? "Yes" : "No"));
    Serial.println("Authenticated: " + String(authenticated ? "Yes" : "No"));
    Serial.println("Commands: CLEAR, SHOW, STATUS");
    Serial.println("--------------\n");
  }
  
  delay(10); /* Tốc độ vòng lặp chính: ~100Hz */
}