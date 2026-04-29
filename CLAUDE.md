# Smart Car Access (SCA) — Project Context

> Đọc file này trước khi làm bất kỳ task nào trong project SCA.

---

## Kiến trúc tổng quan

```
Android App (Kotlin)
    │ Retrofit2 HTTP
    ▼
FastAPI Server (Python) ──── SQLite (car_access.db)
    │ HTTP (A7680C AT cmd)       vehicles, friend_keys tables
    ▼
Anchor ESP32-S3 (FreeRTOS)
    │ BLE (GATT)
    ▼
Tag ESP32-S3 (FreeRTOS)
    │ UWB SS-TWR (DW3000 STS)
    └─ khoảng cách < ngưỡng → CAN unlock (MCP2515)
```

### Hai flow chính
**Owner Flow**: Android → `/owner-pairing` → pairing_key → Anchor lưu NVS → BLE HMAC auth → UWB → CAN unlock

**Friend Sharing Flow** *(đang phát triển)*:
```
Owner Android → POST /friend-sharing/create → claim_token
Friend Android → GET /friend-sharing/claim/{token} → FriendKeyBundle (friend_key + ECDSA sig)
Anchor → POST /validate-friend-key → verify sig → friend_key encrypted
Tag ESP32 (friend) → decrypt → lưu NVS → HMAC auth với friend_key
```

---

## [FIRMWARE] FreeRTOS Architecture

### Hardware (QUAN TRỌNG: DW3000 + MCP2515 share SPI2 bus)
| Component | Chip      | Bus   | Role                        |
|-----------|-----------|-------|-----------------------------|
| Anchor    | ESP32-S3  | —     | BLE server, CAN controller  |
| Tag       | ESP32-S3  | —     | BLE client, UWB initiator   |
| UWB       | DW3000    | SPI2  | SS-TWR ranging, STS anti-relay |
| CAN       | MCP2515   | SPI2  | UDS over CAN 100kbps        |
| LTE       | A7680C    | UART2 | AT commands, HTTP POST      |

### Task pinning (Anchor)
```
Core 0: bleTask  (Priority 3) — BLE stack + HMAC auth
Core 1: uwbTask  (Priority 4) — DW3000 timing-critical (HIGHEST)
Core 1: canTask  (Priority 2) — MCP2515 CAN commands
```

### IPC primitives
```c
EventGroupHandle_t sysEvents;  // EVT_CONNECTED(0), EVT_AUTHED(1), EVT_UWB_ACTIVE(2)
QueueHandle_t bleQueue;        // BleCmdMsg {type, data[32], dataLen}, depth 8
QueueHandle_t uwbQueue;        // uint8_t {UWB_CMD_INIT, UWB_CMD_DEINIT}, depth 4
QueueHandle_t canQueue;        // uint8_t {CAN_CMD_LOCK, CAN_CMD_UNLOCK}, depth 4
SemaphoreHandle_t spiMutex;   // arbitrate DW3000 vs MCP2515
```

### SPI mutex — BẮT BUỘC dùng khi truy cập DW3000 hoặc MCP2515
```c
if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    // DW3000 hoặc MCP2515 operation
    xSemaphoreGive(spiMutex);
} else {
    Serial.println("[TASK] spiMutex timeout");
    // handle error, KHÔNG crash
}
```

### BLE callback rule — callbacks chỉ được xQueueSend, KHÔNG xử lý logic
```c
// ĐÚNG
void onWrite(BLECharacteristic* pChar) override {
    BleCmdMsg msg = { .type = BLE_AUTH_VERIFY, .dataLen = 32 };
    memcpy(msg.data, pChar->getData(), 32);
    xQueueSend(bleQueue, &msg, pdMS_TO_TICKS(10));  // non-blocking
}
// SAI: tính toán HMAC, gọi CAN, delay trong callback → race condition / watchdog
```

### connectionGen pattern — tránh stale challenge
```c
// onConnect tăng connectionGen, đính kèm vào BLE_SEND_CHALLENGE
// bleTask kiểm tra: if (myGen != connectionGen) break; // bỏ qua lệnh cũ
```

### Firmware coding rules (từ embedded-systems skill)
- `volatile` cho mọi biến shared với ISR hoặc giữa tasks
- `float` thay `double` — ESP32-S3 FPU chỉ hỗ trợ single-precision (double ~10x chậm)
- `snprintf` thay `sprintf` — không ngoại lệ
- `getData()/getLength()` thay `getValue()` trong BLE callbacks (zero-copy, no heap)
- `memcmp` thay `String.startsWith()` trong callbacks (no String heap allocation)
- ISR: chỉ set flag / send queue, KHÔNG xử lý logic, KHÔNG blocking call
- `portYIELD_FROM_ISR(xHigherPriorityTaskWoken)` sau mọi `FromISR` API call
- `configASSERT(uxTaskGetStackHighWaterMark(NULL) > 32)` trong debug builds

### UWB STS anti-relay
```c
// Reload STS IV trước mỗi RX cycle để sync với Tag
dwt_configurestsloadiv();
// Kiểm tra STS quality — reject frame nếu invalid (relay attack)
if (dwt_readstsquality(&stsQual) < 0) { dwt_forcetrxoff(); return; }
// STS key = memcpy từ pairingKey (16 bytes) → cả Anchor & Tag dùng chung
```

### CAN (MCP2515) — deselect DW3000 trước khi dùng bus
```c
// Trong canTask, sau khi lấy spiMutex:
if (xEventGroupGetBits(sysEvents) & EVT_UWB_ACTIVE) {
    dwt_forcetrxoff();           // DW3000 idle
    digitalWrite(PIN_SS, HIGH); // deselect DW3000 CS
}
// Sau đó mới gọi MCP2515 operations
```

### KHÔNG làm trong firmware
- Blocking operations trong ISR
- Dynamic allocation (`malloc`/`new`) không có bounds checking
- Cross-core BLE `writeValue` từ Core 1 → dùng `bleWriteQueue`
- NVS read/write từ ISR context
- NVS key string > 15 chars (Preferences library limit)
- `portMAX_DELAY` trừ khi có lý do rõ ràng trong comment

---

## [SERVER] FastAPI + SQLite

### Database schema
```sql
-- vehicles: xe đã pair
CREATE TABLE vehicles (
    vehicle_id TEXT PRIMARY KEY,
    pairing_id TEXT NOT NULL,
    pairing_key TEXT NOT NULL,  -- base64 encoded
    created_at TEXT NOT NULL
);

-- friend_keys: friend sharing
CREATE TABLE friend_keys (
    friend_id   TEXT PRIMARY KEY,
    vehicle_id  TEXT NOT NULL,
    friend_key  TEXT NOT NULL,
    friend_name TEXT,
    expires_at  TEXT NOT NULL,
    owner_sig   TEXT NOT NULL,   -- ECDSA signature (base64)
    is_revoked  INTEGER DEFAULT 0,
    created_at  TEXT NOT NULL,
    claim_token TEXT UNIQUE NOT NULL
);
```

### API Endpoints
| Method | Path | Mô tả |
|--------|------|-------|
| POST | `/secure-check-pairing` | Anchor fetch pairing key (ECDH P-256 + AES-GCM) |
| POST | `/owner-pairing` | Android pair xe |
| GET | `/check-pairing/{vehicle_id}` | Check pairing status |
| GET | `/vehicle/{vehicle_id}` | Thông tin xe |
| GET | `/vehicles` | Danh sách xe |
| DELETE | `/vehicle/{vehicle_id}` | Xóa xe |
| POST | `/friend-sharing/create` | Owner tạo friend share |
| GET | `/friend-sharing/claim/{token}` | Friend lấy key bundle |
| POST | `/validate-friend-key` | Anchor validate friend key |
| DELETE | `/friend-sharing/{friend_id}` | Revoke friend key |
| GET | `/friend-sharing/list/{vehicle_id}` | Danh sách friend keys |
| GET | `/server-public-key` | Server ECDSA public key |

### Crypto flow (Server)
```
/secure-check-pairing:
  1. Anchor gửi: vehicle_id + client_public_key_b64 (ECDH P-256)
  2. Server: ECDH shared secret → HKDF → KEK 16 bytes
  3. Server: AES-128-GCM encrypt(pairing_key) → encrypted_data_b64 + nonce_b64
  4. Server trả: server_public_key_b64 + encrypted_data_b64 + nonce_b64

/friend-sharing: ECDSA P-256 sign bundle:
  message = f"{friend_id}:{vehicle_id}:{friend_key_hex}:{expires_at}"
  signature = server_signing_key.sign(message, ECDSA(SHA256))
```

### FastAPI coding rules (từ fastapi-expert skill)
```python
# ĐÚNG: Pydantic V2, sync endpoint (SQLite không async)
class FriendShareCreateRequest(BaseModel):
    vehicle_id: str
    friend_name: str
    ttl_hours: int = Field(gt=0, le=168)  # validate input

@app.post("/friend-sharing/create", response_model=FriendShareCreateResponse)
def create_friend_share(req: FriendShareCreateRequest):
    vehicle = get_vehicle_pairing(req.vehicle_id)
    if not vehicle:
        raise HTTPException(status_code=404, detail="Vehicle not found")
    ...

# SAI: Pydantic V1 syntax
# class Config: → dùng model_config = ModelConfig(...)
# @validator → dùng @field_validator
```

### Python coding rules (từ python-pro skill)
- Type hints cho mọi function signature
- `X | None` thay `Optional[X]` (Python 3.10+)
- Parameterized SQL queries — KHÔNG string concatenation
- `secrets.token_hex()` cho token generation
- Không expose sensitive data (pairing_key, sig) trong error messages

### KHÔNG làm trong server
- Raw string SQL: `f"SELECT * WHERE id = {user_input}"` → SQL injection
- Log pairing_key hoặc private key raw
- Return internal exception details (stack trace) trong HTTP response
- Dùng `SELECT *` trong production queries

---

## [ANDROID] Kotlin App

### Package structure
```
com.example.uwb/
├── UI/              # Fragments: Welcome, Login, EnterVin, VerifyingVin,
│                    #   PairingLoading, VehicleInfo
├── Bluetooth/       # BluetoothManager (BLE scan + GATT)
├── network/         # ApiService (Retrofit suspend), ApiClient
├── model/           # PairingRequest/Response, FriendShareRequest/Response,
│                    #   FriendKeyBundle
├── dataLg/          # KeyManager, PairedDeviceStore, FriendShareStore, SessionManager
├── repository/      # PairingRepository, UsbRepository
├── usb/             # USB OTG: UsbManagerHelper, UsbConnection, UsbConstants
└── VinValid/        # VinValidator
```

### KeyManager — pattern quan trọng nhất
```kotlin
// LUÔN check KeyManager trước khi gọi /owner-pairing
// Server tạo key mới mỗi lần → Anchor vẫn giữ key cũ trong NVS → AUTH_FAIL
fun pairVehicle(vin: String) {
    val existingKey = KeyManager.loadPairingKey(vin)
    if (existingKey != null) {
        // Tái sử dụng — không gọi /owner-pairing
        proceedWithExistingKey(existingKey)
        return
    }
    // Chỉ gọi API nếu thực sự chưa có key
    apiService.ownerPairing(PairingRequest(vin, pubKeyB64))
}
```

### Kotlin coding rules (từ kotlin-specialist skill)
```kotlin
// ĐÚNG: structured concurrency với lifecycleScope
lifecycleScope.launch {
    try {
        val response = withContext(Dispatchers.IO) {
            apiService.createFriendShare(req)
        }
        // update UI trên Main dispatcher
        updateUI(response)
    } catch (e: Exception) {
        handleError(e)
    }
}

// SAI
GlobalScope.launch { ... }     // memory leak
runBlocking { ... }            // blocks main thread
val result = apiService.call() // blocking call on main thread
```

### Kotlin KHÔNG làm
- `!!` trừ khi contract đảm bảo non-null (comment lý do)
- `GlobalScope.launch` → dùng `viewModelScope` hoặc `lifecycleScope`
- Ignore coroutine cancellation (cancel parent scope on teardown)
- Platform-specific code trong common modules (nếu sau này KMP)

---

## [SECURITY] Checklist tích hợp

### Firmware security
- [x] HMAC-SHA256 challenge-response (mbedTLS)
- [x] STS mode 1 trên UWB — chống relay attack
- [x] `connectionGen` — chống stale session replay
- [ ] Constant-time HMAC compare (`mbedtls_ct_memcmp` thay `memcmp`)
- [ ] Watchdog timer cho mỗi task

### Server security
- [x] ECDH P-256 + AES-128-GCM cho key exchange
- [x] ECDSA P-256 sign friend bundle
- [x] `secrets.token_hex()` cho claim_token
- [x] `is_revoked` + TTL check
- [ ] Rate limiting trên `/owner-pairing` và `/secure-check-pairing`
- [ ] HTTPS (hiện HTTP) cho production

### Android security
- [x] SharedPreferences lưu pairing key
- [ ] Android Keystore cho production (SharedPreferences không encrypted)
- [ ] Certificate pinning cho Retrofit client

---

## Friend Sharing — Trạng thái hoàn chỉnh

### Status (2026-04-29)

| Layer | File | Trạng thái |
|-------|------|-----------|
| Server | `Server/main_2.py` | ✅ Hoàn chỉnh — dùng `main_2.py`, KHÔNG `main.py` |
| Android model | `model/FriendKeyBundle.kt` | ✅ Đã fix fields: `issuer_sig_b64`, `bundle_version`, `permissions`, `issued_at` |
| Android network | `network/ApiService.kt` | ✅ `createFriendShare`, `claimFriendShare`, `getServerPublicKey` |
| Android UI | `UI/FriendSharingFragment.kt` | ✅ Owner tạo QR + Friend quét QR + claim bundle |
| Android UI | `UI/OwnerFriendListFragment.kt` | ✅ Quản lý danh sách friend keys (revoke, xem) |
| Android crypto | `crypto/EcdsaVerifier.kt` | ✅ Verify ECDSA trên Android trước khi nạp Tag |
| Android provisioning | `Bluetooth/BluetoothFragment.kt` | ✅ USB provisioning: `SET_SERVER_PUBKEY`, `SET_TIME`, `SET_BUNDLE` |
| Android manifest | `AndroidManifest.xml` | ✅ `launchMode="singleTop"` — fix USB attach reset back stack |
| Anchor firmware | `FreeRTOS_Anchor_TestSimFetchKey.ino` | ✅ Hoàn chỉnh — KHÔNG cần sửa |
| Tag firmware | `esp32_firmware/s3_super_mini_central/s3_super_mini_central.ino` | ✅ Friend mode đã implement |

### Friend Sharing Flow (hoàn chỉnh)

```
Owner (phone A):
  FriendSharingFragment → POST /friend-sharing/create → nhận claim_url → hiển thị QR

Friend (phone B):
  FriendSharingFragment → quét QR → GET /friend-sharing/claim/{token}
  → EcdsaVerifier.verifyFriendBundle() (offline verify trên Android)
  → navigate tới BluetoothFragment với ARG_FRIEND_BUNDLE_JSON
  → cắm Tag vào USB
  → BluetoothFragment.provisionFriendBundle():
      1. SET_SERVER_PUBKEY <b64>  → Tag lưu NVS "sca_tag/srv_pub"
      2. SET_TIME <epoch>         → Tag set RTC
      3. SET_BUNDLE <b64json>     → Tag decode JSON → verify ECDSA → pack wire → lưu NVS

Tag (ESP32-S3):
  isFriendMode = true → bleTask gọi connectAsFriend()
  → scan BLE tìm FRIEND_SERVICE_UUID (0000FACE-...)
  → write 195-byte binary bundle lên FRIEND_BUNDLE_CHAR_UUID (0000FA01-...)
  → nhận binary notify [0x00, reason] từ FRIEND_STATUS_CHAR_UUID (0000FA03-...)
  → Serial: "FRIEND_ACCESS_GRANTED" hoặc "FRIEND_ACCESS_DENIED"

Anchor (ESP32-S3):
  BundleSubmitCallbacks.onWrite() → friendMgmtTask
  → ft_parse_bundle_wire() → friend_token_verify() (offline: version, vehicle, perms, time, revocation, ECDSA)
  → POST /validate-friend-key (online, cần WiFi, lần đầu)
  → ble_notify_friend_status(): binary [accepted=0, reason]
```

### Quy tắc quan trọng — KHÔNG được quên

**1. Tag firmware đúng đường dẫn:**
```
AndroidApp/esp32_firmware/s3_super_mini_central/s3_super_mini_central.ino
KHÔNG phải: Src/FreeRTOS/FreeRTOS_Tag/FreeRTOS_Tag.ino  ← sai
```

**2. Anchor gửi binary notification (KHÔNG phải string):**
```c
// Anchor gửi: uint8_t payload[2] = { accepted, reason }
// accepted = 0 → OK; accepted != 0 → FAIL
// Tag đọc: pData[0] == 0 → FRIEND_OK
//          pData[0] != 0 → FRIEND_FAIL, reason = pData[1]
```

**3. Bundle wire format (123 + sigLen bytes):**
```
[0]       version (1 byte)
[1..8]    friend_id (8 bytes raw)
[9..40]   vehicle_id (32 bytes, null-padded)
[41..56]  friend_key (16 bytes raw)
[57]      permissions (1 byte bitmask)
[58..89]  issued_at_iso (32 bytes, null-padded)
[90..121] expires_at_iso (32 bytes, null-padded)
[122]     issuer_sig_len (1 byte)
[123..]   issuer_sig DER (up to 72 bytes)
```

**4. Canonical message ECDSA (phải khớp server ↔ Tag ↔ Anchor):**
```
"v{version}|{friend_id_hex}|{vehicle_id}|{friend_key_hex}|{issued_at_iso}|{expires_at_iso}|{permissions}"
```

**5. sendCmd trong BluetoothFragment phải dùng prefix filter:**
```kotlin
// Luôn pass expectedPrefix để skip debug lines từ Tag
sendCmd("SET_SERVER_PUBKEY $b64", "SERVER_PUBKEY_")
sendCmd("SET_TIME $epoch",        "TIME_")
sendCmd("SET_BUNDLE $b64",        "BUNDLE_")
```

**6. Server: dùng `main_2.py`** — `main.py` thiếu `/validate-challenge`, `/friend-used`, `/cars/{vid}/revocations`

**7. Anchor cần WiFi** khi validate bundle lần đầu (cache miss). Đảm bảo WiFi config đúng trong `anchor_config.h`.

**8. UX flow đúng (friend device):**
```
Quét QR → app navigate BluetoothFragment (bundle JSON sẵn) → cắm Tag
KHÔNG được cắm Tag trước → Android restart MainActivity → mất back stack
(Đã fix bằng launchMode="singleTop" nhưng vẫn nên quét trước)
```

### Bước tiếp theo để test

1. **Nạp Tag firmware** — Arduino IDE, file `s3_super_mini_central.ino`
2. **Build & install Android app** — nhiều file đã thay đổi
3. **Chạy server** — `python main_2.py` (không phải `main.py`)
4. **Không cần nạp lại Anchor** — firmware đã đúng, NVS giữ nguyên pairing key
5. **Test flow:**
   - Owner: mở app → Friend Sharing → tạo QR (nhập tên + TTL)
   - Friend: mở app → Friend Sharing → Quét QR → cắm Tag → chờ `BUNDLE_OK`
   - Friend: đặt Tag gần Anchor → chờ `FRIEND_ACCESS_GRANTED`

---

## File Structure

```
SCA/
├── Src/FreeRTOS/
│   └── FreeRTOS_Anchor_TestSimFetchKey/   # Anchor firmware (MAIN) — ESP32-S3 Anchor
│       ├── FreeRTOS_Anchor_TestSimFetchKey.ino
│       ├── anchor_config.h                # PIN defs, UUIDs, VEHICLE_ID, WiFi creds
│       ├── friend_types.h                 # Wire format structs, constants
│       ├── friend_token.h                 # ECDSA verify + bundle parser
│       ├── friend_mgmt.h                  # Online validate, revocation sync
│       ├── friend_revocation.h            # NVS blacklist
│       ├── can_commands.h                 # CANCommands class
│       └── can_frames.h                   # CAN frame definitions
├── AndroidApp/
│   ├── esp32_firmware/
│   │   └── s3_super_mini_central/         # Tag firmware (MAIN) — ESP32-S3 Tag
│   │       ├── s3_super_mini_central.ino  # ← File đúng cho Tag (KHÔNG dùng FreeRTOS_Tag)
│   │       └── tag_config.h               # UUIDs, thresholds, wire constants
│   └── app/src/main/java/com/example/uwb/
│       ├── UI/                            # Fragments: Welcome, EnterVin, FriendSharing,
│       │                                  #   OwnerFriendList, PairingLoading, VehicleInfo
│       ├── Bluetooth/                     # BluetoothFragment (BLE scan + USB provisioning)
│       ├── crypto/                        # EcdsaVerifier (Android-side bundle verify)
│       ├── network/                       # ApiService, ApiClient
│       ├── model/                         # FriendKeyBundle, FriendShareRequest/Response
│       ├── dataLg/                        # KeyManager, ServerPublicKeyStore, FriendShareStore
│       ├── adapter/                       # FriendShareAdapter (RecyclerView)
│       └── repository/                    # PairingRepository
├── Server/
│   ├── main_2.py                          # FastAPI PRODUCTION — dùng cái này
│   ├── main.py                            # Thiếu friend endpoints — KHÔNG dùng
│   ├── car_access.db                      # SQLite
│   └── friend_client.py                   # Test: Friend flow simulator
├── lib/
│   ├── Dw3000/                            # UWB driver library
│   └── autowp-mcp2515/                    # CAN driver library
└── Tools/
    ├── nvs_clear.py                       # Xóa NVS flash (dùng khi cần reset Anchor)
    └── view_keys.py                       # Xem keys trong NVS
```

---

## Skills áp dụng theo task

| Task | Skill |
|------|-------|
| Viết/sửa firmware C (FreeRTOS, SPI, BLE, UWB) | `embedded-systems` |
| FastAPI endpoint, Pydantic model | `fastapi-expert` |
| Python server code quality | `python-pro` |
| SQLite queries, schema | `sql-pro` |
| Android Kotlin, coroutines | `kotlin-specialist` |
| Crypto/auth security | `secure-code-guardian` |
| Security audit, vulnerability | `security-reviewer` |
| REST API design | `api-designer` |
| Code review bất kỳ | `code-reviewer` |
| Debug lỗi bất kỳ | `debugging-wizard` |
