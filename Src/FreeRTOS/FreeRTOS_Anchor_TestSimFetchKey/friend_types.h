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
#define VEHICLE_ID_MAX      32
#define FRIEND_KEY_LEN      16
#define ECDSA_SIG_DER_MAX   72         // P-256 ECDSA DER (70–72 bytes)

// Bundle wire-format geometry
#define BUNDLE_WIRE_HEADER_LEN  123    // bytes before signature payload
#define BUNDLE_WIRE_MAX_LEN     (BUNDLE_WIRE_HEADER_LEN + ECDSA_SIG_DER_MAX)

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
    uint8_t  friend_id[FRIEND_ID_LEN];
    char     vehicle_id[VEHICLE_ID_MAX];
    uint8_t  friend_key[FRIEND_KEY_LEN];
    uint8_t  permissions;
    uint32_t issued_at;               // UTC Unix timestamp (parsed from ISO)
    uint32_t expires_at;              // UTC Unix timestamp
    uint8_t  issuer_sig[ECDSA_SIG_DER_MAX];
    size_t   issuer_sig_len;
    char     issued_at_iso[32];       // original ISO string — for canonical msg
    char     expires_at_iso[32];
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

/**
 * Parse a naive ISO 8601 string ("YYYY-MM-DDTHH:MM:SS[.ffffff]") to UTC
 * Unix timestamp. Requires the process TZ to be "UTC0" (set via tzset()).
 * Returns 0 on parse failure.
 */
static inline uint32_t ft_parse_iso_to_unix(const char *iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) return 0;
    struct tm t = {};
    t.tm_year  = y - 1900;
    t.tm_mon   = mo - 1;
    t.tm_mday  = d;
    t.tm_hour  = h;
    t.tm_min   = mi;
    t.tm_sec   = s;
    t.tm_isdst = 0;
    time_t ts = mktime(&t);  // correct when TZ=UTC0 is set in setup()
    return (ts < 0) ? 0u : (uint32_t)ts;
}
