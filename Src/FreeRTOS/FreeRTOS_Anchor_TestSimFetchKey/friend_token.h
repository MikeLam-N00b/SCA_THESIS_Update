#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <esp_err.h>
#include "friend_types.h"
#include "friend_revocation.h"   // friend_revocation_is_revoked()

// =============================================================================
// Friend Token Verification
//
// Offline ECDSA-P256-SHA256 verification against the server's signing public
// key, which is cached in NVS namespace "sca_anchor" (key "server_pub").
//
// The canonical signed message format MUST match the server exactly:
//   "v{version}|{friend_id_hex}|{vehicle_id}|{friend_key_hex}|{issued_at_iso}|{expires_at_iso}|{permissions}"
//
// Verification order: version → vehicle_id → permissions → time → revocation → ECDSA
// Earlier checks are cheap; ECDSA is last to avoid wasting CPU on invalid bundles.
// =============================================================================

static const char *TAG_TOKEN = "FRIEND_TOKEN";

// ── Wire-format parser ────────────────────────────────────────────────────────

/**
 * Parse the BLE binary wire format into a friend_bundle_t.
 *
 * Offsets (0-based):
 *  [0]       version (1 byte)
 *  [1..8]    friend_id (8 bytes)
 *  [9..40]   vehicle_id (32 bytes, null-padded)
 *  [41..56]  friend_key (16 bytes)
 *  [57]      permissions (1 byte)
 *  [58..89]  issued_at_iso (32 bytes, null-padded)
 *  [90..121] expires_at_iso (32 bytes, null-padded)
 *  [122]     issuer_sig_len (1 byte)
 *  [123..]   issuer_sig (DER, up to 72 bytes)
 */
static bool ft_parse_bundle_wire(const uint8_t *buf, size_t len,
                                  friend_bundle_t *out) {
    if (len < BUNDLE_WIRE_HEADER_LEN) return false;
    uint8_t sig_len = buf[122];
    if (sig_len == 0 || sig_len > ECDSA_SIG_DER_MAX) return false;
    if (len < (size_t)(BUNDLE_WIRE_HEADER_LEN + sig_len)) return false;

    out->version     = buf[0];
    memcpy(out->friend_id,  buf + 1,  FRIEND_ID_LEN);
    memcpy(out->vehicle_id, buf + 9,  VEHICLE_ID_MAX - 1);
    out->vehicle_id[VEHICLE_ID_MAX - 1] = '\0';
    memcpy(out->friend_key, buf + 41, FRIEND_KEY_LEN);
    out->permissions = buf[57];

    // Copy ISO strings; ensure null-termination within our buffer
    memcpy(out->issued_at_iso,  buf + 58, 31);  out->issued_at_iso[31]  = '\0';
    memcpy(out->expires_at_iso, buf + 90, 31);  out->expires_at_iso[31] = '\0';

    out->issuer_sig_len = sig_len;
    memcpy(out->issuer_sig, buf + 123, sig_len);

    // Parse ISO → Unix for time comparisons
    out->issued_at  = ft_parse_iso_to_unix(out->issued_at_iso);
    out->expires_at = ft_parse_iso_to_unix(out->expires_at_iso);
    return true;
}

// ── Server public key NVS storage ────────────────────────────────────────────

/**
 * Persist the server ECDSA signing public key (DER-encoded SubjectPublicKeyInfo)
 * in NVS.  Called once during pairing bootstrap, then used for every offline verify.
 */
static esp_err_t friend_token_save_server_pubkey(const uint8_t *der_bytes,
                                                   size_t len) {
    if (len == 0 || len > 128) return ESP_ERR_INVALID_ARG;
    Preferences p;
    if (!p.begin("sca_anchor", false)) return ESP_FAIL;
    bool ok = p.putBytes("server_pub", der_bytes, len);
    p.end();
    if (ok) Serial.printf("[%s] Server pubkey saved (%u bytes DER)\n",
                          TAG_TOKEN, (unsigned)len);
    return ok ? ESP_OK : ESP_FAIL;
}

/** Load the cached server public key.  *inout_len is both the buffer size (in)
 *  and the actual bytes read (out).  Returns ESP_ERR_NOT_FOUND if not cached. */
static esp_err_t friend_token_load_server_pubkey(uint8_t *out_der,
                                                   size_t  *inout_len) {
    Preferences p;
    if (!p.begin("sca_anchor", true)) return ESP_FAIL;
    size_t stored = p.getBytesLength("server_pub");
    if (stored == 0 || stored > *inout_len) { p.end(); return ESP_ERR_NOT_FOUND; }
    p.getBytes("server_pub", out_der, stored);
    *inout_len = stored;
    p.end();
    return ESP_OK;
}

// ── Main verification ─────────────────────────────────────────────────────────

/**
 * Verify a friend bundle completely offline.
 *
 * Steps (cheap-to-expensive order):
 *   1. Bundle version
 *   2. Vehicle ID match  (prevents cross-vehicle bundle reuse)
 *   3. Permission check
 *   4. Time window       (issued_at ≤ now ≤ expires_at)
 *   5. Revocation check  (NVS blacklist lookup)
 *   6. ECDSA-P256-SHA256 signature  (mbedtls_pk_verify — most expensive)
 *
 * Constant-time semantics: mbedtls_pk_verify internally uses constant-time
 * comparison for the ECDSA core; we don't short-circuit on partial results.
 *
 * @param expected_vehicle_id  Must equal VEHICLE_ID from anchor_config.h.
 */
static token_verify_result_t friend_token_verify(
        const friend_bundle_t  *bundle,
        uint32_t                current_unix_time,
        friend_permission_t     requested_permission,
        const char             *expected_vehicle_id) {

    // 1. Version
    if (bundle->version != BUNDLE_VERSION) {
        Serial.printf("[%s] Version mismatch: got %u, want %u\n",
                      TAG_TOKEN, bundle->version, BUNDLE_VERSION);
        return TOKEN_ERR_VERSION_MISMATCH;
    }

    // 2. Vehicle ID — null-safe comparison up to VEHICLE_ID_MAX
    if (strncmp(bundle->vehicle_id, expected_vehicle_id, VEHICLE_ID_MAX) != 0) {
        Serial.printf("[%s] Vehicle mismatch: '%s'\n", TAG_TOKEN, bundle->vehicle_id);
        return TOKEN_ERR_VEHICLE_MISMATCH;
    }

    // 3. Permission
    if (!(bundle->permissions & (uint8_t)requested_permission)) {
        Serial.printf("[%s] Permission denied: bundle=0x%02X req=0x%02X\n",
                      TAG_TOKEN, bundle->permissions, (uint8_t)requested_permission);
        return TOKEN_ERR_PERMISSION_DENIED;
    }

    // 4. Time window
    if (bundle->issued_at == 0 || bundle->expires_at == 0) return TOKEN_ERR_INTERNAL;
    if (current_unix_time < bundle->issued_at)  return TOKEN_ERR_NOT_YET_VALID;
    if (current_unix_time > bundle->expires_at) return TOKEN_ERR_EXPIRED;

    // 5. Revocation
    if (friend_revocation_is_revoked(bundle->friend_id)) {
        Serial.printf("[%s] Friend %02x%02x... is revoked\n",
                      TAG_TOKEN, bundle->friend_id[0], bundle->friend_id[1]);
        return TOKEN_ERR_REVOKED;
    }

    // 6. ECDSA signature — reconstruct the exact canonical message the server signed
    char friend_id_hex[FRIEND_ID_LEN  * 2 + 1];
    char friend_key_hex[FRIEND_KEY_LEN * 2 + 1];
    ft_bytes_to_hex(bundle->friend_id,  FRIEND_ID_LEN,  friend_id_hex);
    ft_bytes_to_hex(bundle->friend_key, FRIEND_KEY_LEN, friend_key_hex);

    // Buffer: "v1|{16}|{≤31}|{32}|{≤31}|{≤31}|{≤3}" ≈ 153 chars max → 256 safe
    char msg[256];
    snprintf(msg, sizeof(msg), "v%d|%s|%s|%s|%s|%s|%d",
             (int)bundle->version,
             friend_id_hex,
             bundle->vehicle_id,
             friend_key_hex,
             bundle->issued_at_iso,
             bundle->expires_at_iso,
             (int)bundle->permissions);

    // SHA-256 of the canonical message
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)msg, strlen(msg), hash, 0);

    // Load and parse cached server public key
    uint8_t pub_der[128];
    size_t  pub_len = sizeof(pub_der);
    if (friend_token_load_server_pubkey(pub_der, &pub_len) != ESP_OK) {
        Serial.printf("[%s] Server pubkey not in NVS — pair first\n", TAG_TOKEN);
        return TOKEN_ERR_INTERNAL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_public_key(&pk, pub_der, pub_len);
    if (ret != 0) {
        Serial.printf("[%s] pk_parse failed: -0x%04X\n", TAG_TOKEN, (unsigned)(-ret));
        mbedtls_pk_free(&pk);
        return TOKEN_ERR_INTERNAL;
    }

    // mbedtls_pk_verify uses constant-time ECDSA comparison internally
    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32,
                             bundle->issuer_sig, bundle->issuer_sig_len);
    mbedtls_pk_free(&pk);

    if (ret != 0) {
        Serial.printf("[%s] Signature invalid: -0x%04X\n", TAG_TOKEN, (unsigned)(-ret));
        return TOKEN_ERR_BAD_SIGNATURE;
    }

    return TOKEN_OK;
}
