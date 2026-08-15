/*
 * SECURE KEY STORAGE FOR SMART CAR ACCESS - ARDUINO IDE
 * 
 * Hướng dẫn sử dụng trong Arduino IDE:
 * 1. Copy toàn bộ code này vào sketch của bạn
 * 2. Hoặc tạo file mới: Sketch -> Add File -> đặt tên "SecureKeyStorage.h"
 * 3. Tools -> Partition Scheme -> chọn "Default 4MB with spiffs"
 * 4. Upload và sử dụng
 * 
 * Cài đặt thư viện cần thiết:
 * - Preferences (đã có sẵn trong ESP32)
 * - mbedTLS (đã có sẵn trong ESP32)
 */

#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>

// ============================================
// CẤU TRÚC DỮ LIỆU
// ============================================

struct VehicleKeyData {
  uint8_t vehicle_key[32];      // AES-256 key từ server
  char vin[18];                 // VIN number
  uint64_t device_id;           // ESP32 chip ID
  uint32_t created_at;          // Timestamp tạo key
  uint32_t expires_at;          // Timestamp hết hạn
  uint8_t checksum[32];         // SHA-256 checksum
  uint8_t version;              // Version
  bool is_valid;                // Valid flag
};

// ============================================
// GLOBAL VARIABLES
// ============================================

Preferences preferences;
const char* KEY_NAMESPACE = "car_access";
const char* KEY_NAME = "vkey";
const uint8_t KEY_VERSION = 1;

// ============================================
// HELPER FUNCTIONS
// ============================================

// Lấy Device ID
uint64_t getDeviceID() {
  return ESP.getEfuseMac();
}

// Tính checksum
void calculateChecksum(VehicleKeyData* data, uint8_t* checksum) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  
  mbedtls_sha256_update(&ctx, data->vehicle_key, 32);
  mbedtls_sha256_update(&ctx, (uint8_t*)data->vin, 18);
  mbedtls_sha256_update(&ctx, (uint8_t*)&data->device_id, 8);
  mbedtls_sha256_update(&ctx, (uint8_t*)&data->created_at, 4);
  mbedtls_sha256_update(&ctx, (uint8_t*)&data->expires_at, 4);
  mbedtls_sha256_update(&ctx, &data->version, 1);
  
  mbedtls_sha256_finish(&ctx, checksum);
  mbedtls_sha256_free(&ctx);
}

// Verify checksum
bool verifyChecksum(VehicleKeyData* data) {
  uint8_t calculated[32];
  calculateChecksum(data, calculated);
  return memcmp(calculated, data->checksum, 32) == 0;
}

// ============================================
// MAIN FUNCTIONS - DÙNG TRỰC TIẾP
// ============================================

// LƯU KEY (gọi sau khi nhận từ server)
bool saveVehicleKey(const uint8_t* vehicle_key, const char* vin) {
  VehicleKeyData data;
  
  // Copy key
  memcpy(data.vehicle_key, vehicle_key, 32);
  
  // Copy VIN
  strncpy(data.vin, vin, 17);
  data.vin[17] = '\0';
  
  // Device binding
  data.device_id = getDeviceID();
  
  // Timestamps (1 năm = 31536000 giây)
  data.created_at = millis() / 1000;
  data.expires_at = data.created_at + (365UL * 24 * 60 * 60);
  
  // Metadata
  data.version = KEY_VERSION;
  data.is_valid = true;
  
  // Tính checksum
  calculateChecksum(&data, data.checksum);
  
  // Lưu vào NVS
  preferences.begin(KEY_NAMESPACE, false);
  size_t written = preferences.putBytes(KEY_NAME, &data, sizeof(VehicleKeyData));
  preferences.end();
  
  if (written != sizeof(VehicleKeyData)) {
    Serial.println("✗ Lưu key thất bại");
    return false;
  }
  
  Serial.println("✓ Đã lưu key thành công");
  Serial.print("  VIN: "); Serial.println(vin);
  Serial.print("  Device ID: "); Serial.println((unsigned long)data.device_id, HEX);
  
  return true;
}

// ĐỌC KEY (gọi khi cần dùng cho BLE/unlock)
bool loadVehicleKey(VehicleKeyData* data) {
  preferences.begin(KEY_NAMESPACE, true);
  size_t len = preferences.getBytes(KEY_NAME, data, sizeof(VehicleKeyData));
  preferences.end();
  
  // Kiểm tra có key không
  if (len != sizeof(VehicleKeyData)) {
    Serial.println("✗ Không tìm thấy key");
    return false;
  }
  
  // Kiểm tra version
  if (data->version != KEY_VERSION) {
    Serial.println("✗ Version key không đúng");
    return false;
  }
  
  // Verify checksum
  if (!verifyChecksum(data)) {
    Serial.println("✗ Key bị hỏng (checksum sai)");
    return false;
  }
  
  // Verify device binding
  if (data->device_id != getDeviceID()) {
    Serial.println("✗ Key không thuộc thiết bị này");
    return false;
  }
  
  // Kiểm tra hết hạn
  uint32_t now = millis() / 1000;
  if (data->expires_at > 0 && now > data->expires_at) {
    Serial.println("✗ Key đã hết hạn");
    return false;
  }
  
  // Kiểm tra valid flag
  if (!data->is_valid) {
    Serial.println("✗ Key đã bị vô hiệu hóa");
    return false;
  }
  
  Serial.println("✓ Đọc key thành công");
  return true;
}

// KIỂM TRA CÓ KEY KHÔNG
bool hasVehicleKey() {
  preferences.begin(KEY_NAMESPACE, true);
  bool exists = preferences.isKey(KEY_NAME);
  preferences.end();
  return exists;
}

// XÓA KEY
bool eraseVehicleKey() {
  preferences.begin(KEY_NAMESPACE, false);
  bool removed = preferences.remove(KEY_NAME);
  preferences.end();
  
  if (removed) {
    Serial.println("✓ Đã xóa key");
  } else {
    Serial.println("✗ Xóa key thất bại");
  }
  return removed;
}

// IN THÔNG TIN KEY
void printKeyInfo() {
  VehicleKeyData data;
  
  if (!loadVehicleKey(&data)) {
    Serial.println("Không có thông tin key");
    return;
  }
  
  Serial.println("\n=== THÔNG TIN KEY ===");
  Serial.print("VIN: "); Serial.println(data.vin);
  Serial.print("Device ID: "); Serial.println((unsigned long)data.device_id, HEX);
  
  uint32_t age = (millis() / 1000) - data.created_at;
  Serial.print("Tuổi: "); Serial.print(age / 86400); Serial.println(" ngày");
  
  if (data.expires_at > 0) {
    uint32_t remaining = data.expires_at - (millis() / 1000);
    Serial.print("Còn lại: "); Serial.print(remaining / 86400); Serial.println(" ngày");
  } else {
    Serial.println("Hết hạn: Không");
  }
  
  Serial.print("Trạng thái: "); Serial.println(data.is_valid ? "Hợp lệ" : "Không hợp lệ");
  Serial.println("=====================\n");
}

// ============================================
// VÍ DỤ SỬ DỤNG TRONG SKETCH CỦA BẠN
// ============================================

// GLOBAL để lưu key hiện tại
VehicleKeyData currentKey;

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n=== Smart Car Access ===\n");
  
  // Kiểm tra có key chưa
  if (hasVehicleKey()) {
    Serial.println("Đã có key trong bộ nhớ");
    printKeyInfo();
    
    // Load key vào biến global
    if (loadVehicleKey(&currentKey)) {
      Serial.println("✓ Sẵn sàng sử dụng\n");
      // Bây giờ bạn có thể dùng currentKey.vehicle_key
    }
  } else {
    Serial.println("Chưa có key - cần request từ server\n");
    
    // TODO: Gọi hàm request key từ server của bạn
    // requestKeyFromServer();
    
    // DEMO: Giả lập nhận key từ server
    demoSaveKey();
  }
  
  // Setup BLE, UWB của bạn ở đây...
}

void loop() {
  // Code BLE, UWB của bạn...
  delay(1000);
}

// ============================================
// INTEGRATION VỚI CODE CỦA BẠN
// ============================================

// 1. SAU KHI NHẬN KEY TỪ SERVER (HTTP response)
void onServerResponseReceived(String response) {
  // Giả sử server trả về JSON: {"vehicle_key": "0123456789ABCDEF...", "vin": "1HGBH41JXMN109186"}
  
  // Parse JSON (dùng ArduinoJson hoặc parse thủ công)
  // Ví dụ đơn giản:
  int keyStart = response.indexOf("vehicle_key\":\"") + 14;
  int keyEnd = response.indexOf("\"", keyStart);
  String keyHex = response.substring(keyStart, keyEnd);
  
  int vinStart = response.indexOf("vin\":\"") + 6;
  int vinEnd = response.indexOf("\"", vinStart);
  String vin = response.substring(vinStart, vinEnd);
  
  // Convert hex string to bytes
  uint8_t key[32];
  for (int i = 0; i < 32; i++) {
    String byteStr = keyHex.substring(i*2, i*2 + 2);
    key[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
  }
  
  // LƯU KEY
  if (saveVehicleKey(key, vin.c_str())) {
    // Load vào biến global để dùng
    loadVehicleKey(&currentKey);
    Serial.println("Key đã sẵn sàng sử dụng!");
  }
}

// 2. DÙNG KEY CHO BLE AUTHENTICATION
void setupBLEWithKey() {
  VehicleKeyData keyData;
  
  if (!loadVehicleKey(&keyData)) {
    Serial.println("Không có key - không thể setup BLE");
    return;
  }
  
  // Dùng keyData.vehicle_key cho BLE security
  // Ví dụ: tạo passkey từ key
  uint32_t passkey = 0;
  memcpy(&passkey, keyData.vehicle_key, 4);
  
  Serial.print("BLE Passkey: ");
  Serial.println(passkey);
  
  // Setup BLE security với passkey này
  // BLESecurity* pSecurity = new BLESecurity();
  // pSecurity->setStaticPIN(passkey);
  // ...
}

// 3. MÃ HÓA UNLOCK COMMAND
void sendEncryptedUnlock() {
  VehicleKeyData keyData;
  
  if (!loadVehicleKey(&keyData)) {
    Serial.println("Không thể unlock - không có key");
    return;
  }
  
  // Plaintext command
  uint8_t command[16] = "UNLOCK";
  uint8_t encrypted[16];
  
  // Encrypt bằng AES
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, keyData.vehicle_key, 256);
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, command, encrypted);
  mbedtls_aes_free(&aes);
  
  // Gửi qua BLE
  Serial.println("Gửi lệnh unlock đã mã hóa...");
  // pCharacteristic->setValue(encrypted, 16);
  // pCharacteristic->notify();
}

// 4. XỬ LÝ UWB RANGING (nếu cần verify key)
void onUWBRangingComplete(float distance) {
  if (distance < 2.0) {  // Trong vòng 2m
    Serial.println("Trong phạm vi - gửi unlock");
    
    // Kiểm tra có key không
    if (hasVehicleKey()) {
      sendEncryptedUnlock();
    } else {
      Serial.println("Cần request key từ server trước");
    }
  }
}

// ============================================
// DEMO FUNCTION
// ============================================

void demoSaveKey() {
  Serial.println("DEMO: Tạo key mẫu...\n");
  
  // Key mẫu (trong thực tế nhận từ server)
  uint8_t demo_key[32] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00
  };
  
  const char* demo_vin = "1HGBH41JXMN109186";
  
  saveVehicleKey(demo_key, demo_vin);
  
  // Load lại để test
  if (loadVehicleKey(&currentKey)) {
    printKeyInfo();
  }
}

// ============================================
// UTILITY: Convert bytes to hex string (để debug)
// ============================================

String bytesToHex(const uint8_t* bytes, size_t len) {
  String hex = "";
  for (size_t i = 0; i < len; i++) {
    if (bytes[i] < 16) hex += "0";
    hex += String(bytes[i], HEX);
  }
  hex.toUpperCase();
  return hex;
}