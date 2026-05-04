
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <SPI.h>
#include "dw3000.h"
#include <mcp2515.h>
#include "anchor_config.h"
#include "can_commands.h"
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/base64.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "friend_types.h"
#include "friend_revocation.h"
#include "friend_cache.h"
#include "friend_token.h"
#include "friend_mgmt.h"

// =============================================================================
// FreeRTOS IPC primitives
// =============================================================================

static EventGroupHandle_t sysEvents;
#define EVT_CONNECTED   (1 << 0)   // Tag vừa kết nối BLE
#define EVT_AUTHED      (1 << 1)   // Tag đã xác thực HMAC OK
#define EVT_UWB_ACTIVE  (1 << 2)   // UWB đã khởi tạo và đang ranging

// BLE queue — commands cho bleTask
enum BleCmdType : uint8_t {
    BLE_SEND_CHALLENGE,     // generate + gửi challenge tới Tag
    BLE_AUTH_VERIFY,        // xác minh HMAC response từ Tag
    BLE_NOTIFY_UWB_ACTIVE,  // gửi "UWB_ACTIVE" notification tới Tag
    BLE_RESTART_ADV,        // restart BLE advertising
};
struct BleCmdMsg {
    BleCmdType type;
    uint8_t    data[32];   // dùng cho BLE_AUTH_VERIFY: chứa HMAC response
    uint8_t    dataLen;
};
static QueueHandle_t bleQueue;  // depth 8

// UWB queue — commands cho uwbTask
#define UWB_CMD_INIT   (0U)
#define UWB_CMD_DEINIT (1U)
static QueueHandle_t uwbQueue;  // depth 4

// CAN queue — commands cho canTask
#define CAN_CMD_LOCK   (0U)
#define CAN_CMD_UNLOCK (1U)
static QueueHandle_t canQueue;  // depth 4

// SPI mutex — arbitrate giữa DW3000 (uwbTask) và MCP2515 (canTask)
// Cả hai dùng SPI2 bus chung trên ESP32-S3
static SemaphoreHandle_t spiMutex;

// Friend bundle queue — friend_bundle_t items from BLE callback → friendMgmtTask
static QueueHandle_t bundleQueue;  // depth 2

// =============================================================================
// State variables (tối thiểu — phần lớn state nằm trong EventGroup)
// =============================================================================

static volatile bool    carUnlocked          = false;
static volatile bool    hasKey               = false;
static volatile bool    bleStarted           = false;
static volatile bool    deviceConnected      = false;
static volatile bool    authenticated        = false;
// Set by friendMgmtTask when friend bundle is verified OK.
// Allows CharacteristicCallbacks to accept TAG_UWB_READY from friend sessions.
// Cleared on every onConnect.
static volatile bool    s_friendBundleVerified = false;
// Incremented on every onConnect — bleTask embeds this in BLE_SEND_CHALLENGE so
// stale messages from previous sessions can be detected and discarded.
static volatile uint8_t connectionGen        = 0;

static BLEServer*       pBleServer       = nullptr;  // global để bleTask có thể ngắt kết nối

// =============================================================================
// Hardware handles
// =============================================================================

static BLECharacteristic *pCharacteristic          = nullptr;
static BLECharacteristic *pChallengeCharacteristic = nullptr;
static BLECharacteristic *pAuthCharacteristic      = nullptr;

// Friend sharing — BLE characteristics and bundle receive state
static BLECharacteristic *pBundleSubmitChar = nullptr;  // Write
static BLECharacteristic *pFriendStatusChar = nullptr;  // Notify
static uint8_t   s_bundleBuf[BUNDLE_BIN_SIZE];
static size_t    s_bundleBufLen = 0;
static MCP2515*     pMcp2515    = nullptr;
static CANCommands* pCanControl = nullptr;

// =============================================================================
// Auth state
// =============================================================================

static uint8_t currentChallenge[16];
static uint8_t pairingKey[16];
// responseBuffer: được ghi bởi AuthChar callback (Core 0 BLE stack task)
// Chỉ được đọc sau khi đã copy vào BleCmdMsg — không cần volatile
static uint8_t  responseBuffer[32];
static uint8_t  responseBufferLen = 0;

// =============================================================================
// UWB frame buffers + config
// =============================================================================

// Channel 5 | 1024-symbol preamble | PAC 32 | code 9 | 850 kbps | STS mode 1
static dwt_config_t uwbConfig = {
    5, DWT_PLEN_1024, DWT_PAC32, 9, 9, 1,
    DWT_BR_850K, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
    1001, DWT_STS_MODE_1, DWT_STS_LEN_256, DWT_PDOA_M0
};

// STS key/IV — derived từ pairingKey khi initUWB() chạy
// Cả Anchor và Tag dùng cùng pairingKey → cùng STS key → authenticate UWB frame
static dwt_sts_cp_key_t sts_key;
static dwt_sts_cp_iv_t  sts_iv;
static bool stsConfigured = false;
extern dwt_txconfig_t txconfig_options;

// Header: 0x41 0x88 = IEEE 802.15.4 frame control; 0xCA 0xDE = PAN ID
static uint8_t rx_poll_msg[] = {0x41U,0x88U,0U,0xCAU,0xDEU,'W','A','V','E',0xE0U,0U,0U};
static uint8_t tx_resp_msg[] = {0x41U,0x88U,0U,0xCAU,0xDEU,'V','E','W','A',0xE1U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U};
static uint8_t  rx_buffer[MSG_BUFFER_SIZE];
static uint8_t  frame_seq_nb = 0U;

// =============================================================================
// NVS + crypto state
// =============================================================================

static Preferences              preferences;
static char                     bleKeyHex[33] = "";   // 32 hex chars + null
static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context ctr_drbg;

// =============================================================================
// Crypto helpers
// =============================================================================

static void hexStringToBytes(const char* hex, uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; i++)
        sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
}

static void generateChallenge(uint8_t* challenge, size_t length) {
    mbedtls_ctr_drbg_random(&ctr_drbg, challenge, length);
}

static bool computeHMAC(const uint8_t* key, size_t keyLen,
                        const uint8_t* data, size_t dataLen,
                        uint8_t* output) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return (mbedtls_md_hmac(md, key, keyLen, data, dataLen, output) == 0);
}

// HKDF-SHA256 theo RFC 5869 — thay thế mbedtls_hkdf() không có trong SDK cũ.
// salt=NULL/0 → dùng 32 zero bytes (RFC 5869 §2.2).
// Chỉ cần output <= 32 bytes (1 block SHA-256).
static bool hkdfSha256(const uint8_t* salt, size_t saltLen,
                       const uint8_t* ikm,  size_t ikmLen,
                       const uint8_t* info, size_t infoLen,
                       uint8_t* out, size_t outLen) {
    if (outLen > 32) return false;
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    // Extract: PRK = HMAC-SHA256(salt, IKM)
    uint8_t zeros[32] = {};
    const uint8_t* s  = (salt && saltLen > 0) ? salt : zeros;
    size_t         sl = (salt && saltLen > 0) ? saltLen : 32;
    uint8_t prk[32];
    if (mbedtls_md_hmac(md, s, sl, ikm, ikmLen, prk) != 0) return false;

    // Expand: T(1) = HMAC-SHA256(PRK, info || 0x01)
    uint8_t counter = 0x01;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, 1) != 0) { mbedtls_md_free(&ctx); return false; }
    mbedtls_md_hmac_starts(&ctx, prk, 32);
    mbedtls_md_hmac_update(&ctx, info, infoLen);
    mbedtls_md_hmac_update(&ctx, &counter, 1);
    uint8_t t1[32];
    mbedtls_md_hmac_finish(&ctx, t1);
    mbedtls_md_free(&ctx);

    memcpy(out, t1, outLen);
    return true;
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
// NVS key storage
// =============================================================================

static void checkStoredKey() {
    preferences.begin("ble-keys", true);
    if (preferences.isKey("bleKey")) {
        preferences.getString("bleKey", bleKeyHex, sizeof(bleKeyHex));
        hasKey = (bleKeyHex[0] != '\0');
    } else {
        hasKey = false;
    }
    preferences.end();
}

static void saveKeyToNVS(const char* key) {
    preferences.begin("ble-keys", false);
    preferences.putString("bleKey", key);
    preferences.end();
    strncpy(bleKeyHex, key, sizeof(bleKeyHex) - 1);
    bleKeyHex[sizeof(bleKeyHex) - 1] = '\0';
    hasKey = true;
    Serial.println("Key saved to NVS");
}

// =============================================================================
// SIM Module key provisioning (thay thế WiFi)
// Dùng AT commands qua HardwareSerial để POST /secure-check-pairing
// =============================================================================

static HardwareSerial simSerial(2);  // UART2 trên ESP32-S3

// Gửi lệnh AT, chờ token hoặc "ERROR" trong timeout_ms, trả về toàn bộ response
static String simAtCmd(const char* cmd, const char* expectToken,
                       unsigned long timeout_ms = 5000) {
    while (simSerial.available()) simSerial.read();
    Serial.printf("[AT] >> %s\n", cmd);
    simSerial.print(cmd);
    simSerial.print("\r");
    unsigned long t = millis();
    String buf = "";
    while (millis() - t < timeout_ms) {
        while (simSerial.available()) buf += (char)simSerial.read();
        if (buf.indexOf(expectToken) >= 0) break;
        if (buf.indexOf("ERROR") >= 0)     break;
    }
    String trimmed = buf; trimmed.trim();
    Serial.printf("[AT] << %s\n", trimmed.c_str());
    return buf;
}

static bool simAtOk(const char* cmd, unsigned long timeout_ms = 5000) {
    return simAtCmd(cmd, "OK", timeout_ms).indexOf("OK") >= 0;
}

static bool simInit() {
    simSerial.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
    Serial.println("[SIM] Cho module khoi dong (8s)...");
    vTaskDelay(pdMS_TO_TICKS(8000));
    while (simSerial.available()) simSerial.read();

    bool ok = false;
    for (int i = 0; i < 5; i++) {
        if (simAtOk("AT")) { ok = true; break; }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!ok) { Serial.println("[SIM] LOI: AT khong phan hoi"); return false; }
    simAtOk("ATE0");

    // Kiểm tra SIM có sẵn sàng không
    String cpin = simAtCmd("AT+CPIN?", "OK", 3000);
    if (cpin.indexOf("READY") < 0) {
        Serial.println("[SIM] LOI: SIM chua san sang (bi khoa PIN hoac khong co SIM)");
        return false;
    }

    Serial.println("[SIM] Cho dang ky mang...");
    unsigned long t = millis();
    bool netOk = false;
    while (millis() - t < 60000) {
        // Thử CEREG (LTE) trước, fallback sang CREG (2G/3G)
        String r = simAtCmd("AT+CEREG?", "OK", 3000);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { netOk = true; break; }
        r = simAtCmd("AT+CREG?", "OK", 3000);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { netOk = true; break; }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (!netOk) { Serial.println("[SIM] LOI: khong co mang"); return false; }

    String cgdcont = String("AT+CGDCONT=1,\"IP\",\"") + SIM_APN + "\"";
    simAtOk(cgdcont.c_str());
    simAtOk("AT+CGACT=1,1", 10000);
    String ip = simAtCmd("AT+CGPADDR=1", "OK", 5000);
    Serial.println("[SIM] " + ip);
    return true;
}

// HTTP POST bằng AT commands A7680C, trả về response body hoặc "" khi lỗi
static String simHttpPost(const char* url, const char* body) {
    int bodyLen = strlen(body);
    simAtOk("AT+HTTPTERM", 3000);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!simAtOk("AT+HTTPINIT")) { Serial.println("[HTTP] HTTPINIT failed"); return ""; }

    String urlCmd = String("AT+HTTPPARA=\"URL\",\"") + url + "\"";
    if (!simAtOk(urlCmd.c_str(), 5000)) { simAtOk("AT+HTTPTERM", 2000); return ""; }
    if (!simAtOk("AT+HTTPPARA=\"CONTENT\",\"application/json\"")) {
        simAtOk("AT+HTTPTERM", 2000); return "";
    }

    // Gửi body — chờ prompt "DOWNLOAD"
    String dataCmd = String("AT+HTTPDATA=") + bodyLen + ",10000";
    while (simSerial.available()) simSerial.read();
    simSerial.println(dataCmd);
    unsigned long t = millis(); String buf = "";
    while (millis() - t < 8000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        if (buf.indexOf("DOWNLOAD") >= 0) break;
    }
    if (buf.indexOf("DOWNLOAD") < 0) { simAtOk("AT+HTTPTERM", 2000); return ""; }

    simSerial.print(body);
    vTaskDelay(pdMS_TO_TICKS(200));
    buf = ""; t = millis();
    while (millis() - t < 5000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        if (buf.indexOf("OK") >= 0) break;
    }

    // POST (action=1)
    while (simSerial.available()) simSerial.read();
    simSerial.println("AT+HTTPACTION=1");
    buf = ""; t = millis();
    int httpStatus = 0, respLen = 0;
    while (millis() - t < 30000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        int idx = buf.indexOf("+HTTPACTION:");
        if (idx >= 0) {
            String line = buf.substring(idx);
            int c1 = line.indexOf(','), c2 = line.indexOf(',', c1 + 1);
            int nl = line.indexOf('\n', c2);
            if (c1 > 0 && c2 > 0) {
                httpStatus = line.substring(c1 + 1, c2).toInt();
                respLen    = line.substring(c2 + 1, nl > 0 ? nl : c2 + 10).toInt();
                break;
            }
        }
    }
    Serial.printf("[HTTP] Status: %d, Len: %d\n", httpStatus, respLen);
    if (httpStatus != 200 || respLen <= 0) { simAtOk("AT+HTTPTERM", 2000); return ""; }

    // Đọc response — đọc đúng respLen bytes (dữ liệu base64 có thể chứa "OK")
    String readCmd = String("AT+HTTPREAD=0,") + respLen;
    while (simSerial.available()) simSerial.read();
    simSerial.println(readCmd);
    buf = ""; t = millis();
    while (millis() - t < 10000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        int hdrIdx = buf.indexOf("+HTTPREAD:");
        if (hdrIdx >= 0 && buf.indexOf("\r\n", hdrIdx) >= 0) break;
    }
    int hdrIdx = buf.indexOf("+HTTPREAD:");
    int hdrEnd = buf.indexOf("\r\n", hdrIdx);
    if (hdrIdx < 0 || hdrEnd < 0) { simAtOk("AT+HTTPTERM", 2000); return ""; }

    String respBody = buf.substring(hdrEnd + 2);
    t = millis();
    while ((int)respBody.length() < respLen && millis() - t < 10000)
        while (simSerial.available()) respBody += (char)simSerial.read();
    respBody = respBody.substring(0, respLen);
    respBody.trim();
    simAtOk("AT+HTTPTERM", 2000);
    return respBody;
}

// Lấy pairing key từ server qua SIM. Thành công → ghi 32-char hex vào keyHexOut (char[33]).
// Dùng global ctr_drbg đã seed trong setup().
static bool fetchPairingKeyViaSim(char* keyHexOut) {
    // Tạo EC key pair P-256
    mbedtls_pk_context our_pk;
    mbedtls_pk_init(&our_pk);
    if (mbedtls_pk_setup(&our_pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(our_pk),
                            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        Serial.println("[KEY] LOI: tao EC key");
        mbedtls_pk_free(&our_pk); return false;
    }

    uint8_t pubder[128];
    int pubder_len = mbedtls_pk_write_pubkey_der(&our_pk, pubder, sizeof(pubder));
    if (pubder_len < 0) {
        Serial.println("[KEY] LOI: export pubkey");
        mbedtls_pk_free(&our_pk); return false;
    }
    char pubkey_b64[200]; size_t b64len;
    if (mbedtls_base64_encode((uint8_t*)pubkey_b64, sizeof(pubkey_b64), &b64len,
                              pubder + sizeof(pubder) - pubder_len, pubder_len) != 0) {
        Serial.println("[KEY] LOI: base64 encode");
        mbedtls_pk_free(&our_pk); return false;
    }
    pubkey_b64[b64len] = '\0';

    // POST /secure-check-pairing
    char body[512];
    {
        StaticJsonDocument<384> req;
        req["vehicle_id"]            = VEHICLE_ID;
        req["client_public_key_b64"] = pubkey_b64;
        serializeJson(req, body, sizeof(body));
    }
    String endpoint = String(SERVER_FALLBACK) + "/secure-check-pairing";
    Serial.printf("[HTTP] POST %s\n", endpoint.c_str());
    String respBody = simHttpPost(endpoint.c_str(), body);
    if (respBody.length() == 0) {
        Serial.println("[HTTP] LOI: khong nhan duoc response");
        mbedtls_pk_free(&our_pk); return false;
    }
    Serial.println("[HTTP] Response: " + respBody.substring(0, 80));

    // Parse JSON response
    StaticJsonDocument<768> resp;
    if (deserializeJson(resp, respBody) != DeserializationError::Ok) {
        Serial.println("[KEY] LOI: parse JSON response");
        mbedtls_pk_free(&our_pk); return false;
    }
    const char* srv_pub_b64   = resp["server_public_key_b64"];
    const char* encrypted_b64 = resp["encrypted_data_b64"];
    const char* nonce_b64     = resp["nonce_b64"];
    if (!srv_pub_b64 || !encrypted_b64 || !nonce_b64) {
        Serial.println("[KEY] LOI: thieu truong JSON");
        mbedtls_pk_free(&our_pk); return false;
    }

    // Base64 decode
    uint8_t srv_pub_der[128]; size_t srv_pub_len;
    uint8_t encrypted[256];   size_t enc_len;
    uint8_t nonce[12];        size_t nonce_len;
    if (mbedtls_base64_decode(srv_pub_der, sizeof(srv_pub_der), &srv_pub_len,
                              (const uint8_t*)srv_pub_b64, strlen(srv_pub_b64)) != 0 ||
        mbedtls_base64_decode(encrypted, sizeof(encrypted), &enc_len,
                              (const uint8_t*)encrypted_b64, strlen(encrypted_b64)) != 0 ||
        mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len,
                              (const uint8_t*)nonce_b64, strlen(nonce_b64)) != 0 || nonce_len != 12) {
        Serial.println("[KEY] LOI: base64 decode");
        mbedtls_pk_free(&our_pk); return false;
    }
    if (enc_len < 17) {
        Serial.println("[KEY] LOI: kich thuoc payload");
        mbedtls_pk_free(&our_pk); return false;
    }

    // Load server public key và ECDH
    mbedtls_pk_context srv_pk; mbedtls_pk_init(&srv_pk);
    if (mbedtls_pk_parse_public_key(&srv_pk, srv_pub_der, srv_pub_len) != 0) {
        Serial.println("[KEY] LOI: parse server pubkey");
        mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk); return false;
    }
    mbedtls_ecdh_context ecdh; mbedtls_ecdh_init(&ecdh);
    if (mbedtls_ecdh_get_params(&ecdh, mbedtls_pk_ec(our_pk), MBEDTLS_ECDH_OURS)   != 0 ||
        mbedtls_ecdh_get_params(&ecdh, mbedtls_pk_ec(srv_pk), MBEDTLS_ECDH_THEIRS) != 0) {
        Serial.println("[KEY] LOI: ECDH setup");
        mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk); return false;
    }
    uint8_t shared_secret[32]; size_t shared_len = 0;
    if (mbedtls_ecdh_calc_secret(&ecdh, &shared_len, shared_secret, sizeof(shared_secret),
                                 mbedtls_ctr_drbg_random, &ctr_drbg) != 0 || shared_len != 32) {
        Serial.println("[KEY] LOI: ECDH calc secret");
        mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk); return false;
    }
    mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk);

    // HKDF → KEK 16 bytes
    uint8_t kek[16];
    if (!hkdfSha256(NULL, 0, shared_secret, 32,
                    (const uint8_t*)"secure-check-kek", 16, kek, 16)) {
        Serial.println("[KEY] LOI: HKDF"); return false;
    }

    // AES-128-GCM decrypt (16 bytes cuối của encrypted là auth tag)
    size_t ct_len = enc_len - 16;
    uint8_t plaintext[256] = {};
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128) != 0 ||
        mbedtls_gcm_auth_decrypt(&gcm, ct_len, nonce, 12,
                                 NULL, 0, encrypted + ct_len, 16,
                                 encrypted, plaintext) != 0) {
        Serial.println("[KEY] LOI: AES-GCM decrypt");
        mbedtls_gcm_free(&gcm); return false;
    }
    mbedtls_gcm_free(&gcm);
    plaintext[ct_len] = '\0';

    // Parse plaintext JSON
    StaticJsonDocument<512> keyDoc;
    if (deserializeJson(keyDoc, (char*)plaintext) != DeserializationError::Ok) {
        Serial.println("[KEY] LOI: parse JSON plaintext"); return false;
    }
    if (!keyDoc["paired"].as<bool>()) {
        Serial.printf("[KEY] Xe '%s' chua duoc dang ky\n", VEHICLE_ID); return false;
    }
    const char* keyHex = keyDoc["pairing_key"];
    if (!keyHex || strlen(keyHex) != 32) {
        Serial.println("[KEY] LOI: pairing_key khong hop le"); return false;
    }
    strncpy(keyHexOut, keyHex, 33);
    return true;
}

// =============================================================================
// WiFi key provisioning
// =============================================================================

static bool wifiInit() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] LOI: khong ket noi duoc");
        return false;
    }
    Serial.println("[WiFi] Connected: " + WiFi.localIP().toString());

    // Sync system clock via SNTP — required for bundle TTL validation.
    // TZ=UTC0 ensures mktime() treats struct tm as UTC (set in setup()).
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("[WiFi] Syncing SNTP time");
    for (int i = 0; i < 20 && time(nullptr) < 1000000000L; i++) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("[WiFi] %s (t=%lu)\n",
                  time(nullptr) >= 1000000000L ? "Time OK" : "SNTP timeout — timestamps may be wrong",
                  (unsigned long)time(nullptr));
    return true;
}

static String wifiHttpPost(const char* url, const char* body) {
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    if (code != 200) {
        Serial.printf("[WiFi HTTP] Status: %d\n", code);
        http.end();
        return "";
    }
    String resp = http.getString();
    http.end();
    return resp;
}

// Lấy server ECDSA signing public key từ /pairing-bootstrap và lưu NVS.
// Gọi một lần sau khi pairing key đã được fetch thành công.
static void fetchServerSigningKey() {
    String url = String(SERVER_FALLBACK) + "/pairing-bootstrap";
    HTTPClient http;
    http.begin(url);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[KEY] /pairing-bootstrap HTTP %d — signing key not cached\n", code);
        http.end(); return;
    }
    String resp = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) {
        Serial.println("[KEY] /pairing-bootstrap: JSON parse error"); return;
    }
    const char *pub_b64 = doc["server_signing_public_key_b64"];
    if (!pub_b64) { Serial.println("[KEY] /pairing-bootstrap: missing pubkey field"); return; }

    uint8_t pub_der[128]; size_t pub_len;
    if (mbedtls_base64_decode(pub_der, sizeof(pub_der), &pub_len,
                              (const uint8_t *)pub_b64, strlen(pub_b64)) != 0) {
        Serial.println("[KEY] pubkey base64 decode failed"); return;
    }
    friend_token_save_server_pubkey(pub_der, pub_len);

    // Cache bundle_version and max_key_ttl_hours in NVS for revocation cleanup
    uint8_t  bver    = doc["bundle_version"]    | (uint8_t)BUNDLE_VERSION;
    uint16_t max_ttl = doc["max_key_ttl_hours"] | (uint16_t)720;
    Preferences p;
    if (p.begin("sca_anchor", false)) {
        p.putUChar("bundle_ver", bver);
        p.putUShort("max_ttl_h", max_ttl);
        p.end();
        Serial.printf("[KEY] Bootstrap: bundle_ver=%u max_ttl_h=%u\n", bver, max_ttl);
    }
}

// Giống fetchPairingKeyViaSim() nhưng dùng WiFi/HTTPClient thay AT commands
static bool fetchPairingKeyViaWifi(char* keyHexOut) {
    mbedtls_pk_context our_pk;
    mbedtls_pk_init(&our_pk);
    if (mbedtls_pk_setup(&our_pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(our_pk),
                            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        Serial.println("[KEY] LOI: tao EC key");
        mbedtls_pk_free(&our_pk); return false;
    }

    uint8_t pubder[128];
    int pubder_len = mbedtls_pk_write_pubkey_der(&our_pk, pubder, sizeof(pubder));
    if (pubder_len < 0) {
        Serial.println("[KEY] LOI: export pubkey");
        mbedtls_pk_free(&our_pk); return false;
    }
    char pubkey_b64[200]; size_t b64len;
    if (mbedtls_base64_encode((uint8_t*)pubkey_b64, sizeof(pubkey_b64), &b64len,
                              pubder + sizeof(pubder) - pubder_len, pubder_len) != 0) {
        Serial.println("[KEY] LOI: base64 encode");
        mbedtls_pk_free(&our_pk); return false;
    }
    pubkey_b64[b64len] = '\0';

    char body[512];
    {
        StaticJsonDocument<384> req;
        req["vehicle_id"]            = VEHICLE_ID;
        req["client_public_key_b64"] = pubkey_b64;
        serializeJson(req, body, sizeof(body));
    }
    String endpoint = String(SERVER_FALLBACK) + "/secure-check-pairing";
    Serial.printf("[WiFi HTTP] POST %s\n", endpoint.c_str());
    String respBody = wifiHttpPost(endpoint.c_str(), body);
    if (respBody.length() == 0) {
        Serial.println("[WiFi HTTP] LOI: khong nhan duoc response");
        mbedtls_pk_free(&our_pk); return false;
    }
    Serial.println("[WiFi HTTP] Response: " + respBody.substring(0, 80));

    StaticJsonDocument<768> resp;
    if (deserializeJson(resp, respBody) != DeserializationError::Ok) {
        Serial.println("[KEY] LOI: parse JSON response");
        mbedtls_pk_free(&our_pk); return false;
    }
    const char* srv_pub_b64   = resp["server_public_key_b64"];
    const char* encrypted_b64 = resp["encrypted_data_b64"];
    const char* nonce_b64     = resp["nonce_b64"];
    if (!srv_pub_b64 || !encrypted_b64 || !nonce_b64) {
        Serial.println("[KEY] LOI: thieu truong JSON");
        mbedtls_pk_free(&our_pk); return false;
    }

    uint8_t srv_pub_der[128]; size_t srv_pub_len;
    uint8_t encrypted[256];   size_t enc_len;
    uint8_t nonce[12];        size_t nonce_len;
    if (mbedtls_base64_decode(srv_pub_der, sizeof(srv_pub_der), &srv_pub_len,
                              (const uint8_t*)srv_pub_b64, strlen(srv_pub_b64)) != 0 ||
        mbedtls_base64_decode(encrypted, sizeof(encrypted), &enc_len,
                              (const uint8_t*)encrypted_b64, strlen(encrypted_b64)) != 0 ||
        mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len,
                              (const uint8_t*)nonce_b64, strlen(nonce_b64)) != 0 || nonce_len != 12) {
        Serial.println("[KEY] LOI: base64 decode");
        mbedtls_pk_free(&our_pk); return false;
    }
    if (enc_len < 17) {
        Serial.println("[KEY] LOI: kich thuoc payload");
        mbedtls_pk_free(&our_pk); return false;
    }

    mbedtls_pk_context srv_pk; mbedtls_pk_init(&srv_pk);
    if (mbedtls_pk_parse_public_key(&srv_pk, srv_pub_der, srv_pub_len) != 0) {
        Serial.println("[KEY] LOI: parse server pubkey");
        mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk); return false;
    }
    mbedtls_ecdh_context ecdh; mbedtls_ecdh_init(&ecdh);
    if (mbedtls_ecdh_get_params(&ecdh, mbedtls_pk_ec(our_pk), MBEDTLS_ECDH_OURS)   != 0 ||
        mbedtls_ecdh_get_params(&ecdh, mbedtls_pk_ec(srv_pk), MBEDTLS_ECDH_THEIRS) != 0) {
        Serial.println("[KEY] LOI: ECDH setup");
        mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk); return false;
    }
    uint8_t shared_secret[32]; size_t shared_len = 0;
    if (mbedtls_ecdh_calc_secret(&ecdh, &shared_len, shared_secret, sizeof(shared_secret),
                                 mbedtls_ctr_drbg_random, &ctr_drbg) != 0 || shared_len != 32) {
        Serial.println("[KEY] LOI: ECDH calc secret");
        mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk); return false;
    }
    mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk);

    uint8_t kek[16];
    if (!hkdfSha256(NULL, 0, shared_secret, 32,
                    (const uint8_t*)"secure-check-kek", 16, kek, 16)) {
        Serial.println("[KEY] LOI: HKDF"); return false;
    }

    size_t ct_len = enc_len - 16;
    uint8_t plaintext[256] = {};
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128) != 0 ||
        mbedtls_gcm_auth_decrypt(&gcm, ct_len, nonce, 12,
                                 NULL, 0, encrypted + ct_len, 16,
                                 encrypted, plaintext) != 0) {
        Serial.println("[KEY] LOI: AES-GCM decrypt");
        mbedtls_gcm_free(&gcm); return false;
    }
    mbedtls_gcm_free(&gcm);
    plaintext[ct_len] = '\0';

    StaticJsonDocument<512> keyDoc;
    if (deserializeJson(keyDoc, (char*)plaintext) != DeserializationError::Ok) {
        Serial.println("[KEY] LOI: parse JSON plaintext"); return false;
    }
    if (!keyDoc["paired"].as<bool>()) {
        Serial.printf("[KEY] Xe '%s' chua duoc dang ky\n", VEHICLE_ID); return false;
    }
    const char* keyHex = keyDoc["pairing_key"];
    if (!keyHex || strlen(keyHex) != 32) {
        Serial.println("[KEY] LOI: pairing_key khong hop le"); return false;
    }
    strncpy(keyHexOut, keyHex, 33);
    return true;
}

// =============================================================================
// CAN helpers
// =============================================================================

static void canLock() {
    if (!pCanControl) return;
    bool ok = pCanControl->lockCar();
    carUnlocked = false;  // cập nhật state bất kể CAN result (an toàn: assume locked)
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
// BLE callbacks — chỉ gửi message vào queue, KHÔNG làm việc nặng tại đây
//
// Trước: BLE callback set volatile flags → loop() Core 1 poll và xử lý
// Sau:   BLE callback xQueueSend → task nhận và xử lý ngay đúng context
// =============================================================================

class AuthCharacteristicCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) override {
        // Dùng getData()/getLength() thay getValue() — zero-copy, không heap allocate
        uint8_t* pData = pChar->getData();
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
    void onWrite(BLECharacteristic* pChar) override {
        if (!pChar) return;
        // Dùng getData()/getLength() thay getValue() — không tạo String heap copy
        const uint8_t* pData = pChar->getData();
        size_t         len   = pChar->getLength();
        if (!pData || len == 0) return;

        // startsWith bằng memcmp — không allocate
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
            bool authed    = (xEventGroupGetBits(sysEvents) & EVT_AUTHED) != 0;
            bool friendOk  = s_friendBundleVerified;
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
    void onConnect(BLEServer* pServer) override {
        deviceConnected       = true;
        authenticated         = false;
        responseBufferLen     = 0;
        s_bundleBufLen        = 0;  // discard any partial bundle from previous session
        s_friendBundleVerified = false;
        connectionGen++;   // new generation — invalidates any pending BLE_SEND_CHALLENGE
        xEventGroupSetBits(sysEvents, EVT_CONNECTED);
        xEventGroupClearBits(sysEvents, EVT_AUTHED | EVT_UWB_ACTIVE);
        Serial.printf("BLE: Tag connected (gen=%u)\n", (unsigned)connectionGen);

        // Gửi BLE_SEND_CHALLENGE vào bleQueue, đính kèm generation ID.
        // bleTask kiểm tra ID trước khi gửi challenge — nếu lệch (session cũ) thì bỏ qua.
        BleCmdMsg msg = {};
        msg.type    = BLE_SEND_CHALLENGE;
        msg.data[0] = connectionGen;
        xQueueSend(bleQueue, &msg, pdMS_TO_TICKS(10));
    }

    void onDisconnect(BLEServer* pServer) override {
        deviceConnected = false;
        authenticated   = false;
        xEventGroupClearBits(sysEvents, EVT_CONNECTED | EVT_AUTHED | EVT_UWB_ACTIVE);
        Serial.println("BLE: Tag disconnected");

        uint8_t cmd;
        cmd = UWB_CMD_DEINIT; xQueueSend(uwbQueue, &cmd, pdMS_TO_TICKS(10));
        s_friendBundleVerified = false;

        // Lock only if car was actually unlocked — handles both owner and friend mode.
        // If bundle was rejected or Tag disconnects before VERIFIED, car is still locked.
        if (carUnlocked) {
            cmd = CAN_CMD_LOCK; xQueueSend(canQueue, &cmd, pdMS_TO_TICKS(10));
        } else {
            Serial.println("[onDisconnect] Car not unlocked — skipping auto-lock");
        }

        BleCmdMsg msg = {}; msg.type = BLE_RESTART_ADV;
        xQueueSend(bleQueue, &msg, pdMS_TO_TICKS(10));
    }
};

// =============================================================================
// Friend sharing — helpers and BLE callback for bundle submission
// =============================================================================

/**
 * Check whether the most recent UWB ranging session indicates the device is
 * within proximity.  Returns true when EVT_UWB_ACTIVE is set (meaning the
 * DW3000 is actively ranging with the connected device).
 * Production enhancement: also compare the last measured distance against a
 * configurable threshold stored in NVS.
 */
static bool uwb_check_proximity_ok(void) {
    return (xEventGroupGetBits(sysEvents) & EVT_UWB_ACTIVE) != 0;
}

/**
 * Send a 2-byte status notification to the guest via the friend status
 * characteristic.  byte[0]: 0 = accepted, 1 = rejected.  byte[1]: reason code.
 */
static void ble_notify_friend_status(uint8_t accepted, token_verify_result_t reason) {
    if (!pFriendStatusChar) return;
    uint8_t payload[2] = { accepted, (uint8_t)reason };
    pFriendStatusChar->setValue(payload, 2);
    pFriendStatusChar->notify();
}

/**
 * BLE Write callback for CHAR_BUNDLE_SUBMIT_UUID.
 *
 * Accumulates fragmented BLE writes (MTU may be < 195-byte bundle).
 * When enough bytes are present to include the signature, parses the
 * wire format and pushes a friend_bundle_t into bundleQueue.
 *
 * BLE stack rule: this callback ONLY queues — all heavy work (ECDSA, HTTP,
 * CAN) happens in friendMgmtTask on Core 0.
 */
class BundleSubmitCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        const uint8_t *data = pChar->getData();
        size_t len = pChar->getLength();
        if (!data || len == 0) return;

        // Accumulate into receive buffer (guards against overflow)
        size_t space   = sizeof(s_bundleBuf) - s_bundleBufLen;
        size_t toCopy  = (len < space) ? len : space;
        memcpy(s_bundleBuf + s_bundleBufLen, data, toCopy);
        s_bundleBufLen += toCopy;

        // Bundle is exactly BUNDLE_BIN_SIZE (106) bytes — fixed format, no length field
        if (s_bundleBufLen < BUNDLE_BIN_SIZE) return;  // accumulating fragments

        // Full bundle received — parse and hand off to task
        friend_bundle_t bundle;
        if (ft_parse_bundle_wire(s_bundleBuf, BUNDLE_BIN_SIZE, &bundle)) {
            BaseType_t sent = xQueueSend(bundleQueue, &bundle, pdMS_TO_TICKS(10));
            Serial.printf("[BLE] Bundle queued=%d id=%02x%02x...\n",
                          (int)sent,
                          bundle.friend_id[0], bundle.friend_id[1]);
        } else {
            Serial.println("[BLE] Bundle parse failed — discarding");
        }
        s_bundleBufLen = 0;  // reset for next bundle
    }
};

// =============================================================================
// BLE server init
// =============================================================================

static void startBLE() {
    hexStringToBytes(bleKeyHex, pairingKey, 16);
    printHex("Pairing key: ", pairingKey, 16);

    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setPower(ESP_PWR_LVL_P9);
    pBleServer = BLEDevice::createServer();
    pBleServer->setCallbacks(new MyServerCallbacks());

    BLEService* pService = pBleServer->createService(SERVICE_UUID);

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

    // ── Friend Sharing GATT service (0xFACE) ─────────────────────────────────
    BLEService *pFriendService = pBleServer->createService(FRIEND_SERVICE_UUID);

    // 0xFA01: guest writes the binary bundle (up to 195 bytes, may be fragmented)
    pBundleSubmitChar = pFriendService->createCharacteristic(
        FRIEND_BUNDLE_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    pBundleSubmitChar->setCallbacks(new BundleSubmitCallbacks());

    // 0xFA03: anchor notifies guest with 2-byte [status, reason] result
    pFriendStatusChar = pFriendService->createCharacteristic(
        FRIEND_STATUS_CHAR_UUID,
        BLECharacteristic::PROPERTY_NOTIFY);
    pFriendStatusChar->addDescriptor(new BLE2902());

    pFriendService->start();
    Serial.println("BLE: Friend service started (0xFACE)");

    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    pAdv->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();
    Serial.println("BLE advertising: " + String(DEVICE_NAME));
}

// Đọc key từ NVS. Nếu chưa có → fetch qua WiFi → lưu NVS
static void executeMainFlow() {
    checkStoredKey();

    if (!hasKey) {
        // --- WiFi (đang dùng) ---
        Serial.println("No key in NVS — fetching via WiFi...");
        if (wifiInit()) {
            char wifiKey[33];
            if (fetchPairingKeyViaWifi(wifiKey)) {
                saveKeyToNVS(wifiKey);
                fetchServerSigningKey();  // cache ECDSA signing key for offline friend verify
            } else {
                Serial.println("[WiFi] Failed to fetch pairing key — pair the vehicle first.");
            }
        } else {
            Serial.println("[WiFi] Connect failed — cannot fetch key.");
        }

        // --- SIM (dự phòng — bỏ comment để dùng thay WiFi) ---
        // if (simInit()) {
        //     char simKey[33];
        //     if (fetchPairingKeyViaSim(simKey))
        //         saveKeyToNVS(simKey);
        //     else
        //         Serial.println("[SIM] Failed to fetch pairing key — pair the vehicle first.");
        // } else {
        //     Serial.println("[SIM] Module init failed — cannot fetch key.");
        // }

        if (!hasKey) { Serial.println("No key — BLE not started."); return; }
    }

    Serial.printf("Key loaded: %.8s...\n", bleKeyHex);
    startBLE();
    bleStarted = true;
}

// =============================================================================
// UWB init / deinit
// =============================================================================

static bool initUWB() {
    Serial.println("UWB: initializing...");
    digitalWrite(CAN_CS, HIGH);  // deselect MCP2515 trước
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
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    // Derive STS key từ pairingKey (16 bytes = 4 × uint32_t)
    // Cả Anchor và Tag dùng cùng pairingKey → STS key khớp → UWB frame được xác thực
    memcpy(&sts_key, pairingKey, sizeof(sts_key));
    // IV: upper 96 bits cố định, lower 32 bits = counter reset mỗi ranging
    sts_iv.iv0 = 0x00000001U;
    sts_iv.iv1 = 0x00000000U;
    sts_iv.iv2 = 0x00000000U;
    sts_iv.iv3 = 0x00000000U;
    dwt_configurestskey(&sts_key);
    dwt_configurestsiv(&sts_iv);
    dwt_configurestsloadiv();
    stsConfigured = true;

    Serial.println("UWB: ready (STS mode 1)");
    return true;
fail:
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    return false;
}

static void deinitUWB() {
    dwt_forcetrxoff();
    dwt_softreset();
    vTaskDelay(pdMS_TO_TICKS(2));
    // Giữ DW3000 ở RESET để không drive MISO, tránh conflict với MCP2515 trên SPI bus
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    stsConfigured = false;
    Serial.println("UWB: stopped");
}

// =============================================================================
// UWB responder loop (SS-TWR) — chạy trong uwbTask (Core 1)
// Logic giống hệt BLE_UWB_Anchor, nhưng dùng vTaskDelay thay delay()
// =============================================================================

static void uwbResponderLoop() {
    // Reload STS IV counter trước mỗi RX để sync với Tag
    dwt_writetodevice(STS_IV0_ID, 0, 4, (uint8_t*)&sts_iv.iv0);
    dwt_configurestsloadiv();

    dwt_rxenable(DWT_START_RX_IMMEDIATE);
    uint32_t status_reg = 0U;
    unsigned long t0 = millis();
    while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) &
             (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR))) {
        if ((millis() - t0) > 100UL) { dwt_forcetrxoff(); return; }
        taskYIELD();
    }
    if (!(status_reg & SYS_STATUS_RXFCG_BIT_MASK)) {
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
        dwt_forcetrxoff(); return;
    }

    // Kiểm tra STS quality — từ chối frame nếu STS không hợp lệ (relay attack)
    int16_t stsQual;
    if (dwt_readstsquality(&stsQual) < 0) {
        dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_GOOD);
        dwt_forcetrxoff(); return;
    }

    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    uint32_t frame_len = dwt_read32bitreg(RX_FINFO_ID) & RXFLEN_MASK;
    if (frame_len == 0 || frame_len > sizeof(rx_buffer)) { dwt_forcetrxoff(); return; }

    dwt_readrxdata(rx_buffer, frame_len, 0U);
    rx_buffer[ALL_MSG_SN_IDX] = 0U;
    if (memcmp(rx_buffer, rx_poll_msg, ALL_MSG_COMMON_LEN) != 0) { dwt_forcetrxoff(); return; }

    uint64_t poll_rx_ts   = get_rx_timestamp_u64();
    uint32_t resp_tx_time = (uint32_t)((poll_rx_ts + ((uint64_t)POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8);

    dwt_forcetrxoff();
    dwt_setdelayedtrxtime(resp_tx_time);
    uint64_t resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

    resp_msg_set_ts(&tx_resp_msg[RESP_MSG_POLL_RX_TS_IDX], poll_rx_ts);
    resp_msg_set_ts(&tx_resp_msg[RESP_MSG_RESP_TX_TS_IDX], resp_tx_ts);
    tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;

    dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0U);
    dwt_writetxfctrl(sizeof(tx_resp_msg), 0U, 1);
    if (dwt_starttx(DWT_START_TX_DELAYED) != DWT_SUCCESS) {
        dwt_forcetrxoff(); return;
    }
    unsigned long tx_t0 = millis();
    while (!(dwt_read32bitreg(SYS_STATUS_ID) & SYS_STATUS_TXFRS_BIT_MASK)) {
        if ((millis() - tx_t0) > 10UL) { dwt_forcetrxoff(); return; }
    }
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
    frame_seq_nb++;
}

// =============================================================================
// TASK: bleTask — Core 0, Priority 3
//
// Xử lý tất cả BLE-side work items từ bleQueue.
// Chạy trên Core 0 cùng với BLE stack — không cần cross-core BLE calls.
// =============================================================================

static void bleTask(void* param) {
    BleCmdMsg msg;
    static unsigned long lastAdvRefresh = 0;
    Serial.println("[bleTask] started on core " + String(xPortGetCoreID()));

    for (;;) {
        // Đợi command với timeout 200ms để check periodic tasks
        if (xQueueReceive(bleQueue, &msg, pdMS_TO_TICKS(200)) == pdTRUE) {
            switch (msg.type) {

            case BLE_SEND_CHALLENGE: {
                uint8_t myGen = msg.data[0];
                // Bỏ qua ngay nếu là lệnh cũ từ session trước — không delay, không gửi.
                if (myGen != (uint8_t)connectionGen) {
                    Serial.printf("[BLE] Challenge gen=%u lỗi thời (hiện=%u) — bỏ qua\n",
                                  myGen, (unsigned)connectionGen);
                    break;
                }
                // Sinh challenge và setValue TRƯỚC khi delay để fallback readValue()
                // của Tag luôn trả về challenge đúng nếu notify bị miss.
                generateChallenge(currentChallenge, 16);
                pChallengeCharacteristic->setValue(currentChallenge, 16);
                // Chờ Tag ghi CCCD (đăng ký nhận notify).
                vTaskDelay(pdMS_TO_TICKS(CHALLENGE_SEND_DELAY_MS));
                // Kiểm tra lại sau delay — tránh trường hợp session mới bắt đầu trong lúc chờ.
                if (myGen != (uint8_t)connectionGen) {
                    Serial.printf("[BLE] Challenge gen=%u lỗi thời sau delay — không notify\n",
                                  myGen);
                    break;
                }
                pChallengeCharacteristic->notify();
                printHex("[AUTH] Key:       ", pairingKey,       16);
                printHex("[AUTH] Challenge:  ", currentChallenge, 16);
                Serial.println("[BLE] Challenge sent");
                break;
            }

            case BLE_AUTH_VERIFY: {
                printHex("[AUTH] Key:       ", pairingKey,       16);
                printHex("[AUTH] Challenge:  ", currentChallenge, 16);
                printHex("[AUTH] Tag resp:   ", msg.data,         32);
                uint8_t expected[32];
                if (!computeHMAC(pairingKey, 16, currentChallenge, 16, expected)) {
                    Serial.println("[BLE] HMAC compute failed — disconnecting");
                    pBleServer->disconnect(pBleServer->getConnId());
                    break;
                }
                printHex("[AUTH] Expected:   ", expected, 32);
                if (memcmp(msg.data, expected, 32) == 0) {
                    authenticated = true;
                    xEventGroupSetBits(sysEvents, EVT_AUTHED);
                    pAuthCharacteristic->setValue("AUTH_OK");
                    pAuthCharacteristic->notify();
                    Serial.println("[BLE] Auth OK");
                } else {
                    Serial.println("[BLE] Auth FAIL — disconnecting");
                    pAuthCharacteristic->setValue("AUTH_FAIL");
                    pAuthCharacteristic->notify();
                    vTaskDelay(pdMS_TO_TICKS(50));
                    pBleServer->disconnect(pBleServer->getConnId());
                }
                break;
            }

            case BLE_NOTIFY_UWB_ACTIVE:
                // Gửi sau khi uwbTask đã init DW3000 thành công
                if (pCharacteristic) {
                    pCharacteristic->setValue("UWB_ACTIVE");
                    pCharacteristic->notify();
                    Serial.println("[BLE] Sent UWB_ACTIVE to Tag");
                }
                break;

            case BLE_RESTART_ADV:
                vTaskDelay(pdMS_TO_TICKS(50));
                BLEDevice::startAdvertising();
                lastAdvRefresh = millis();
                Serial.println("[BLE] Advertising restarted");
                break;
            }
        }

        // Periodic: refresh advertising nếu disconnected > 10s
        // (ESP32-S3 BLE stack đôi khi tự dừng advertising sau disconnect)
        if (!deviceConnected && (millis() - lastAdvRefresh > 10000)) {
            lastAdvRefresh = millis();
            BLEDevice::startAdvertising();
        }
    }
}

// =============================================================================
// TASK: uwbTask — Core 1, Priority 4 (cao nhất)
//
// Lý do priority cao: DW3000 SS-TWR có timing nhạy cảm.
// Với FreeRTOS pin cứng Core 1 priority 4 → không có task nào cùng core
// có thể preempt → POLL_RX_TO_RESP_TX_DLY_UUS giảm từ 8000 µs → 2500 µs.
//
// spiMutex: uwbTask hold mutex trong suốt uwbResponderLoop().
// canTask đợi mutex → sau khi uwbTask release → canTask mới truy cập SPI.
// Không cần forcetrxoff hack ở loop() nữa.
// =============================================================================

static void uwbTask(void* param) {
    uint8_t cmd;
    Serial.println("[uwbTask] started on core " + String(xPortGetCoreID()));

    for (;;) {
        // Trạng thái IDLE: đợi UWB_CMD_INIT
        while (xQueueReceive(uwbQueue, &cmd, portMAX_DELAY) != pdTRUE || cmd != UWB_CMD_INIT);

        // Init DW3000 (không cần mutex vì CAN chưa dùng SPI ở giai đoạn này)
        if (!initUWB()) {
            Serial.println("[uwbTask] Init failed — waiting for next command");
            continue;
        }

        // Báo bleTask gửi "UWB_ACTIVE" notification sang Tag
        BleCmdMsg notifyMsg = {}; notifyMsg.type = BLE_NOTIFY_UWB_ACTIVE;
        xQueueSend(bleQueue, &notifyMsg, pdMS_TO_TICKS(100));
        xEventGroupSetBits(sysEvents, EVT_UWB_ACTIVE);

        // Trạng thái ACTIVE: ranging loop
        for (;;) {
            // Check deinit command (non-blocking peek)
            if (xQueueReceive(uwbQueue, &cmd, 0) == pdTRUE && cmd == UWB_CMD_DEINIT) break;

            // Take SPI mutex → ranging → release
            // canTask sẽ đợi ở đây nếu cần gửi CAN command
            if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                uwbResponderLoop();
                xSemaphoreGive(spiMutex);
                // Delay 5ms để canTask (priority thấp hơn) có cơ hội lấy mutex.
                // taskYIELD() không đủ vì uwbTask vẫn là highest-priority ready task.
                vTaskDelay(pdMS_TO_TICKS(5));
            } else {
                Serial.println("[uwbTask] spiMutex timeout — skip iteration");
            }
        }

        // Trạng thái DEINIT
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            deinitUWB();
            xSemaphoreGive(spiMutex);
        }
        xEventGroupClearBits(sysEvents, EVT_UWB_ACTIVE);
    }
}

// =============================================================================
// TASK: canTask — Core 1, Priority 2 (thấp nhất)
//
// Đợi CAN_CMD_LOCK / CAN_CMD_UNLOCK từ canQueue.
// Trước khi dùng SPI: lấy spiMutex (đợi uwbTask release sau 1 ranging iteration).
// dwt_forcetrxoff() để DW3000 không drive MISO trong khi MCP2515 dùng bus.
// =============================================================================

static void canTask(void* param) {
    uint8_t cmd;
    Serial.println("[canTask] started on core " + String(xPortGetCoreID()));

    for (;;) {
        if (xQueueReceive(canQueue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        Serial.printf("===CAN=== cmd=%d received\n", cmd); Serial.flush();

        // Lấy SPI mutex — uwbTask sẽ release sau mỗi uwbResponderLoop() call (~100ms max)
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            Serial.println("===CAN=== spiMutex timeout"); Serial.flush();
            continue;
        }

        Serial.println("===CAN=== got mutex"); Serial.flush();

        // Nếu DW3000 đang active, force idle và deselect trước khi CAN dùng bus
        if (xEventGroupGetBits(sysEvents) & EVT_UWB_ACTIVE) {
            dwt_forcetrxoff();
            digitalWrite(PIN_SS, HIGH);
        }

        Serial.println("===CAN=== calling unlock/lock"); Serial.flush();

        if (cmd == CAN_CMD_LOCK)   canLock();
        if (cmd == CAN_CMD_UNLOCK) canUnlock();

        Serial.println("===CAN=== done"); Serial.flush();

        xSemaphoreGive(spiMutex);
    }
}

// =============================================================================
// TASK: friendMgmtTask — Core 0, Priority 2
//
// Waits for parsed friend_bundle_t from bundleQueue (placed there by
// BundleSubmitCallbacks::onWrite).  Performs the full verification + unlock
// sequence for guest devices.
//
// Verification order:
//   1. Clock sanity (SNTP must have synced)
//   2. Offline ECDSA + time + revocation (friend_token_verify)
//   3. Cache lookup — first-time use triggers optional online validate
//   4. UWB proximity check (EVT_UWB_ACTIVE)
//   5. CAN unlock + BLE notification + usage report
// =============================================================================

static void friendMgmtTask(void *param) {
    friend_bundle_t bundle;
    Serial.printf("[friendMgmtTask] started on core %d\n", xPortGetCoreID());

    for (;;) {
        if (xQueueReceive(bundleQueue, &bundle, portMAX_DELAY) != pdTRUE) continue;

        // Print full bundle detail so Serial Monitor shows exactly what was received
        {
            char fid_hex[FRIEND_ID_LEN * 2 + 1];
            char fkey_hex[FRIEND_KEY_LEN * 2 + 1];
            for (int i = 0; i < FRIEND_ID_LEN;  i++) snprintf(fid_hex  + i*2, 3, "%02x", bundle.friend_id[i]);
            for (int i = 0; i < FRIEND_KEY_LEN; i++) snprintf(fkey_hex + i*2, 3, "%02x", bundle.friend_key[i]);
            Serial.printf("[FRIEND] === Bundle received ===\n");
            Serial.printf("[FRIEND]   friend_id  : %s\n", fid_hex);
            Serial.printf("[FRIEND]   friend_key : %s\n", fkey_hex);
            Serial.printf("[FRIEND]   permissions: 0x%02X\n", bundle.permissions);
            Serial.printf("[FRIEND]   issued_at  : %lu\n", (unsigned long)bundle.issued_at);
            Serial.printf("[FRIEND]   expires_at : %lu\n", (unsigned long)bundle.expires_at);
        }

        // Guard: require a synced clock for time-based checks
        uint32_t now = (uint32_t)time(NULL);
        Serial.printf("[FRIEND] Clock: now=%lu %s\n",
                      (unsigned long)now,
                      now >= 1000000000UL ? "OK" : "NOT_SYNCED");
if (now < 1000000000UL) {
    // Clock chưa sync — dùng issued_at làm anchor tạm thời
    now = bundle.issued_at + 1;  // giả định "vừa issued"
    Serial.println("[FRIEND] Warning: clock not synced — using issued_at as reference");
}

        // Step 1 — offline ECDSA + time + revocation
        token_verify_result_t r = friend_token_verify(&bundle, now, PERM_UNLOCK, VEHICLE_ID);
        Serial.printf("[FRIEND] Offline verify: %s (%d)\n", ft_result_str(r), (int)r);

        if (r != TOKEN_OK) {
            ble_notify_friend_status(1, r);
            continue;
        }

        // Step 2 — cache lookup; first-time use requires online validate
        cached_friend_t cf;
        bool was_cached = (friend_cache_get(bundle.friend_id, &cf) == ESP_OK);
        Serial.printf("[FRIEND] Cache: %s\n", was_cached ? "HIT" : "MISS");

if (!was_cached) {
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[FRIEND] Cache miss — starting online validate");
        if (friend_mgmt_validate_online(&bundle, SERVER_FALLBACK,
                                         VEHICLE_ID, pairingKey) != ESP_OK) {
            Serial.println("[FRIEND] Server rejected bundle");
            ble_notify_friend_status(1, TOKEN_ERR_INTERNAL);
            continue;
        }
        Serial.println("[FRIEND] Server accepted bundle");
    } else {
        // MISS + offline → trust ECDSA (đã verify OK ở Step 1)
        Serial.println("[FRIEND] Accept offline (revocation not checked)");
        // Không continue → fall through xuống Step 3 (UWB proximity)
    }
}

        // Step 3 — authorize UWB session and notify Tag.
        // Tag will discover the main service, send TAG_UWB_READY, do actual ranging,
        // then send VERIFIED when close enough → CharacteristicCallbacks queues
        // CAN_CMD_UNLOCK.  We do NOT unlock here — proximity is enforced by UWB.
        s_friendBundleVerified = true;
        ble_notify_friend_status(0, TOKEN_OK);
        Serial.printf("[FRIEND] Bundle accepted for %02x%02x%02x%02x... — waiting for UWB\n",
                      bundle.friend_id[0], bundle.friend_id[1],
                      bundle.friend_id[2], bundle.friend_id[3]);

        // Step 5 — persist / update cache entry
        if (!was_cached) {
            cached_friend_t new_cf = {};
            new_cf.version        = bundle.version;
            memcpy(new_cf.friend_id,  bundle.friend_id,  FRIEND_ID_LEN);
            memcpy(new_cf.friend_key, bundle.friend_key, FRIEND_KEY_LEN);
            new_cf.permissions    = bundle.permissions;
            new_cf.issued_at      = bundle.issued_at;
            new_cf.expires_at     = bundle.expires_at;
            new_cf.last_validated = now;
            new_cf.uses_count     = 1;
            friend_cache_put(&new_cf);
        } else {
            friend_cache_increment_usage(bundle.friend_id);
        }

        // Step 6 — async usage report (non-blocking, best-effort)
        if (WiFi.status() == WL_CONNECTED) {
            friend_mgmt_report_usage(bundle.friend_id, "UNLOCK",
                                     SERVER_FALLBACK, VEHICLE_ID, pairingKey);
        }
    }
}

// =============================================================================
// TASK: revocationSyncTask — Core 0, Priority 1 (lowest)
//
// Polls /cars/{vehicle_id}/revocations every 5 minutes when WiFi is up.
// Also runs cache + revocation blacklist cleanup on each cycle.
// Runs on Core 0 alongside bleTask and friendMgmtTask; lower priority than
// both so it never blocks time-sensitive BLE operations.
// =============================================================================

static void revocationSyncTask(void *param) {
    Serial.printf("[revSyncTask] started on core %d\n", xPortGetCoreID());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5UL * 60 * 1000));  // sleep 5 minutes

        uint32_t ts = (uint32_t)time(NULL);
        bool wifiUp = (WiFi.status() == WL_CONNECTED);
        Serial.printf("[revSyncTask] Wakeup: t=%lu WiFi=%s\n",
                      (unsigned long)ts, wifiUp ? "up" : "down");

        if (!wifiUp) continue;

        // Delta-sync revocations
        friend_mgmt_sync_revocations(SERVER_FALLBACK, VEHICLE_ID);

        // Remove expired cache entries
        friend_cache_remove_expired();

        // Remove stale revocation records older than max_key_ttl_hours
        uint16_t max_ttl_h = 720;
        {
            Preferences p;
            if (p.begin("sca_anchor", true)) {
                max_ttl_h = p.getUShort("max_ttl_h", 720);
                p.end();
            }
        }
        friend_revocation_cleanup_old((uint32_t)max_ttl_h * 3600UL);
    }
}

// =============================================================================
// setup
// =============================================================================

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Set timezone to UTC so mktime() / ft_parse_iso_to_unix() is correct.
    // Must be done before any time() or mktime() call.
    setenv("TZ", "UTC0", 1);
    tzset();

    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("\nSmart Car Anchor [FreeRTOS] - Vehicle: %s (reset: %d)\n", VEHICLE_ID, reason);
    if (reason == ESP_RST_PANIC) Serial.println("WARNING: previous reset was a CRASH");

    // Hold DW3000 in reset during init
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    pinMode(PIN_SS,  OUTPUT); digitalWrite(PIN_SS,  HIGH);
    pinMode(CAN_CS,  OUTPUT); digitalWrite(CAN_CS,  HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Seed mbedTLS CSPRNG
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    const char* pers = "anchor_rtos";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char*)pers, strlen(pers)) != 0) {
        Serial.println("mbedTLS init failed — halting"); while(1);
    }

    // Khởi tạo FreeRTOS primitives
    sysEvents   = xEventGroupCreate();
    bleQueue    = xQueueCreate(8, sizeof(BleCmdMsg));
    uwbQueue    = xQueueCreate(4, sizeof(uint8_t));
    canQueue    = xQueueCreate(4, sizeof(uint8_t));
    spiMutex    = xSemaphoreCreateMutex();
    bundleQueue = xQueueCreate(2, sizeof(friend_bundle_t));

    if (!sysEvents || !bleQueue || !uwbQueue || !canQueue || !spiMutex || !bundleQueue) {
        Serial.println("FreeRTOS primitives alloc failed — halting"); while(1);
    }

    // Load key qua SIM (nếu chưa có trong NVS) rồi start BLE — synchronous trước khi tạo tasks
    executeMainFlow();
    
    SPI.begin();

    // Init CAN
    pMcp2515   = new MCP2515(CAN_CS);
    pCanControl = new CANCommands(pMcp2515);
    if (!pCanControl->initialize(CAN_CS, CAN_100KBPS, MCP_CLOCK))
        Serial.println("CAN: init failed — continuing without CAN");

    // Tạo FreeRTOS tasks và pin vào đúng core
    xTaskCreatePinnedToCore(bleTask,           "BLE_Task",    BLE_TASK_STACK,    NULL, BLE_TASK_PRIO,    NULL, BLE_TASK_CORE);
    xTaskCreatePinnedToCore(uwbTask,           "UWB_Task",    UWB_TASK_STACK,    NULL, UWB_TASK_PRIO,    NULL, UWB_TASK_CORE);
    xTaskCreatePinnedToCore(canTask,           "CAN_Task",    CAN_TASK_STACK,    NULL, CAN_TASK_PRIO,    NULL, CAN_TASK_CORE);
    xTaskCreatePinnedToCore(friendMgmtTask,    "Friend_Task", FRIEND_TASK_STACK, NULL, FRIEND_TASK_PRIO, NULL, FRIEND_TASK_CORE);
    xTaskCreatePinnedToCore(revocationSyncTask,"RevSync_Task",REVSYNC_TASK_STACK,NULL, REVSYNC_TASK_PRIO,NULL, REVSYNC_TASK_CORE);

    Serial.println("All tasks created — FreeRTOS scheduler running");
    Serial.printf("Core 0: bleTask(P%d) friendMgmtTask(P%d) revSyncTask(P%d)\n",
                  BLE_TASK_PRIO, FRIEND_TASK_PRIO, REVSYNC_TASK_PRIO);
    Serial.printf("Core 1: uwbTask(P%d) canTask(P%d)\n",
                  UWB_TASK_PRIO, CAN_TASK_PRIO);
}

// loop() không còn cần thiết trong kiến trúc FreeRTOS
void loop() {
    vTaskDelete(NULL);
}
