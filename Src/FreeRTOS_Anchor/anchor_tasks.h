#pragma once
// anchor_tasks.h — FreeRTOS task implementations: ble, uwb, can, friendMgmt, revocationSync.

#include <WiFi.h>
#include <Preferences.h>
#include "anchor_ble.h"
#include "anchor_uwb.h"
#include "friend_cache.h"
#include "friend_revocation.h"
#include "friend_mgmt.h"

// =============================================================================
// TASK: bleTask — Core 0, Priority 3
// Handles all BLE-side work items from bleQueue on the same core as the BLE stack.
// =============================================================================

static void bleTask(void *param) {
    BleCmdMsg msg;
    static unsigned long lastAdvRefresh = 0;
    Serial.println("[bleTask] started on core " + String(xPortGetCoreID()));

    for (;;) {
        if (xQueueReceive(bleQueue, &msg, pdMS_TO_TICKS(200)) == pdTRUE) {
            switch (msg.type) {

            case BLE_SEND_CHALLENGE: {
                uint8_t myGen = msg.data[0];
                if (myGen != (uint8_t)connectionGen) {
                    Serial.printf("[BLE] Challenge gen=%u lỗi thời (hiện=%u) — bỏ qua\n",
                                  myGen, (unsigned)connectionGen);
                    break;
                }
                // setValue before delay so fallback readValue() from Tag always sees the challenge
                generateChallenge(currentChallenge, 16);
                pChallengeCharacteristic->setValue(currentChallenge, 16);
                vTaskDelay(pdMS_TO_TICKS(CHALLENGE_SEND_DELAY_MS));
                if (myGen != (uint8_t)connectionGen) {
                    Serial.printf("[BLE] Challenge gen=%u lỗi thời sau delay — không notify\n", myGen);
                    break;
                }
                pChallengeCharacteristic->notify();
                printHex("[AUTH] Key:       ", pairingKey,       16);
                printHex("[AUTH] Challenge:  ", currentChallenge, 16);
                Serial.println("[BLE] Challenge sent");
                break;
            }

            case BLE_AUTH_VERIFY: {
                printHex("[AUTH] Key:       ", pairingKey,       16);
                printHex("[AUTH] Challenge:  ", currentChallenge, 16);
                printHex("[AUTH] Tag resp:   ", msg.data,         32);
                uint8_t expected[32];
                if (!computeHMAC(pairingKey, 16, currentChallenge, 16, expected)) {
                    Serial.println("[BLE] HMAC compute failed — disconnecting");
                    pBleServer->disconnect(pBleServer->getConnId());
                    break;
                }
                printHex("[AUTH] Expected:   ", expected, 32);
                if (memcmp(msg.data, expected, 32) == 0) {
                    authenticated = true;
                    xEventGroupSetBits(sysEvents, EVT_AUTHED);
                    pAuthCharacteristic->setValue("AUTH_OK");
                    pAuthCharacteristic->notify();
                    Serial.println("[BLE] Auth OK");
                } else {
                    Serial.println("[BLE] Auth FAIL — disconnecting");
                    pAuthCharacteristic->setValue("AUTH_FAIL");
                    pAuthCharacteristic->notify();
                    vTaskDelay(pdMS_TO_TICKS(50));
                    pBleServer->disconnect(pBleServer->getConnId());
                }
                break;
            }

            case BLE_NOTIFY_UWB_ACTIVE:
                if (pCharacteristic) {
                    pCharacteristic->setValue("UWB_ACTIVE");
                    pCharacteristic->notify();
                    Serial.println("[BLE] Sent UWB_ACTIVE to Tag");
                }
                break;

            case BLE_RESTART_ADV:
                vTaskDelay(pdMS_TO_TICKS(50));
                BLEDevice::startAdvertising();
                lastAdvRefresh = millis();
                Serial.println("[BLE] Advertising restarted");
                break;
            }
        }

        // Periodic: refresh advertising if disconnected for > 10s
        // (ESP32-S3 BLE stack sometimes stops advertising after disconnect)
        if (!deviceConnected && (millis() - lastAdvRefresh > 10000)) {
            lastAdvRefresh = millis();
            BLEDevice::startAdvertising();
        }
    }
}

// =============================================================================
// TASK: uwbTask — Core 1, Priority 4 (highest)
// High priority keeps DW3000 SS-TWR timing tight (allows POLL_RX_TO_RESP_TX_DLY_UUS ~2500 µs).
// spiMutex arbitrates SPI bus with canTask.
// =============================================================================

static void uwbTask(void *param) {
    uint8_t cmd;
    Serial.println("[uwbTask] started on core " + String(xPortGetCoreID()));

    for (;;) {
        while (xQueueReceive(uwbQueue, &cmd, portMAX_DELAY) != pdTRUE || cmd != UWB_CMD_INIT);

        if (!initUWB()) {
            Serial.println("[uwbTask] Init failed — waiting for next command");
            continue;
        }

        BleCmdMsg notifyMsg = {}; notifyMsg.type = BLE_NOTIFY_UWB_ACTIVE;
        xQueueSend(bleQueue, &notifyMsg, pdMS_TO_TICKS(100));
        xEventGroupSetBits(sysEvents, EVT_UWB_ACTIVE);

        for (;;) {
            if (xQueueReceive(uwbQueue, &cmd, 0) == pdTRUE && cmd == UWB_CMD_DEINIT) break;

            if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                uwbResponderLoop();
                xSemaphoreGive(spiMutex);
                // 5ms yield lets canTask (lower priority) acquire mutex when needed
                vTaskDelay(pdMS_TO_TICKS(5));
            } else {
                Serial.println("[uwbTask] spiMutex timeout — skip iteration");
            }
        }

        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            deinitUWB();
            xSemaphoreGive(spiMutex);
        }
        xEventGroupClearBits(sysEvents, EVT_UWB_ACTIVE);
    }
}

// =============================================================================
// TASK: canTask — Core 1, Priority 2
// Waits for CAN commands; acquires spiMutex before using MCP2515 on shared SPI bus.
// =============================================================================

static void canTask(void *param) {
    uint8_t cmd;
    Serial.println("[canTask] started on core " + String(xPortGetCoreID()));

    for (;;) {
        if (xQueueReceive(canQueue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        Serial.printf("===CAN=== cmd=%d received\n", cmd); Serial.flush();

        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            Serial.println("===CAN=== spiMutex timeout"); Serial.flush();
            continue;
        }

        Serial.println("===CAN=== got mutex"); Serial.flush();

        if (xEventGroupGetBits(sysEvents) & EVT_UWB_ACTIVE) {
            dwt_forcetrxoff();
            digitalWrite(PIN_SS, HIGH);
        }

        Serial.println("===CAN=== calling unlock/lock"); Serial.flush();

        if (cmd == CAN_CMD_LOCK)   canLock();
        if (cmd == CAN_CMD_UNLOCK) canUnlock();

        Serial.println("===CAN=== done"); Serial.flush();

        xSemaphoreGive(spiMutex);
    }
}

// =============================================================================
// TASK: friendMgmtTask — Core 0, Priority 2
// Verification sequence: offline ECDSA → cache → UWB proximity → CAN unlock
// =============================================================================

static void friendMgmtTask(void *param) {
    friend_bundle_t bundle;
    Serial.printf("[friendMgmtTask] started on core %d\n", xPortGetCoreID());

    for (;;) {
        if (xQueueReceive(bundleQueue, &bundle, portMAX_DELAY) != pdTRUE) continue;

        {
            char fid_hex[FRIEND_ID_LEN * 2 + 1];
            char fkey_hex[FRIEND_KEY_LEN * 2 + 1];
            for (int i = 0; i < FRIEND_ID_LEN;  i++) snprintf(fid_hex  + i*2, 3, "%02x", bundle.friend_id[i]);
            for (int i = 0; i < FRIEND_KEY_LEN; i++) snprintf(fkey_hex + i*2, 3, "%02x", bundle.friend_key[i]);
            Serial.printf("[FRIEND] === Bundle received ===\n");
            Serial.printf("[FRIEND]   friend_id  : %s\n", fid_hex);
            Serial.printf("[FRIEND]   friend_key : %s\n", fkey_hex);
            Serial.printf("[FRIEND]   permissions: 0x%02X\n", bundle.permissions);
            Serial.printf("[FRIEND]   issued_at  : %lu\n", (unsigned long)bundle.issued_at);
            Serial.printf("[FRIEND]   expires_at : %lu\n", (unsigned long)bundle.expires_at);
        }

        uint32_t now = (uint32_t)time(NULL);
        Serial.printf("[FRIEND] Clock: now=%lu %s\n",
                      (unsigned long)now,
                      now >= 1000000000UL ? "OK" : "NOT_SYNCED");
        if (now < 1000000000UL) {
            // Clock not synced — use issued_at as temporary reference
            now = bundle.issued_at + 1;
            Serial.println("[FRIEND] Warning: clock not synced — using issued_at as reference");
        }

        // Step 1 — offline ECDSA + time + revocation
        token_verify_result_t r = friend_token_verify(&bundle, now, PERM_UNLOCK, VEHICLE_ID);
        Serial.printf("[FRIEND] Offline verify: %s (%d)\n", ft_result_str(r), (int)r);

        if (r != TOKEN_OK) {
            ble_notify_friend_status(1, r);
            continue;
        }

        // Step 2 — cache lookup; first-time use may require online validate
        cached_friend_t cf;
        bool was_cached = (friend_cache_get(bundle.friend_id, &cf) == ESP_OK);
        Serial.printf("[FRIEND] Cache: %s\n", was_cached ? "HIT" : "MISS");

        if (!was_cached) {
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("[FRIEND] Cache miss — starting online validate");
                if (friend_mgmt_validate_online(&bundle, SERVER_FALLBACK,
                                                VEHICLE_ID, pairingKey) != ESP_OK) {
                    Serial.println("[FRIEND] Server rejected bundle");
                    ble_notify_friend_status(1, TOKEN_ERR_INTERNAL);
                    continue;
                }
                Serial.println("[FRIEND] Server accepted bundle");
            } else {
                // Offline miss — trust ECDSA (already verified OK in Step 1)
                Serial.println("[FRIEND] Accept offline (revocation not checked)");
            }
        }

        // Step 3 — authorize UWB session; actual unlock happens after VERIFIED from Tag
        memcpy(s_activeFriendKey, bundle.friend_key, FRIEND_KEY_LEN);
        s_uwbFriendMode = true;
        s_friendBundleVerified = true;
        ble_notify_friend_status(0, TOKEN_OK);
        Serial.printf("[FRIEND] Bundle accepted for %02x%02x%02x%02x... — waiting for UWB\n",
                      bundle.friend_id[0], bundle.friend_id[1],
                      bundle.friend_id[2], bundle.friend_id[3]);

        // Step 4 — persist / update cache entry
        if (!was_cached) {
            cached_friend_t new_cf = {};
            new_cf.version        = bundle.version;
            memcpy(new_cf.friend_id,  bundle.friend_id,  FRIEND_ID_LEN);
            memcpy(new_cf.friend_key, bundle.friend_key, FRIEND_KEY_LEN);
            new_cf.permissions    = bundle.permissions;
            new_cf.issued_at      = bundle.issued_at;
            new_cf.expires_at     = bundle.expires_at;
            new_cf.last_validated = now;
            new_cf.uses_count     = 1;
            friend_cache_put(&new_cf);
        } else {
            friend_cache_increment_usage(bundle.friend_id);
        }

        // Step 5 — async usage report (best-effort)
        if (WiFi.status() == WL_CONNECTED) {
            friend_mgmt_report_usage(bundle.friend_id, "UNLOCK",
                                     SERVER_FALLBACK, VEHICLE_ID, pairingKey);
        }
    }
}

// =============================================================================
// TASK: revocationSyncTask — Core 0, Priority 1 (lowest)
// Polls revocations every 5 minutes and cleans up expired cache/blacklist entries.
// =============================================================================

static void revocationSyncTask(void *param) {
    Serial.printf("[revSyncTask] started on core %d\n", xPortGetCoreID());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5UL * 60 * 1000));

        uint32_t ts = (uint32_t)time(NULL);
        bool wifiUp = (WiFi.status() == WL_CONNECTED);
        Serial.printf("[revSyncTask] Wakeup: t=%lu WiFi=%s\n",
                      (unsigned long)ts, wifiUp ? "up" : "down");

        if (!wifiUp) continue;

        friend_mgmt_sync_revocations(SERVER_FALLBACK, VEHICLE_ID);
        friend_cache_remove_expired();

        uint16_t max_ttl_h = 720;
        {
            Preferences p;
            if (p.begin("sca_anchor", true)) {
                max_ttl_h = p.getUShort("max_ttl_h", 720);
                p.end();
            }
        }
        friend_revocation_cleanup_old((uint32_t)max_ttl_h * 3600UL);
    }
}
