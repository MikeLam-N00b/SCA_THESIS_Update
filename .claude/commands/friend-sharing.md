# Friend Sharing — SCA Project

Bạn là senior engineer hiểu toàn bộ SCA system. Hỗ trợ implement/debug/review phần Friend Sharing.

## Architecture Friend Sharing

```
Owner Android App
  └─ POST /friend-sharing/create {vehicle_id, friend_name, ttl_hours}
       └─ Server: sinh friend_key (16 bytes random), ECDSA sign bundle
          └─ trả: {claim_token, friend_id, expires_at}

Owner share claim_token (QR / link) cho Friend

Friend Android App
  └─ GET /friend-sharing/claim/{claim_token}
       └─ Server: trả FriendKeyBundle {friend_key_hex, sig_b64, expires_at, ...}
          └─ Friend Android: lưu FriendShareStore

Friend Tag ESP32 (firmware cần implement)
  └─ Nhận FriendKeyBundle qua BLE từ Friend Android
  └─ Verify ECDSA sig (server public key)
  └─ Check expires_at
  └─ Dùng friend_key thay pairingKey cho HMAC challenge

Owner Android App
  └─ DELETE /friend-sharing/{friend_id}   # revoke
  └─ GET /friend-sharing/list/{vehicle_id}  # list
```

## Server crypto format
```python
# Bundle message được sign:
message = f"{friend_id}:{vehicle_id}:{friend_key_hex}:{expires_at}"
# expires_at format: ISO 8601 UTC, ví dụ "2026-04-30T10:00:00"

# FriendKeyBundle response:
{
    "friend_id": "abc123...",
    "vehicle_id": "VIN123",
    "friend_key_hex": "aabbccdd...",  # 32 hex chars = 16 bytes
    "expires_at": "2026-04-30T10:00:00",
    "sig_b64": "base64(ECDSA_signature)"
}
```

## Firmware verify pattern (chưa implement)
```c
// Cần implement trong Anchor firmware:
static bool verifyFriendBundle(const char* friendId, const char* vehicleId,
                                const char* keyHex, const char* expiresAt,
                                const uint8_t* sig, size_t sigLen) {
    // 1. Build message: "friendId:vehicleId:keyHex:expiresAt"
    char msg[256];
    snprintf(msg, sizeof(msg), "%s:%s:%s:%s", friendId, vehicleId, keyHex, expiresAt);

    // 2. Load server public key (hardcode DER hoặc lấy từ NVS)
    // 3. mbedtls_ecdsa_read_signature() verify
    // 4. Check expires_at < now (cần RTC hoặc timestamp từ BLE)
    // return true nếu valid
}
```

## Các phần cần làm

**Server** ✅ Done — không cần sửa

**Android** ❌ Cần làm:
- Fragment cho Owner: tạo friend share, nhập friend_name, ttl_hours
- Fragment cho Friend: nhập claim_token (hoặc scan QR)
- `FriendShareStore` lưu received bundle
- BLE: gửi FriendKeyBundle sang Anchor qua BLE characteristic

**Firmware** ❌ Cần làm:
- Parse FriendKeyBundle từ BLE JSON
- Verify ECDSA signature
- Check TTL expiry
- Dùng friend_key cho HMAC challenge

## Task / câu hỏi cụ thể:
$ARGUMENTS
