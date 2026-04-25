# Anchor Firmware Extension — Friend Sharing Feature

## Context

This is the **ESP32-S3 anchor firmware** for the **Smart Car Access (SCA)** thesis project. The system already has:

- BLE-based owner authentication with HMAC challenge-response
- UWB ranging via DWM3000 (SPI)
- CAN integration via MCP2515 (for vehicle ECU commands)
- LTE connectivity via A7680C (PPP/AT commands)
- NVS flash for persistent storage
- FreeRTOS-based task architecture
- Existing pairing flow with cloud server (FastAPI)

## Goal

Add a **Friend Sharing** feature that allows the vehicle owner to delegate temporary, permission-limited access to friends via a cloud-issued, cryptographically-signed bundle. The anchor must verify these bundles **offline** using a cached server public key, and enforce revocation via periodic polling.

## Server Reference

The backend (`main.py` v3 — already implemented) exposes these endpoints relevant to the anchor:

| Endpoint | Method | Auth | Used by anchor for |
|----------|--------|------|---------------------|
| `/owner-pairing` | POST | None | Initial pairing — extends to receive `server_signing_public_key_b64` |
| `/pairing-bootstrap` | GET | None | Alternative way to fetch server pub key |
| `/validate-challenge/{vehicle_id}` | GET | None | Get nonce before validating |
| `/validate-friend-key` | POST | X-Anchor-MAC | Online validate on cache miss |
| `/friend-used` | POST | X-Anchor-MAC | Report successful unlock |
| `/cars/{vehicle_id}/revocations?since=` | GET | None | Poll revocation list |
| `/cars/{vehicle_id}/activity` | POST | X-Anchor-MAC | Log access events |
| `/health` | GET | None | Connectivity check |

## Cryptographic Format Specification

### 1. Friend Bundle (signed by server, presented by Guest via BLE)

**Canonical signed message format** (UTF-8, exactly as below — anchor MUST reconstruct identically for ECDSA verify):

```
v{version}|{friend_id}|{vehicle_id}|{friend_key_hex}|{issued_at}|{expires_at}|{permissions}
```

Example:
```
v1|a73a9fc680b792e2|VH001|9ebf37a4b1fb0d138f...|2026-04-22T08:38:44.572182|2026-04-23T08:38:44.572192|1
```

**Field details**:
- `version`: uint8, currently `1`
- `friend_id`: 16 hex chars (8 random bytes)
- `vehicle_id`: ASCII string, max 32 chars
- `friend_key_hex`: 32 hex chars (16 bytes random key, used for future BLE session)
- `issued_at`: ISO 8601 timestamp (naive, no timezone)
- `expires_at`: ISO 8601 timestamp (naive, no timezone)
- `permissions`: integer (decimal in message), bitmask:
  - `0x01` = PERM_UNLOCK
  - `0x02` = PERM_LOCK

**Signature**: ECDSA over SECP256R1 (P-256) with SHA-256, in DER format (variable 70-72 bytes, but spec endpoint will return raw or DER — must handle DER from server).

### 2. Anchor → Server HMAC authentication

For protected endpoints (`/validate-friend-key`, `/friend-used`, `/cars/{vid}/activity`), the anchor must include these HTTP headers:

- `X-Anchor-Timestamp`: Unix timestamp as string (e.g. `"1729876543"`)
- `X-Anchor-MAC`: base64 of HMAC-SHA256 over the message:

```
{vehicle_id}|{timestamp}|{body_sha256_b64}
```

Where:
- `body_sha256_b64` = base64(SHA-256(raw_request_body_bytes))
- The HMAC key is the **`pairing_key`** (16 bytes, already received during pairing and stored in NVS)

Server allows ±5 minute timestamp window.

### 3. Anti-replay nonce flow (Sequence 3)

Before calling `/validate-friend-key`:
1. `GET /validate-challenge/{vehicle_id}` → receive `{"nonce": "32hex", "expires_in": 120}`
2. Include `nonce` field in the validate request body
3. Nonce is single-use, server-side TTL 120 seconds

## Required Changes to Anchor Firmware

### A. New Component: `friend_mgmt`

Create a new ESP-IDF component at:
```
components/friend_mgmt/
├── CMakeLists.txt
├── include/
│   ├── friend_mgmt.h          # Public API
│   ├── friend_token.h         # ECDSA verify
│   ├── friend_cache.h         # NVS friend key cache
│   └── friend_revocation.h    # NVS blacklist
├── friend_token.c
├── friend_cache.c
├── friend_revocation.c
└── friend_mgmt_task.c          # Main FreeRTOS task
```

Dependencies (add to component CMakeLists.txt): `mbedtls`, `nvs_flash`, `esp_http_client`, `json`, plus internal dependency on existing `lte_modem`, `ble_gatt`, `can_driver`.

### B. NVS Storage Schema

Add these NVS namespaces (no partition table change required, just new namespaces):

**Namespace `sca_anchor`**:
- Key `"server_pub"`: blob, 91 bytes, DER-encoded SECP256R1 public key (cached during pairing)
- Key `"bundle_ver"`: uint8, currently expected to be `1`
- Key `"max_ttl_h"`: uint16, max key TTL in hours (default 720)
- Key `"rev_last_sync"`: string, ISO timestamp of last successful revocation poll

**Namespace `sca_friends`**:
- Key format: `"f_{friend_id_hex}"` (e.g. `"f_a73a9fc680b792e2"`)
- Value: blob, struct `cached_friend_t` (defined below)

**Namespace `sca_revoked`**:
- Key format: `"r_{friend_id_hex}"`
- Value: uint32, `revoked_at` Unix timestamp

### C. Data Structures

```c
// friend_token.h
#define BUNDLE_VERSION     1
#define FRIEND_ID_LEN      8       // bytes (16 hex chars)
#define VEHICLE_ID_MAX     32
#define FRIEND_KEY_LEN     16
#define ECDSA_SIG_DER_MAX  72      // P-256 ECDSA in DER

typedef enum {
    PERM_UNLOCK = 0x01,
    PERM_LOCK   = 0x02,
} friend_permission_t;

typedef enum {
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

// What Guest sends over BLE (or what we cache after online validate)
typedef struct {
    uint8_t  version;
    uint8_t  friend_id[FRIEND_ID_LEN];
    char     vehicle_id[VEHICLE_ID_MAX];
    uint8_t  friend_key[FRIEND_KEY_LEN];
    uint8_t  permissions;
    uint32_t issued_at;          // Unix timestamp
    uint32_t expires_at;          // Unix timestamp
    uint8_t  issuer_sig[ECDSA_SIG_DER_MAX];
    size_t   issuer_sig_len;
    char     issued_at_iso[32];   // ISO string for canonical message reconstruction
    char     expires_at_iso[32];
} friend_bundle_t;

// What we store in NVS after first validation
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  friend_id[FRIEND_ID_LEN];
    uint8_t  friend_key[FRIEND_KEY_LEN];
    uint8_t  permissions;
    uint32_t issued_at;
    uint32_t expires_at;
    uint32_t last_validated;    // last online validate timestamp
    uint32_t uses_count;
} cached_friend_t;
```

### D. Required Functions (public API)

```c
// friend_token.h - Offline ECDSA verification
esp_err_t friend_token_init(void);

token_verify_result_t friend_token_verify(
    const friend_bundle_t *bundle,
    uint32_t current_unix_time,
    friend_permission_t requested_permission
);

esp_err_t friend_token_save_server_pubkey(
    const uint8_t *der_bytes, size_t len
);

esp_err_t friend_token_load_server_pubkey(
    uint8_t *out_der, size_t *inout_len
);

// friend_cache.h - NVS cache operations
esp_err_t friend_cache_get(
    const uint8_t friend_id[8], cached_friend_t *out
);

esp_err_t friend_cache_put(
    const cached_friend_t *cf
);

esp_err_t friend_cache_increment_usage(
    const uint8_t friend_id[8]
);

esp_err_t friend_cache_remove_expired(void);

// friend_revocation.h - Blacklist
bool friend_revocation_is_revoked(const uint8_t friend_id[8]);

esp_err_t friend_revocation_add(
    const uint8_t friend_id[8], uint32_t revoked_at
);

esp_err_t friend_revocation_cleanup_old(uint32_t max_ttl_seconds);

// friend_mgmt.h - Top-level orchestration
esp_err_t friend_mgmt_init(void);

esp_err_t friend_mgmt_start_tasks(void);

token_verify_result_t friend_mgmt_handle_bundle(
    const friend_bundle_t *bundle,
    friend_permission_t requested_permission
);

esp_err_t friend_mgmt_validate_online(
    const friend_bundle_t *bundle
);

esp_err_t friend_mgmt_sync_revocations(void);

esp_err_t friend_mgmt_report_usage(
    const uint8_t friend_id[8],
    const char *event_type    // "UNLOCK" or "LOCK"
);
```

### E. ECDSA Verification Implementation Notes

Use `mbedtls_pk_*` API for verification:

```c
// Pseudocode for friend_token_verify_signature():
// 1. Build canonical message string with snprintf
// 2. SHA-256 hash with mbedtls_sha256
// 3. mbedtls_pk_parse_public_key(&pk, server_pub_der, len)
// 4. mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, sig_len)
// 5. Use constant-time error handling (don't short-circuit on partial match)
```

**Critical**: The canonical message MUST be byte-exact with what the server signs. The server uses Python's `f"v{version}|{friend_id}|{vehicle_id}|{friend_key_hex}|{issued_at}|{expires_at}|{permissions}".encode()`. Any difference (extra space, different number formatting) breaks verification.

### F. New BLE GATT Service

Add a new GATT service to the existing BLE stack:

```c
#define FRIEND_SERVICE_UUID         0xFACE
#define CHAR_BUNDLE_SUBMIT_UUID     0xFA01  // Write — Guest submits bundle
#define CHAR_AUTH_STATUS_UUID       0xFA03  // Notify — verify result
```

**Bundle wire format** (binary, written in chunks if > BLE MTU):

```
[0]      version (1 byte)
[1..8]   friend_id (8 bytes)
[9..40]  vehicle_id (32 bytes, null-padded)
[41..56] friend_key (16 bytes)
[57]     permissions (1 byte)
[58..89] issued_at_iso (32 bytes ASCII, null-padded)
[90..121] expires_at_iso (32 bytes ASCII, null-padded)
[122]    issuer_sig_len (1 byte)
[123..194] issuer_sig (variable, up to 72 bytes DER)
```

Total: ~195 bytes — likely needs MTU negotiation to 247 or use `WriteLong` for fragmented write.

When characteristic is written, push parsed `friend_bundle_t` to a FreeRTOS queue consumed by `friend_mgmt_task`.

### G. friend_mgmt_task Logic Flow

```c
void friend_mgmt_task(void *arg) {
    friend_bundle_t bundle;
    while (1) {
        if (xQueueReceive(bundle_queue, &bundle, portMAX_DELAY) == pdTRUE) {
            uint32_t now = (uint32_t)time(NULL);
            
            // 1. Try offline verify first
            token_verify_result_t r = friend_token_verify(&bundle, now, PERM_UNLOCK);
            
            cached_friend_t cf;
            bool was_cached = (friend_cache_get(bundle.friend_id, &cf) == ESP_OK);
            
            if (!was_cached && r == TOKEN_OK) {
                // First time seeing this friend — try online validate as belt-and-suspenders
                if (lte_is_connected()) {
                    if (friend_mgmt_validate_online(&bundle) != ESP_OK) {
                        ble_notify_status(STATUS_REJECTED, TOKEN_ERR_INTERNAL);
                        continue;
                    }
                }
                // Cache it
                cached_friend_t new_cf = { /* ... fill from bundle ... */ };
                friend_cache_put(&new_cf);
            }
            
            if (r == TOKEN_OK) {
                // Trigger UWB ranging in existing flow
                if (uwb_check_proximity_ok()) {
                    can_send_unlock();
                    friend_cache_increment_usage(bundle.friend_id);
                    ble_notify_status(STATUS_ACCEPTED, TOKEN_OK);
                    
                    // Async report (don't block)
                    if (lte_is_connected()) {
                        friend_mgmt_report_usage(bundle.friend_id, "UNLOCK");
                    }
                } else {
                    ble_notify_status(STATUS_REJECTED, TOKEN_ERR_INTERNAL);
                }
            } else {
                ble_notify_status(STATUS_REJECTED, r);
                // Log attempt for audit
                if (lte_is_connected()) {
                    log_event_to_server(bundle.friend_id, "UNLOCK", "REJECTED", r);
                }
            }
        }
    }
}
```

### H. Revocation Sync Task

```c
void revocation_sync_task(void *arg) {
    char last_sync[32];
    if (nvs_get_str(handle, "rev_last_sync", last_sync, &len) != ESP_OK) {
        strcpy(last_sync, "1970-01-01T00:00:00");
    }
    
    while (1) {
        if (lte_is_connected()) {
            friend_mgmt_sync_revocations();
            // Cleanup expired blacklist entries
            uint16_t max_ttl_h = 720;
            nvs_get_u16(handle, "max_ttl_h", &max_ttl_h);
            friend_revocation_cleanup_old(max_ttl_h * 3600);
        }
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));  // 5 minutes
    }
}
```

### I. Pairing Flow Extension

Modify the existing pairing handler to:

1. Parse new fields from `/owner-pairing` response:
   - `server_signing_public_key_b64` (base64 of DER public key)
   - `bundle_version`
   - `max_key_ttl_hours`

2. Save to NVS:
   ```c
   uint8_t pubkey_der[128];
   size_t pubkey_len = mbedtls_base64_decode(...);
   friend_token_save_server_pubkey(pubkey_der, pubkey_len);
   nvs_set_u8(handle, "bundle_ver", bundle_version);
   nvs_set_u16(handle, "max_ttl_h", max_key_ttl_hours);
   ```

3. Initialize `friend_mgmt` after successful pairing:
   ```c
   friend_mgmt_init();
   friend_mgmt_start_tasks();
   ```

### J. HTTP Client Helpers (anchor → server)

Implement helper functions that handle HMAC authentication:

```c
esp_err_t lte_http_get_authenticated(
    const char *url,
    char *response_buf, size_t buf_len, int *out_status
);

esp_err_t lte_http_post_authenticated(
    const char *url,
    const char *body, size_t body_len,
    const char *vehicle_id,
    const uint8_t pairing_key[16],
    char *response_buf, size_t buf_len, int *out_status
);
```

The POST helper internally:
1. Computes `body_sha256_b64`
2. Builds MAC message: `"{vehicle_id}|{timestamp}|{body_sha256_b64}"`
3. Computes `X-Anchor-MAC` via HMAC-SHA256 with `pairing_key`
4. Sets headers `X-Anchor-MAC`, `X-Anchor-Timestamp`, `Content-Type: application/json`
5. Performs POST via existing LTE HTTP stack

## Acceptance Criteria

The implementation is complete when:

1. ✅ Anchor can verify a valid bundle **offline** (no network) using cached server public key
2. ✅ Tampered bundles (any field modified) are rejected with `TOKEN_ERR_BAD_SIGNATURE`
3. ✅ Expired bundles are rejected with `TOKEN_ERR_EXPIRED`
4. ✅ Bundles with `issued_at` in future are rejected with `TOKEN_ERR_NOT_YET_VALID`
5. ✅ Revoked `friend_id`s are rejected with `TOKEN_ERR_REVOKED`
6. ✅ Bundles for wrong `vehicle_id` are rejected with `TOKEN_ERR_VEHICLE_MISMATCH`
7. ✅ UNLOCK request without `PERM_UNLOCK` bit is rejected with `TOKEN_ERR_PERMISSION_DENIED`
8. ✅ Cache miss triggers online validate; cache hit skips it
9. ✅ Revocation sync runs every 5 minutes when LTE available
10. ✅ Sync uses delta query (`?since=<last_sync>`) for efficiency
11. ✅ Anchor authenticates to server via X-Anchor-MAC HMAC headers correctly
12. ✅ Server-rejected validations (HTTP 401/403) are logged but don't crash anchor
13. ✅ NVS cleanup removes expired friends and old revocation entries
14. ✅ All operations work with existing pairing/BLE/UWB/CAN tasks without conflict
15. ✅ Constant-time signature comparison (no early-exit on partial match)

## Constraints & Considerations

- **Memory**: ESP32-S3 has 512KB SRAM. Each `cached_friend_t` is ~38 bytes; budget for 50 cached friends = 2KB.
- **NVS size**: With 50 friends × ~80 bytes overhead = ~4KB. Add to existing partition table if needed.
- **mbedTLS**: Already in ESP-IDF. Enable `MBEDTLS_ECDSA_C`, `MBEDTLS_ECP_DP_SECP256R1_ENABLED`, `MBEDTLS_PK_PARSE_C` in `sdkconfig`.
- **Time sync**: Anchor must have approximately correct RTC. Use SNTP via LTE, or fetch `server_time` from `/health` endpoint and adjust.
- **BLE MTU**: Negotiate to 247 bytes if possible to fit bundle in one transfer.
- **Concurrent access**: Use mutex around NVS operations. The existing pairing flow may write NVS concurrently.
- **Power**: LTE modem should sleep between syncs. Wake only for revocation poll, validate, or report.
- **Failure modes**: If LTE unavailable during cache miss, reject the bundle (cannot validate first-time use offline). Log this as `TOKEN_ERR_INTERNAL` with reason `"lte_unavailable"`.

## Code Style

- Follow ESP-IDF coding conventions
- Use `ESP_LOGI`/`ESP_LOGW`/`ESP_LOGE` with TAG `"FRIEND_MGMT"`, `"FRIEND_TOKEN"`, etc.
- Return `esp_err_t` from all public functions except verification (which returns `token_verify_result_t`)
- Use `__attribute__((packed))` for any struct that goes to NVS or wire
- Document each public function with brief doxygen-style comment

## Out of Scope (do NOT implement)

- BLE pairing/bonding changes — use existing flow
- UWB ranging logic — call existing `uwb_check_proximity_ok()`
- CAN frame construction — call existing `can_send_unlock()` / `can_send_lock()`
- LTE modem AT commands — use existing `lte_*` API
- New partition table — use NVS namespaces in existing partition
- Secure Element integration (ATECC608) — future work
- Mobile app — separate project
- MQTT (we use HTTP polling instead)

## Testing Approach

After implementation, test with:

1. **Unit test**: Hardcode a bundle + signature generated by server, verify offline pass
2. **Tamper test**: Flip 1 bit in signature → must reject
3. **Expiry test**: Set RTC ahead of expires_at → must reject
4. **Cache miss flow**: Reset NVS friend cache, send valid bundle → must trigger online validate
5. **Revocation sync**: Trigger server revoke, wait 5 min, send same bundle → must reject
6. **Permission test**: Bundle with PERM_LOCK only, request UNLOCK → must reject

You can use Postman or curl to interact with the server alongside the anchor for end-to-end testing.

---

**Please implement this in a way that integrates cleanly with the existing codebase. Start by reviewing existing components (`pairing`, `ble_gatt`, `lte_modem`, `nvs_helpers`) so the new code follows the same patterns. Ask clarifying questions if any part of this brief is ambiguous before writing code.**
