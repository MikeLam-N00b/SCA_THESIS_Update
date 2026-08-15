
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <SPI.h>
#include "dw3000.h"
#include <mcp2515.h>
#include "anchor_config.h"
#include "can_commands.h"
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/base64.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "friend_types.h"
#include "friend_revocation.h"
#include "friend_cache.h"
#include "friend_token.h"
#include "friend_mgmt.h"

#include "anchor_ipc.h"
#include "anchor_crypto.h"
#include "anchor_nvs.h"
#include "anchor_transport.h"
#include "anchor_ble.h"
#include "anchor_uwb.h"
#include "anchor_tasks.h"

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Must be set before any time()/mktime() call so bundle TTL validation uses UTC.
    setenv("TZ", "UTC0", 1);
    tzset();

    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("\nSmart Car Anchor [FreeRTOS] - Vehicle: %s (reset: %d)\n", VEHICLE_ID, reason);
    if (reason == ESP_RST_PANIC) Serial.println("WARNING: previous reset was a CRASH");

    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    pinMode(PIN_SS,  OUTPUT); digitalWrite(PIN_SS,  HIGH);
    pinMode(CAN_CS,  OUTPUT); digitalWrite(CAN_CS,  HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Seed mbedTLS CSPRNG
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    const char *pers = "anchor_rtos";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)pers, strlen(pers)) != 0) {
        Serial.println("mbedTLS init failed — halting"); while (1);
    }

    // Create FreeRTOS primitives
    sysEvents   = xEventGroupCreate();
    bleQueue    = xQueueCreate(8, sizeof(BleCmdMsg));
    uwbQueue    = xQueueCreate(4, sizeof(uint8_t));
    canQueue    = xQueueCreate(4, sizeof(uint8_t));
    spiMutex    = xSemaphoreCreateMutex();
    bundleQueue = xQueueCreate(2, sizeof(friend_bundle_t));

    if (!sysEvents || !bleQueue || !uwbQueue || !canQueue || !spiMutex || !bundleQueue) {
        Serial.println("FreeRTOS primitives alloc failed — halting"); while (1);
    }

    // Check NVS for stored pairing key.
    // If absent, key will be fetched from server on first BLE connection (BLE_FETCH_KEY flow).
    checkStoredKey();
    if (hasKey) {
        Serial.printf("Key found in NVS: %.8s...\n", bleKeyHex);
    } else {
        Serial.println("No key in NVS — will fetch from server when Tag connects via BLE.");
    }

    startBLE();
    bleStarted = true;

    SPI.begin();

    pMcp2515    = new MCP2515(CAN_CS);
    pCanControl = new CANCommands(pMcp2515);
    if (!pCanControl->initialize(CAN_CS, CAN_100KBPS, MCP_CLOCK))
        Serial.println("CAN: init failed — continuing without CAN");

    xTaskCreatePinnedToCore(bleTask,            "BLE_Task",     BLE_TASK_STACK,    NULL, BLE_TASK_PRIO,    NULL, BLE_TASK_CORE);
    xTaskCreatePinnedToCore(uwbTask,            "UWB_Task",     UWB_TASK_STACK,    NULL, UWB_TASK_PRIO,    NULL, UWB_TASK_CORE);
    xTaskCreatePinnedToCore(canTask,            "CAN_Task",     CAN_TASK_STACK,    NULL, CAN_TASK_PRIO,    NULL, CAN_TASK_CORE);
    xTaskCreatePinnedToCore(friendMgmtTask,     "Friend_Task",  FRIEND_TASK_STACK, NULL, FRIEND_TASK_PRIO, NULL, FRIEND_TASK_CORE);
    xTaskCreatePinnedToCore(revocationSyncTask, "RevSync_Task", REVSYNC_TASK_STACK,NULL, REVSYNC_TASK_PRIO,NULL, REVSYNC_TASK_CORE);

    Serial.println("All tasks created — FreeRTOS scheduler running");
    Serial.printf("Core 0: bleTask(P%d) friendMgmtTask(P%d) revSyncTask(P%d)\n",
                  BLE_TASK_PRIO, FRIEND_TASK_PRIO, REVSYNC_TASK_PRIO);
    Serial.printf("Core 1: uwbTask(P%d) canTask(P%d)\n", UWB_TASK_PRIO, CAN_TASK_PRIO);
}

void loop() {
    vTaskDelete(NULL);
}
