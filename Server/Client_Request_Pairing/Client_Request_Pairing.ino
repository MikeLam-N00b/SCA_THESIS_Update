#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

// Manual HKDF implementation for compatibility
int hkdf_sha256(const unsigned char* salt, size_t salt_len,
                const unsigned char* ikm, size_t ikm_len,
                const unsigned char* info, size_t info_len,
                unsigned char* okm, size_t okm_len) {
  unsigned char prk[32];
  
  // Extract step: PRK = HMAC-SHA256(salt, IKM)
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (salt == NULL || salt_len == 0) {
    unsigned char zero_salt[32] = {0};
    mbedtls_md_hmac(md_info, zero_salt, 32, ikm, ikm_len, prk);
  } else {
    mbedtls_md_hmac(md_info, salt, salt_len, ikm, ikm_len, prk);
  }
  
  // Expand step
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

// WiFi credentials
const char* ssid = "nubia Neo 2";
const char* password = "29092004";

// Server endpoint
const char* serverUrl = "http://10.0.7.178:8000/owner-pairing";

// Vehicle ID
const char* vehicleId = "VIN123456";

// mbedTLS contexts
mbedtls_pk_context vehicle_key;
mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== ESP32 Smart Car Access Client ===");
  
  // Initialize mbedTLS
  mbedtls_pk_init(&vehicle_key);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg);
  
  // Seed random number generator
  const char* pers = "vehicle_ecdh";
  int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char*)pers, strlen(pers));
  if (ret != 0) {
    Serial.printf("mbedtls_ctr_drbg_seed failed: %d\n", ret);
    return;
  }
  
  // Connect to WiFi
  connectWiFi();
  
  // Perform pairing
  performPairing();
}

void loop() {
  // Nothing in loop for this example
  delay(10000);
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void performPairing() {
  Serial.println("\n--- Starting Pairing Process ---");
  
  // Generate EC key pair (SECP256R1 / P-256)
  Serial.println("Generating EC key pair...");
  int ret = mbedtls_pk_setup(&vehicle_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (ret != 0) {
    Serial.printf("mbedtls_pk_setup failed: %d\n", ret);
    return;
  }
  
  ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                             mbedtls_pk_ec(vehicle_key),
                             mbedtls_ctr_drbg_random,
                             &ctr_drbg);
  if (ret != 0) {
    Serial.printf("mbedtls_ecp_gen_key failed: %d\n", ret);
    return;
  }
  
  // Export public key in DER format
  unsigned char vehicle_pub_der[200];
  int pub_len = mbedtls_pk_write_pubkey_der(&vehicle_key, vehicle_pub_der, sizeof(vehicle_pub_der));
  if (pub_len < 0) {
    Serial.printf("mbedtls_pk_write_pubkey_der failed: %d\n", pub_len);
    return;
  }
  
  // DER is written from the end of buffer, so adjust pointer
  unsigned char* pub_start = vehicle_pub_der + sizeof(vehicle_pub_der) - pub_len;
  
  // Base64 encode public key using mbedtls
  size_t olen;
  unsigned char vehicle_pub_b64[300];
  ret = mbedtls_base64_encode(vehicle_pub_b64, sizeof(vehicle_pub_b64), &olen, pub_start, pub_len);
  if (ret != 0) {
    Serial.printf("Base64 encode failed: %d\n", ret);
    return;
  }
  vehicle_pub_b64[olen] = '\0';
  
  String vehicle_pub_b64_str = String((char*)vehicle_pub_b64);
  Serial.println("Vehicle public key (base64): " + vehicle_pub_b64_str.substring(0, 50) + "...");
  
  // Send HTTP request
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverUrl);
    http.addHeader("Content-Type", "application/json");
    
    // Create JSON request
    StaticJsonDocument<512> requestDoc;
    requestDoc["vehicle_id"] = vehicleId;
    requestDoc["vehicle_public_key_b64"] = vehicle_pub_b64_str;
    
    String requestBody;
    serializeJson(requestDoc, requestBody);
    
    Serial.println("\nSending pairing request...");
    int httpCode = http.POST(requestBody);
    
    if (httpCode == 200) {
      String response = http.getString();
      Serial.println("Server response received!");
      
      // Parse JSON response
      StaticJsonDocument<1024> responseDoc;
      DeserializationError error = deserializeJson(responseDoc, response);
      
      if (error) {
        Serial.print("JSON parse failed: ");
        Serial.println(error.c_str());
        return;
      }
      
      String pairing_id = responseDoc["pairing_id"];
      String server_pub_b64 = responseDoc["server_public_key_b64"];
      String encrypted_key_b64 = responseDoc["encrypted_pairing_key_b64"];
      String nonce_b64 = responseDoc["nonce_b64"];
      
      Serial.println("Pairing ID: " + pairing_id);
      
      // Decode server public key
      unsigned char server_pub_der[200];
      size_t server_pub_len;
      ret = mbedtls_base64_decode(server_pub_der, sizeof(server_pub_der), &server_pub_len,
                                   (const unsigned char*)server_pub_b64.c_str(), 
                                   server_pub_b64.length());
      if (ret != 0) {
        Serial.printf("Failed to decode server public key: %d\n", ret);
        return;
      }
      
      // Perform ECDH and decrypt pairing key
      decryptPairingKey(server_pub_der, server_pub_len, encrypted_key_b64, nonce_b64);
      
    } else {
      Serial.printf("HTTP request failed, code: %d\n", httpCode);
      Serial.println(http.getString());
    }
    
    http.end();
  }
}

void decryptPairingKey(unsigned char* server_pub_der, size_t server_pub_len, String encrypted_key_b64, String nonce_b64) {
  Serial.println("\n--- Decrypting Pairing Key ---");
  
  // Parse server public key
  mbedtls_pk_context server_key;
  mbedtls_pk_init(&server_key);
  
  int ret = mbedtls_pk_parse_public_key(&server_key, server_pub_der, server_pub_len);
  if (ret != 0) {
    Serial.printf("Failed to parse server public key: %d\n", ret);
    mbedtls_pk_free(&server_key);
    return;
  }
  
  // Perform ECDH
  unsigned char shared_secret[32];
  size_t shared_len;
  
  mbedtls_ecdh_context ecdh;
  mbedtls_ecdh_init(&ecdh);
  
  // Set up ECDH with our private key and server's public key
  mbedtls_ecp_group* grp = &mbedtls_pk_ec(vehicle_key)->grp;
  mbedtls_mpi* d = &mbedtls_pk_ec(vehicle_key)->d;
  mbedtls_ecp_point* Q_peer = &mbedtls_pk_ec(server_key)->Q;
  
  ret = mbedtls_ecdh_compute_shared(grp, &ecdh.z, Q_peer, d,
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
  
  if (ret != 0) {
    Serial.printf("ECDH compute shared failed: %d\n", ret);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return;
  }
  
  // Export shared secret
  ret = mbedtls_mpi_write_binary(&ecdh.z, shared_secret, 32);
  if (ret != 0) {
    Serial.printf("Failed to write shared secret: %d\n", ret);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return;
  }
  
  Serial.println("ECDH shared secret computed");
  
  // Derive KEK using HKDF
  unsigned char kek[16];
  const unsigned char info[] = "owner-pairing-kek";
  
  ret = hkdf_sha256(NULL, 0, shared_secret, 32, info, strlen((char*)info), kek, 16);
  
  if (ret != 0) {
    Serial.printf("HKDF failed: %d\n", ret);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return;
  }
  
  Serial.println("KEK derived");
  
  // Decode encrypted pairing key and nonce using mbedtls
  unsigned char encrypted_key[100];
  unsigned char nonce[12];
  size_t encrypted_len, nonce_len;
  
  ret = mbedtls_base64_decode(encrypted_key, sizeof(encrypted_key), &encrypted_len,
                               (const unsigned char*)encrypted_key_b64.c_str(),
                               encrypted_key_b64.length());
  if (ret != 0) {
    Serial.printf("Failed to decode encrypted key: %d\n", ret);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return;
  }
  
  ret = mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len,
                               (const unsigned char*)nonce_b64.c_str(),
                               nonce_b64.length());
  if (ret != 0 || nonce_len != 12) {
    Serial.printf("Failed to decode nonce: %d\n", ret);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return;
  }
  
  // Decrypt using AES-GCM
  unsigned char pairing_key[16];
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  
  ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128);
  if (ret != 0) {
    Serial.printf("GCM setkey failed: %d\n", ret);
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return;
  }
  
  // AES-GCM decrypt (last 16 bytes are the tag)
  size_t ciphertext_len = encrypted_len - 16;
  unsigned char tag[16];
  memcpy(tag, encrypted_key + ciphertext_len, 16);
  
  ret = mbedtls_gcm_auth_decrypt(&gcm,
                                  ciphertext_len,
                                  nonce, 12,
                                  NULL, 0,
                                  tag, 16,
                                  encrypted_key,
                                  pairing_key);
  
  if (ret != 0) {
    Serial.printf("GCM decrypt failed: %d\n", ret);
    mbedtls_gcm_free(&gcm);
    mbedtls_ecdh_free(&ecdh);
    mbedtls_pk_free(&server_key);
    return;
  }
  
  // Print recovered pairing key
  Serial.print("Recovered pairing key (hex): ");
  for (int i = 0; i < 16; i++) {
    Serial.printf("%02x", pairing_key[i]);
  }
  Serial.println();
  Serial.printf("Pairing key length: %d bytes\n", 16);
  
  // Store pairing key in EEPROM or preferences here
  Serial.println("\n✓ Pairing successful!");
  
  // Cleanup
  mbedtls_gcm_free(&gcm);
  mbedtls_ecdh_free(&ecdh);
  mbedtls_pk_free(&server_key);
}
