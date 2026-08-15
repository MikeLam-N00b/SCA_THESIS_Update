#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
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
// Binary bundle format (106 bytes fixed):
//   [0]      version (1 byte)
//   [1..8]   friend_id (8 bytes)
//   [9..16]  vehicle_hash (8 bytes = SHA-256(VIN)[:8])
//   [17..32] friend_key (16 bytes)
//   [33..36] issued_at uint32 BE
//   [37..40] expires_at uint32 BE
//   [41]     permissions uint8
//   [42..73] sig_r (32 bytes)
//   [74..105] sig_s (32 bytes)
//
// Signing input: SHA-256(bundle[0..41])
// Verification order: version → vehicle_hash → permissions → time → revocation → ECDSA
// =============================================================================

static const char *TAG_TOKEN = "FRIEND_TOKEN";

// ── Wire-format parser ────────────────────────────────────────────────────────

/**
 * Parse the 106-byte binary bundle into a friend_bundle_t.
 */
static bool ft_parse_bundle_wire(const uint8_t *buf, size_t len,
                                  friend_bundle_t *out) {
    if (len != BUNDLE_BIN_SIZE) return false;

    out->version = buf[OFF_VERSION];
    if (out->version != BUNDLE_VERSION) return false;

    memcpy(out->friend_id,    buf + OFF_FRIEND_ID,    FRIEND_ID_LEN);
    memcpy(out->vehicle_hash, buf + OFF_VEHICLE_HASH,  8);
    memcpy(out->friend_key,   buf + OFF_FRIEND_KEY,   FRIEND_KEY_LEN);

    out->issued_at  = ft_read_be32(buf + OFF_ISSUED_AT);
    out->expires_at = ft_read_be32(buf + OFF_EXPIRES_AT);
    out->permissions = buf[OFF_PERMISSIONS];

    memcpy(out->sig_r, buf + OFF_SIG_R, 32);
    memcpy(out->sig_s, buf + OFF_SIG_S, 32);
    memcpy(out->raw,   buf,             BUNDLE_BIN_SIZE);
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
 *   2. Vehicle hash match  (SHA-256(expected_vehicle_id)[:8])
 *   3. Permission check
 *   4. Time window       (issued_at ≤ now ≤ expires_at)
 *   5. Revocation check  (NVS blacklist lookup)
 *   6. ECDSA-P256-SHA256 signature on signed_part (bundle->raw[:42])
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

    // 2. Vehicle hash — compare SHA-256(expected_vehicle_id)[:8] with bundle->vehicle_hash
    uint8_t expected_hash[32];
    mbedtls_sha256((const unsigned char *)expected_vehicle_id,
                   strlen(expected_vehicle_id), expected_hash, 0);
    if (memcmp(bundle->vehicle_hash, expected_hash, 8) != 0) {
        Serial.printf("[%s] VEHICLE_MISMATCH\n", TAG_TOKEN);
        return TOKEN_ERR_VEHICLE_MISMATCH;
    }

    // 3. Permission
    if (!(bundle->permissions & (uint8_t)requested_permission)) {
        Serial.printf("[%s] Permission denied: bundle=0x%02X req=0x%02X\n",
                      TAG_TOKEN, bundle->permissions, (uint8_t)requested_permission);
        return TOKEN_ERR_PERMISSION_DENIED;
    }

    // 4. Time window
    if (bundle->issued_at == 0 || bundle->expires_at == 0) {
        Serial.printf("[%s] TIME_INVALID: issued=%lu expires=%lu\n",
                      TAG_TOKEN, (unsigned long)bundle->issued_at, (unsigned long)bundle->expires_at);
        return TOKEN_ERR_INTERNAL;
    }
    Serial.printf("[%s] Time: now=%lu  issued=%lu  expires=%lu\n",
                  TAG_TOKEN,
                  (unsigned long)current_unix_time,
                  (unsigned long)bundle->issued_at,
                  (unsigned long)bundle->expires_at);
    if (current_unix_time < bundle->issued_at) {
        Serial.printf("[%s] NOT_YET_VALID: starts in %lu s\n",
                      TAG_TOKEN, (unsigned long)(bundle->issued_at - current_unix_time));
        return TOKEN_ERR_NOT_YET_VALID;
    }
    if (current_unix_time > bundle->expires_at) {
        Serial.printf("[%s] EXPIRED: expired %lu s ago\n",
                      TAG_TOKEN, (unsigned long)(current_unix_time - bundle->expires_at));
        return TOKEN_ERR_EXPIRED;
    }

    // 5. Revocation
    if (friend_revocation_is_revoked(bundle->friend_id)) {
        Serial.printf("[%s] Friend %02x%02x... is revoked\n",
                      TAG_TOKEN, bundle->friend_id[0], bundle->friend_id[1]);
        return TOKEN_ERR_REVOKED;
    }

    // 6. ECDSA signature on SHA-256(bundle->raw[:42]) using raw r/s
    uint8_t hash[32];
    mbedtls_sha256(bundle->raw, BUNDLE_SIGNED_PART, hash, 0);

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
    if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY) {
        mbedtls_pk_free(&pk);
        return TOKEN_ERR_INTERNAL;
    }

    mbedtls_ecdsa_context ecdsa;
    mbedtls_ecdsa_init(&ecdsa);
    ret = mbedtls_ecdsa_from_keypair(&ecdsa, mbedtls_pk_ec(pk));
    if (ret != 0) {
        mbedtls_ecdsa_free(&ecdsa); mbedtls_pk_free(&pk);
        return TOKEN_ERR_INTERNAL;
    }

    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    mbedtls_mpi_read_binary(&r, bundle->sig_r, 32);
    mbedtls_mpi_read_binary(&s, bundle->sig_s, 32);

    ret = mbedtls_ecdsa_verify(&ecdsa.MBEDTLS_PRIVATE(grp),
                               hash, 32,
                               &ecdsa.MBEDTLS_PRIVATE(Q),
                               &r, &s);

    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
    mbedtls_ecdsa_free(&ecdsa); mbedtls_pk_free(&pk);

    if (ret != 0) {
        Serial.printf("[%s] BAD_SIGNATURE: mbedtls -0x%04X\n", TAG_TOKEN, (unsigned)(-ret));
        return TOKEN_ERR_BAD_SIGNATURE;
    }

    return TOKEN_OK;
}
