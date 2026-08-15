#pragma once
// anchor_nvs.h — NVS key persistence helpers.

#include <Preferences.h>
#include "anchor_ipc.h"

static Preferences preferences;
static char        bleKeyHex[33] = "";

static void checkStoredKey() {
    preferences.begin("ble-keys", true);
    if (preferences.isKey("bleKey")) {
        preferences.getString("bleKey", bleKeyHex, sizeof(bleKeyHex));
        hasKey = (bleKeyHex[0] != '\0');
    } else {
        hasKey = false;
    }
    preferences.end();
}

static void saveKeyToNVS(const char *key) {
    preferences.begin("ble-keys", false);
    preferences.putString("bleKey", key);
    preferences.end();
    strncpy(bleKeyHex, key, sizeof(bleKeyHex) - 1);
    bleKeyHex[sizeof(bleKeyHex) - 1] = '\0';
    hasKey = true;
    Serial.println("Key saved to NVS");
}
