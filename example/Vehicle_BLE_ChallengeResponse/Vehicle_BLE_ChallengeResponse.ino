#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <mbedtls/md.h>

// ============================================
// CONFIGURATION
// ============================================
#define DEVICE_NAME "SmartCar_Vehicle"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // Phone gửi response
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"  // Vehicle gửi challenge

// Pairing key (32 hex chars = 16 bytes)
// Trong thực tế, key này được lấy từ Preferences (đã pair với server)
const char* PAIRING_KEY_HEX = "e9b8da4e60206bd04bd554c6a94e4e0e";

// ============================================
// GLOBAL VARIABLES
// ============================================
BLEServer* pServer = nullptr;
BLECharacteristic* pAuthCharacteristic = nullptr;
BLECharacteristic* pChallengeCharacteristic = nullptr;

bool deviceConnected = false;
bool authenticated = false;
uint8_t currentChallenge[16];
uint8_t pairingKey[16];

// Buffer for receiving response in multiple packets
std::string responseBuffer = "";
unsigned long lastWriteTime = 0;

// ============================================
// HELPER FUNCTIONS
// ============================================

// Convert hex string to bytes
void hexStringToBytes(const char* hexString, uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    sscanf(hexString + 2*i, "%2hhx", &bytes[i]);
  }
}

// Generate random challenge
void generateChallenge(uint8_t* challenge, size_t length) {
  for (size_t i = 0; i < length; i++) {
    challenge[i] = random(0, 256);
  }
}

// Compute HMAC-SHA256
bool computeHMAC(const uint8_t* key, size_t keyLen,
                 const uint8_t* data, size_t dataLen,
                 uint8_t* output) {
  
  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  
  int ret = mbedtls_md_hmac(md_info, key, keyLen, data, dataLen, output);
  
  return (ret == 0);
}

// Print bytes as hex
void printHex(const char* label, const uint8_t* data, size_t length) {
  Serial.print(label);
  for (size_t i = 0; i < length; i++) {
    if (data[i] < 16) Serial.print("0");
    Serial.print(data[i], HEX);
  }
  Serial.println();
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
    
    // Wait for phone to subscribe to notifications
    Serial.println("⏳ Waiting 2 seconds for phone to subscribe...");
    delay(2000);
    
    // Generate and send challenge
    generateChallenge(currentChallenge, 16);
    
    Serial.println("\n🔐 Generating Challenge-Response Authentication:");
    printHex("Challenge: ", currentChallenge, 16);
    
    // Send challenge to phone
    pChallengeCharacteristic->setValue(currentChallenge, 16);
    pChallengeCharacteristic->notify();
    
    Serial.println("✓ Challenge sent to phone");
    Serial.println("⏳ Waiting for response...");
    Serial.println();
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    authenticated = false;
    
    Serial.println("\n==================================================");
    Serial.println("📱 Phone Disconnected");
    Serial.println("==================================================\n");
    
    // Restart advertising
    BLEDevice::startAdvertising();
    Serial.println("📡 Advertising restarted...\n");
  }
};

class AuthCharacteristicCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    Serial.println("\n📥 Received write on Auth characteristic!");
    
    std::string value = pCharacteristic->getValue();
    
    Serial.println("Received chunk length: " + String(value.length()));
    
    // Debug: print first few bytes
    if (value.length() > 0) {
      Serial.print("First bytes: ");
      for (int i = 0; i < min(8, (int)value.length()); i++) {
        if ((uint8_t)value[i] < 16) Serial.print("0");
        Serial.print((uint8_t)value[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }
    
    // Buffer the data
    responseBuffer += value;
    lastWriteTime = millis();
    
    Serial.println("Total buffered length: " + String(responseBuffer.length()));
    
    // Wait for complete 32-byte response
    if (responseBuffer.length() >= 32) {
      Serial.println("\n✓ Complete response received!");
      
      // Take only first 32 bytes in case of overflow
      std::string completeResponse = responseBuffer.substr(0, 32);
      responseBuffer = ""; // Clear buffer
      
      uint8_t receivedResponse[32];
      memcpy(receivedResponse, completeResponse.c_str(), 32);
      
      printHex("Response received: ", receivedResponse, 32);
    
    // Compute expected response: HMAC(key, challenge)
    uint8_t expectedResponse[32];
    bool success = computeHMAC(pairingKey, 16, currentChallenge, 16, expectedResponse);
    
    if (!success) {
      Serial.println("❌ HMAC computation failed");
      responseBuffer = "";
      pServer->disconnect(pServer->getConnId());
      return;
    }
    
    printHex("Expected response: ", expectedResponse, 32);
    
    // Compare responses
    bool match = (memcmp(receivedResponse, expectedResponse, 32) == 0);
    
    Serial.println();
    if (match) {
      authenticated = true;
      
      Serial.println("==================================================");
      Serial.println("✅ AUTHENTICATION SUCCESSFUL!");
      Serial.println("==================================================");
      Serial.println("Phone is authorized to access vehicle");
      Serial.println("==================================================\n");
      
      // Send success confirmation
      pAuthCharacteristic->setValue("AUTH_OK");
      pAuthCharacteristic->notify();
      
    } else {
      authenticated = false;
      
      Serial.println("==================================================");
      Serial.println("❌ AUTHENTICATION FAILED!");
      Serial.println("==================================================");
      Serial.println("Wrong key - Disconnecting...");
      Serial.println("==================================================\n");
      
      // Send failure and disconnect
      pAuthCharacteristic->setValue("AUTH_FAIL");
      pAuthCharacteristic->notify();
      
      delay(100);
      pServer->disconnect(pServer->getConnId());
    }
    
    responseBuffer = ""; // Clear buffer after processing
    }
  }
};

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==================================================");
  Serial.println("🚗 Smart Car - Challenge-Response BLE Auth");
  Serial.println("==================================================");
  
  // Convert pairing key from hex string to bytes
  hexStringToBytes(PAIRING_KEY_HEX, pairingKey, 16);
  printHex("Pairing Key: ", pairingKey, 16);
  Serial.println("==================================================\n");
  
  // Initialize BLE
  BLEDevice::init(DEVICE_NAME);
  
  // Create BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  // Create BLE Service
  BLEService* pService = pServer->createService(SERVICE_UUID);
  
  // Create Challenge Characteristic (READ + NOTIFY)
  pChallengeCharacteristic = pService->createCharacteristic(
    CHALLENGE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pChallengeCharacteristic->addDescriptor(new BLE2902());
  
  // Create Auth Characteristic (WRITE + NOTIFY)
  pAuthCharacteristic = pService->createCharacteristic(
    AUTH_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  pAuthCharacteristic->setCallbacks(new AuthCharacteristicCallbacks());
  pAuthCharacteristic->addDescriptor(new BLE2902());
  
  // Start service
  pService->start();
  
  // Start advertising
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("📡 BLE Server Started");
  Serial.println("📡 Advertising as: " + String(DEVICE_NAME));
  Serial.println("🔐 Challenge-Response Authentication Enabled");
  Serial.println("⏳ Waiting for phone connection...\n");
}

// ============================================
// LOOP
// ============================================
void loop() {
  // Monitor authentication status
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 10000) {
    lastPrint = millis();
    
    Serial.println("--- Status ---");
    Serial.println("Connected: " + String(deviceConnected ? "Yes" : "No"));
    Serial.println("Authenticated: " + String(authenticated ? "Yes" : "No"));
    Serial.println("--------------\n");
  }
  
  delay(1000);
}
