/**
 * ESP32_SIM_GetKey.ino — Test: lấy pairing key từ SCA server qua SIM module
 *
 * Thư viện cần cài (Arduino Library Manager):
 *   - ArduinoJson  (bblanchon/ArduinoJson)
 *   TinyGSM và ArduinoHttpClient KHÔNG cần (dùng AT commands trực tiếp)
 *   mbedTLS: có sẵn trong ESP32 Arduino core
 */

#include <HardwareSerial.h>
#include <ArduinoJson.h>

// mbedTLS (built-in ESP32 core)
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/gcm.h>
#include <mbedtls/base64.h>

// =============================================================================
// CẤU HÌNH
// =============================================================================
#define simSerial   Serial2
#define RX_PIN      8
#define TX_PIN      6
#define SIM_BAUD    115200

#define APN         "v-internet"   // Viettel; Mobifone: "m-wap"; Vinaphone: "m3-world"

#define SERVER_BASE_URL "http://139.59.232.153:8000"
#define VEHICLE_ID      "1HGBH41JXMN109186"
// =============================================================================

// ─── Helpers crypto ───────────────────────────────────────────────────────────

static bool b64Decode(const char* src, uint8_t* dst, size_t* outLen, size_t dstMax) {
    return mbedtls_base64_decode(dst, dstMax, outLen,
                                 (const uint8_t*)src, strlen(src)) == 0;
}

static bool b64Encode(const uint8_t* src, size_t srcLen, char* dst, size_t dstMax) {
    size_t outLen;
    if (mbedtls_base64_encode((uint8_t*)dst, dstMax, &outLen, src, srcLen) != 0)
        return false;
    dst[outLen] = '\0';
    return true;
}

static void hexToBytes(const char* hex, uint8_t* out, size_t len) {
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    for (size_t i = 0; i < len; i++)
        out[i] = (nibble(hex[2*i]) << 4) | nibble(hex[2*i+1]);
}

// ─── AT command helpers ────────────────────────────────────────────────────────

// Gửi lệnh AT, chờ token mong đợi trong timeout_ms, trả về toàn bộ response
static String atCmd(const char* cmd, const char* expectToken,
                    unsigned long timeout_ms = 5000) {
    while (simSerial.available()) simSerial.read();  // flush
    Serial.printf("[AT] >> %s\n", cmd);
    simSerial.println(cmd);
    unsigned long t = millis();
    String buf = "";
    while (millis() - t < timeout_ms) {
        while (simSerial.available()) buf += (char)simSerial.read();
        if (buf.indexOf(expectToken) >= 0) break;
        if (buf.indexOf("ERROR") >= 0) break;
    }
    // In response (trim để đỡ dài)
    String trimmed = buf; trimmed.trim();
    Serial.printf("[AT] << %s\n", trimmed.c_str());
    return buf;
}

static bool atOk(const char* cmd, unsigned long timeout_ms = 5000) {
    return atCmd(cmd, "OK", timeout_ms).indexOf("OK") >= 0;
}

// ─── Khởi động SIM ────────────────────────────────────────────────────────────

bool simInit() {
    simSerial.begin(SIM_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);

    Serial.println("[SIM] Cho module khoi dong (8s)...");
    delay(8000);
    while (simSerial.available()) simSerial.read();  // flush banner

    // Thử AT tối đa 5 lần
    bool atOk_ = false;
    for (int i = 0; i < 5; i++) {
        if (atOk("AT")) { atOk_ = true; break; }
        delay(1000);
    }
    if (!atOk_) { Serial.println("[SIM] LOI: AT khong phan hoi"); return false; }
    Serial.println("[SIM] AT OK");

    atOk("ATE0");  // echo off

    // ── Chẩn đoán SIM ────────────────────────────────────────────────────────
    // Kiểm tra SIM có nhận không (READY = OK, SIM PIN = bị khóa PIN)
    String cpinResp = atCmd("AT+CPIN?", "OK", 3000);
    Serial.println("[SIM] CPIN: " + cpinResp);
    if (cpinResp.indexOf("READY") < 0) {
        Serial.println("[SIM] LOI: SIM chua san sang (bi khoa PIN hoac khong co SIM)");
        return false;
    }

    // Kiểm tra signal quality (0-31 = có sóng, 99 = không có sóng)
    atCmd("AT+CSQ", "OK", 2000);

    // Đặt chế độ mạng tự động (phòng trường hợp bị set manual)
    atOk("AT+COPS=0", 5000);

    // Chờ đăng ký mạng
    // A7680C là module 4G LTE: dùng AT+CEREG (EPS) thay vì AT+CREG (CS/2G/3G)
    Serial.println("[SIM] Cho dang ky mang...");
    unsigned long t = millis();
    bool netOk = false;
    while (millis() - t < 60000) {
        // Thử CEREG (LTE) trước, fallback sang CREG (2G/3G)
        String r = atCmd("AT+CEREG?", "OK", 3000);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { netOk = true; break; }
        r = atCmd("AT+CREG?", "OK", 3000);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { netOk = true; break; }
        delay(2000);
    }
    if (!netOk) {
        // In thêm thông tin debug khi thất bại
        atCmd("AT+CSQ",    "OK", 2000);   // signal lúc thất bại
        atCmd("AT+COPS?",  "OK", 3000);   // operator hiện tại
        atCmd("AT+CNMP?",  "OK", 2000);   // network mode (2=auto, 38=LTE only)
        Serial.println("[SIM] LOI: khong co mang");
        return false;
    }
    Serial.println("[SIM] Mang OK");
    atCmd("AT+CSQ",   "OK", 2000);   // signal quality (0-31, 99=unknown)
    atCmd("AT+COPS?", "OK", 3000);   // operator name

    // Kích hoạt PDP context (dữ liệu di động)
    Serial.printf("[SIM] Ket noi data (APN: %s)...\n", APN);

    // Đặt APN
    String cgdcont = String("AT+CGDCONT=1,\"IP\",\"") + APN + "\"";
    atOk(cgdcont.c_str());

    // Kích hoạt context
    atOk("AT+CGACT=1,1", 10000);

    // Kiểm tra IP
    String ipResp = atCmd("AT+CGPADDR=1", "OK", 5000);
    Serial.println("[SIM] " + ipResp);

    return true;
}

// ─── HTTP POST bằng AT commands của A7680C ────────────────────────────────────

static String atHttpPost(const char* url, const char* body) {
    int bodyLen = strlen(body);

    // Đóng session cũ nếu có
    atOk("AT+HTTPTERM", 3000);
    delay(300);

    if (!atOk("AT+HTTPINIT")) {
        Serial.println("[AT-HTTP] HTTPINIT that bai");
        return "";
    }

    String urlCmd = String("AT+HTTPPARA=\"URL\",\"") + url + "\"";
    if (!atOk(urlCmd.c_str(), 5000)) {
        atOk("AT+HTTPTERM", 2000); return "";
    }

    if (!atOk("AT+HTTPPARA=\"CONTENT\",\"application/json\"")) {
        atOk("AT+HTTPTERM", 2000); return "";
    }

    // Gửi body data — chờ "DOWNLOAD" prompt
    String dataCmd = String("AT+HTTPDATA=") + bodyLen + ",10000";
    while (simSerial.available()) simSerial.read();
    simSerial.println(dataCmd);

    unsigned long t = millis();
    String buf = "";
    while (millis() - t < 8000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        if (buf.indexOf("DOWNLOAD") >= 0) break;
    }
    if (buf.indexOf("DOWNLOAD") < 0) {
        Serial.println("[AT-HTTP] Khong nhan duoc DOWNLOAD prompt");
        atOk("AT+HTTPTERM", 2000); return "";
    }

    simSerial.print(body);
    delay(500);
    // Chờ OK sau khi gửi data
    buf = "";
    t = millis();
    while (millis() - t < 5000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        if (buf.indexOf("OK") >= 0) break;
    }

    // Thực hiện POST (action=1)
    while (simSerial.available()) simSerial.read();
    simSerial.println("AT+HTTPACTION=1");

    // Chờ +HTTPACTION:1,<status>,<length>
    buf = "";
    t = millis();
    int httpStatus = 0, respLen = 0;
    while (millis() - t < 30000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        int idx = buf.indexOf("+HTTPACTION:");
        if (idx >= 0) {
            // +HTTPACTION:1,200,1234
            String line = buf.substring(idx);
            int c1 = line.indexOf(',');
            int c2 = line.indexOf(',', c1 + 1);
            int nl = line.indexOf('\n', c2);
            if (c1 > 0 && c2 > 0) {
                httpStatus = line.substring(c1 + 1, c2).toInt();
                respLen    = line.substring(c2 + 1, nl > 0 ? nl : c2 + 10).toInt();
                break;
            }
        }
    }

    Serial.printf("[AT-HTTP] Status: %d, Length: %d\n", httpStatus, respLen);

    if (httpStatus != 200 || respLen <= 0) {
        atOk("AT+HTTPTERM", 2000); return "";
    }

    // Đọc response — đọc đúng respLen bytes, không dùng "OK" làm điểm dừng
    // vì dữ liệu base64 có thể chứa chuỗi "OK"
    String readCmd = String("AT+HTTPREAD=0,") + respLen;
    while (simSerial.available()) simSerial.read();
    simSerial.println(readCmd);

    // Chờ header "+HTTPREAD:<start>,<len>\r\n" xuất hiện
    buf = "";
    t = millis();
    while (millis() - t < 10000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        int hdrIdx = buf.indexOf("+HTTPREAD:");
        if (hdrIdx >= 0 && buf.indexOf("\r\n", hdrIdx) >= 0) break;
    }

    int hdrIdx = buf.indexOf("+HTTPREAD:");
    int hdrEnd = buf.indexOf("\r\n", hdrIdx);
    if (hdrIdx < 0 || hdrEnd < 0) {
        Serial.println("[AT-HTTP] Khong tim thay HTTPREAD header");
        atOk("AT+HTTPTERM", 2000);
        return "";
    }

    // Lấy phần data đã đọc được sau header, rồi đọc tiếp cho đủ respLen bytes
    String respBody = buf.substring(hdrEnd + 2);
    t = millis();
    while ((int)respBody.length() < respLen && millis() - t < 10000) {
        while (simSerial.available()) respBody += (char)simSerial.read();
    }
    respBody = respBody.substring(0, respLen);
    respBody.trim();

    atOk("AT+HTTPTERM", 2000);
    return respBody;
}

// ─── Lấy pairing key từ server ────────────────────────────────────────────────

bool fetchPairingKey(uint8_t* keyOut16) {

    // ── RNG ──────────────────────────────────────────────────────────────────
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const uint8_t*)"sca_sim", 7) != 0) {
        Serial.println("[KEY] LOI: RNG"); return false;
    }

    // ── Tạo EC key pair P-256 ────────────────────────────────────────────────
    Serial.println("[KEY] Tao EC key pair...");
    mbedtls_pk_context our_pk;
    mbedtls_pk_init(&our_pk);
    if (mbedtls_pk_setup(&our_pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(our_pk),
                            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        Serial.println("[KEY] LOI: tao key");
        mbedtls_pk_free(&our_pk); return false;
    }

    uint8_t pubder[128];
    int pubder_len = mbedtls_pk_write_pubkey_der(&our_pk, pubder, sizeof(pubder));
    if (pubder_len < 0) {
        Serial.println("[KEY] LOI: export public key");
        mbedtls_pk_free(&our_pk); return false;
    }
    uint8_t* pubder_ptr = pubder + sizeof(pubder) - pubder_len;

    char pubkey_b64[200];
    if (!b64Encode(pubder_ptr, pubder_len, pubkey_b64, sizeof(pubkey_b64))) {
        Serial.println("[KEY] LOI: base64 encode");
        mbedtls_pk_free(&our_pk); return false;
    }

    // ── HTTP POST via AT commands ─────────────────────────────────────────────
    char body[512];
    {
        StaticJsonDocument<384> req;
        req["vehicle_id"]            = VEHICLE_ID;
        req["client_public_key_b64"] = pubkey_b64;
        serializeJson(req, body, sizeof(body));
    }

    String endpoint = String(SERVER_BASE_URL) + "/secure-check-pairing";
    Serial.printf("[HTTP] POST %s\n", endpoint.c_str());

    String respBody = atHttpPost(endpoint.c_str(), body);
    if (respBody.length() == 0) {
        Serial.println("[HTTP] LOI: khong nhan duoc response");
        mbedtls_pk_free(&our_pk); return false;
    }
    Serial.println("[HTTP] Response: " + respBody.substring(0, 80) + "...");

    // ── Parse JSON response ───────────────────────────────────────────────────
    StaticJsonDocument<768> resp;
    if (deserializeJson(resp, respBody) != DeserializationError::Ok) {
        Serial.println("[KEY] LOI: parse JSON phan hoi");
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
    uint8_t encrypted[256];   size_t encrypted_len;
    uint8_t nonce[12];        size_t nonce_len;

    if (!b64Decode(srv_pub_b64,   srv_pub_der, &srv_pub_len,   sizeof(srv_pub_der)) ||
        !b64Decode(encrypted_b64, encrypted,   &encrypted_len, sizeof(encrypted))   ||
        !b64Decode(nonce_b64,     nonce,        &nonce_len,     sizeof(nonce))) {
        Serial.println("[KEY] LOI: base64 decode");
        mbedtls_pk_free(&our_pk); return false;
    }

    if (nonce_len != 12 || encrypted_len < 17) {
        Serial.println("[KEY] LOI: kich thuoc khong hop le");
        mbedtls_pk_free(&our_pk); return false;
    }

    // ── Load server public key ────────────────────────────────────────────────
    mbedtls_pk_context srv_pk;
    mbedtls_pk_init(&srv_pk);
    if (mbedtls_pk_parse_public_key(&srv_pk, srv_pub_der, srv_pub_len) != 0) {
        Serial.println("[KEY] LOI: parse server public key");
        mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk); return false;
    }

    // ── ECDH ─────────────────────────────────────────────────────────────────
    mbedtls_ecdh_context ecdh;
    mbedtls_ecdh_init(&ecdh);
    if (mbedtls_ecdh_get_params(&ecdh, mbedtls_pk_ec(our_pk), MBEDTLS_ECDH_OURS)   != 0 ||
        mbedtls_ecdh_get_params(&ecdh, mbedtls_pk_ec(srv_pk), MBEDTLS_ECDH_THEIRS) != 0) {
        Serial.println("[KEY] LOI: ECDH setup");
        mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk);
        return false;
    }
    uint8_t shared_secret[32];
    size_t  shared_len = 0;
    if (mbedtls_ecdh_calc_secret(&ecdh, &shared_len, shared_secret, sizeof(shared_secret),
                                 mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        Serial.println("[KEY] LOI: ECDH tinh toan");
        mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&our_pk); mbedtls_pk_free(&srv_pk);
        return false;
    }
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&our_pk);
    mbedtls_pk_free(&srv_pk);

    // ── HKDF → KEK ───────────────────────────────────────────────────────────
    uint8_t kek[16];
    if (mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                     NULL, 0, shared_secret, 32,
                     (const uint8_t*)"secure-check-kek", 16, kek, 16) != 0) {
        Serial.println("[KEY] LOI: HKDF"); return false;
    }

    // ── AES-128-GCM decrypt ───────────────────────────────────────────────────
    size_t   ct_len   = encrypted_len - 16;
    uint8_t* ct       = encrypted;
    uint8_t* auth_tag = encrypted + ct_len;

    uint8_t plaintext[256] = {};
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128) != 0 ||
        mbedtls_gcm_auth_decrypt(&gcm, ct_len, nonce, 12,
                                 NULL, 0, auth_tag, 16,
                                 ct, plaintext) != 0) {
        Serial.println("[KEY] LOI: AES-GCM giai ma");
        mbedtls_gcm_free(&gcm); return false;
    }
    mbedtls_gcm_free(&gcm);
    plaintext[ct_len] = '\0';

    // ── Parse plaintext JSON ──────────────────────────────────────────────────
    StaticJsonDocument<512> keyDoc;
    if (deserializeJson(keyDoc, (char*)plaintext) != DeserializationError::Ok) {
        Serial.println("[KEY] LOI: parse JSON plaintext"); return false;
    }
    if (!keyDoc["paired"].as<bool>()) {
        Serial.printf("[KEY] Xe '%s' chua duoc dang ky!\n", VEHICLE_ID); return false;
    }

    const char* keyHex = keyDoc["pairing_key"];
    if (!keyHex || strlen(keyHex) != 32) {
        Serial.println("[KEY] LOI: pairing_key khong hop le"); return false;
    }
    hexToBytes(keyHex, keyOut16, 16);

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return true;
}

// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ESP32 SIM Get Key Test ===");

    if (!simInit()) {
        Serial.println("\n[FAIL] Khong the khoi dong SIM.");
        return;
    }

    uint8_t pairingKey[16];
    if (!fetchPairingKey(pairingKey)) {
        Serial.printf("\n[FAIL] Khong lay duoc key. SERVER=%s VEHICLE=%s\n",
                      SERVER_BASE_URL, VEHICLE_ID);
        return;
    }

    Serial.println("\n========== KET QUA ==========");
    Serial.printf("Vehicle ID  : %s\n", VEHICLE_ID);
    Serial.print("Pairing Key : ");
    for (int i = 0; i < 16; i++) Serial.printf("%02x", pairingKey[i]);
    Serial.println();
    Serial.println("==============================");
}

void loop() {
    delay(10000);
}
