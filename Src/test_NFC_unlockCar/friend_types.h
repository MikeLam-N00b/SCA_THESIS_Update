#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

// =============================================================================
// Friend Sharing — shared types, constants, and utility helpers
// All headers in friend_*.h include this as their base.
// =============================================================================

#define BUNDLE_VERSION      1
#define FRIEND_ID_LEN       8          // bytes → 16 lowercase hex chars
#define FRIEND_KEY_LEN      16

// Binary bundle format (106 bytes fixed)
#define BUNDLE_BIN_SIZE     106        // total bytes
#define BUNDLE_SIGNED_PART  42         // signed_part = bytes[0..41]
#define BUNDLE_SIG_SIZE     64         // raw r||s = bytes[42..105]

// Field offsets within the 106-byte bundle
#define OFF_VERSION         0
#define OFF_FRIEND_ID       1          // 8 bytes
#define OFF_VEHICLE_HASH    9          // 8 bytes = SHA-256(VIN)[:8]
#define OFF_FRIEND_KEY      17         // 16 bytes
#define OFF_ISSUED_AT       33         // uint32 BE
#define OFF_EXPIRES_AT      37         // uint32 BE
#define OFF_PERMISSIONS     41         // uint8
#define OFF_SIG_R           42         // 32 bytes
#define OFF_SIG_S           74         // 32 bytes

// NVS sizing constraints
#define FRIEND_CACHE_MAX        50
#define REVOCATION_LIST_MAX     100

// ── Access permission bitmask (mirrors server PERM_* constants) ──────────────

typedef enum : uint8_t {
    PERM_UNLOCK = 0x01,
    PERM_LOCK   = 0x02,
} friend_permission_t;

// ── Verification result codes ─────────────────────────────────────────────────

typedef enum : int {
    TOKEN_OK = 0,
    TOKEN_ERR_VERSION_MISMATCH,
    TOKEN_ERR_BAD_SIGNATURE,
    TOKEN_ERR_NOT_YET_VALID,
    TOKEN_ERR_EXPIRED,
    TOKEN_ERR_REVOKED,
    TOKEN_ERR_VEHICLE_MISMATCH,
    TOKEN_ERR_PERMISSION_DENIED,
    TOKEN_ERR_INTERNAL,
} token_verify_result_t;

static inline const char* ft_result_str(token_verify_result_t r) {
    switch (r) {
        case TOKEN_OK:                    return "OK";
        case TOKEN_ERR_VERSION_MISMATCH:  return "VERSION_MISMATCH";
        case TOKEN_ERR_BAD_SIGNATURE:     return "BAD_SIGNATURE";
        case TOKEN_ERR_NOT_YET_VALID:     return "NOT_YET_VALID";
        case TOKEN_ERR_EXPIRED:           return "EXPIRED";
        case TOKEN_ERR_REVOKED:           return "REVOKED";
        case TOKEN_ERR_VEHICLE_MISMATCH:  return "VEHICLE_MISMATCH";
        case TOKEN_ERR_PERMISSION_DENIED: return "PERMISSION_DENIED";
        case TOKEN_ERR_INTERNAL:          return "INTERNAL";
        default:                          return "UNKNOWN";
    }
}

// ── On-wire bundle (received over BLE from guest device) ─────────────────────

typedef struct {
    uint8_t  version;
    uint8_t  friend_id[FRIEND_ID_LEN];   // 8 bytes binary
    uint8_t  vehicle_hash[8];            // SHA-256(VIN)[:8]
    uint8_t  friend_key[FRIEND_KEY_LEN]; // 16 bytes AES key
    uint32_t issued_at;                  // UTC Unix epoch
    uint32_t expires_at;                 // UTC Unix epoch
    uint8_t  permissions;                // bitmask
    uint8_t  sig_r[32];                  // ECDSA r raw
    uint8_t  sig_s[32];                  // ECDSA s raw
    uint8_t  raw[BUNDLE_BIN_SIZE];       // original 106 bytes (for re-verify)
} friend_bundle_t;

// ── NVS-cached friend record ─────────────────────────────────────────────────

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  friend_id[FRIEND_ID_LEN];
    uint8_t  friend_key[FRIEND_KEY_LEN];
    uint8_t  permissions;
    uint32_t issued_at;
    uint32_t expires_at;
    uint32_t last_validated;          // Unix ts of last online check
    uint32_t uses_count;
} cached_friend_t;

// =============================================================================
// Shared utility helpers (static inline — used by friend_token.h + friend_mgmt.h)
// =============================================================================

/** Convert raw bytes to lowercase hex string. out must be 2*len+1 bytes. */
static inline void ft_bytes_to_hex(const uint8_t *in, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

/** Parse big-endian uint32 from 4 bytes. */
static inline uint32_t ft_read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/** Parse ISO-8601 UTC string "YYYY-MM-DDTHH:MM:SS" → Unix epoch.
 *  Used for revocation timestamps from the server's revocation list. */
static inline uint32_t ft_parse_iso_to_unix(const char *iso) {
    struct tm t = {};
    int y, mo, d, h, mi, s;
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6)
        return 0;
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h;        t.tm_min  = mi;     t.tm_sec  = s;
    return (uint32_t)mktime(&t);
}
