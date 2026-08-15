#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

// ============================================
// WIFI & SERVER CONFIGURATION
// ============================================
const char* ssid = "nubia Neo 2";
const char* password = "29092004";

// serverBaseUrl tự động cập nhật qua mDNS sau khi WiFi kết nối
// Fallback nếu mDNS không tìm thấy server
String serverBaseUrl = "http://10.0.7.178:8000";

const char* vehicleId = "1HGBH41JXMN109186";

// ============================================
// BLE CONFIGURATION
// ============================================
#define DEVICE_NAME "SmartCar_Vehicle"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"

// ============================================
// GLOBAL VARIABLES
// ============================================
// Storage
Preferences preferences;
String bleKeyHex = "";
bool hasKey = false;

// BLE
BLEServer* pServer = nullptr;
BLECharacteristic* pAuthCharacteristic = nullptr;
BLECharacteristic* pChallengeCharacteristic = nullptr;
bool deviceConnected = false;
bool authenticated = false;
uint8_t currentChallenge[16];
uint8_t pairingKey[16];
std::string responseBuffer = "";
unsigned long lastWriteTime = 0;

// mbedTLS
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;

// ============================================
// HELPER FUNCTIONS
// ============================================
void hexStringToBytes(const char* hexString, uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    sscanf(hexString + 2*i, "%2hhx", &bytes[i]);
  }
}

void generateChallenge(uint8_t* challenge, size_t length) {
  for (size_t i = 0; i < length; i++) {
    challenge[i] = random(0, 256);
  }
}

bool computeHMAC(const uint8_t* key, size_t keyLen,
                 const uint8_t* data, size_t dataLen,
                 uint8_t* output) {
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int ret = mbedtls_md_hmac(md_info, key, keyLen, data, dataLen, output);
  return (ret == 0);
}

void printHex(const char* label, const uint8_t* data, size_t length) {
  Serial.print(label);
  for (size_t i = 0; i < length; i++) {
    if (data[i] < 16) Serial.print("0");
    Serial.print(data[i], HEX);
  }
  Serial.println();
}

int hkdf_sha256(const unsigned char* salt, size_t salt_len,
                const unsigned char* ikm, size_t ikm_len,
                const unsigned char* info, size_t info_len,
                unsigned char* okm, size_t okm_len) {
  unsigned char prk[32];
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  
  if (salt == NULL || salt_len == 0) {
    unsigned char zero_salt[32] = {0};
    mbedtls_md_hmac(md_info, zero_salt, 32, ikm, ikm_len, prk);
  } else {
    mbedtls_md_hmac(md_info, salt, salt_len, ikm, ikm_len, prk);
  }
  
  unsigned char t[32];
  unsigned char counter = 1;
  size_t t_len = 0;
  size_t offset = 0;
  
  while (offset < okm_len) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md_info, 1);
    mbedtls_md_hmac_starts(&ctx, prk, 32);
    
    if (t_len > 0) {
      mbedtls_md_hmac_update(&ctx, t, t_len);
    }
    mbedtls_md_hmac_update(&ctx, info, info_len);
    mbedtls_md_hmac_update(&ctx, &counter, 1);
    mbedtls_md_hmac_finish(&ctx, t);
    mbedtls_md_free(&ctx);
    
    t_len = 32;
    size_t copy_len = (okm_len - offset < 32) ? (okm_len - offset) : 32;
    memcpy(okm + offset, t, copy_len);
    offset += copy_len;
    counter++;
  }
  
  return 0;
}

// ============================================
// STORAGE FUNCTIONS
// ============================================
void checkStoredKey() {
  preferences.begin("ble-keys", true);
  
  if (preferences.isKey("bleKey")) {
    bleKeyHex = preferences.getString("bleKey", "");
    hasKey = (bleKeyHex.length() > 0);
  } else {
    hasKey = false;
  }
  
  preferences.end();
}

void saveKeyToMemory(String key) {
  Serial.println("Saving key to memory...");
  
  preferences.begin("ble-keys", false);
  preferences.putString("bleKey", key);
  preferences.end();
  
  Serial.println("✓ Key saved to memory!\n");
  
  bleKeyHex = key;
  hasKey = true;
}

void clearStoredKey() {
  Serial.println("Clearing stored key...");
  
  preferences.begin("ble-keys", false);
  preferences.remove("bleKey");
  preferences.end();
  
  bleKeyHex = "";
  hasKey = false;
  
  Serial.println("✓ Key cleared!\n");
}

// ============================================
// WIFI & SERVER FUNCTIONS
// ============================================
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected!");
    Serial.println("IP: " + WiFi.localIP().toString() + "\n");
  } else {
    Serial.println("\n❌ WiFi connection failed!\n");
  }
}

void discoverServer() {
  Serial.println("Discovering server via mDNS...");

  if (!MDNS.begin("esp32-vehicle")) {
    Serial.println("mDNS init failed, using fallback: " + serverBaseUrl);
    return;
  }

  int n = MDNS.queryService("http", "tcp");
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      if (MDNS.port(i) == 8000) {
        serverBaseUrl = "http://" + MDNS.address(i).toString() + ":8000";
        Serial.println("✓ Server found via mDNS: " + serverBaseUrl);
        return;
      }
    }
  }
  Serial.println("Server not found via mDNS, using fallback: " + serverBaseUrl);
}

String fetchKeyFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected!");
    return "";
  }
  
  Serial.println("Fetching key from server...");
  Serial.println("Server: " + String(serverBaseUrl));
  
  // Generate ephemeral EC key pair
  mbedtls_pk_context client_key;
  mbedtls_pk_init(&client_key);
  
  int ret = mbedtls_pk_setup(&client_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) {
    Serial.printf("❌ Key setup failed: %d\n", ret);
    return "";
  }
  
  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                             mbedtls_pk_ec(client_key),
                             mbedtls_ctr_drbg_random,
                             &ctr_drbg);
  if (ret != 0) {
    Serial.printf("❌ Key generation failed: %d\n", ret);
    mbedtls_pk_free(&client_key);
    return "";
  }
  
  // Export public key
  unsigned char client_pub_der[200];
  int pub_len = mbedtls_pk_write_pubkey_der(&client_key, client_pub_der, sizeof(client_pub_der));
  if (pub_len < 0) {
    Serial.printf("❌ Export failed: %d\n", pub_len);
    mbedtls_pk_free(&client_key);
    return "";
  }
  
  unsigned char* pub_start = client_pub_der + sizeof(client_pub_der) - pub_len;
  
  // Base64 encode
  size_t olen;
  unsigned char client_pub_b64[300];
  ret = mbedtls_base64_encode(client_pub_b64, sizeof(client_pub_b64), &olen, pub_start, pub_len);
  if (ret != 0) {
    Serial.printf("❌ Base64 failed: %d\n", ret);
    mbedtls_pk_free(&client_key);
    return "";
  }
  client_pub_b64[olen] = '\0';
  
  // Send request
  HTTPClient http;
  String url = String(serverBaseUrl) + "/secure-check-pairing";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  StaticJsonDocument<512> requestDoc;
  requestDoc["vehicle_id"] = vehicleId;
  requestDoc["client_public_key_b64"] = String((char*)client_pub_b64);
  
  String requestBody;
  serializeJson(requestDoc, requestBody);
  
  Serial.println("Sending secure request...");
  int httpCode = http.POST(requestBody);
  
  String key = "";
  
  if (httpCode == 200) {
    String response = http.getString();
    
    StaticJsonDocument<1024> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, response);
    
    if (!error) {
      String server_pub_b64 = responseDoc["server_public_key_b64"];
      String encrypted_data_b64 = responseDoc["encrypted_data_b64"];
      String nonce_b64 = responseDoc["nonce_b64"];
      
      key = decryptResponse(&client_key, server_pub_b64, encrypted_data_b64, nonce_b64);
    } else {
      Serial.println("❌ JSON parse failed");
    }
  } else {
    Serial.printf("❌ HTTP failed: %d\n", httpCode);
  }
  
  http.end();
  mbedtls_pk_free(&client_key);
  
  return key;
}

String decryptResponse(mbedtls_pk_context* client_key, String server_pub_b64, 
                       String encrypted_data_b64, String nonce_b64) {
  
  // Decode server public key
  unsigned char server_pub_der[200];
  size_t server_pub_len;
  int ret = mbedtls_base64_decode(server_pub_der, sizeof(server_pub_der), &server_pub_len,
                                   (const unsigned char*)server_pub_b64.c_str(),
                                   server_pub_b64.length());
  if (ret != 0) {
    Serial.println("❌ Decode server key failed");
    return "";
  }
  
  // Parse server public key
  mbedtls_pk_context server_key;
  mbedtls_pk_init(&server_key);
  ret = mbedtls_pk_parse_public_key(&server_key, server_pub_der, server_pub_len);
  if (ret != 0) {
    Serial.println("❌ Parse server key failed");
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // ECDH - Compatible with mbedTLS 3.x
  mbedtls_ecdh_context ecdh;
  mbedtls_ecdh_init(&ecdh);
  
  // Setup ECDH context
  ret = mbedtls_ecdh_setup(&ecdh, MBEDTLS_ECP_DP_SECP256R1);
  if (ret != 0) {
    Serial.println("❌ ECDH setup failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Get our private key
  mbedtls_ecp_keypair *client_ec = (mbedtls_ecp_keypair*)mbedtls_pk_ec(*client_key);
  ret = mbedtls_ecdh_get_params(&ecdh, client_ec, MBEDTLS_ECDH_OURS);
  if (ret != 0) {
    Serial.println("❌ ECDH get client params failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Get peer public key
  mbedtls_ecp_keypair *server_ec = (mbedtls_ecp_keypair*)mbedtls_pk_ec(server_key);
  ret = mbedtls_ecdh_get_params(&ecdh, server_ec, MBEDTLS_ECDH_THEIRS);
  if (ret != 0) {
    Serial.println("❌ ECDH get server params failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Compute shared secret
  unsigned char shared_secret[32];
  size_t olen;
  ret = mbedtls_ecdh_calc_secret(&ecdh, &olen, shared_secret, sizeof(shared_secret),
                                  mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0 || olen != 32) {
    Serial.println("❌ Shared secret failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Derive KEK
  unsigned char kek[16];
  const unsigned char info[] = "secure-check-kek";
  ret = hkdf_sha256(NULL, 0, shared_secret, 32, info, strlen((char*)info), kek, 16);
  if (ret != 0) {
    Serial.println("❌ HKDF failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Decode encrypted data
  unsigned char encrypted_data[200];
  unsigned char nonce[12];
  size_t encrypted_len, nonce_len;
  
  ret = mbedtls_base64_decode(encrypted_data, sizeof(encrypted_data), &encrypted_len,
                               (const unsigned char*)encrypted_data_b64.c_str(),
                               encrypted_data_b64.length());
  if (ret != 0) {
    Serial.println("❌ Decode data failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  ret = mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len,
                               (const unsigned char*)nonce_b64.c_str(),
                               nonce_b64.length());
  if (ret != 0 || nonce_len != 12) {
    Serial.println("❌ Decode nonce failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Decrypt
  unsigned char decrypted[200];
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  
  ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128);
  if (ret != 0) {
    Serial.println("❌ GCM setkey failed");
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  size_t ciphertext_len = encrypted_len - 16;
  unsigned char tag[16];
  memcpy(tag, encrypted_data + ciphertext_len, 16);
  
  ret = mbedtls_gcm_auth_decrypt(&gcm, ciphertext_len, nonce, 12, NULL, 0,
                                  tag, 16, encrypted_data, decrypted);
  
  if (ret != 0) {
    Serial.println("❌ Decryption failed");
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  decrypted[ciphertext_len] = '\0';
  
  // Parse JSON
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, (char*)decrypted);
  
  if (error) {
    Serial.println("❌ Parse failed");
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  bool isPaired = doc["paired"];
  String key = "";
  
  if (isPaired) {
    key = doc["pairing_key"].as<String>();
    
    Serial.println("\n--- Server Response ---");
    Serial.println("Status: PAIRED");
    Serial.println("Pairing ID: " + doc["pairing_id"].as<String>());
    Serial.println("Paired At: " + doc["paired_at"].as<String>());
    Serial.println("Pairing Key: " + key);
    Serial.println("-----------------------\n");
  } else {
    Serial.println("\n--- Server Response ---");
    Serial.println("Status: NOT PAIRED");
    Serial.println("Message: " + doc["message"].as<String>());
    Serial.println("-----------------------\n");
  }
  
  // Cleanup
  mbedtls_gcm_free(&gcm);
  mbedtls_ecdh_free(&ecdh);
  mbedtls_pk_free(&server_key);
  
  return key;
}

// ============================================
// BLE CALLBACKS
// ============================================
class ServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    authenticated = false;
    
    Serial.println("\n==================================================");
    Serial.println("📱 Phone Connected!");
    Serial.println("==================================================");
    
    Serial.println("⏳ Waiting 2 seconds for phone to subscribe...");
    delay(2000);
    
    generateChallenge(currentChallenge, 16);
    
    Serial.println("\n🔐 Challenge-Response Authentication:");
    printHex("Challenge: ", currentChallenge, 16);
    
    pChallengeCharacteristic->setValue(currentChallenge, 16);
    pChallengeCharacteristic->notify();
    
    Serial.println("✓ Challenge sent to phone");
    Serial.println("⏳ Waiting for response...\n");
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    authenticated = false;
    responseBuffer = "";
    
    Serial.println("\n==================================================");
    Serial.println("📱 Phone Disconnected");
    Serial.println("==================================================\n");
    
    BLEDevice::startAdvertising();
    Serial.println("📡 Advertising restarted...\n");
  }
};

class AuthCharacteristicCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    Serial.println("\n📥 Received write on Auth characteristic!");
    
    std::string value = pCharacteristic->getValue();
    
    Serial.println("Received chunk: " + String(value.length()) + " bytes");
    
    responseBuffer += value;
    lastWriteTime = millis();
    
    Serial.println("Total buffered: " + String(responseBuffer.length()) + " bytes");
    
    if (responseBuffer.length() >= 32) {
      Serial.println("\n✓ Complete response received!");
      
      std::string completeResponse = responseBuffer.substr(0, 32);
      responseBuffer = "";
      
      uint8_t receivedResponse[32];
      memcpy(receivedResponse, completeResponse.c_str(), 32);
      
      printHex("Response received: ", receivedResponse, 32);
      
      uint8_t expectedResponse[32];
      bool success = computeHMAC(pairingKey, 16, currentChallenge, 16, expectedResponse);
      
      if (!success) {
        Serial.println("❌ HMAC computation failed");
        pServer->disconnect(pServer->getConnId());
        return;
      }
      
      printHex("Expected response: ", expectedResponse, 32);
      
      bool match = (memcmp(receivedResponse, expectedResponse, 32) == 0);
      
      Serial.println();
      if (match) {
        authenticated = true;
        
        Serial.println("==================================================");
        Serial.println("✅ AUTHENTICATION SUCCESSFUL!");
        Serial.println("==================================================");
        Serial.println("Phone is authorized to access vehicle");
        Serial.println("==================================================\n");
        
        pAuthCharacteristic->setValue("AUTH_OK");
        pAuthCharacteristic->notify();
        
      } else {
        authenticated = false;
        
        Serial.println("==================================================");
        Serial.println("❌ AUTHENTICATION FAILED!");
        Serial.println("==================================================");
        Serial.println("Wrong key - Disconnecting...");
        Serial.println("==================================================\n");
        
        pAuthCharacteristic->setValue("AUTH_FAIL");
        pAuthCharacteristic->notify();
        
        delay(100);
        pServer->disconnect(pServer->getConnId());
      }
    }
  }
};

// ============================================
// BLE SETUP
// ============================================
void startBLE() {
  Serial.println("\n🔐 Starting BLE Server...");
  
  // Convert hex key to bytes
  hexStringToBytes(bleKeyHex.c_str(), pairingKey, 16);
  printHex("Pairing Key: ", pairingKey, 16);
  
  BLEDevice::init(DEVICE_NAME);
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  BLEService* pService = pServer->createService(SERVICE_UUID);
  
  pChallengeCharacteristic = pService->createCharacteristic(
    CHALLENGE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pChallengeCharacteristic->addDescriptor(new BLE2902());
  
  pAuthCharacteristic = pService->createCharacteristic(
    AUTH_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  pAuthCharacteristic->setCallbacks(new AuthCharacteristicCallbacks());
  pAuthCharacteristic->addDescriptor(new BLE2902());
  
  pService->start();
  
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("✓ BLE Server Started");
  Serial.println("📡 Advertising as: " + String(DEVICE_NAME));
  Serial.println("⏳ Waiting for phone connection...\n");
}

// ============================================
// MAIN FLOW
// ============================================
void executeMainFlow() {
  Serial.println("\n=== MAIN FLOW START ===\n");
  
  // STEP 1: Check stored key
  Serial.println("STEP 1: Checking for stored key in Preferences...");
  checkStoredKey();
  
  if (hasKey) {
    // Key exists - Start BLE
    Serial.println("✓ Key found in memory!");
    Serial.println("Key: " + bleKeyHex);
    Serial.println("\nSTEP 2: Starting BLE with stored key...");
    startBLE();
    
  } else {
    // No key - Fetch from server
    Serial.println("✗ No key in memory");
    Serial.println("\nSTEP 2: Fetching key from server...");
    
    String serverKey = fetchKeyFromServer();
    
    if (serverKey.length() > 0) {
      Serial.println("✓ Key received from server!");
      Serial.println("Key: " + serverKey);
      
      // Save to memory
      saveKeyToMemory(serverKey);
      
      Serial.println("\nSTEP 3: Starting BLE with server key...");
      startBLE();
      
    } else {
      Serial.println("\n❌ Failed to get key from server");
      Serial.println("Vehicle not paired - Cannot start BLE");
      Serial.println("\nPlease pair the vehicle first, then restart.\n");
    }
  }
  
  Serial.println("=== MAIN FLOW END ===\n");
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==================================================");
  Serial.println("🚗 Smart Car - Complete Vehicle System");
  Serial.println("==================================================");
  Serial.println("Vehicle ID: " + String(vehicleId));
  Serial.println("==================================================\n");
  
  // Initialize mbedTLS
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  
  const char* pers = "vehicle_secure";
  int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)pers, strlen(pers));
  if (ret != 0) {
    Serial.printf("❌ mbedtls init failed: %d\n", ret);
    return;
  }
  
  // Connect to WiFi
  connectWiFi();

  // Tự động tìm server qua mDNS
  discoverServer();
  Serial.println("Server URL: " + serverBaseUrl);
  
  // Execute main flow
  executeMainFlow();
}

// ============================================
// LOOP
// ============================================
void loop() {
  // Handle serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd == "CLEAR" || cmd == "clear") {
      clearStoredKey();
      Serial.println("✓ Key cleared! Please restart ESP32.\n");
      return;
    }
    
    if (cmd == "SHOW" || cmd == "show") {
      checkStoredKey();
      if (hasKey) {
        Serial.println("Stored key: " + bleKeyHex);
      } else {
        Serial.println("No key stored");
      }
      return;
    }
    
    if (cmd == "STATUS" || cmd == "status") {
      Serial.println("\n--- System Status ---");
      Serial.println("Has Key: " + String(hasKey ? "Yes" : "No"));
      Serial.println("BLE Connected: " + String(deviceConnected ? "Yes" : "No"));
      Serial.println("Authenticated: " + String(authenticated ? "Yes" : "No"));
      Serial.println("--------------------\n");
      return;
    }
  }
  
  // Monitor status
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 30000) {
    lastPrint = millis();
    
    Serial.println("\n--- Status ---");
    Serial.println("Connected: " + String(deviceConnected ? "Yes" : "No"));
    Serial.println("Authenticated: " + String(authenticated ? "Yes" : "No"));
    Serial.println("Commands: CLEAR, SHOW, STATUS");
    Serial.println("--------------\n");
  }
  
  delay(1000);
}
