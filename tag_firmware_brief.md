# Tag Firmware Extension — Friend Sharing v3

## Context

This is the **Tag firmware** for the Smart Car Access (SCA) project — a key-fob-style device the user carries. The Tag is an **ESP32-S3 Super Mini** that acts as:

- **BLE Central** — connects to the vehicle Anchor (ESP32 DevKit V1)
- **UWB Tag** — performs ranging with DW3000 anchor for proximity-based unlock
- **USB Serial slave** — receives commands from the Android app over CDC/ACM @ 115200 baud

The Tag is **NOT** a phone. The Android app on the user's phone connects to the Tag over USB, and the Tag handles all BLE/UWB interaction with the vehicle.

## Existing Code

**File**: `esp32_firmware/s3_super_mini_central/s3_super_mini_central.ino` (~740 lines)

**Existing serial protocol** (Android → Tag):
```
"SET_KEY:<32 hex chars>\n"   → store pairing key, start BLE scan
"DISCONNECT\n"               → disconnect BLE
```

**Existing serial protocol** (Tag → Android):
```
"READY"                          → boot complete, waiting for key
"KEY_OK:<hex>"                   → key accepted
"KEY_INVALID"                    → key wrong format
"SCANNING..."                    → scanning BLE for Anchor
"FOUND:<mac>"                    → Anchor found
"CONNECTED:<mac>"                → BLE connected + HMAC auth OK
"KEY_FORWARDED_TO_ANCHOR:<hex>"  → key forwarded
"DISCONNECTED"                   → BLE lost
"DISTANCE:<value>"               → UWB distance in meters
"UNLOCK:<dist>m"                 → tag in unlock zone
"LOCK:<dist>m"                   → tag exited unlock zone
```

**Existing tasks** (FreeRTOS):
- `usbSerialTask` — Core 1, parses serial commands
- `bleTask` — Core 0, BLE scan/connect/auth with Anchor
- `uwbTask` — Core 1, UWB ranging via DW3000 SPI

**Existing crypto**: HMAC-SHA256 challenge-response with Anchor (uses `pairingKey` 16 bytes as HMAC key).

**Existing data structures**:
```c
static uint8_t pairingKey[16];        // raw bytes
static char    pairingKeyHex[33];     // hex string for serial echo
```

## Goal

Extend the Tag firmware to support **Friend Sharing** with the new server v3 features:

1. Receive a complete signed **friend bundle** from app (not just the key)
2. **Verify ECDSA signature offline** using cached server public key
3. Enforce **permission bitmask** (UNLOCK / LOCK) — reject actions not granted
4. Maintain backward compatibility with `SET_KEY:<hex>` for owner pairing key

The Tag does NOT need to talk to the cloud directly — the app handles all cloud communication and forwards verified bundles to the Tag.

## Required Changes

### A. Extend Serial Protocol

**New commands** (Android → Tag):

```
"SET_BUNDLE:<base64_bundle>\n"
    → Store full friend bundle (replaces SET_KEY for friend mode)
    → Bundle is base64-encoded binary blob (see format below)
    → On success: "BUNDLE_OK:<friend_id_hex>:<perms_hex>"
    → On bad sig: "BUNDLE_INVALID:BAD_SIG"
    → On expired: "BUNDLE_INVALID:EXPIRED"
    → On bad version: "BUNDLE_INVALID:VERSION"

"SET_SERVER_PUBKEY:<base64_der>\n"
    → Cache server's ECDSA public key (DER format, ~91 bytes)
    → Sent once during pairing
    → On success: "SERVER_PUBKEY_OK"
    → On invalid: "SERVER_PUBKEY_INVALID"

"SET_TIME:<unix_seconds>\n"
    → Set RTC for expiry checks (Tag has no battery-backed RTC)
    → On success: "TIME_OK:<unix_seconds>"

"GET_STATUS\n"
    → Query current state
    → Response: "STATUS:mode=<owner|friend|none>,perms=<hex>,key=<friend_id_or_owner>,expires=<unix>"
```

**Existing commands** (`SET_KEY`, `DISCONNECT`) must remain unchanged for backward compatibility.

**New state events** (Tag → Android):

```
"BUNDLE_OK:<friend_id>:<perms>"      → bundle accepted
"BUNDLE_INVALID:<reason>"            → bundle rejected
"PERM_DENIED:<action>"               → tried action without permission
"PERM_GRANTED:<action>"              → action allowed by bundle perms
"EXPIRED"                            → current bundle expired during use
```

### B. New Data Structures

Add to top of `s3_super_mini_central.ino` (or split into `friend_bundle.h`):

```c
// =============================================================================
// Friend Bundle - matches server's signed structure
// =============================================================================

#define BUNDLE_VERSION         1
#define FRIEND_ID_LEN          8
#define FRIEND_KEY_LEN         16
#define VEHICLE_ID_MAX         32
#define ECDSA_SIG_DER_MAX      72

#define PERM_UNLOCK            0x01
#define PERM_LOCK              0x02
#define PERM_ALL               (PERM_UNLOCK | PERM_LOCK)

typedef enum {
    OP_MODE_NONE = 0,
    OP_MODE_OWNER,        // legacy SET_KEY flow
    OP_MODE_FRIEND        // new SET_BUNDLE flow
} op_mode_t;

typedef struct {
    uint8_t  version;
    uint8_t  friend_id[FRIEND_ID_LEN];
    char     vehicle_id[VEHICLE_ID_MAX];
    uint8_t  friend_key[FRIEND_KEY_LEN];
    uint8_t  permissions;
    uint32_t issued_at;       // Unix time
    uint32_t expires_at;      // Unix time
    char     issued_at_iso[32];   // for canonical message reconstruction
    char     expires_at_iso[32];
    uint8_t  issuer_sig[ECDSA_SIG_DER_MAX];
    size_t   issuer_sig_len;
} friend_bundle_t;

typedef enum {
    BV_OK = 0,
    BV_VERSION_MISMATCH,
    BV_BAD_SIGNATURE,
    BV_EXPIRED,
    BV_NOT_YET_VALID,
    BV_NO_SERVER_KEY,
    BV_PARSE_ERROR,
} bundle_verify_result_t;
```

Global state additions:

```c
static op_mode_t        currentMode      = OP_MODE_NONE;
static friend_bundle_t  activeBundle;     // valid only when currentMode == OP_MODE_FRIEND
static uint8_t          serverPublicKeyDer[128];
static size_t           serverPublicKeyDerLen = 0;
static uint32_t         currentUnixTime  = 0;  // updated by SET_TIME
static uint32_t         lastTimeUpdateMs = 0;  // millis() at last SET_TIME
```

### C. Bundle Wire Format (binary, base64-encoded over serial)

**Bundle size**: ~245 bytes raw → ~328 bytes base64.

**Layout** (little-endian where applicable):

```
Offset  Size  Field
------  ----  -----
0       1     version (uint8)
1       8     friend_id (8 bytes)
9       32    vehicle_id (null-padded ASCII)
41      16    friend_key (16 bytes)
57      1     permissions (uint8)
58      32    issued_at_iso (null-padded ASCII)
90      32    expires_at_iso (null-padded ASCII)
122     1     issuer_sig_len (uint8)
123     72    issuer_sig (variable, padded to 72)
total: 195 bytes
```

Provide parser:

```c
static bool parse_bundle_from_base64(const char *b64, friend_bundle_t *out);
```

### D. ECDSA Verification (Offline)

Use mbedTLS already linked in ESP32 Arduino core:

```c
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

static bundle_verify_result_t verify_bundle_signature(const friend_bundle_t *b) {
    if (serverPublicKeyDerLen == 0) return BV_NO_SERVER_KEY;
    if (b->version != BUNDLE_VERSION) return BV_VERSION_MISMATCH;
    
    // Reconstruct canonical message - MUST match server format byte-for-byte
    char friend_id_hex[FRIEND_ID_LEN * 2 + 1];
    char friend_key_hex[FRIEND_KEY_LEN * 2 + 1];
    bytesToHex(b->friend_id, FRIEND_ID_LEN, friend_id_hex);
    bytesToHex(b->friend_key, FRIEND_KEY_LEN, friend_key_hex);
    
    char msg[512];
    int msg_len = snprintf(msg, sizeof(msg),
        "v%u|%s|%s|%s|%s|%s|%u",
        b->version,
        friend_id_hex,
        b->vehicle_id,
        friend_key_hex,
        b->issued_at_iso,
        b->expires_at_iso,
        b->permissions);
    
    if (msg_len <= 0 || msg_len >= (int)sizeof(msg)) return BV_PARSE_ERROR;
    
    // SHA-256
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t*)msg, msg_len, hash, 0);
    
    // Parse public key + verify
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_public_key(&pk, serverPublicKeyDer, serverPublicKeyDerLen);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return BV_NO_SERVER_KEY;
    }
    
    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
                             hash, 32,
                             b->issuer_sig, b->issuer_sig_len);
    mbedtls_pk_free(&pk);
    
    return (ret == 0) ? BV_OK : BV_BAD_SIGNATURE;
}
```

**CRITICAL**: The canonical message format must be byte-exact with what the server produces. Server uses Python:
```python
f"v{version}|{friend_id}|{vehicle_id}|{friend_key_hex}|{issued_at}|{expires_at}|{permissions}".encode()
```

Note that `permissions` is rendered as **decimal integer** (e.g. `1` not `0x01`), and ISO timestamps include microseconds (e.g. `2026-04-25T10:30:00.123456`).

### E. Time Management

Tag has no battery-backed RTC. Time tracking strategy:

```c
static uint32_t get_current_unix_time() {
    if (currentUnixTime == 0) return 0;  // not yet set
    uint32_t elapsed_ms = millis() - lastTimeUpdateMs;
    return currentUnixTime + (elapsed_ms / 1000);
}

static bool is_expired(uint32_t expires_at) {
    uint32_t now = get_current_unix_time();
    if (now == 0) return false;  // can't check, assume valid (defensive)
    return now > expires_at;
}
```

App should send `SET_TIME:<unix>` after every USB connection. If time unset, expiry check is skipped (logged warning), and Tag relies on Anchor to verify.

### F. Permission Enforcement

Permission check happens at the moment a "would unlock/lock" event is detected:

```c
static bool check_permission_for_action(uint8_t requested_perm, const char *action_name) {
    if (currentMode == OP_MODE_OWNER) {
        // Owner has all permissions implicitly
        return true;
    }
    if (currentMode == OP_MODE_FRIEND) {
        if ((activeBundle.permissions & requested_perm) == 0) {
            Serial.printf("PERM_DENIED:%s\n", action_name);
            return false;
        }
        if (is_expired(activeBundle.expires_at)) {
            Serial.println("EXPIRED");
            return false;
        }
        Serial.printf("PERM_GRANTED:%s\n", action_name);
        return true;
    }
    return false;
}
```

Integrate into UWB-triggered actions:
- Before reporting `UNLOCK:<dist>m` to app → check `check_permission_for_action(PERM_UNLOCK, "UNLOCK")`
- Before reporting `LOCK:<dist>m` → check `check_permission_for_action(PERM_LOCK, "LOCK")`

If permission denied, do NOT send the action message. Instead send `PERM_DENIED:<action>`.

### G. Modify Serial Command Parser (`usbSerialTask`)

Existing function at line ~541. Extend the parser:

```c
static void usbSerialTask(void* param) {
    String buf = "";
    for (;;) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n') {
                buf.trim();
                if (buf.length() == 0) { buf = ""; continue; }

                // === EXISTING ===
                if (buf.startsWith("SET_KEY:")) {
                    // ... existing logic, BUT also set:
                    currentMode = OP_MODE_OWNER;
                    // ... rest unchanged
                }
                else if (buf == "DISCONNECT") {
                    // ... existing logic
                }
                
                // === NEW v3 commands ===
                else if (buf.startsWith("SET_BUNDLE:")) {
                    String b64 = buf.substring(11);
                    if (handle_set_bundle(b64.c_str())) {
                        currentMode = OP_MODE_FRIEND;
                        // Use friend_key as pairing key for BLE+UWB layer
                        memcpy(pairingKey, activeBundle.friend_key, 16);
                        bytesToHex(activeBundle.friend_key, 16, pairingKeyHex);
                        pairingKeyHex[32] = '\0';
                        authFailed = false;
                        xEventGroupSetBits(sysEvents, EVT_KEY_SET);
                        // (BLE auth response already echoed by handle_set_bundle)
                    }
                }
                else if (buf.startsWith("SET_SERVER_PUBKEY:")) {
                    String b64 = buf.substring(18);
                    handle_set_server_pubkey(b64.c_str());
                }
                else if (buf.startsWith("SET_TIME:")) {
                    String tStr = buf.substring(9);
                    handle_set_time(tStr.toInt());
                }
                else if (buf == "GET_STATUS") {
                    handle_get_status();
                }
                
                buf = "";
            } else {
                if (buf.length() < 512) buf += c;  // increased from 64 to fit base64 bundle
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

Implement helper functions:

```c
static bool handle_set_bundle(const char *b64) {
    friend_bundle_t b;
    if (!parse_bundle_from_base64(b64, &b)) {
        Serial.println("BUNDLE_INVALID:PARSE_ERROR");
        return false;
    }
    
    bundle_verify_result_t r = verify_bundle_signature(&b);
    if (r != BV_OK) {
        const char *reason = (r == BV_BAD_SIGNATURE) ? "BAD_SIG" :
                              (r == BV_VERSION_MISMATCH) ? "VERSION" :
                              (r == BV_NO_SERVER_KEY) ? "NO_SERVER_KEY" :
                              "UNKNOWN";
        Serial.printf("BUNDLE_INVALID:%s\n", reason);
        return false;
    }
    
    // Time-based checks
    uint32_t now = get_current_unix_time();
    if (now > 0) {
        if (now < b.issued_at) {
            Serial.println("BUNDLE_INVALID:NOT_YET_VALID");
            return false;
        }
        if (now > b.expires_at) {
            Serial.println("BUNDLE_INVALID:EXPIRED");
            return false;
        }
    }
    
    memcpy(&activeBundle, &b, sizeof(b));
    
    char fid_hex[FRIEND_ID_LEN * 2 + 1];
    bytesToHex(b.friend_id, FRIEND_ID_LEN, fid_hex);
    Serial.printf("BUNDLE_OK:%s:%02X\n", fid_hex, b.permissions);
    return true;
}

static void handle_set_server_pubkey(const char *b64) {
    size_t out_len = 0;
    int ret = mbedtls_base64_decode(serverPublicKeyDer, sizeof(serverPublicKeyDer),
                                     &out_len,
                                     (const uint8_t*)b64, strlen(b64));
    if (ret != 0 || out_len == 0) {
        serverPublicKeyDerLen = 0;
        Serial.println("SERVER_PUBKEY_INVALID");
        return;
    }
    
    // Optional: validate it parses as ECDSA P-256
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    if (mbedtls_pk_parse_public_key(&pk, serverPublicKeyDer, out_len) != 0) {
        mbedtls_pk_free(&pk);
        serverPublicKeyDerLen = 0;
        Serial.println("SERVER_PUBKEY_INVALID");
        return;
    }
    mbedtls_pk_free(&pk);
    
    serverPublicKeyDerLen = out_len;
    Serial.printf("SERVER_PUBKEY_OK:%u\n", (unsigned)out_len);
}

static void handle_set_time(uint32_t t) {
    if (t < 1700000000U) {  // sanity: must be after 2023
        Serial.println("TIME_INVALID");
        return;
    }
    currentUnixTime = t;
    lastTimeUpdateMs = millis();
    Serial.printf("TIME_OK:%u\n", t);
}

static void handle_get_status() {
    const char *mode_str = "none";
    if (currentMode == OP_MODE_OWNER) mode_str = "owner";
    else if (currentMode == OP_MODE_FRIEND) mode_str = "friend";
    
    char fid_or_owner[24] = "owner";
    uint8_t perms = 0xFF;
    uint32_t expires = 0;
    
    if (currentMode == OP_MODE_FRIEND) {
        bytesToHex(activeBundle.friend_id, FRIEND_ID_LEN, fid_or_owner);
        perms = activeBundle.permissions;
        expires = activeBundle.expires_at;
    }
    
    Serial.printf("STATUS:mode=%s,perms=%02X,key=%s,expires=%u\n",
                  mode_str, perms, fid_or_owner, expires);
}
```

### H. Update UWB Action Reporting

In `uwbInitiatorLoop()` (around line 290-310 in existing code), where the Tag currently sends `VERIFIED:` / `UNLOCK:` / `LOCK:` messages, wrap them with permission check:

```c
// Before sending UNLOCK:
if (shouldUnlock && !tagInUnlockZone) {
    if (check_permission_for_action(PERM_UNLOCK, "UNLOCK")) {
        tagInUnlockZone = true;
        BleWriteMsg wm;
        wm.len = (uint8_t)snprintf(wm.data, sizeof(wm.data),
                                    "VERIFIED:%.1fm", filtDist);
        if (connected) xQueueSend(bleWriteQueue, &wm, 0);
        Serial.printf("UNLOCK:%.1fm\n", filtDist);
    }
}

// Before sending LOCK:
if (shouldLock && tagInUnlockZone) {
    if (check_permission_for_action(PERM_LOCK, "LOCK")) {
        tagInUnlockZone = false;
        // ... send LOCK message to Anchor
        Serial.printf("LOCK:%.1fm\n", filtDist);
    }
}
```

### I. Helper Functions Needed

Add these at the top of the file (or in a new `friend_bundle.h`):

```c
static void bytesToHex(const uint8_t *bytes, size_t len, char *outHex) {
    for (size_t i = 0; i < len; i++) {
        snprintf(outHex + i * 2, 3, "%02x", bytes[i]);
    }
}

static bool parse_bundle_from_base64(const char *b64, friend_bundle_t *out) {
    uint8_t raw[256];
    size_t raw_len = 0;
    int ret = mbedtls_base64_decode(raw, sizeof(raw), &raw_len,
                                     (const uint8_t*)b64, strlen(b64));
    if (ret != 0 || raw_len < 195) return false;
    
    out->version = raw[0];
    memcpy(out->friend_id, &raw[1], 8);
    memcpy(out->vehicle_id, &raw[9], 32);
    out->vehicle_id[31] = '\0';   // ensure null-term
    memcpy(out->friend_key, &raw[41], 16);
    out->permissions = raw[57];
    memcpy(out->issued_at_iso, &raw[58], 32);
    out->issued_at_iso[31] = '\0';
    memcpy(out->expires_at_iso, &raw[90], 32);
    out->expires_at_iso[31] = '\0';
    out->issuer_sig_len = raw[122];
    if (out->issuer_sig_len > ECDSA_SIG_DER_MAX) return false;
    memcpy(out->issuer_sig, &raw[123], out->issuer_sig_len);
    
    // Parse ISO timestamps to Unix
    out->issued_at = iso_to_unix(out->issued_at_iso);
    out->expires_at = iso_to_unix(out->expires_at_iso);
    
    return true;
}

static uint32_t iso_to_unix(const char *iso) {
    // Parse "2026-04-25T10:30:00" or "2026-04-25T10:30:00.123456"
    // Use strptime if available, or manual parse
    int Y, M, D, h, m, s;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return 0;
    
    struct tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = s;
    return (uint32_t)mktime(&t);  // assumes UTC; adjust if needed
}
```

## Acceptance Criteria

The implementation is complete when:

1. ✅ `SET_KEY:<hex>` still works for owner mode (backward compat)
2. ✅ `SET_BUNDLE:<base64>` parses, verifies ECDSA, and accepts valid bundles
3. ✅ Tampered bundles (any field modified) → respond `BUNDLE_INVALID:BAD_SIG`
4. ✅ `SET_SERVER_PUBKEY:<base64>` caches DER public key correctly
5. ✅ Without cached server pubkey, `SET_BUNDLE` responds `BUNDLE_INVALID:NO_SERVER_KEY`
6. ✅ Expired bundles (when time is set) → `BUNDLE_INVALID:EXPIRED`
7. ✅ Friend bundle's `friend_key` is used as pairing key for existing BLE+UWB flow
8. ✅ `PERM_DENIED:UNLOCK` sent when bundle has only PERM_LOCK and tag enters unlock zone
9. ✅ `GET_STATUS` returns current mode + permissions
10. ✅ Existing UWB ranging + BLE auth flow unchanged (HMAC challenge-response with Anchor)
11. ✅ Serial buffer increased to ≥512 chars to fit base64 bundle (~328 chars)

## Constraints

- **Memory**: ESP32-S3 Super Mini has 512KB SRAM, plenty of room
- **mbedTLS**: Already in Arduino ESP32 core. Need `mbedtls/pk.h`, `mbedtls/sha256.h`, `mbedtls/base64.h`
- **Stack size**: bundle parsing uses ~256 bytes raw + structs. usbSerialTask stack should be ≥4KB
- **Flash**: server pubkey + active bundle live in RAM only (not NVS) — they get re-sent on each USB connect
- **No NVS persistence required**: Tag is stateless across reboots — app re-provisions on connect

## Out of Scope (DO NOT implement)

- NVS persistence of bundle/pubkey (app re-sends on each connection)
- Cloud HTTP communication from Tag (Tag has no LTE/WiFi config)
- Multiple simultaneous bundles (only one active bundle at a time)
- Revocation list on Tag (handled by Anchor via LTE polling — separate brief)
- Changes to UWB DW3000 ranging algorithm
- Changes to BLE scan / auth handshake with Anchor
- HMAC algorithm changes (already SHA-256, fine as-is)

## Code Style

- Follow existing `s3_super_mini_central.ino` conventions
- All new functions `static` unless exported
- Use `Serial.println()`/`Serial.printf()` for state messages (not ESP_LOG)
- Vietnamese comments OK (existing code has them)
- Place new functions before `setup()` or in logical groups with section comments

## Testing Approach

1. **Round-trip test**: server creates bundle → app forwards via SET_BUNDLE → Tag accepts. Use Python script to drive USB Serial.
2. **Tamper test**: modify 1 byte of base64 → Tag rejects with BAD_SIG
3. **Permission test**: bundle with `permissions=0x02` (LOCK only) → Tag sends `PERM_DENIED:UNLOCK` when entering unlock zone
4. **Expiry test**: SET_TIME to future → bundle marked expired
5. **Backward compat**: send legacy `SET_KEY:<hex>` → still works as before, sets `currentMode = OP_MODE_OWNER`

## Suggested File Structure

Keep all in one .ino file (existing style), but add section dividers:

```c
// =============================================================================
// FRIEND SHARING v3 — Bundle parsing, ECDSA verify, permission enforcement
// =============================================================================

// (data structures, parser, verifier, permission check, command handlers)

// =============================================================================
// FRIEND SHARING v3 — End
// =============================================================================
```

If em prefers split, create:
- `s3_super_mini_central/friend_bundle.h` — types
- `s3_super_mini_central/friend_bundle.cpp` — parser, verifier, permission helpers

## Critical Notes for Claude Code

1. **Canonical message format MUST match server byte-for-byte**. Server format:
   ```
   v1|a73a9fc680b792e2|VH001|9ebf37a4...|2026-04-25T10:30:00.123456|2026-04-26T10:30:00.123456|1
   ```
   Note: `permissions` is rendered as **decimal**, no leading zeros.

2. **ISO timestamps from server include microseconds**. Tag must store them as strings exactly as received and use those strings (not reformatted) when reconstructing the canonical message.

3. **Existing `pairingKey[16]` array is reused** in friend mode (set to `friend_key`). The HMAC challenge-response with Anchor doesn't need to know it's a friend key — it's just a 16-byte key from BLE/UWB perspective.

4. **Do not modify `bleTask` BLE handshake logic** — it works with whatever 16 bytes are in `pairingKey`.

5. **Do not break existing serial output format** — Android app parses `KEY_OK:`, `CONNECTED:`, `DISTANCE:`, etc. exactly.

---

**Please review existing `s3_super_mini_central.ino` carefully (especially `usbSerialTask`, `uwbInitiatorLoop`, BLE callbacks) before adding code, so the new logic integrates cleanly with the FreeRTOS task structure. Ask before changing any task priority or core assignment.**
