# CLAUDE.md — Smart Car Access (SCA) Friend Sharing

> **Mục đích file này**: Lưu trạng thái session làm việc với Claude (chat & Code).
> Khi mở session mới, paste hoặc reference file này để context không bị mất.
> Cập nhật mỗi khi hoàn thành milestone.

**Project**: Smart Car Access — Đồ án tốt nghiệp HCMUTE 2026
**Author**: Hiếu (Automotive Engineering Technology, embedded automotive software)
**Last updated**: 2026-05-12
**Defense timeline**: ~2 tuần từ ngày update này

---

## 1. Project Overview

### Architecture (4 components)

```
┌──────────────────┐         ┌──────────────────┐
│  Android App     │ HTTPS   │  Cloud Server    │
│  (Owner+Guest)   │◀───────▶│  FastAPI v3      │
└────────┬─────────┘         └────────┬─────────┘
         │ USB Serial                  │ HTTP+HMAC
         │ CDC/ACM 115200              │ (A7680C LTE)
         ▼                             ▼
┌──────────────────┐  BLE+UWB ┌──────────────────┐
│ ESP32-S3 Tag     │◀────────▶│ ESP32 Anchor     │
│ (key fob)        │          │ (in vehicle)     │
│ DW3000 STS       │          │ MCP2515 CAN      │
└──────────────────┘          └──────────────────┘
```

### Tech Stack

- **Server**: Python FastAPI + SQLite + ECDSA P-256 + ECDH + AES-GCM
- **Android App**: Kotlin + Retrofit2 + Fragment + ViewBinding + ZXing (QR)
- **Tag firmware**: Arduino C++ on ESP32-S3 Super Mini, FreeRTOS, mbedTLS, DW3000
- **Anchor firmware**: Arduino C++ on ESP32, FreeRTOS, BLE peripheral, mbedTLS, MCP2515, A7680C LTE

---

## 2. Status of Each Component

### 2.1 Server (FastAPI v3) — ✅ COMPLETE (known issues below)

**File**: `Server/main.py` (1,307 lines)
**Test file**: `Server/test_flow.py` (18/18 pass)

**Endpoints implemented (19 total)**:

| Endpoint | Method | Auth | Sequence |
|----------|--------|------|----------|
| `/` | GET | — | Info |
| `/health` | GET | — | Utility |
| `/server-public-key` | GET | — | Utility |
| `/pairing-bootstrap` | GET | — | S2 |
| `POST /owner-pairing` | POST | ECDH | S1 |
| `POST /secure-check-pairing` | POST | ECDH | S1 |
| `GET /check-pairing/{vid}` | GET | — | S1 |
| `GET /vehicle/{vid}` | GET | — | S1 |
| `GET /vehicles` | GET | — | S1 |
| `DELETE /vehicle/{vid}` | DELETE | X-Owner-Key | S1 |
| `POST /friend-sharing/create` | POST | X-Owner-Key | S1 |
| `DELETE /friend-sharing/{fid}` | DELETE | X-Owner-Key | S1 |
| `GET /friend-sharing/list/{vid}` | GET | X-Owner-Key | S1 |
| `GET /friend-sharing/claim/{token}` | GET | single-use token | S2 |
| `GET /validate-challenge/{vid}` | GET | — | S3 |
| `POST /validate-friend-key` | POST | X-Anchor-MAC | S3 |
| `POST /friend-used` | POST | X-Anchor-MAC | S3 |
| `GET /cars/{vid}/revocations` | GET | — | S4 |
| `POST /cars/{vid}/activity` | POST/GET | X-Anchor-MAC / X-Owner-Key | Logging |

**Database schema (SQLite)**:

- **`vehicles`**: `vehicle_id`, `pairing_id`, `pairing_key` (base64 16B), `owner_api_key`, `created_at`
- **`friend_keys`**: `friend_id` (16 hex), `vehicle_id`, `friend_key` (base64 16B), `friend_name`, `permissions` (bitmask), `issued_at`, `expires_at`, `issuer_sig` (base64 106B), `is_revoked`, `revoked_at`, `uses_count`, `last_used_at`, `claim_token` (unique), `claimed`, `claimed_at`, `bundle_b64` (base64 106B), `created_at`
- **`access_events`**: `id`, `vehicle_id`, `friend_id`, `event_type`, `result`, `timestamp`, `details`
- **Indices**: `idx_events_vehicle`, `idx_events_time`, `idx_friends_vehicle`, `idx_friends_revoked`

**Auth mechanisms**:
- **X-Owner-Key**: `secrets.compare_digest()` trên `owner_api_key` từ DB
- **X-Anchor-MAC**: HMAC-SHA256(`pairing_key`, `"{vid}|{ts}|{sha256(body)_b64}"`) — timestamp window ±5 phút
- **ECDH**: dùng trong `/owner-pairing` và `/secure-check-pairing` cho key agreement

**Known issues (pre-deploy phải fix)**:
- `owner_api_key` đang được in plaintext ra stdout (line 856) — cần redact
- CORS `allow_origins=["*"]` (line 336) — restrict trước khi deploy public
- Friend keys lưu base64 plaintext trong SQLite — không encrypt at rest
- In-memory nonce cache `_active_nonces: dict` — process restart sẽ mất; dùng Redis production
- Concurrent `/claim/{token}` requests có thể race (hiếm nhưng cần database-level lock)

---

### 2.2 Android App — ⚠️ ~80% COMPLETE

**Location**: `AndroidApp/app/src/main/java/com/example/uwb/`
**Total**: 44 Kotlin files

**Cấu trúc file**:

```
UI/Fragments (10 files)
├── WelcomeFragment.kt
├── LoginFragment.kt
├── EnterVinFragment.kt
├── VehicleInfoFragment.kt
├── VerifyingVinFragment.kt
├── PairingLoadingFragment.kt
├── FriendSharingFragment.kt      (326 lines) ← Owner: tạo share, gen QR; Guest: scan QR claim
├── OwnerFriendListFragment.kt    ← Owner: list/revoke friends
├── UwbFragment.kt                ← UWB distance display
└── PortraitCaptureActivity.kt

Network (3 files)
├── ApiService.kt                 (54 lines) ← Retrofit2 interface, 6 suspend functions
├── ApiClient.kt                  ← Retrofit singleton
└── PairingRepository.kt

Storage (5 files)
├── KeyManager.kt                 (198 lines) ← SharedPrefs: owner_key_{vin}, friend_key_{vin}_{fid}
├── FriendShareStore.kt           ← Gson-backed: track claim_url, expiry
├── ServerPublicKeyStore.kt       ← Cache server ECDSA pubkey
├── SessionManager.kt
└── PairedDeviceStore.kt

Crypto (4 files)
├── EcdsaVerifier.kt              (74 lines) ← verifyBundleBinary(106B, serverPubKeyDerB64)
├── GeneratePrivateKeyEcc.kt
├── AesGcmUtil.kt
└── HKDF_SHA256.kt

Models (8 files)
├── FriendKeyBundle.kt            ← 106-byte bundle property accessors
├── FriendShareRequest/Response.kt
├── PairingRequest/Response.kt
└── ServerPublicKeyResponse.kt

Transport (4 files)
├── UsbTransport.kt               ← Abstract USB interface
├── UsbConnection.kt              ← CDC/ACM 115200, ESP32 vendor ID 0x303A
├── UsbRepository.kt
└── UsbConstants.kt

Bluetooth (2 files)
├── BluetoothFragment.kt          (283 lines) ← BLE scan + USB provisioning
└── BluetoothManager.kt

Adapters (2 files)
├── FriendListAdapter.kt
└── VinValidator.kt
```

**Những gì đã làm được**:
- Owner pairing flow (ECDH + HKDF + AES-GCM)
- Friend share creation + QR generation
- Guest claim + ECDSA local verify (`EcdsaVerifier.kt`)
  - Nếu cached key fail → refetch fresh key và retry (good UX)
  - Nếu verify fail → reject bundle, không provision
- KeyManager namespace tách biệt (đã fix bug cũ), có migration từ legacy `key_*`
- `ServerPublicKeyStore` cache pubkey
- `OwnerFriendListFragment` (list + revoke)
- USB transport layer (CDC/ACM), BLE scan/connect với 10s scan window

**Known issues**:
- `KeyManager.clearKey()` chỉ xóa SharedPreferences, không xóa in-memory session — key leak nếu user switch account trong cùng session
- SharedPreferences lưu keys plaintext — nên dùng `EncryptedSharedPreferences`
- `fromHex()` trong KeyManager sẽ throw `IndexOutOfBoundsException` nếu hex string length lẻ
- `FriendSharingFragment`: không có debounce trên nút "Create QR" — rapid clicks gây race
- `EcdsaVerifier`: swallow mọi exception → return `false`, khó debug (không phân biệt format error vs signature failure)
- `ApiService.kt`: không có timeout config, không có retry/circuit-breaker

**Còn thiếu / cần test**:
- USB provisioning commands (`SET_BUNDLE`, `SET_SERVER_PUBKEY`, `SET_TIME`) trong `BluetoothFragment` — cần verify thực tế sau khi Tag connect
- End-to-end test với server deploy HTTPS thật
- Unit test + instrumented test
- Production build config (server URL, signing keystore)

---

### 2.3 Anchor Firmware (ESP32 trong xe) — ⚠️ ~85% COMPLETE

**Location**: `Src/FreeRTOS_Anchor/`

**Cấu trúc file (actual, đã review)**:

```
FreeRTOS_Anchor.ino     (133 lines)  ← Main sketch, task init, setup/loop
anchor_config.h         (91 lines)   ← Config constants, hardware pins, UUIDs
anchor_ble.h            (244 lines)  ← BLE GATT server (standard auth + friend sharing)
anchor_crypto.h         (66 lines)   ← mbedTLS: CSPRNG, HMAC, HKDF-SHA256
anchor_ipc.h            (83 lines)   ← FreeRTOS primitives, shared state, event group bits
anchor_nvs.h            (30 lines)   ← NVS key persistence
anchor_tasks.h          (341 lines)  ← 6 FreeRTOS task implementations (incl. simInitTask)
anchor_transport.h      (503 lines)  ← SIM A7680C AT commands, ECDH key provisioning (SIM-only, no WiFi)
anchor_uwb.h            (137 lines)  ← DW3000 init, SS-TWR responder loop
can_commands.h          (79 lines)   ← CAN unlock/lock API wrapper
can_frames.h            (106 lines)  ← CAN frame definitions (15 unlock, 16 lock frames)
friend_mgmt.h           (277 lines)  ← HTTP helpers (via SIM), online validate, revocation sync
friend_token.h          (~100 lines) ← ECDSA bundle verify, ft_parse_bundle_wire
friend_types.h          (132 lines)  ← Shared types, constants, enums
friend_cache.h          (~80 lines)  ← NVS caching (namespace "sca_friends")
friend_revocation.h     (~80 lines)  ← Blacklist management, delta-sync cursor
```

**Total**: ~2,300 lines

**FreeRTOS tasks**:

| Task | Core | Priority | Stack | Role |
|------|------|----------|-------|------|
| bleTask | 0 | 3 | 10240B | BLE GATT server, challenge-response auth, bundle rx |
| uwbTask | 1 | 4 | 8192B | DW3000 SS-TWR responder, STS relay mitigation |
| canTask | 1 | 2 | 4096B | MCP2515 CAN lock/unlock (15/16 frames) |
| friendMgmtTask | 0 | 2 | 16384B | 5-step verify: ECDSA→time→revoke→cache→online→UWB→CAN |
| simInitTask | 0 | 1 | 12288B | One-shot: SIM init + NTP sync → tự xóa sau khi xong |
| revocationSyncTask | 0 | 1 | 8192B | Delta-sync revocations mỗi 5 phút |

**Protocols implemented**:
- **BLE**: GATT server, challenge-response HMAC-SHA256, fragmented 106-byte bundle submission
- **UWB (DW3000)**: SS-TWR responder, STS Mode 1, Channel 5, 850 kbps, 3000µs TX delay
- **CAN (MCP2515)**: 100 kbps, 50ms inter-frame delay
- **HMAC-SHA256**: X-Anchor-MAC header authentication
- **ECDSA P-256**: Offline verify via mbedTLS, cached server pubkey trong NVS
- **SIM ECDH**: Key provisioning qua `/secure-check-pairing` trên SIM module, HKDF + AES-GCM decrypt
- **LTE (A7680C)**: transport duy nhất — tất cả HTTP, ECDH, NTP sync đều qua SIM AT commands

**SPI sharing**: uwbTask và canTask share SPI bus qua `spiMutex`
- uwbTask timeout: 500ms — canTask timeout: 2000ms

**Hardware config** (từ `anchor_config.h`) — **PHẢI ĐỔI TRƯỚC DEMO**:
- Vehicle ID: `"1HGBH41JXMN109186"` (hardcode — đổi trước deploy)
- Server: `"http://139.59.232.153:8000"` (public IP — đổi sang HTTPS trước deploy)
- SIM APN: `"v-internet"` (Viettel)

**Bugs đã fix (2026-05-12)**:
- ✅ **`responseBuffer` overflow** (`anchor_ble.h:58-60`): đã có bounds check (`toCopy = min(len, 32 - responseBufferLen)`)
- ✅ **simAtCmd busy-wait watchdog**: đã thêm `vTaskDelay(10ms)` trong vòng lặp — IDLE task không bị chết đói nữa
- ✅ **simInitTask stack overflow**: tăng 8192 → 12288 bytes
- ✅ **simInit() CFUN=0 không được restore** (`anchor_transport.h`): `s_simSleeping` reset về `false` sau mỗi ESP32 reset nhưng module vẫn ở CFUN=0 → thêm `AT+CFUN=1` sau ATE0 trong `simInit()`

**Bugs còn lại phải xử lý**:
1. **Clock-not-synced fallback** (`anchor_tasks.h:228-231`): Khi SNTP fail, dùng `bundle.issued_at + 1` làm time reference thay vì reject bundle — insecure
2. **Volatile state race**: `s_friendBundleVerified`, `carUnlocked` là `volatile` nhưng bleTask có thể đọc stale value trước khi friendMgmtTask write

**Còn thiếu**:
- Retry logic cho CAN send failures
- Full integration test với Server + Tag

---

### 2.4 Tag Firmware (ESP32-S3 key fob) — ⏳ TODO

**Location**: `Src/FreeRTOS_Anchor/` (xem các file tag-specific)

**Existing functionality**:
- USB Serial parser (`SET_KEY`, `DISCONNECT`)
- BLE Central scan + connect to Anchor
- HMAC challenge-response auth với Anchor
- DW3000 UWB ranging với STS encryption

**Cần thêm cho v3**:
- Parser cho `SET_BUNDLE`, `SET_SERVER_PUBKEY`, `SET_TIME`, `GET_STATUS`
- ECDSA offline verify với mbedTLS (106-byte bundle)
- Permission enforcement trong UWB action loop
- Backward compat: `SET_KEY` vẫn work

**Priority**: Làm sau khi Android + Anchor hoàn thành integration test.

---

### 2.5 Measure_Ampere (Utility) — ✅ STANDALONE

**File**: `example/Measure_Ampere/Measure_Ampere.ino` (45 lines)
- INA226 shunt current sensor (0.1Ω, 500mA max)
- I2C: SDA=GPIO8, SCL=GPIO9
- Output: Voltage, current (mA), power (mW) mỗi 500ms, deadband 1.0mA
- **Không tích hợp** vào main firmware — dùng để đo power consumption riêng

---

## 3. Bundle Format — Critical Cross-Component Spec

**Wire format: 106 bytes binary** (dùng ở tất cả 3 component)

```
Offset  Field           Size  Encoding
0       version         1     uint8 (= 1)
1-8     friend_id       8     raw binary
9-16    vehicle_hash    8     SHA-256(VIN)[:8]
17-32   friend_key      16    raw binary (AES-128 key)
33-36   issued_at       4     uint32 big-endian (unix epoch)
37-40   expires_at      4     uint32 big-endian (unix epoch)
41      permissions     1     bitmask
42-73   sig_r           32    ECDSA r component (raw, big-endian)
74-105  sig_s           32    ECDSA s component (raw, big-endian)
```

- **Signed part**: bytes [0..41] (42 bytes)
- **Signature**: raw r||s = bytes [42..105] (64 bytes)
- ECDSA signature algorithm: `SHA256withECDSA`, DER-encoded khi verify trên Android/mbedTLS
- Android `EcdsaVerifier.kt`: manually constructs ASN.1 DER từ raw r||s (pad 0x00 nếu MSB ≥ 0x80)

**Permission bitmask**:
```
PERM_UNLOCK = 0x01  (bit 0)
PERM_LOCK   = 0x02  (bit 1)
```

---

## 4. USB Serial Commands (App → Tag)

```
SET_KEY:<32-hex>\n              legacy owner mode (vẫn support)
SET_BUNDLE:<base64-106B>\n      friend bundle (v3)
SET_SERVER_PUBKEY:<base64>\n    cache server ECDSA pubkey trên Tag
SET_TIME:<unix_seconds>\n       sync clock (Tag không có RTC)
GET_STATUS\n                    query state
DISCONNECT\n
```

---

## 5. Anchor Authentication (X-Anchor-MAC)

```
X-Anchor-Timestamp: <unix_seconds>
X-Anchor-MAC: base64(HMAC-SHA256(pairing_key, "{vehicle_id}|{timestamp}|{SHA256(body)_base64}"))
```

- Timestamp window: ±5 phút
- Body digest: SHA-256 raw bytes, rồi base64-encode
- Dùng cho: `/validate-friend-key`, `/friend-used`, `POST /cars/{vid}/activity`

---

## 6. Key Decisions (Summary)

| Decision | What | Why |
|----------|------|-----|
| Offline-first | Anchor verify bundle local, poll revocation 5 phút | Latency 300ms vs 3-10s, works offline, industry standard (CCC DK 3.0) |
| ECDSA P-256 | Server signs bundle, anchor verify với cached pubkey | Non-repudiation; compromised anchor không forge được bundle |
| 106-byte binary bundle | Fixed-width binary (vs JSON/text) | Deterministic, firmware-friendly, không cần JSON parser trên anchor |
| Permission bitmask | UNLOCK=0x01, LOCK=0x02 | Minimal cho demo, extensible |
| Single-use claim token | Marked `claimed=1` sau first fetch | Prevents token sharing/leakage |
| USB bridge Phone→Tag | Phone nói với Tag qua USB CDC/ACM | Architecture hiện tại của project, Tag cần dedicated UWB hardware |
| 1 app 2 roles | Owner + Guest cùng APK | Users thường là cả owner xe mình + guest xe bạn |
| HTTP + HMAC thay vì TLS | Plain HTTP với HMAC body signing | ESP32 + mbedTLS TLS handshake 5-8s trên LTE; HMAC đạt cùng integrity <1s |
| Skip server report khi ECDSA fail | Chỉ show toast, không gửi lên server | False positives flood; server enforce ở Seq 3 là đủ |

---

## 7. Open Issues

### 🔴 Critical — phải fix trước demo

- [x] ~~**`responseBuffer` overflow**~~ — đã fix (bounds check có sẵn)
- [x] ~~**simAtCmd watchdog crash**~~ — đã fix (thêm `vTaskDelay(10ms)` trong busy-wait)
- [x] ~~**simInitTask stack overflow**~~ — đã fix (8192 → 12288 bytes)
- [ ] **Clock-not-synced insecure fallback** (`anchor_tasks.h:228-231`) — reject bundle nếu SNTP fail thay vì dùng issued_at+1 làm reference
- [ ] **Anchor config hardcode** — `VEHICLE_ID`, `SERVER_FALLBACK` trong `anchor_config.h` phải đổi trước deploy
- [ ] **Deploy server HTTPS** — hiện `http://139.59.232.153:8000`. Cần HTTPS.
- [ ] **Backup `server_signing_key.pem`** — mất file này = mất toàn bộ bundle trust

### 🟡 Medium — quan trọng cho production

- [ ] **KeyManager in-memory not cleared** — `clearKey()` không xóa in-memory session, key leak nếu switch account
- [ ] **SharedPreferences plaintext** — dùng `EncryptedSharedPreferences`
- [ ] **Server CORS wide open** — restrict `allow_origins` trước khi deploy
- [ ] **`owner_api_key` printed to stdout** — redact trước khi deploy (server `main.py:856`)
- [ ] **In-memory nonce cache** — server restart sẽ clear; dùng Redis trong production

### 🟢 Low — nice-to-have

- [ ] **Tag time sync** — Tag không có RTC, dựa vào `SET_TIME` từ app
- [ ] **USB provisioning verify** — test thực tế: connect Tag → app gửi SET_SERVER_PUBKEY + SET_TIME + SET_BUNDLE
- [ ] **EcdsaVerifier exception swallowing** — phân biệt format error vs signature failure
- [ ] **`fromHex()` bounds validation** — throw nếu hex string length lẻ
- [ ] **Debounce "Create QR" button** — tránh rapid clicks gây duplicate share creation

---

## 8. Defense Talking Points

### 60-second pitch

> "Hệ thống Friend Sharing thiết kế theo nguyên tắc **offline-first** chuẩn automotive: ECDSA P-256 cho cryptographic bundle verification, HMAC-SHA256 cho anchor authentication. Trade-off revocation timeliness (5 phút) vs offline availability tuân theo CCC Digital Key 3.0 và IEEE 1609.2-2022. Defense-in-depth: (1) ECDSA bundle signature, (2) UWB proximity ranging chống relay attack, (3) HMAC challenge-response BLE pairing, (4) permission bitmask enforcement."

### Key Q&A

**Q: Sao không check online mỗi unlock?**  
A: Latency 3-10s vs 300ms; không work trong hầm xe; tốn pin/data 10x. Tesla, BMW, CCC chuẩn đều offline-first.

**Q: Revocation gap bao lâu?**  
A: Tối đa bằng poll interval (5 phút) + network delay. Với internet available, anchor sync trong 5 phút. Nếu offline lâu hơn → fallback online validate khi cache miss.

**Q: Sao không dùng TLS/mTLS anchor–server?**  
A: ESP32 + mbedTLS TLS handshake 5-8s trên LTE chậm; HTTP + HMAC body signing đạt cùng integrity với overhead <1s. Future: mutual TLS.

**Q: Server signing key bị leak?**  
A: Single point of failure. Mitigation roadmap: key rotation, HSM, per-vehicle signing key.

**Q: Replay attack trên BLE?**  
A: Bundle có timestamp TTL nhưng không có per-session nonce. Future work: 16-byte nonce từ anchor + HMAC(friend_key, nonce).


---

## 9. Implementation Order Remaining

### Tuần này (Critical path)
1. ✅ ~~Fix `responseBuffer` overflow~~ — đã có sẵn trong code
2. ✅ ~~Fix simAtCmd watchdog~~ — đã fix 2026-05-12
3. ✅ ~~Fix simInitTask stack overflow~~ — đã fix 2026-05-12
4. [ ] **Fix clock-not-synced fallback** (`anchor_tasks.h:228-231`) — reject nếu SNTP fail
5. [ ] **Đổi hardcode** trong `anchor_config.h`: VEHICLE_ID, server URL
6. [ ] **Deploy server HTTPS** lên Railway/Render
7. [ ] **Backup `server_signing_key.pem`** (ngay bây giờ)

### Sau đó (Integration)
8. Redact sensitive log output trong `Server/main.py`
9. Test Android end-to-end với server HTTPS thật
10. Verify USB provisioning flow (`SET_BUNDLE`, `SET_SERVER_PUBKEY`, `SET_TIME`)
11. Tag firmware: thêm SET_BUNDLE parser + ECDSA verify (mbedTLS)
12. Anchor firmware: full integration test (BLE rx → verify → CAN unlock)
13. End-to-end demo: pair → create share → claim (Guest phone) → USB to Tag → BLE to Anchor → CAN unlock
14. Record demo video

---

## 10. Update Log

| Date | What changed | Component |
|------|--------------|-----------|
| 2026-04-22 | Server v2 written and tested | Server |
| 2026-04-22 | Server v3 với anchor auth + nonce | Server |
| 2026-04-25 | All component briefs written | Briefs |
| 2026-04-25 | Android implementation session | Android |
| 2026-05-06 | Full codebase review; CLAUDE.md rewritten với actual state | All |
| 2026-05-11 | Deep code review tất cả components; phát hiện critical bugs; CLAUDE.md sync với code thực | All |
| 2026-05-12 | Xóa WiFi khỏi Anchor firmware (SIM là transport duy nhất); fix simInitTask watchdog + stack overflow; cập nhật CLAUDE.md | Anchor |

---

**End of CLAUDE.md** — Cập nhật date ở đầu file và thêm vào Update Log mỗi khi hoàn thành milestone.
