# Smart Car Access System - Source Code

## Tổng quan

Hệ thống khóa thông minh xe hơi sử dụng BLE + UWB, được tổ chức theo kiến trúc modular cho Arduino IDE.

## Cấu trúc

```
src/
├── tag/SmartCarTag/          # User device (chìa khóa di động)
│   ├── SmartCarTag.ino       # Main entry point
│   ├── config.h              # Configuration constants
│   ├── crypto.h/cpp          # Key management, ECDH, AES-GCM
│   ├── server_api.h/cpp      # HTTP API with server
│   ├── ble_handler.h/cpp     # BLE client communication
│   └── uwb_ranging.h/cpp     # UWB ranging (initiator)
│
└── anchor/SmartCarAnchor/    # Vehicle device
    ├── SmartCarAnchor.ino    # Main entry point
    ├── config.h              # Configuration constants
    ├── crypto.h/cpp          # Key verification, session
    ├── ble_server.h/cpp      # BLE server communication
    ├── uwb_ranging.h/cpp     # UWB ranging (responder)
    └── vehicle_features.h/cpp # Vehicle control (unlock/lock)
```

## Flow theo Sequence Diagram

### Tag (User Device):
1. **Bước 1-3**: User nhập VIN → Server generate Vehicle_K → Tag lưu vào NVS
2. **Bước 4**: Start BLE scan, tìm vehicle anchor
3. **Bước 5-6**: Check stored key, request từ server nếu không có
4. **Bước 7-10**: BLE key exchange và verify
5. **Bước 11**: Secure session established
6. **Bước 14-15**: Request unlock
7. **Bước 16-17**: Start UWB ranging

### Anchor (Vehicle):
1. **Bước 4**: BLE advertising (chờ tag kết nối)
2. **Bước 5**: Check pairing data có sẵn không
3. **Bước 7-8**: Respond to BLE key exchange
4. **Bước 9-10**: Verify key match và create session
5. **Bước 11**: Session established
6. **Bước 15**: Process unlock request
7. **Bước 17**: Respond to UWB ranging

## Cài đặt

### Yêu cầu:
- Arduino IDE 2.x
- ESP32 board support
- Thư viện:
  - DWM3000 (từ folder DWM3000/)
  - ArduinoJson
  - Preferences (built-in ESP32)
  - mbedTLS (built-in ESP32)

### Cấu hình:

#### Tag (SmartCarTag):
1. Mở file `src/tag/SmartCarTag/config.h`
2. Cấu hình WiFi:
   ```cpp
   #define WIFI_SSID "your_wifi_ssid"
   #define WIFI_PASSWORD "your_wifi_password"
   ```
3. Cấu hình Server URL:
   ```cpp
   #define SERVER_URL "http://your_server_ip:8000"
   ```
4. Cấu hình Vehicle ID:
   ```cpp
   #define VEHICLE_ID "VIN123456"
   ```

#### Anchor (SmartCarAnchor):
1. Mở file `src/anchor/SmartCarAnchor/config.h`
2. Cấu hình Vehicle ID phải khớp với Tag:
   ```cpp
   #define VEHICLE_ID "VIN123456"
   ```

### Upload code:

1. **Anchor (Vehicle)**:
   - Mở `src/anchor/SmartCarAnchor/SmartCarAnchor.ino` trong Arduino IDE
   - Chọn board: ESP32 Dev Module
   - Chọn port
   - Upload

2. **Tag (User Device)**:
   - Mở `src/tag/SmartCarTag/SmartCarTag.ino` trong Arduino IDE
   - Chọn board: ESP32 Dev Module
   - Chọn port
   - Upload

## Sử dụng

### 1. Khởi động Anchor (Vehicle)
```
========================================
  Smart Car Anchor - Vehicle Device
========================================
Vehicle ID: VIN123456
Device: SmartCarAnchor_01
========================================

[Crypto] Initializing...
[Crypto] ✓ Initialized
[BLE] Initializing server...
[BLE] ✓ Server started
[BLE] 📡 Advertising...

[Main] ✓ System ready!
[Main] Waiting for tag connection...
```

### 2. Khởi động Tag (User Device)
```
========================================
    Smart Car Tag - User Device
========================================
Vehicle ID: VIN123456
Device: SmartCarTag_01
========================================

[Crypto] Initializing...
[Crypto] ✓ Initialized

[State] Checking pairing status...
[State] No pairing key found
[State] === Starting Pairing Process ===

[WiFi] Connecting...
[WiFi] ✓ Connected!

[Server] === Starting Pairing Process ===
[Crypto] Generating EC key pair...
[Server] Pairing ID: abc123
[Crypto] ✓ Pairing key decrypted
[Server] ✓ Pairing successful!
```

### 3. Kết nối BLE
Tag sẽ tự động scan và kết nối với Anchor:
```
[BLE] 🔍 Scanning for Anchor...
[BLE] ✓ Found Anchor
[BLE] 🔗 Connecting to Anchor...
[BLE] ✓ Connected to Anchor!
```

### 4. Key Exchange
```
[BLE] === Performing Key Exchange ===
[BLE] Step 7: Initiating key exchange...
[BLE] Step 9: Verifying key...
[BLE] Step 11: ✓ Secure session established!
```

### 5. Menu Commands (Tag)
```
========================================
    System Ready - Available Commands:
========================================
  1 - Request Unlock
  2 - Start Ranging
  3 - Status Info
========================================
Enter command:
```

- **Nhấn '1'**: Gửi unlock request đến vehicle
- **Nhấn '2'**: Bắt đầu UWB ranging (đo khoảng cách)
- **Nhấn '3'**: Hiển thị system status

### 6. Unlock Vehicle
```
[State] Processing unlock request...
[BLE] Step 15: ✓ Unlock request sent

// Trên Anchor:
[Main] Processing unlock request...
[Vehicle] === UNLOCK ===
[Vehicle] 🔓 Door unlocked!
[Main] ✓ Vehicle unlocked
```

### 7. UWB Ranging
```
[State] Starting ranging mode...
[UWB] Initializing (Tag/Initiator)...
[UWB] ✓ Initialized and ready!

[UWB] 📏 Distance: 2.45 m
[UWB] 📏 Distance: 2.38 m
[UWB] 📏 Distance: 2.41 m

// Nhấn 'x' để thoát ranging mode
```

## Troubleshooting

### BLE không kết nối:
- Kiểm tra BLE_SERVICE_UUID và BLE_CHARACTERISTIC_UUID giống nhau ở cả Tag và Anchor
- Reset cả hai thiết bị

### WiFi không kết nối:
- Kiểm tra SSID và password trong config.h
- Kiểm tra signal WiFi

### UWB không hoạt động:
- Kiểm tra kết nối DWM3000 module
- Kiểm tra pin configuration (RST, IRQ, SS)
- Kiểm tra SPI connections

### Pairing failed:
- Kiểm tra server đang chạy
- Kiểm tra SERVER_URL đúng
- Kiểm tra VEHICLE_ID

## Debug

Để bật full debug log, trong Arduino IDE:
- Tools → Core Debug Level → Verbose

Serial Monitor settings:
- Baud rate: 115200
- Line ending: Newline

## Bảo mật

### Stored Keys:
- **Vehicle_K**: Lưu trong NVS với SHA256 hash verification
- **Pairing_K**: Lưu trong NVS, encrypted từ server
- **Session_K**: Lưu tạm thời, tự động xóa khi disconnect

### Encryption:
- ECDH (Elliptic Curve Diffie-Hellman) cho key exchange
- HKDF (HMAC-based Key Derivation) cho KEK
- AES-GCM cho encrypted pairing key

## Tác giả

Smart Car Access System
Version 1.0 - January 2026
