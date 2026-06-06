#pragma once
// anchor_tasks.h — FreeRTOS task implementations: ble, uwb, can, simInit, friendMgmt, revocationSync.

#include <Preferences.h>
#include "anchor_ble.h"
#include "anchor_uwb.h"
#include "friend_cache.h"
#include "friend_revocation.h"
#include "friend_mgmt.h"

// =============================================================================
// TASK: simInitTask — Core 0, Priority 1 (one-shot)
// Background SIM init so setup() / BLE are not blocked when key is already in NVS.
// Sets EVT_SIM_READY on success; tasks that need network wait on this bit.
// =============================================================================

static void simInitTask(void *param) {
    Serial.printf("[simInitTask] started on core %d\n", xPortGetCoreID());
    if (simInit()) {
        // EVT_SIM_INITIALIZED is permanent — signals that simSerial is open and module
        // was successfully registered. Used by BLE_KEY_CHECK to gate simAcquire().
        xEventGroupSetBits(sysEvents, EVT_SIM_INITIALIZED);
        simSyncTime();
        xEventGroupSetBits(sysEvents, EVT_SIM_READY);
        Serial.printf("[simInitTask] SIM ready — online ops enabled (t=%lu)\n",
                      (unsigned long)time(NULL));
        // Time synced — sleep SIM until revocationSyncTask or friendMgmtTask needs it
        simEnterSleep();
    } else {
        Serial.println("[simInitTask] SIM init failed — online ops disabled");
    }
    vTaskDelete(NULL);
}

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

            // ---------------------------------------------------------------
            // BLE_KEY_CHECK — triggered by onConnect.
            // Reads NVS for pairing key; if absent, waits for SIM and fetches
            // from server. Queues BLE_SEND_CHALLENGE once key is ready.
            // Disconnects TAG on any unrecoverable failure.
            // ---------------------------------------------------------------
            case BLE_KEY_CHECK: {
                uint8_t myGen = msg.data[0];
                if (myGen != (uint8_t)connectionGen || !deviceConnected) break;

                checkStoredKey();   // fresh NVS read

                if (hasKey) {
                    // Key already provisioned — load it into pairingKey
                    hexStringToBytes(bleKeyHex, pairingKey, 16);
                    printHex("[KEY_CHECK] Key loaded from NVS: ", pairingKey, 16);
                } else {
                    Serial.println("[KEY_CHECK] No key in NVS — initialising SIM to fetch key...");

                    // Open a SIM session.  Two cases:
                    //   a) EVT_SIM_INITIALIZED not set: SIM was never started
                    //      (no-key boot path) — call simInit() from scratch.
                    //   b) EVT_SIM_INITIALIZED set: simInitTask already ran
                    //      (e.g. key deleted after pairing) — use simAcquire().
                    bool simSessionOpen = false;

                    if (!(xEventGroupGetBits(sysEvents) & EVT_SIM_INITIALIZED)) {
                        // SIM serial never opened — init from scratch under mutex
                        if (xSemaphoreTake(simMutex, pdMS_TO_TICKS(30000)) == pdTRUE) {
                            if (simInit()) {
                                simSyncTime();
                                xEventGroupSetBits(sysEvents, EVT_SIM_INITIALIZED | EVT_SIM_READY);
                                simSessionOpen = true;
                            } else {
                                Serial.println("[KEY_CHECK] simInit() failed");
                                xSemaphoreGive(simMutex);
                            }
                        } else {
                            Serial.println("[KEY_CHECK] SIM mutex timeout");
                        }
                    } else {
                        // SIM already initialised — simAcquire() handles wakeup
                        simSessionOpen = simAcquire(30000);
                    }

                    bool fetched = false;
                    char newKey[33];
                    if (simSessionOpen) {
                        fetched = fetchPairingKeyViaSim(newKey);
                        if (fetched) {
                            saveKeyToNVS(newKey);
                            fetchServerSigningKey();
                        }
                        simRelease();  // sleeps SIM + releases mutex
                    }

                    if (!fetched) {
                        Serial.println("[KEY_CHECK] Key fetch failed — disconnecting TAG");
                        pBleServer->disconnect(pBleServer->getConnId());
                        break;
                    }
                    hexStringToBytes(bleKeyHex, pairingKey, 16);
                    printHex("[KEY_CHECK] Key fetched from server: ", pairingKey, 16);
                }

                // Abort if TAG disconnected while we were fetching
                if (myGen != (uint8_t)connectionGen || !deviceConnected) {
                    Serial.printf("[KEY_CHECK] TAG disconnected during key check (gen=%u) — skip\n",
                                  myGen);
                    break;
                }

                // Key is ready — proceed to send challenge
                BleCmdMsg chal = {};
                chal.type    = BLE_SEND_CHALLENGE;
                chal.data[0] = myGen;
                xQueueSend(bleQueue, &chal, pdMS_TO_TICKS(10));
                break;
            }

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

        // Step 2 — offline revocation check is already done inside friend_token_verify().
        // Trust ECDSA signature without online server validation — revocation is
        // handled by revocationSyncTask polling every 5 minutes.
        cached_friend_t cf;
        bool was_cached = (friend_cache_get(bundle.friend_id, &cf) == ESP_OK);
        Serial.printf("[FRIEND] Cache: %s\n", was_cached ? "HIT" : "MISS (first use — trust ECDSA)");

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
    }
}

// =============================================================================
// TASK: revocationSyncTask — Core 0, Priority 1 (lowest)
// Polls revocations every 5 minutes and cleans up expired cache/blacklist entries.
// =============================================================================

static void revocationSyncTask(void *param) {
    Serial.printf("[revSyncTask] started on core %d\n", xPortGetCoreID());

    // Print heartbeat every 60s so user can confirm system is alive
    unsigned long lastHeartbeat = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60UL * 1000));

        uint32_t ts = (uint32_t)time(NULL);
        EventBits_t simBits = xEventGroupGetBits(sysEvents);
        bool simInited = (simBits & EVT_SIM_INITIALIZED) != 0;
        bool simActive = (simBits & EVT_SIM_READY)       != 0;
        const char *simState = !simInited ? "not ready"
                             : simActive  ? "active"
                             :              "sleeping";
        Serial.printf("[SYS] alive t=%lu BLE=%s SIM=%s\n",
                      (unsigned long)ts,
                      !bleStarted     ? "not started" :
                      deviceConnected ? "connected"   : "advertising",
                      simState);

        // Revocation sync every 5 minutes (after 5 heartbeat ticks)
        lastHeartbeat++;
        if (lastHeartbeat < 5) continue;

        if (!simInited) continue;  // SIM never came up — skip sync

        // Wake SIM, run sync, then sleep again
        if (simAcquire()) {
            friend_mgmt_sync_revocations(SERVER_FALLBACK, VEHICLE_ID);
            simRelease();
        }

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
        lastHeartbeat = 0;
    }
}
