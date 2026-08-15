#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <mbedtls/md.h>

// ============================================
// CONFIGURATION
// ============================================
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define AUTH_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHALLENGE_CHAR_UUID "ceb5483e-36e1-4688-b7f5-ea07361b26a9"

// Pairing key (phải giống với Vehicle)
const char* CORRECT_KEY_HEX = "e9b8da4e60206bd04bd554c6a94e4e0e";

// Test với wrong key
const char* WRONG_KEY_HEX = "0000000000000000000000000000000";

// Chọn key để test
bool useCorrectKey = true;  // Đổi thành false để test với wrong key

// ============================================
// GLOBAL VARIABLES
// ============================================
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pAuthCharacteristic = nullptr;
BLERemoteCharacteristic* pChallengeCharacteristic = nullptr;

bool connected = false;
bool authenticated = false;
uint8_t pairingKey[16];

// ============================================
// HELPER FUNCTIONS
// ============================================

void hexStringToBytes(const char* hexString, uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    sscanf(hexString + 2*i, "%2hhx", &bytes[i]);
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

// ============================================
// BLE CALLBACKS
// ============================================

// Callback when notification received
void notifyCallback(BLERemoteCharacteristic* pCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  
  String charUUID = pCharacteristic->getUUID().toString().c_str();
  
  // Challenge received
  if (charUUID == CHALLENGE_CHAR_UUID) {
    if (length == 16) {
      Serial.println("\n==================================================");
      Serial.println("🔐 Challenge Received from Vehicle!");
      Serial.println("==================================================");
      
      printHex("Challenge: ", pData, 16);
      
      // Compute response: HMAC(key, challenge)
      uint8_t response[32];
      bool success = computeHMAC(pairingKey, 16, pData, 16, response);
      
      if (success) {
        printHex("My Response: ", response, 32);
        
        Serial.println("\n📤 Sending response to vehicle...");
        
        // Send response
        pAuthCharacteristic->writeValue(response, 32);
        
        Serial.println("✓ Response sent!");
        Serial.println("⏳ Waiting for verification...\n");
        
      } else {
        Serial.println("❌ HMAC computation failed");
      }
    }
  }
  
  // Auth result received
  else if (charUUID == AUTH_CHAR_UUID) {
    std::string value((char*)pData, length);
    
    Serial.println("==================================================");
    if (value == "AUTH_OK") {
      authenticated = true;
      Serial.println("✅ AUTHENTICATION SUCCESSFUL!");
      Serial.println("==================================================");
      Serial.println("Phone has been granted access to vehicle");
      Serial.println("You can now control the vehicle");
    } else {
      authenticated = false;
      Serial.println("❌ AUTHENTICATION FAILED!");
      Serial.println("==================================================");
      Serial.println("Wrong key - Access denied");
    }
    Serial.println("==================================================\n");
  }
}

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* pClient) {
    Serial.println("✓ Connected to vehicle!");
  }

  void onDisconnect(BLEClient* pClient) {
    connected = false;
    authenticated = false;
    Serial.println("\n❌ Disconnected from vehicle\n");
  }
};

// ============================================
// CONNECT TO VEHICLE
// ============================================
bool connectToVehicle() {
  Serial.println("\n🔍 Scanning for vehicle...");
  
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  
  BLEScanResults foundDevices = pBLEScan->start(5);
  
  Serial.printf("Found %d devices\n", foundDevices.getCount());
  
  // Find vehicle
  BLEAdvertisedDevice* targetDevice = nullptr;
  
  for (int i = 0; i < foundDevices.getCount(); i++) {
    BLEAdvertisedDevice device = foundDevices.getDevice(i);
    
    if (device.haveServiceUUID() && device.isAdvertisingService(BLEUUID(SERVICE_UUID))) {
      Serial.println("\n✓ Found Smart Car Vehicle!");
      Serial.println("  Name: " + String(device.getName().c_str()));
      Serial.println("  Address: " + String(device.getAddress().toString().c_str()));
      
      targetDevice = new BLEAdvertisedDevice(device);
      break;
    }
  }
  
  if (targetDevice == nullptr) {
    Serial.println("❌ Vehicle not found");
    return false;
  }
  
  // Create client
  Serial.println("\n📡 Connecting to vehicle...");
  
  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new ClientCallbacks());
  
  if (!pClient->connect(targetDevice)) {
    Serial.println("❌ Connection failed");
    delete targetDevice;
    return false;
  }
  
  Serial.println("✓ Connected!");
  
  // Request larger MTU for sending 32-byte HMAC
  Serial.println("📡 Requesting MTU 512...");
  pClient->setMTU(517); // 517 = 512 data + 5 byte header
  delay(500); // Wait for MTU exchange
  Serial.println("✓ MTU request sent");
  
  // Get service
  BLERemoteService* pRemoteService = pClient->getService(SERVICE_UUID);
  if (pRemoteService == nullptr) {
    Serial.println("❌ Service not found");
    pClient->disconnect();
    delete targetDevice;
    return false;
  }
  
  Serial.println("✓ Service found!");
  
  // Get characteristics
  pChallengeCharacteristic = pRemoteService->getCharacteristic(CHALLENGE_CHAR_UUID);
  pAuthCharacteristic = pRemoteService->getCharacteristic(AUTH_CHAR_UUID);
  
  if (pChallengeCharacteristic == nullptr || pAuthCharacteristic == nullptr) {
    Serial.println("❌ Characteristics not found");
    pClient->disconnect();
    delete targetDevice;
    return false;
  }
  
  Serial.println("✓ Characteristics found!");
  
  // Register for notifications
  Serial.println("\n📝 Registering for notifications...");
  
  pChallengeCharacteristic->registerForNotify(notifyCallback);
  Serial.println("✓ Subscribed to Challenge notifications");
  
  pAuthCharacteristic->registerForNotify(notifyCallback);
  Serial.println("✓ Subscribed to Auth notifications");
  
  Serial.println("⏳ Waiting for challenge from vehicle...");
  
  // Give time for notifications to be registered
  delay(1000);
  
  // Read challenge manually if not received via notification
  Serial.println("\n🔍 Trying to read challenge manually...");
  std::string challengeValue = pChallengeCharacteristic->readValue();
  
  if (challengeValue.length() == 16) {
    Serial.println("✓ Challenge read successfully!");
    printHex("Challenge: ", (uint8_t*)challengeValue.c_str(), 16);
    
    // Compute and send response
    uint8_t response[32];
    bool success = computeHMAC(pairingKey, 16, (uint8_t*)challengeValue.c_str(), 16, response);
    
    if (success) {
      printHex("My Response: ", response, 32);
      
      Serial.println("\n📤 Sending response to vehicle...");
      Serial.println("Response size: " + String(sizeof(response)) + " bytes");
      
      // Debug: print first few bytes being sent
      Serial.print("First 8 bytes being sent: ");
      for (int i = 0; i < 8; i++) {
        if (response[i] < 16) Serial.print("0");
        Serial.print(response[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
      
      // Write response (no return value)
      pAuthCharacteristic->writeValue(response, 32);
      Serial.println("✓ Response write called");
      
      delay(1000);  // Tăng delay lên 1 giây
      
      // Read auth result
      Serial.println("\n🔍 Reading authentication result...");
      std::string authResult = pAuthCharacteristic->readValue();
      
      Serial.println("Auth Result length: " + String(authResult.length()));
      Serial.println("Auth Result: " + String(authResult.c_str()));
      
      if (authResult == "AUTH_OK") {
        authenticated = true;
        Serial.println("\n==================================================");
        Serial.println("✅ AUTHENTICATION SUCCESSFUL!");
        Serial.println("==================================================\n");
      } else if (authResult == "AUTH_FAIL") {
        Serial.println("\n==================================================");
        Serial.println("❌ AUTHENTICATION FAILED!");
        Serial.println("==================================================\n");
      } else {
        Serial.println("\n⚠️ Unknown auth result or still pending...");
      }
    }
  } else {
    Serial.println("❌ Challenge not available");
  }
  
  Serial.println();
  
  connected = true;
  delete targetDevice;
  
  return true;
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==================================================");
  Serial.println("📱 Smart Phone - BLE Client");
  Serial.println("==================================================");
  
  // Select key
  const char* keyToUse = useCorrectKey ? CORRECT_KEY_HEX : WRONG_KEY_HEX;
  hexStringToBytes(keyToUse, pairingKey, 16);
  
  Serial.println("Testing with: " + String(useCorrectKey ? "CORRECT" : "WRONG") + " key");
  printHex("Pairing Key: ", pairingKey, 16);
  Serial.println("==================================================\n");
  
  // Initialize BLE
  BLEDevice::init("SmartPhone");
  
  // Connect to vehicle
  connectToVehicle();
}

// ============================================
// LOOP
// ============================================
void loop() {
  // Monitor status
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 10000) {
    lastPrint = millis();
    
    if (connected) {
      Serial.println("--- Status ---");
      Serial.println("Connected: Yes");
      Serial.println("Authenticated: " + String(authenticated ? "Yes" : "No"));
      Serial.println("--------------\n");
    } else {
      Serial.println("⚠️  Not connected to vehicle");
      Serial.println("   Press RESET to retry\n");
    }
  }
  
  delay(1000);
}
