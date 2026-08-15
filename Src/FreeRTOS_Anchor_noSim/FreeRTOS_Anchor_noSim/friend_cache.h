#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <esp_err.h>
#include "friend_types.h"

// =============================================================================
// Friend Key Cache
//
// NVS namespace: "sca_friends"  (11 chars ✓)
//
// Per-friend blob: key = 'f' + first 7 bytes of friend_id as 14 hex chars
//   → 15 chars total, exactly at the Preferences limit.
//   Value: cached_friend_t (38 bytes packed).
//
// ID index blob: key = "fc_idx"  (6 chars ✓)
//   Value: packed uint8_t[FRIEND_ID_LEN][N], N ≤ FRIEND_CACHE_MAX (400 bytes).
//   Maintained so remove_expired can iterate without ESP-IDF NVS iterator API.
// =============================================================================

static const char *TAG_CACHE = "FRIEND_CACHE";

// ── NVS key helpers ───────────────────────────────────────────────────────────

/**
 * Build a 15-char NVS key from prefix char + first 7 bytes of friend_id
 * expressed as 14 lowercase hex chars.  key[16] must be caller-provided.
 *
 * Example: 'f' + id[0..6] → "fa73a9fc680b79" (15 chars + NUL)
 */
static inline void fc_make_key(char prefix, const uint8_t fid[FRIEND_ID_LEN],
                                char key[16]) {
    static const char hex[] = "0123456789abcdef";
    key[0] = prefix;
    for (int i = 0; i < 7; i++) {
        key[1 + i * 2]     = hex[fid[i] >> 4];
        key[1 + i * 2 + 1] = hex[fid[i] & 0x0f];
    }
    key[15] = '\0';
}

// ── Index helpers ─────────────────────────────────────────────────────────────

static inline size_t fc_load_index(Preferences &p,
                                    uint8_t *buf,   // FRIEND_ID_LEN * FRIEND_CACHE_MAX
                                    size_t   buf_bytes) {
    size_t stored = p.getBytesLength("fc_idx");
    if (stored == 0 || stored > buf_bytes) return 0;
    p.getBytes("fc_idx", buf, stored);
    return stored / FRIEND_ID_LEN;
}

// ── Public API ────────────────────────────────────────────────────────────────

/**
 * Retrieve a cached friend.  Returns ESP_ERR_NOT_FOUND if not present.
 * Read-only open (RO mode avoids unnecessary NVS write-open overhead).
 */
static esp_err_t friend_cache_get(const uint8_t friend_id[FRIEND_ID_LEN],
                                   cached_friend_t *out) {
    char key[16];
    fc_make_key('f', friend_id, key);

    Preferences p;
    if (!p.begin("sca_friends", true)) return ESP_FAIL;
    size_t sz = p.getBytesLength(key);
    if (sz != sizeof(cached_friend_t)) { p.end(); return ESP_ERR_NOT_FOUND; }
    p.getBytes(key, out, sizeof(cached_friend_t));
    p.end();
    return ESP_OK;
}

/**
 * Store or overwrite a cached friend, and update the ID index.
 * Called after successful first-time validation.
 */
static esp_err_t friend_cache_put(const cached_friend_t *cf) {
    char key[16];
    fc_make_key('f', cf->friend_id, key);

    Preferences p;
    if (!p.begin("sca_friends", false)) return ESP_FAIL;

    if (!p.putBytes(key, cf, sizeof(cached_friend_t))) { p.end(); return ESP_FAIL; }

    // Append to ID index if not already present
    uint8_t idx[FRIEND_ID_LEN * FRIEND_CACHE_MAX] = {};
    size_t count = fc_load_index(p, idx, sizeof(idx));
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (memcmp(idx + i * FRIEND_ID_LEN, cf->friend_id, FRIEND_ID_LEN) == 0) {
            found = true; break;
        }
    }
    if (!found && count < FRIEND_CACHE_MAX) {
        memcpy(idx + count * FRIEND_ID_LEN, cf->friend_id, FRIEND_ID_LEN);
        p.putBytes("fc_idx", idx, (count + 1) * FRIEND_ID_LEN);
    }

    p.end();
    Serial.printf("[%s] Cached friend %02x%02x...\n",
                  TAG_CACHE, cf->friend_id[0], cf->friend_id[1]);
    return ESP_OK;
}

/**
 * Atomically increment uses_count and update last_validated timestamp.
 * Requires a read-modify-write, so the entry must already be cached.
 */
static esp_err_t friend_cache_increment_usage(const uint8_t friend_id[FRIEND_ID_LEN]) {
    cached_friend_t cf;
    esp_err_t ret = friend_cache_get(friend_id, &cf);
    if (ret != ESP_OK) return ret;
    cf.uses_count++;
    cf.last_validated = (uint32_t)time(NULL);
    return friend_cache_put(&cf);
}

/**
 * Walk the ID index and remove any entry whose expires_at < now.
 * Safe to call from any task; not from ISR.
 * Call periodically from revocationSyncTask to bound NVS growth.
 */
static esp_err_t friend_cache_remove_expired(void) {
    uint32_t now = (uint32_t)time(NULL);
    if (now < 1000000000UL) return ESP_OK;  // clock not synced — skip

    Preferences p;
    if (!p.begin("sca_friends", false)) return ESP_FAIL;

    uint8_t idx[FRIEND_ID_LEN * FRIEND_CACHE_MAX] = {};
    size_t count = fc_load_index(p, idx, sizeof(idx));

    size_t write_idx = 0;
    uint32_t removed = 0;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *fid = idx + i * FRIEND_ID_LEN;
        char key[16];
        fc_make_key('f', fid, key);

        size_t sz = p.getBytesLength(key);
        if (sz == sizeof(cached_friend_t)) {
            cached_friend_t cf;
            p.getBytes(key, &cf, sizeof(cf));
            if (cf.expires_at >= now) {
                // Still valid — keep in index
                if (write_idx != i)
                    memcpy(idx + write_idx * FRIEND_ID_LEN, fid, FRIEND_ID_LEN);
                write_idx++;
            } else {
                p.remove(key);
                removed++;
            }
        }
        // Missing blob (inconsistent index): silently drop from index
    }

    if (removed > 0 || write_idx < count) {
        p.putBytes("fc_idx", idx, write_idx * FRIEND_ID_LEN);
        Serial.printf("[%s] Removed %u expired entries (%u remain)\n",
                      TAG_CACHE, (unsigned)removed, (unsigned)write_idx);
    }
    p.end();
    return ESP_OK;
}
