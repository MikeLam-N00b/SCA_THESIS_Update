#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <esp_err.h>
#include "friend_types.h"

// =============================================================================
// Friend Revocation Blacklist
//
// NVS namespace: "sca_revoked"  (11 chars ✓)
// Index key:     "rv_idx"       ( 6 chars ✓)
//   Blob value: packed array of revocation_entry_t[N], N ≤ REVOCATION_LIST_MAX
//
// All operations use Preferences (Arduino wrapper around ESP-IDF NVS).
// NVS is internally thread-safe; no extra mutex needed for single-key ops.
// =============================================================================

static const char *TAG_REVOC = "FRIEND_REVOC";

/** Packed entry stored in the "rv_idx" blob. */
typedef struct __attribute__((packed)) {
    uint8_t  friend_id[FRIEND_ID_LEN];
    uint32_t revoked_at;              // Unix timestamp when revoked
} revocation_entry_t;

// ── Internal helper ───────────────────────────────────────────────────────────

static inline size_t revoc_load_index(Preferences &p,
                                       revocation_entry_t *buf,
                                       size_t buf_count) {
    size_t blob_len = p.getBytesLength("rv_idx");
    size_t max_bytes = buf_count * sizeof(revocation_entry_t);
    if (blob_len == 0 || blob_len > max_bytes) return 0;
    p.getBytes("rv_idx", buf, blob_len);
    return blob_len / sizeof(revocation_entry_t);
}

// ── Public API ────────────────────────────────────────────────────────────────

/**
 * Return true if friend_id is in the local revocation blacklist.
 * O(N) scan, N ≤ REVOCATION_LIST_MAX — acceptable for ≤ 100 entries.
 */
static bool friend_revocation_is_revoked(const uint8_t friend_id[FRIEND_ID_LEN]) {
    Preferences p;
    if (!p.begin("sca_revoked", true)) return false;

    revocation_entry_t buf[REVOCATION_LIST_MAX];
    size_t count = revoc_load_index(p, buf, REVOCATION_LIST_MAX);
    p.end();

    for (size_t i = 0; i < count; i++) {
        if (memcmp(buf[i].friend_id, friend_id, FRIEND_ID_LEN) == 0) return true;
    }
    return false;
}

/**
 * Add a friend_id to the blacklist.
 * If already present, updates revoked_at and returns ESP_OK.
 */
static esp_err_t friend_revocation_add(const uint8_t friend_id[FRIEND_ID_LEN],
                                        uint32_t revoked_at) {
    Preferences p;
    if (!p.begin("sca_revoked", false)) return ESP_FAIL;

    revocation_entry_t buf[REVOCATION_LIST_MAX] = {};
    size_t count = revoc_load_index(p, buf, REVOCATION_LIST_MAX);

    // Update if already present
    for (size_t i = 0; i < count; i++) {
        if (memcmp(buf[i].friend_id, friend_id, FRIEND_ID_LEN) == 0) {
            buf[i].revoked_at = revoked_at;
            p.putBytes("rv_idx", buf, count * sizeof(revocation_entry_t));
            p.end();
            return ESP_OK;
        }
    }

    if (count >= REVOCATION_LIST_MAX) {
        Serial.printf("[%s] List full (%u entries)\n", TAG_REVOC, (unsigned)count);
        p.end();
        return ESP_ERR_NO_MEM;
    }

    memcpy(buf[count].friend_id, friend_id, FRIEND_ID_LEN);
    buf[count].revoked_at = revoked_at;
    count++;

    bool ok = p.putBytes("rv_idx", buf, count * sizeof(revocation_entry_t));
    p.end();

    Serial.printf("[%s] Revoked friend %02x%02x... (%u total)\n",
                  TAG_REVOC, friend_id[0], friend_id[1], (unsigned)count);
    return ok ? ESP_OK : ESP_FAIL;
}

/**
 * Remove revocation entries whose revoked_at is older than max_ttl_seconds.
 * After max_ttl_seconds the underlying key has certainly expired anyway,
 * so the revocation record serves no purpose.
 * Safe to call from any task; not from ISR.
 */
static esp_err_t friend_revocation_cleanup_old(uint32_t max_ttl_seconds) {
    uint32_t now = (uint32_t)time(NULL);
    if (now < 1000000000UL) return ESP_OK;  // clock not yet synced

    Preferences p;
    if (!p.begin("sca_revoked", false)) return ESP_FAIL;

    revocation_entry_t buf[REVOCATION_LIST_MAX] = {};
    size_t count = revoc_load_index(p, buf, REVOCATION_LIST_MAX);

    size_t write_idx = 0;
    for (size_t i = 0; i < count; i++) {
        // Keep entries that are still within the max TTL window
        if (buf[i].revoked_at + max_ttl_seconds >= now) {
            buf[write_idx++] = buf[i];
        }
    }

    if (write_idx < count) {
        p.putBytes("rv_idx", buf, write_idx * sizeof(revocation_entry_t));
        Serial.printf("[%s] Cleaned %u old entries (%u remain)\n",
                      TAG_REVOC, (unsigned)(count - write_idx), (unsigned)write_idx);
    }
    p.end();
    return ESP_OK;
}
