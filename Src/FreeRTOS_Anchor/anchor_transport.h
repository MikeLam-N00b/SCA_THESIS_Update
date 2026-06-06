#pragma once
// anchor_transport.h — SIM (A7680C AT commands) + ECDH key provisioning.

#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include "anchor_config.h"
#include "anchor_crypto.h"
#include "anchor_nvs.h"
#include "friend_token.h"

static HardwareSerial simSerial(2);  // UART2 on ESP32-S3

// =============================================================================
// SIM (A7680C) AT command helpers
// =============================================================================

static String simAtCmd(const char *cmd, const char *expectToken,
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
        vTaskDelay(pdMS_TO_TICKS(10));  // yield so IDLE task can reset task watchdog
    }
    String trimmed = buf; trimmed.trim();
    Serial.printf("[AT] << %s\n", trimmed.c_str());
    return buf;
}

static bool simAtOk(const char *cmd, unsigned long timeout_ms = 5000) {
    return simAtCmd(cmd, "OK", timeout_ms).indexOf("OK") >= 0;
}

static bool simInit() {
    simSerial.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Try AT first — if module is already on, skip PWRKEY toggle
    bool ok = false;
    for (int i = 0; i < 3; i++) {
        if (simAtOk("AT", 2000)) { ok = true; break; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!ok) {
        // Module not responding — toggle PWRKEY to power it on
#if SIM_PWRKEY_PIN >= 0
        Serial.println("[SIM] Bat module qua PWRKEY...");
        pinMode(SIM_PWRKEY_PIN, OUTPUT);
        digitalWrite(SIM_PWRKEY_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite(SIM_PWRKEY_PIN, LOW);   // Pull LOW to power on
        vTaskDelay(pdMS_TO_TICKS(1200));     // Hold ~1.2s (A7680C datasheet: >1s)
        digitalWrite(SIM_PWRKEY_PIN, HIGH);  // Release

        // Wait for "RDY" from module (up to 15s), also accept if AT responds
        Serial.println("[SIM] Cho module san sang (toi da 15s)...");
        unsigned long t0 = millis();
        String bootBuf = "";
        while (millis() - t0 < 15000) {
            while (simSerial.available()) bootBuf += (char)simSerial.read();
            if (bootBuf.indexOf("RDY") >= 0) {
                Serial.println("[SIM] Module da san sang (RDY)");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        while (simSerial.available()) simSerial.read();

        // Retry AT after boot
        for (int i = 0; i < 5; i++) {
            if (simAtOk("AT", 2000)) { ok = true; break; }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
#else
        // No PWRKEY pin — fall back to fixed wait
        Serial.println("[SIM] Cho module khoi dong (10s)...");
        vTaskDelay(pdMS_TO_TICKS(10000));
        while (simSerial.available()) simSerial.read();
        for (int i = 0; i < 5; i++) {
            if (simAtOk("AT", 2000)) { ok = true; break; }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
#endif
    }

    if (!ok) { Serial.println("[SIM] LOI: AT khong phan hoi"); return false; }
    simAtOk("ATE0");  // Disable echo

    // Restore full functionality — module may be in AT+CFUN=0 (low-power) from a
    // previous simEnterSleep() call that was not undone before the ESP32 reset.
    // s_simSleeping is a RAM variable and resets to false on every boot, so simInit()
    // must always ensure CFUN=1 regardless of what s_simSleeping says.
    // Safe to send even when already in CFUN=1 (returns OK immediately).
    {
        String cfunResp = simAtCmd("AT+CFUN=1", "OK", 15000);
        if (cfunResp.indexOf("OK") < 0)
            Serial.println("[SIM] CFUN=1 no OK — continuing anyway");
        else
            vTaskDelay(pdMS_TO_TICKS(1000));  // give SIM card reader time to re-init
    }

    // SIM card reader may still be initializing after module boot — retry up to 15s
    bool simCardReady = false;
    for (int i = 0; i < 15; i++) {
        String cpin = simAtCmd("AT+CPIN?", "OK", 3000);
        if (cpin.indexOf("READY") >= 0) { simCardReady = true; break; }
        // "SIM failure" / "not inserted" may be transient — keep retrying
        // Hard errors (PIN/PUK locked) contain "SIM PIN" or "SIM PUK"
        if (cpin.indexOf("SIM PIN") >= 0 || cpin.indexOf("SIM PUK") >= 0) break;
        Serial.printf("[SIM] CPIN not ready (attempt %d/15) — waiting...\n", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!simCardReady) {
        Serial.println("[SIM] LOI: SIM chua san sang (bi khoa PIN, khong co SIM, hoac loi phần cung)");
        return false;
    }

    Serial.println("[SIM] Cho dang ky mang...");
    unsigned long t = millis();
    bool netOk = false;
    while (millis() - t < 60000) {
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

// HTTP POST via A7680C AT commands.
// extra_userdata: literal \r\n-separated headers for AT+HTTPPARA="USERDATA" (use "\\r\\n" in C string).
// status_out: if non-null, receives the HTTP status code.
// Returns response body for 2xx responses, "" on error.
static String simHttpPost(const char *url, const char *body,
                           const char *extra_userdata = nullptr,
                           int *status_out = nullptr) {
    int bodyLen = strlen(body);
    simAtOk("AT+HTTPTERM", 3000);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!simAtOk("AT+HTTPINIT")) {
        Serial.println("[HTTP] HTTPINIT failed");
        if (status_out) *status_out = -1; return "";
    }

    String urlCmd = String("AT+HTTPPARA=\"URL\",\"") + url + "\"";
    if (!simAtOk(urlCmd.c_str(), 5000)) {
        simAtOk("AT+HTTPTERM", 2000);
        if (status_out) *status_out = -1; return "";
    }
    if (!simAtOk("AT+HTTPPARA=\"CONTENT\",\"application/json\"")) {
        simAtOk("AT+HTTPTERM", 2000);
        if (status_out) *status_out = -1; return "";
    }

    // Add custom headers via USERDATA (e.g. "X-Foo: bar\\r\\nX-Baz: qux\\r\\n")
    if (extra_userdata) {
        String udCmd = String("AT+HTTPPARA=\"USERDATA\",\"") + extra_userdata + "\"";
        while (simSerial.available()) simSerial.read();
        simSerial.print(udCmd); simSerial.print("\r");
        unsigned long t2 = millis(); String buf2 = "";
        while (millis() - t2 < 5000) {
            while (simSerial.available()) buf2 += (char)simSerial.read();
            if (buf2.indexOf("OK") >= 0 || buf2.indexOf("ERROR") >= 0) break;
        }
        if (buf2.indexOf("OK") < 0) {
            Serial.println("[HTTP] USERDATA set failed");
            simAtOk("AT+HTTPTERM", 2000);
            if (status_out) *status_out = -1; return "";
        }
    }

    String dataCmd = String("AT+HTTPDATA=") + bodyLen + ",10000";
    while (simSerial.available()) simSerial.read();
    simSerial.println(dataCmd);
    unsigned long t = millis(); String buf = "";
    while (millis() - t < 8000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        if (buf.indexOf("DOWNLOAD") >= 0) break;
    }
    if (buf.indexOf("DOWNLOAD") < 0) {
        simAtOk("AT+HTTPTERM", 2000);
        if (status_out) *status_out = -1; return "";
    }

    simSerial.print(body);
    vTaskDelay(pdMS_TO_TICKS(200));
    buf = ""; t = millis();
    while (millis() - t < 5000) {
        while (simSerial.available()) buf += (char)simSerial.read();
        if (buf.indexOf("OK") >= 0) break;
    }

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
    Serial.printf("[HTTP POST] Status: %d, Len: %d\n", httpStatus, respLen);
    if (status_out) *status_out = httpStatus;
    if (httpStatus < 200 || httpStatus >= 300 || respLen <= 0) {
        simAtOk("AT+HTTPTERM", 2000); return "";
    }

    // Read exactly respLen bytes — base64 data may contain "OK"
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

// HTTP GET via A7680C AT commands. Returns response body or "" on error.
static String simHttpGet(const char *url) {
    simAtOk("AT+HTTPTERM", 3000);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!simAtOk("AT+HTTPINIT")) { Serial.println("[HTTP GET] HTTPINIT failed"); return ""; }

    String urlCmd = String("AT+HTTPPARA=\"URL\",\"") + url + "\"";
    if (!simAtOk(urlCmd.c_str(), 5000)) { simAtOk("AT+HTTPTERM", 2000); return ""; }

    while (simSerial.available()) simSerial.read();
    simSerial.println("AT+HTTPACTION=0");
    String buf = ""; unsigned long t = millis();
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
    Serial.printf("[HTTP GET] Status: %d, Len: %d\n", httpStatus, respLen);
    if (httpStatus != 200 || respLen <= 0) { simAtOk("AT+HTTPTERM", 2000); return ""; }

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

// Sync ESP32 system clock via SIM module NTP then AT+CCLK.
// Must be called after simInit() succeeds. Requires TZ=UTC0 set in setup().
static bool simSyncTime() {
    // A7680C syncs time via NITZ (network-provided) automatically on registration.
    // AT+CNTP is optional; skip AT+CNTPCID (not supported on A7680C).
    simAtCmd("AT+CNTP=\"pool.ntp.org\",0", "OK", 3000);
    String ntpResp = simAtCmd("AT+CNTP", "+CNTP:", 10000);
    if (ntpResp.indexOf("+CNTP: 1") < 0 && ntpResp.indexOf("+CNTP:1") < 0)
        Serial.println("[SIM] NTP not confirmed — using NITZ time from AT+CCLK");

    String cclk = simAtCmd("AT+CCLK?", "OK", 3000);
    // Format: +CCLK: "YY/MM/DD,HH:MM:SS+ZZ"
    int q1 = cclk.indexOf('"');
    if (q1 < 0 || (int)cclk.length() < q1 + 18) {
        Serial.println("[SIM] AT+CCLK parse error");
        return false;
    }
    String ts = cclk.substring(q1 + 1, q1 + 18);  // "YY/MM/DD,HH:MM:SS"
    struct tm tmv = {};
    tmv.tm_year = ts.substring(0, 2).toInt() + 100; // 2000-based → 1900-based
    tmv.tm_mon  = ts.substring(3, 5).toInt() - 1;
    tmv.tm_mday = ts.substring(6, 8).toInt();
    tmv.tm_hour = ts.substring(9, 11).toInt();
    tmv.tm_min  = ts.substring(12, 14).toInt();
    tmv.tm_sec  = ts.substring(15, 17).toInt();
    tmv.tm_isdst = 0;

    time_t epoch = mktime(&tmv);  // interpreted as UTC because TZ=UTC0
    if (epoch < 1000000000L) {
        Serial.println("[SIM] Invalid time from AT+CCLK");
        return false;
    }
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    Serial.printf("[SIM] Time synced: %lu (%s)\n", (unsigned long)epoch, ts.c_str());
    return true;
}

// =============================================================================
// Called from setup() before tasks start, when no key is in NVS.
// The SIM module powers up in CFUN=1 (RF on, ~30 mA) by default.
// This opens the UART and sends AT+CFUN=0 immediately so the module
// idles at ~3 mA until BLE_KEY_CHECK needs it.
static void simEarlySleep() {
    simSerial.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
    vTaskDelay(pdMS_TO_TICKS(200));
    // Try AT — module may still be booting; three attempts is enough
    for (int i = 0; i < 3; i++) {
        if (simAtOk("AT", 2000)) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    simAtCmd("AT+CFUN=0", "OK", 5000);
    s_simSleeping = true;
    // Do NOT set EVT_SIM_INITIALIZED — simInit() must still run properly
    // when BLE_KEY_CHECK triggers the full init path.
    Serial.println("[SIM] Early sleep (CFUN=0) — waiting for first TAG connect");
}

// =============================================================================
// SIM sleep / wake — call with simMutex held
// =============================================================================

// Put A7680C into minimum-functionality mode (RF off, ~3 mA).
// Clears EVT_SIM_READY so tasks that gate on it will not attempt HTTP.
static void simEnterSleep() {
    if (s_simSleeping) return;
    simAtCmd("AT+CFUN=0", "OK", 5000);
    s_simSleeping = true;
    xEventGroupClearBits(sysEvents, EVT_SIM_READY);
    Serial.println("[SIM] Sleep (CFUN=0)");
}

// Restore full functionality, re-register on network, re-establish PDP context.
// Sets EVT_SIM_READY on success.  Returns false if network registration fails.
static bool simExitSleep() {
    if (!s_simSleeping) return true;
    Serial.println("[SIM] Waking...");
    simAtCmd("AT+CFUN=1", "OK", 15000);

    bool netOk = false;
    for (int i = 0; i < 30 && !netOk; i++) {
        String r = simAtCmd("AT+CEREG?", "OK", 3000);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) netOk = true;
        else vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!netOk) { Serial.println("[SIM] Wakeup: network timeout"); return false; }

    simAtOk("AT+CGACT=1,1", 10000);
    s_simSleeping = false;
    xEventGroupSetBits(sysEvents, EVT_SIM_READY);
    Serial.println("[SIM] Awake, PDP restored");
    return true;
}

// =============================================================================
// Public SIM session API (acquire / release)
// =============================================================================
// Usage pattern:
//   if (simAcquire()) {
//       simHttpGet(...); simHttpPost(...);
//       simRelease();   // puts module back to sleep and releases mutex
//   }

static bool simAcquire(uint32_t timeoutMs = 30000) {
    if (xSemaphoreTake(simMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        Serial.println("[SIM] simAcquire: mutex timeout");
        return false;
    }
    if (!simExitSleep()) {
        xSemaphoreGive(simMutex);
        return false;
    }
    return true;
}

static void simRelease() {
    simEnterSleep();
    xSemaphoreGive(simMutex);
}

// =============================================================================
// Shared ECDH + AES-GCM key decryption
// =============================================================================

// Parse /secure-check-pairing JSON response and derive pairing key via ECDH + HKDF + AES-GCM.
// our_pk must already be generated. Returns true and writes 32-char hex to keyHexOut on success.
static bool _decryptPairingResponse(mbedtls_pk_context *our_pk,
                                    const String &respBody,
                                    char *keyHexOut) {
    StaticJsonDocument<768> resp;
    if (deserializeJson(resp, respBody) != DeserializationError::Ok) {
        Serial.println("[KEY] LOI: parse JSON response"); return false;
    }
    const char *srv_pub_b64   = resp["server_public_key_b64"];
    const char *encrypted_b64 = resp["encrypted_data_b64"];
    const char *nonce_b64     = resp["nonce_b64"];
    if (!srv_pub_b64 || !encrypted_b64 || !nonce_b64) {
        Serial.println("[KEY] LOI: thieu truong JSON"); return false;
    }

    uint8_t srv_pub_der[128]; size_t srv_pub_len;
    uint8_t encrypted[256];   size_t enc_len;
    uint8_t nonce[12];        size_t nonce_len;
    if (mbedtls_base64_decode(srv_pub_der, sizeof(srv_pub_der), &srv_pub_len,
                              (const uint8_t *)srv_pub_b64, strlen(srv_pub_b64)) != 0 ||
        mbedtls_base64_decode(encrypted, sizeof(encrypted), &enc_len,
                              (const uint8_t *)encrypted_b64, strlen(encrypted_b64)) != 0 ||
        mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len,
                              (const uint8_t *)nonce_b64, strlen(nonce_b64)) != 0 || nonce_len != 12) {
        Serial.println("[KEY] LOI: base64 decode"); return false;
    }
    if (enc_len < 17) { Serial.println("[KEY] LOI: kich thuoc payload"); return false; }

    mbedtls_pk_context srv_pk; mbedtls_pk_init(&srv_pk);
    if (mbedtls_pk_parse_public_key(&srv_pk, srv_pub_der, srv_pub_len) != 0) {
        Serial.println("[KEY] LOI: parse server pubkey");
        mbedtls_pk_free(&srv_pk); return false;
    }
    mbedtls_ecdh_context ecdh; mbedtls_ecdh_init(&ecdh);
    if (mbedtls_ecdh_get_params(&ecdh, mbedtls_pk_ec(*our_pk), MBEDTLS_ECDH_OURS)   != 0 ||
        mbedtls_ecdh_get_params(&ecdh, mbedtls_pk_ec(srv_pk),  MBEDTLS_ECDH_THEIRS) != 0) {
        Serial.println("[KEY] LOI: ECDH setup");
        mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&srv_pk); return false;
    }
    uint8_t shared_secret[32]; size_t shared_len = 0;
    if (mbedtls_ecdh_calc_secret(&ecdh, &shared_len, shared_secret, sizeof(shared_secret),
                                 mbedtls_ctr_drbg_random, &ctr_drbg) != 0 || shared_len != 32) {
        Serial.println("[KEY] LOI: ECDH calc secret");
        mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&srv_pk); return false;
    }
    mbedtls_ecdh_free(&ecdh); mbedtls_pk_free(&srv_pk);

    uint8_t kek[16];
    if (!hkdfSha256(NULL, 0, shared_secret, 32,
                    (const uint8_t *)"secure-check-kek", 16, kek, 16)) {
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
    if (deserializeJson(keyDoc, (char *)plaintext) != DeserializationError::Ok) {
        Serial.println("[KEY] LOI: parse JSON plaintext"); return false;
    }
    if (!keyDoc["paired"].as<bool>()) {
        Serial.printf("[KEY] Xe '%s' chua duoc dang ky\n", VEHICLE_ID); return false;
    }
    const char *keyHex = keyDoc["pairing_key"];
    if (!keyHex || strlen(keyHex) != 32) {
        Serial.println("[KEY] LOI: pairing_key khong hop le"); return false;
    }
    strncpy(keyHexOut, keyHex, 33);
    return true;
}

// Generate ephemeral P-256 key and base64-encode the DER public key.
// Returns pubder_len (>0) on success; caller must mbedtls_pk_free(our_pk).
static int _genEcKeyAndPubB64(mbedtls_pk_context *our_pk,
                               char *pubkey_b64, size_t b64_size) {
    if (mbedtls_pk_setup(our_pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*our_pk),
                            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) {
        Serial.println("[KEY] LOI: tao EC key"); return -1;
    }
    uint8_t pubder[128];
    int pubder_len = mbedtls_pk_write_pubkey_der(our_pk, pubder, sizeof(pubder));
    if (pubder_len < 0) { Serial.println("[KEY] LOI: export pubkey"); return -1; }

    size_t b64len;
    if (mbedtls_base64_encode((uint8_t *)pubkey_b64, b64_size, &b64len,
                              pubder + sizeof(pubder) - pubder_len, pubder_len) != 0) {
        Serial.println("[KEY] LOI: base64 encode"); return -1;
    }
    pubkey_b64[b64len] = '\0';
    return pubder_len;
}

// =============================================================================
// SIM key provisioning
// =============================================================================

static bool fetchPairingKeyViaSim(char *keyHexOut) {
    mbedtls_pk_context our_pk; mbedtls_pk_init(&our_pk);
    char pubkey_b64[200];
    if (_genEcKeyAndPubB64(&our_pk, pubkey_b64, sizeof(pubkey_b64)) < 0) {
        mbedtls_pk_free(&our_pk); return false;
    }

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

    bool ok = _decryptPairingResponse(&our_pk, respBody, keyHexOut);
    mbedtls_pk_free(&our_pk);
    return ok;
}

// Fetch server ECDSA signing public key from /pairing-bootstrap and cache in NVS.
// Call once after pairing key is fetched successfully.
static void fetchServerSigningKey() {
    String url = String(SERVER_FALLBACK) + "/pairing-bootstrap";
    String resp = simHttpGet(url.c_str());
    if (resp.length() == 0) {
        Serial.println("[KEY] /pairing-bootstrap failed — signing key not cached");
        return;
    }

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

