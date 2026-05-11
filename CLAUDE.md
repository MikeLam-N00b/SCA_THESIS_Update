# CLAUDE.md — Smart Car Access (SCA) Friend Sharing

> **Mục đích file này**: Lưu trạng thái session làm việc với Claude (chat & Code).
> Khi mở session mới, paste hoặc reference file này để context không bị mất.
> Cập nhật mỗi khi hoàn thành milestone.

**Project**: Smart Car Access — Đồ án tốt nghiệp HCMUTE 2026
**Author**: Hiếu (Automotive Engineering Technology, embedded automotive software)
**Last updated**: 2026-05-06
**Defense timeline**: ~3 tuần từ ngày update này

---

## 1. Project Overview

### Architecture (4 components)

```
┌──────────────────┐         ┌──────────────────┐
│  Android App     │ HTTPS   │  Cloud Server    │
│  (Owner+Guest)   │◀───────▶│  FastAPI v3      │
└────────┬─────────┘         └────────┬─────────┘
         │ USB Serial                  │ HTTPS
         │ CDC/ACM 115200              │ (qua A7680C LTE)
         ▼                             ▼
┌──────────────────┐  BLE+UWB ┌──────────────────┐
│ ESP32-S3 Tag     │◀────────▶│ ESP32 Anchor     │
│ (key fob)        │          │ (in vehicle)     │
│ DW3000 STS       │          │ MCP2515 CAN      │
└──────────────────┘          └──────────────────┘
```

### Tech Stack

- **Server**: Python FastAPI + SQLite + ECDSA P-256 + ECDH + AES-GCM
- **Android App**: Kotlin + Retrofit + Fragment + ViewBinding + ZXing (QR)
- **Tag firmware**: Arduino C++ on ESP32-S3 Super Mini, FreeRTOS, mbedTLS, DW3000
- **Anchor firmware**: ESP-IDF C, FreeRTOS, BLE peripheral, mbedTLS, A7680C LTE

---

## 2. Status of Each Component

### 2.1 Server (FastAPI v3) — ✅ COMPLETE

**File**: `Server/main.py` (1,308 lines)
**Test file**: `Server/test_flow.py` (18/18 pass)

**Endpoints implemented**:

| Endpoint | Auth | Sequence |
|----------|------|----------|
| `POST /owner-pairing` | — | S1 |
| `POST /secure-check-pairing` | — | S1 |
| `GET /check-pairing/{vid}` | — | S1 |
| `GET /vehicle/{vid}` | X-Owner-Key | S1 |
| `GET /vehicles` | — | S1 |
| `DELETE /vehicle/{vid}` | X-Owner-Key | S1 |
| `POST /friend-sharing/create` | X-Owner-Key | S1 |
| `DELETE /friend-sharing/{fid}` | X-Owner-Key | S1 |
| `GET /friend-sharing/list/{vid}` | X-Owner-Key | S1 |
| `GET /friend-sharing/claim/{token}` | — | S2 |
| `GET /pairing-bootstrap` | — | S2 |
| `GET /validate-challenge/{vid}` | — | S3 |
| `POST /validate-friend-key` | X-Anchor-MAC | S3 |
| `POST /friend-used` | X-Anchor-MAC | S3 |
| `GET /cars/{vid}/revocations?since=` | — | S4 |
| `POST /cars/{vid}/activity` | X-Anchor-MAC | Logging |
| `GET /cars/{vid}/activity?limit=N` | X-Owner-Key | Logging |
| `GET /health` | — | Utility |
| `GET /server-public-key` | — | Utility |

**Database schema (SQLite)**:

- **`vehicles`**: `vehicle_id`, `pairing_id`, `pairing_key` (base64 16B), `owner_api_key`, `created_at`
- **`friend_keys`**: `friend_id` (16 hex), `vehicle_id`, `friend_key` (base64 16B), `friend_name`, `permissions` (bitmask), `issued_at`, `expires_at`, `issuer_sig` (base64 106B), `is_revoked`, `revoked_at`, `uses_count`, `last_used_at`, `claim_token` (unique), `claimed`, `claimed_at`, `bundle_b64` (base64 106B), `created_at`
- **`access_events`**: `id`, `vehicle_id`, `friend_id`, `event_type`, `result`, `timestamp`, `details`

**Auth mechanisms**:
- **X-Owner-Key**: `secrets.compare_digest()` trên `owner_api_key` từ DB
- **X-Anchor-MAC**: HMAC-SHA256(`pairing_key`, `"{vid}|{ts}|{sha256(body)_b64}"`) — timestamp window ±5 phút

**TODO còn lại**:
- Deploy lên Railway/Render với HTTPS
- Backup `server_signing_key.pem` (critical — single point of failure)
- Thay in-memory nonce cache bằng Redis (hiện tại restart sẽ mất)
- Restrict CORS origins (hiện allow *)

---

### 2.2 Android App — ⚠️ ~80% COMPLETE

**Location**: `AndroidApp/app/src/main/java/com/example/uwb/`

**Cấu trúc file (44 Kotlin files)**:

```
UI/Fragments (10 files)
├── WelcomeFragment.kt
├── LoginFragment.kt
├── EnterVinFragment.kt
├── VehicleInfoFragment.kt
├── VerifyingVinFragment.kt
├── PairingLoadingFragment.kt
├── FriendSharingFragment.kt         ← Owner: tạo share, gen QR, quét QR claim
├── OwnerFriendListFragment.kt       ← Owner: list/revoke friends
├── UwbFragment.kt                   ← UWB distance display
└── PortraitCaptureActivity.kt

Network (3 files)
├── ApiService.kt                    ← Retrofit interface (tất cả v3 endpoints)
├── ApiClient.kt                     ← Retrofit singleton
└── PairingRepository.kt

Storage (5 files)
├── KeyManager.kt                    ← SharedPrefs: owner_key_{vin}, friend_key_{vin}_{fid}
├── FriendShareStore.kt              ← Gson-backed: track claim_url, expiry
├── ServerPublicKeyStore.kt          ← Cache server ECDSA pubkey
├── SessionManager.kt
└── PairedDeviceStore.kt

Crypto (4 files)
├── EcdsaVerifier.kt                 ← verifyBundleBinary(106B, serverPubKeyDerB64)
├── GeneratePrivateKeyEcc.kt
├── AesGcmUtil.kt
└── HKDF_SHA256.kt

Models (8 files)
├── FriendKeyBundle.kt               ← 106-byte bundle property accessors
├── FriendShareRequest/Response.kt
├── PairingRequest/Response.kt
└── ServerPublicKeyResponse.kt

Transport (4 files)
├── UsbTransport.kt                  ← Abstract USB interface
├── UsbConnection.kt                 ← CDC/ACM 115200
├── UsbRepository.kt
└── UsbConstants.kt

Bluetooth (2 files)
├── BluetoothFragment.kt             ← BLE scan + connect + USB Tag provisioning
└── BluetoothManager.kt

Adapters (2 files)
├── FriendListAdapter.kt
└── VinValidator.kt
```

**Những gì đã làm được**:
- Owner pairing flow (ECDH + HKDF + AES-GCM)
- Friend share creation + QR generation
- Guest claim + ECDSA local verify (`EcdsaVerifier.kt`)
- KeyManager namespace tách biệt (đã fix bug cũ)
- `ServerPublicKeyStore` cache pubkey
- `OwnerFriendListFragment` (list + revoke)
- USB transport layer (CDC/ACM)
- BLE scan/connect

**Còn thiếu / cần test**:
- USB provisioning commands (`SET_BUNDLE`, `SET_SERVER_PUBKEY`, `SET_TIME`) trong `BluetoothFragment` — cần verify đã gọi đúng sau khi Tag connect
- End-to-end test thực với server deploy HTTPS
- Unit test + instrumented test
- Production build config (server URL, signing)

---

### 2.3 Anchor Firmware (ESP32 trong xe) — ⚠️ ~70% COMPLETE

**Location**: `Src/FreeRTOS_Anchor/FreeRTOS_Anchor_TestSimFetchKey/`

**Cấu trúc file**:

```
FreeRTOS_Anchor_TestSimFetchKey.ino  ← Main sketch (~300+ lines đọc được)
anchor_config.h                      ← Config constants (vehicle_id, server URL, BLE UUIDs, pins)
friend_types.h                       ← Shared structs (friend_bundle_t, cached_friend_t, permission enums)
friend_token.h                       ← Offline ECDSA verify (ft_parse_bundle_wire, friend_token_verify)
friend_cache.h                       ← NVS caching (namespace "sca_friends", max 50 friends)
friend_revocation.h                  ← Revocation blacklist (namespace "sca_revoked", max 100 entries)
friend_mgmt.h                        ← Online validate + usage report (Seq 3 + Seq 4)
can_frames.h                         ← CAN frame definitions
can_commands.h                       ← CANCommands class (unlockCar, lockCar, 15/16 frames)
```

**FreeRTOS tasks**:

| Task | Core | Priority | Stack | Role |
|------|------|----------|-------|------|
| bleTask | 0 | 3 | 10240B | BLE GATT server, nhận bundle từ Tag |
| uwbTask | 1 | 4 | 8192B | DW3000 TWR ranging, STS |
| canTask | 1 | 2 | 4096B | MCP2515 CAN lock/unlock |
| friendMgmtTask | 0 | 2 | 16384B | Online validate (Seq 3) |
| revocationSyncTask | 0 | 1 | 8192B | Poll revocations mỗi 5 phút |

**Protocols implemented**:
- BLE: GATT server cho Tag discovery + bundle submission
- UWB (DW3000): Two-Way Ranging, STS encryption dùng friend_key
- HMAC-SHA256: X-Anchor-MAC header authentication
- ECDSA P-256: Offline verify bundled (mbedTLS, cached server pubkey trong NVS)
- CAN (MCP2515): ISO 11898-1, 1Mbps
- LTE (A7680C): AT commands, HTTP qua SIM module

**Hardware config** (từ `anchor_config.h`):
- Vehicle ID hardcode: `"1HGBH41JXMN109186"` — **phải đổi trước demo**
- Server fallback: `http://10.0.4.83:8000` — **phải đổi sang URL deploy thật**
- SIM APN: hỗ trợ Viettel/Mobifone/Vinaphone

**Còn thiếu / cần verify**:
- BLE bundle rx callback + full parsing flow trong .ino
- UWB STS key derivation từ friend_key
- Full integration test với Server + Tag

---

### 2.4 Tag Firmware (ESP32-S3 key fob) — ⏳ TODO

**File hiện có**: `Src/FreeRTOS/FreeRTOS_Anchor_TestSimFetchKey/` (xem tag_firmware_brief.md nếu còn)

**Existing functionality**:
- USB Serial parser (SET_KEY, DISCONNECT)
- BLE Central scan + connect to Anchor
- HMAC challenge-response auth với Anchor
- DW3000 UWB ranging với STS encryption

**Cần thêm cho v3**:
- Parser cho `SET_BUNDLE`, `SET_SERVER_PUBKEY`, `SET_TIME`, `GET_STATUS`
- ECDSA offline verify với mbedTLS (106-byte bundle)
- Permission enforcement trong UWB action loop
- Backward compat: `SET_KEY` vẫn work

**Priority**: Làm sau khi Android hoàn thành end-to-end test.

---

## 3. Bundle Format — Critical Cross-Component Spec

**Wire format: 106 bytes binary** (dùng ở tất cả 3 component)

```
Offset  Field           Size  Encoding
0       version         1     uint8 (= 1)
1-8     friend_id       8     raw binary
9-16    vehicle_hash    8     SHA-256(VIN)[:8]
17-32   friend_key      16    raw binary (AES key)
33-36   issued_at       4     uint32 big-endian (unix epoch)
37-40   expires_at      4     uint32 big-endian (unix epoch)
41      permissions     1     bitmask
42-73   sig_r           32    ECDSA r component (raw, big-endian)
74-105  sig_s           32    ECDSA s component (raw, big-endian)
```

- **Signed part**: bytes [0..41] (42 bytes)
- **Signature**: raw r||s = bytes [42..105] (64 bytes)
- ECDSA signature algorithm: `SHA256withECDSA`, DER-encoded khi verify trên Android/mbedTLS

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
| Skip server report khi ECDSA fail | Chỉ show toast, không gửi lên server | False positives flood; server enforce ở Seq 3 là đủ |

---

## 7. Open Issues

- [ ] **Deploy server HTTPS** — hiện `http://10.0.4.83:8000` (LAN). Cần public URL trước demo.
- [ ] **Backup `server_signing_key.pem`** — mất file này = mất toàn bộ bundle trust.
- [ ] **Anchor config hardcode** — `vehicle_id` và server URL trong `anchor_config.h` phải đổi trước deploy.
- [ ] **Tag time sync** — Tag không có RTC, dựa vào `SET_TIME` từ app. Xem xét fetch từ `/health` server time.
- [ ] **USB provisioning verify** — cần test thực tế: connect Tag → app tự động gửi SET_SERVER_PUBKEY + SET_TIME + SET_BUNDLE hay không.
- [ ] **In-memory nonce cache** — server restart sẽ clear; dùng Redis trong production.

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

### Tuần này
1. Deploy server lên Railway/Render (HTTPS)
2. Backup `server_signing_key.pem`
3. Test Android app end-to-end với server HTTPS thật

### Sau đó
4. Verify USB provisioning flow trong `BluetoothFragment` (SET_BUNDLE, SET_SERVER_PUBKEY, SET_TIME)
5. Tag firmware: thêm SET_BUNDLE parser + ECDSA verify (mbedTLS)
6. Anchor firmware: full integration test (BLE rx → verify → CAN unlock)
7. End-to-end demo: pair → create share → claim (Guest phone) → USB to Tag → BLE to Anchor → CAN unlock
8. Record demo video

---

## 10. Update Log

| Date | What changed | Component |
|------|--------------|-----------|
| 2026-04-22 | Server v2 written and tested | Server |
| 2026-04-22 | Server v3 với anchor auth + nonce | Server |
| 2026-04-25 | All component briefs written | Briefs |
| 2026-04-25 | Android implementation session | Android |
| 2026-05-06 | Full codebase review; CLAUDE.md rewritten với actual state | All |

---

**End of CLAUDE.md** — Cập nhật date ở đầu file và thêm vào Update Log mỗi khi hoàn thành milestone.
