#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <esp_err.h>
#include <time.h>
#include "friend_types.h"
#include "friend_cache.h"
#include "friend_revocation.h"
// anchor_transport.h must be included before this file to provide simHttpGet/simHttpPost.

// =============================================================================
// Friend Management — HTTP helpers, online validate (Seq 3), revocation sync (Seq 4)
//
// All HTTP functions use the SIM module (A7680C) via simHttpGet / simHttpPost.
// Authenticated endpoints use HMAC-SHA256 over
//   "{vehicle_id}|{unix_timestamp}|{body_sha256_b64}"
// keyed with the 16-byte pairing_key (shared secret from /secure-check-pairing).
//
// Called from friendMgmtTask and revocationSyncTask (see .ino).
// =============================================================================

static const char *TAG_FMGMT = "FRIEND_MGMT";

// ── HMAC / MAC helpers ────────────────────────────────────────────────────────

/**
 * Compute HMAC-SHA256(key, message), base64-encode, write to out_b64.
 * out_b64 must be ≥ 48 bytes (44 base64 chars + NUL).
 */
static void fm_hmac_b64(const uint8_t key[16], const char *message,
                         char out_b64[48]) {
    uint8_t mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md, key, 16,
                    (const unsigned char *)message, strlen(message), mac);
    size_t b64len;
    mbedtls_base64_encode((unsigned char *)out_b64, 48, &b64len, mac, 32);
    out_b64[b64len] = '\0';
}

// ── HTTP transport (via SIM module) ──────────────────────────────────────────

// Plain HTTP GET via SIM. Returns 200 on success, -1 on error.
static int fm_get(const char *url, String &out_body) {
    out_body = simHttpGet(url);
    return out_body.length() > 0 ? 200 : -1;
}

/**
 * Authenticated HTTP POST via SIM with X-Anchor-Timestamp / X-Anchor-MAC headers.
 *
 * MAC = HMAC-SHA256(pairing_key, "{vehicle_id}|{ts}|{SHA256(body)_b64}")
 *
 * Returns HTTP status code or -1 on connection error.
 */
static int fm_post_auth(const char *url, const char *body,
                         const char *vehicle_id, const uint8_t pairing_key[16],
                         String &out_body) {
    char ts[16];
    snprintf(ts, sizeof(ts), "%u", (unsigned)time(NULL));

    uint8_t body_hash[32];
    mbedtls_sha256((const unsigned char *)body, strlen(body), body_hash, 0);
    char body_sha_b64[48]; size_t b64len;
    mbedtls_base64_encode((unsigned char *)body_sha_b64, sizeof(body_sha_b64),
                          &b64len, body_hash, 32);
    body_sha_b64[b64len] = '\0';

    char mac_msg[256];
    snprintf(mac_msg, sizeof(mac_msg), "%s|%s|%s", vehicle_id, ts, body_sha_b64);
    char mac_b64[48];
    fm_hmac_b64(pairing_key, mac_msg, mac_b64);

    // Build USERDATA headers for A7680C — use literal \r\n (4 chars) as separator
    char userdata[256];
    snprintf(userdata, sizeof(userdata),
             "X-Anchor-Timestamp: %s\\r\\nX-Anchor-MAC: %s\\r\\n",
             ts, mac_b64);

    int status = 0;
    out_body = simHttpPost(url, body, userdata, &status);
    if (status == 0) status = out_body.length() > 0 ? 200 : -1;
    return status;
}

// ── Anti-replay nonce ─────────────────────────────────────────────────────────

/**
 * GET /validate-challenge/{vehicle_id} → extract nonce (32 hex chars).
 * out_nonce must be 33 bytes.
 */
static esp_err_t fm_get_nonce(const char *server_base, const char *vehicle_id,
                               char out_nonce[33]) {
    char url[256];
    snprintf(url, sizeof(url), "%s/validate-challenge/%s", server_base, vehicle_id);

    String resp;
    int code = fm_get(url, resp);
    if (code != 200) {
        Serial.printf("[%s] validate-challenge HTTP %d\n", TAG_FMGMT, code);
        return ESP_FAIL;
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, resp) != DeserializationError::Ok) return ESP_FAIL;
    const char *nonce = doc["nonce"];
    if (!nonce || strlen(nonce) < 32) return ESP_FAIL;
    strncpy(out_nonce, nonce, 32);
    out_nonce[32] = '\0';
    Serial.printf("[%s] Nonce: %.8s...\n", TAG_FMGMT, out_nonce);
    return ESP_OK;
}

// ── Sequence 3 — Online validate (cache miss) ─────────────────────────────────

/**
 * Online-validate a friend bundle against the server.
 * Called on first-time presentation (cache miss) as belt-and-suspenders after
 * the offline ECDSA check already passed.
 *
 * Flow: GET nonce → POST /validate-friend-key [X-Anchor-MAC]
 * Returns ESP_OK only when server responds HTTP 200.
 */
static esp_err_t friend_mgmt_validate_online(
        const friend_bundle_t *bundle,
        const char            *server_base,
        const char            *vehicle_id,
        const uint8_t          pairing_key[16]) {

    char nonce[33];
    if (fm_get_nonce(server_base, vehicle_id, nonce) != ESP_OK) return ESP_FAIL;

    // Base64-encode the 106-byte bundle for JSON transport
    char bundle_b64[200]; size_t b64len;
    mbedtls_base64_encode((unsigned char *)bundle_b64, sizeof(bundle_b64), &b64len,
                          bundle->raw, BUNDLE_BIN_SIZE);
    bundle_b64[b64len] = '\0';

    // Minimal request: vehicle_id + nonce + bundle_b64
    StaticJsonDocument<512> req;
    req["vehicle_id"] = vehicle_id;
    req["nonce"]      = nonce;
    req["bundle_b64"] = bundle_b64;

    char body[512];
    serializeJson(req, body, sizeof(body));

    char url[256];
    snprintf(url, sizeof(url), "%s/validate-friend-key", server_base);

    Serial.printf("[%s] POST %s\n", TAG_FMGMT, url);
    Serial.printf("[%s] Request: %.120s\n", TAG_FMGMT, body);  // first 120 chars

    String resp;
    int code = fm_post_auth(url, body, vehicle_id, pairing_key, resp);
    Serial.printf("[%s] validate-friend-key → HTTP %d\n", TAG_FMGMT, code);
    if (resp.length() > 0)
        Serial.printf("[%s] Response: %s\n", TAG_FMGMT, resp.c_str());
    return (code == 200) ? ESP_OK : ESP_FAIL;
}

// ── Sequence 4 — Report usage ─────────────────────────────────────────────────

/**
 * POST /friend-used to report a successful unlock/lock event.
 * Fire-and-forget — failure is logged but not fatal.
 * event_type: "UNLOCK" or "LOCK"
 */
static esp_err_t friend_mgmt_report_usage(
        const uint8_t  friend_id[FRIEND_ID_LEN],
        const char    *event_type,
        const char    *server_base,
        const char    *vehicle_id,
        const uint8_t  pairing_key[16]) {

    char friend_id_hex[FRIEND_ID_LEN * 2 + 1];
    ft_bytes_to_hex(friend_id, FRIEND_ID_LEN, friend_id_hex);

    char ts_iso[32] = "unknown";
    time_t now_t = time(NULL);
    struct tm *tm_info = gmtime(&now_t);
    if (tm_info) strftime(ts_iso, sizeof(ts_iso), "%Y-%m-%dT%H:%M:%S", tm_info);

    StaticJsonDocument<256> req;
    req["vehicle_id"] = vehicle_id;
    req["friend_id"]  = friend_id_hex;
    req["event_type"] = event_type;
    req["timestamp"]  = ts_iso;

    char body[256];
    serializeJson(req, body, sizeof(body));

    char url[256];
    snprintf(url, sizeof(url), "%s/friend-used", server_base);

    String resp;
    int code = fm_post_auth(url, body, vehicle_id, pairing_key, resp);
    Serial.printf("[%s] friend-used (%s) → HTTP %d\n", TAG_FMGMT, event_type, code);
    return (code == 200 || code == 201) ? ESP_OK : ESP_FAIL;
}

// ── Sequence 4 — Revocation sync ──────────────────────────────────────────────

/**
 * Delta-sync revocations from GET /cars/{vehicle_id}/revocations?since={iso}.
 * Uses "rev_last_sync" from NVS "sca_anchor" as the since-cursor.
 * Updates the local blacklist and advances the cursor on success.
 *
 * No authentication required for this endpoint (per server spec).
 * Uses DynamicJsonDocument to handle arbitrarily-sized revocation lists.
 */
static esp_err_t friend_mgmt_sync_revocations(const char *server_base,
                                               const char *vehicle_id) {
    // Load last-sync cursor from NVS
    char last_sync[32] = "1970-01-01T00:00:00";
    {
        Preferences p;
        if (p.begin("sca_anchor", true)) {
            p.getString("rev_last_sync", last_sync, sizeof(last_sync));
            p.end();
        }
    }

    Serial.printf("[%s] Revocation sync start — last_sync='%s'\n", TAG_FMGMT, last_sync);

    char url[320];
    snprintf(url, sizeof(url), "%s/cars/%s/revocations?since=%s",
             server_base, vehicle_id, last_sync);

    String resp;
    int code = fm_get(url, resp);
    if (code != 200) {
        Serial.printf("[%s] revocations HTTP %d\n", TAG_FMGMT, code);
        return ESP_FAIL;
    }

    // DynamicJsonDocument — heap allocation avoids stack pressure for large lists
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, resp) != DeserializationError::Ok) {
        Serial.printf("[%s] revocations JSON parse error\n", TAG_FMGMT);
        return ESP_FAIL;
    }

    uint32_t added = 0;
    for (JsonObject rev : doc["revocations"].as<JsonArray>()) {
        const char *fid_hex    = rev["friend_id"];
        const char *revoked_at = rev["revoked_at"];
        if (!fid_hex || strlen(fid_hex) < FRIEND_ID_LEN * 2) continue;

        uint8_t fid[FRIEND_ID_LEN];
        for (int i = 0; i < FRIEND_ID_LEN; i++)
            sscanf(fid_hex + i * 2, "%2hhx", &fid[i]);

        uint32_t revoked_ts = revoked_at ? ft_parse_iso_to_unix(revoked_at) : 0;
        if (revoked_ts == 0) revoked_ts = (uint32_t)time(NULL);

        if (friend_revocation_add(fid, revoked_ts) == ESP_OK) added++;
    }

    Serial.printf("[%s] Revocation sync done — added=%u new entries\n",
                  TAG_FMGMT, (unsigned)added);

    // Advance cursor to server_time so the next sync is a true delta
    const char *server_time = doc["server_time"];
    if (server_time) {
        Serial.printf("[%s] Cursor advanced → '%s'\n", TAG_FMGMT, server_time);
        Preferences p;
        if (p.begin("sca_anchor", false)) {
            p.putString("rev_last_sync", server_time);
            p.end();
        }
    }
    return ESP_OK;
}
