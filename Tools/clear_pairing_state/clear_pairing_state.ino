#include <Preferences.h>

#define PAIRING_KEY "9ade4a64b4911b84f74b124401efa83b"

static void clearKey(const char *namespaceName, const char *keyName) {
  Preferences prefs;
  if (!prefs.begin(namespaceName, false)) {
    Serial.printf("[CLR] Failed to open namespace %s\n", namespaceName);
    return;
  }

  if (prefs.isKey(keyName)) {
    prefs.remove(keyName);
    Serial.printf("[CLR] Removed %s/%s\n", namespaceName, keyName);
  } else {
    Serial.printf("[CLR] %s/%s not present\n", namespaceName, keyName);
  }

  prefs.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("[CLR] Clearing pairing-related NVS state...");

  clearKey("ble-keys", "bleKey");
  clearKey("sca_tag", "srv_pub");
  clearKey("sca_tag", "friend_wire");
  clearKey("sca_tag", "friend_len");

  Serial.println("[CLR] Done. Reset the board now.");
}

void loop() {
  delay(1000);
}
