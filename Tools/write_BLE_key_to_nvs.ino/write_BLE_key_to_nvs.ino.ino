#include <Preferences.h>

Preferences prefs;

const char *kNamespace = "ble-keys";
const char *kKey       = "bleKey";
const char *kValue     = "9ade4a64b4911b84f74b124401efa83b";

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Writing BLE pairing key to ESP32 NVS...");
  Serial.printf("Namespace: %s\n", kNamespace);
  Serial.printf("Key: %s\n", kKey);
  Serial.printf("Value: %s\n", kValue);

  if (!prefs.begin(kNamespace, false)) {
    Serial.println("ERROR: failed to open Preferences namespace");
    while (true) {
      delay(1000);
    }
  }

  prefs.putString(kKey, kValue);
  prefs.end();

  Serial.println("Done. Reset the board and run the main Anchor firmware.");
}

void loop() {
  delay(1000);
}
