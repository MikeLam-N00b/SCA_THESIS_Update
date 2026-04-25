# Debug Issue — SCA Project

Phân tích và tìm nguyên nhân lỗi dưới đây. Áp dụng systematic debugging approach.

## Step 1: Phân loại lỗi

**Firmware crash/hang?**
- Crash → stack overflow? `uxTaskGetStackHighWaterMark()` check
- Hang → deadlock? Task nào đang chờ mutex/queue?
- Watchdog reset → task bị block quá lâu?
- PANIC → null pointer? array out of bounds?

**Auth fail?**
- `AUTH_FAIL` → key mismatch giữa Anchor NVS và Android KeyManager?
- Challenge stale → `connectionGen` không match?
- HMAC mismatch → byte order? key length? challenge length?

**UWB không hoạt động?**
- `dwt_checkidlerc()` timeout → power/reset issue?
- STS quality fail → IV không sync? Key không khớp?
- Timing → `POLL_RX_TO_RESP_TX_DLY_UUS` quá nhỏ?
- SPI conflict → MCP2515 và DW3000 tranh bus?

**CAN không gửi được?**
- `spiMutex` timeout → uwbTask hold quá lâu?
- MCP2515 init fail → CS pin conflict?
- `dwt_forcetrxoff()` chưa gọi trước CAN op?

**Server 4xx/5xx?**
- 404 → vehicle_id không tồn tại trong DB?
- 400 → Pydantic validation fail → kiểm tra field format?
- 500 → SQLite error → kiểm tra schema migration?
- Crypto error → ECDH/GCM params không khớp?

**Android BLE issue?**
- Scan không thấy → SERVICE_UUID mismatch?
- Connect fail → device busy / already connected?
- Write fail → MTU size? Characteristic property?
- Notify miss → CCCD chưa subscribe?

## Step 2: Gather info

Cung cấp:
- Serial Monitor output (ESP32) hoặc Logcat (Android) hoặc server log
- Module liên quan: Anchor / Tag / Server / Android
- Thời điểm xảy ra: boot / sau N giây / khi thực hiện action gì
- Có reproducible không?

## Step 3: Debug commands

```bash
# Server: kiểm tra DB
sqlite3 SCA/Server/car_access.db "SELECT * FROM vehicles;"
sqlite3 SCA/Server/car_access.db "SELECT friend_id, is_revoked, expires_at FROM friend_keys;"

# Server: test endpoint
python3 SCA/Server/anchor_client.py   # simulate Anchor
python3 SCA/Server/tag_client.py      # simulate Tag
python3 SCA/Server/friend_client.py   # simulate Friend flow

# ESP32: xem NVS keys
python3 SCA/Tools/view_keys.py
# ESP32: xóa NVS (reset về trạng thái chưa pair)
python3 SCA/Tools/nvs_clear.py
```

## Mô tả lỗi:
$ARGUMENTS
