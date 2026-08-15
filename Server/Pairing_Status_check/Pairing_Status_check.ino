#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

// ============================================
// CONFIGURATION
// ============================================
const char* ssid = "nubia Neo 2";
const char* password = "29092004";
const char* serverBaseUrl = "http://10.0.7.178:8000";
const char* vehicleId = "1HGBH41JXMN109186";

// ============================================
// GLOBAL VARIABLES
// ============================================
Preferences preferences;
String bleKey = "";
bool hasKey = false;

mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;

// ============================================
// HKDF IMPLEMENTATION
// ============================================
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
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==================================================");
  Serial.println("ESP32 - Secure Key Management");
  Serial.println("==================================================");
  Serial.println("Vehicle ID: " + String(vehicleId));
  Serial.println("==================================================\n");
  
  // Initialize mbedTLS
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  
  const char* pers = "secure_check";
  int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)pers, strlen(pers));
  if (ret != 0) {
    Serial.printf("❌ mbedtls_ctr_drbg_seed failed: %d\n", ret);
    return;
  }
  
  // Connect to WiFi
  connectWiFi();
  
  // Execute main flow
  executeMainFlow();
}

// ============================================
// LOOP
// ============================================
void loop() {
  // Check for serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd == "CLEAR" || cmd == "clear") {
      clearStoredKey();
      Serial.println("✓ Key cleared! Restarting flow...\n");
      executeMainFlow();
      return;
    }
    
    if (cmd == "SHOW" || cmd == "show") {
      checkStoredKey();
      Serial.println("Current stored key: " + bleKey);
      return;
    }
  }
  
  delay(10000);
  
  // Display current key status
  Serial.println("\n--- Current Status ---");
  if (hasKey) {
    Serial.println("✓ Key Available");
    Serial.println("Key: " + bleKey);
  } else {
    Serial.println("✗ No Key Available");
  }
  Serial.println("Commands: CLEAR (xóa key), SHOW (xem key)");
  Serial.println("---------------------\n");
}

// ============================================
// MAIN FLOW
// ============================================
void executeMainFlow() {
  Serial.println("=== MAIN FLOW START ===\n");
  
  // STEP 1: Check stored key
  Serial.println("STEP 1: Checking for stored key in memory...");
  checkStoredKey();
  
  if (hasKey) {
    // Key exists in memory
    Serial.println("✓ Key found in memory!");
    Serial.println("\n==================================================");
    Serial.println("BLE KEY (from memory)");
    Serial.println("==================================================");
    Serial.println(bleKey);
    Serial.println("==================================================\n");
  } else {
    // No key - fetch from server
    Serial.println("✗ No key in memory");
    Serial.println("\nSTEP 2: Fetching key from server...");
    
    String serverKey = fetchKeyFromServer();
    
    if (serverKey.length() > 0) {
      // Key received from server
      Serial.println("✓ Key received from server!");
      
      // Save to memory
      saveKeyToMemory(serverKey);
      
      // Update global variable
      bleKey = serverKey;
      hasKey = true;
      
      Serial.println("\n==================================================");
      Serial.println("BLE KEY (from server)");
      Serial.println("==================================================");
      Serial.println(bleKey);
      Serial.println("==================================================\n");
    } else {
      Serial.println("✗ Failed to get key from server");
      Serial.println("Vehicle may not be paired yet\n");
    }
  }
  
  Serial.println("=== MAIN FLOW END ===\n");
}

// ============================================
// WIFI CONNECTION
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

// ============================================
// STORAGE FUNCTIONS
// ============================================
void checkStoredKey() {
  preferences.begin("ble-keys", true);  // Read-only
  
  if (preferences.isKey("bleKey")) {
    bleKey = preferences.getString("bleKey", "");
    hasKey = (bleKey.length() > 0);
  } else {
    hasKey = false;
  }
  
  preferences.end();
}

void saveKeyToMemory(String key) {
  Serial.println("Saving key to memory...");
  
  preferences.begin("ble-keys", false);  // Read-write
  preferences.putString("bleKey", key);
  preferences.end();
  
  Serial.println("✓ Key saved to memory!\n");
}

void clearStoredKey() {
  Serial.println("Clearing stored key...");
  
  preferences.begin("ble-keys", false);
  preferences.remove("bleKey");
  preferences.end();
  
  bleKey = "";
  hasKey = false;
  
  Serial.println("✓ Key cleared!\n");
}

// ============================================
// SERVER COMMUNICATION
// ============================================
String fetchKeyFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected!");
    return "";
  }
  
  // Test connectivity
  Serial.println("Testing server connectivity...");
  Serial.println("Server URL: " + String(serverBaseUrl));
  Serial.println("ESP32 IP: " + WiFi.localIP().toString());
  
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
    Serial.println("Error: " + http.errorToString(httpCode));
    
    // Check common errors
    if (httpCode == -1) {
      Serial.println("⚠️ Connection failed - Check:");
      Serial.println("  1. Server is running?");
      Serial.println("  2. Same WiFi network?");
      Serial.println("  3. Firewall blocking port 8000?");
      Serial.println("  4. Server IP correct: " + String(serverBaseUrl));
    }
  }
  
  http.end();
  mbedtls_pk_free(&client_key);
  
  return key;
}

// ============================================
// DECRYPTION
// ============================================
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
  
  // ECDH
  mbedtls_ecdh_context ecdh;
  mbedtls_ecdh_init(&ecdh);
  
  mbedtls_ecp_group* grp = &mbedtls_pk_ec(*client_key)->grp;
  mbedtls_mpi* d = &mbedtls_pk_ec(*client_key)->d;
  mbedtls_ecp_point* Q_peer = &mbedtls_pk_ec(server_key)->Q;
  
  ret = mbedtls_ecdh_compute_shared(grp, &ecdh.z, Q_peer, d,
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0) {
    Serial.println("❌ ECDH failed");
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return "";
  }
  
  // Shared secret
  unsigned char shared_secret[32];
  ret = mbedtls_mpi_write_binary(&ecdh.z, shared_secret, 32);
  if (ret != 0) {
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
    key = doc["pairing_key"].as<String>();  // Đổi từ "ble_key" sang "pairing_key"
    
    Serial.println("\n--- Server Response ---");
    Serial.println("Status: PAIRED");
    Serial.println("Pairing ID: " + doc["pairing_id"].as<String>());
    Serial.println("Paired At: " + doc["paired_at"].as<String>());
    Serial.println("Pairing Key: " + key);  // In ra key luôn để debug
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
