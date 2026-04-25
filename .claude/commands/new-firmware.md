# New Feature — Firmware (SCA)

Bạn là senior embedded systems engineer. Implement tính năng firmware mới cho SCA project theo đúng architecture hiện tại.

## Yêu cầu bắt buộc

### Architecture phải tuân theo
```c
// 1. Logic phức tạp → tạo task riêng + queue
// 2. BLE callback → chỉ xQueueSend, không xử lý
// 3. Mọi SPI access → lấy spiMutex trước
// 4. Crypto → mbedTLS, không tự implement

// Task creation pattern
xTaskCreatePinnedToCore(
    myTask,         // function
    "MY_Task",      // name
    STACK_SIZE,     // words (không phải bytes!)
    NULL,           // params
    PRIORITY,       // tskIDLE_PRIORITY + N
    NULL,           // handle
    CORE_ID         // 0 = BLE core, 1 = timing-critical
);
```

### Nếu feature liên quan đến Friend Sharing
```c
// Bắt buộc: verify ECDSA sig trước khi dùng friend_key
// 1. Nhận FriendKeyBundle qua BLE (JSON: friend_id, vehicle_id, friend_key_hex, expires_at, sig_b64)
// 2. Decode sig từ base64
// 3. Verify: server_pubkey.verify(sig, message, ECDSA(SHA256))
//    message = f"{friend_id}:{vehicle_id}:{friend_key_hex}:{expires_at}"
// 4. Check expires_at chưa quá hạn
// 5. Nếu valid → dùng friend_key_hex (hex → bytes) thay pairingKey

// KHÔNG: lưu key trước, verify sau
// KHÔNG: skip verify vì "đã check trên server"
```

### Nếu feature cần SIM/HTTP
```c
// Dùng simAtCmd() / simHttpPost() pattern đã có
// AT commands: simAtOk(cmd) hoặc simAtCmd(cmd, token, timeout)
// HTTP: simHttpPost(url, jsonBody) → String response
// Parse response: StaticJsonDocument<N> doc; deserializeJson(doc, response)
```

### Output cần có
1. Header file (nếu tạo module mới): include guard, public API
2. Implementation: full function bodies với error handling
3. Integration point: chỗ gọi trong setup() hoặc task nào
4. Resource usage: stack estimate, queue depth, mutex nào dùng

## Tính năng cần implement:
$ARGUMENTS
