# CLAUDE.md — Smart Car Access (SCA) Friend Sharing

> **Mục đích file này**: Lưu trạng thái session làm việc với Claude (chat & Code).
> Khi mở session mới, paste hoặc reference file này để context không bị mất.
> Cập nhật mỗi khi hoàn thành milestone.

**Project**: Smart Car Access — Đồ án tốt nghiệp HCMUTE 2026
**Author**: Hiếu (Automotive Engineering Technology, embedded automotive software)
**Last updated**: 2026-04-25
**Defense timeline**: ~1 tháng từ ngày update này

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

**File**: `main.py` (v3, 1299 lines)
**Test file**: `test_flow.py` (18/18 pass)

**Endpoints implemented**:
- Pairing: `POST /owner-pairing`, `POST /secure-check-pairing`, `GET /pairing-bootstrap`
- Friend create: `POST /friend-sharing/create` (X-Owner-Key auth)
- Friend claim: `GET /friend-sharing/claim/{token}` (single-use)
- Friend list: `GET /friend-sharing/list/{vid}` (X-Owner-Key)
- Friend revoke: `DELETE /friend-sharing/{fid}` (X-Owner-Key)
- Anchor validate: `GET /validate-challenge/{vid}` + `POST /validate-friend-key` (X-Anchor-MAC + nonce)
- Anchor usage: `POST /friend-used` (X-Anchor-MAC)
- Anchor revocation poll: `GET /cars/{vid}/revocations?since=`
- Activity log: `POST /cars/{vid}/activity` (X-Anchor-MAC), `GET /cars/{vid}/activity` (X-Owner-Key)
- Utility: `GET /health`, `GET /server-public-key`

**Security implemented**:
- Owner auth via `X-Owner-Key` header (`secrets.compare_digest`)
- Anchor auth via HMAC-SHA256 over `{vehicle_id}|{timestamp}|{body_sha256_b64}` with `pairing_key`
- ECDSA P-256 signature on friend bundles
- Single-use claim tokens (marked claimed after first fetch)
- Anti-replay nonces (single-use, 120s TTL) on validate endpoint
- Constant-time signature compare
- 5-min timestamp window on anchor requests

**TODO**: Deploy to Railway/Render with HTTPS. Backup `server_signing_key.pem` securely.

### 2.2 Android App — ⏳ IN PROGRESS

**Existing code reviewed**: ~30 Kotlin files in `app/src/main/java/com/example/uwb/`

**Existing structure**:
- `Bluetooth/BluetoothFragment.kt` — BLE scan + USB Tag connection
- `UI/FriendSharingFragment.kt` — QR create + scan (basic v1 implementation)
- `transport/UsbTransport.kt` — CDC/ACM 115200 to Tag
- `crypto/AesGcmUtil.kt`, `HKDF_SHA256.kt`, `GeneratePrivateKeyEcc.kt`
- `dataLg/KeyManager.kt`, `FriendShareStore.kt`, `PairedDeviceStore.kt`
- `network/ApiClient.kt`, `ApiService.kt` (v1 endpoints)

**Brief written**: `android_app_brief.md` (paste to Claude Code)

**Status**: Brief paste-ed to Claude Code, **answering clarifying questions** (round 2 in progress)

**Pending Claude Code questions (already answered, ready to paste back)**:
- Round 1: v3 API spec, USB transport flow, KeyManager namespace conflict, Owner revocation, FriendSharingFragment scope
- Round 2: USB command format, server pubkey cache strategy, ECDSA verify fail action, OwnerFriendListFragment visibility

**Next milestone**: Claude Code lists files to modify/create → review → confirm → implement

### 2.3 Tag Firmware (ESP32-S3) — ⏳ TODO

**Existing file**: `esp32_firmware/s3_super_mini_central/s3_super_mini_central.ino` (~740 lines)

**Existing functionality**:
- USB Serial parser (SET_KEY, DISCONNECT)
- BLE Central scan + connect to Anchor
- HMAC challenge-response auth with Anchor
- DW3000 UWB ranging with STS encryption (uses pairing key as STS key)
- FreeRTOS 3-task architecture (usbSerialTask, bleTask, uwbTask)

**Brief written**: `tag_firmware_brief.md` (paste to Claude Code in separate session)

**Status**: Not yet started — wait for Android App to finish first

### 2.4 Anchor Firmware (ESP32 in vehicle) — ⏳ OPTIONAL

**Brief written**: `anchor_extension_brief.md`

**Status**: Lowest priority — can skip for thesis defense if time-constrained.

**Rationale for deprioritizing**: Demo can show Server + App + Tag pipeline; Anchor revocation polling is a "nice to have" for the offline-first story but not critical for showing the cryptographic correctness.

---

## 3. Key Decisions Made (and Why)

### Decision 1: Offline-first with periodic sync (vs always-online)

**Decision**: Anchor verifies bundles offline using cached server public key. Periodic revocation poll every 5 minutes when LTE is available.

**Why**:
- Latency: 300ms (offline) vs 3-10s (online every unlock)
- Works in parking garages, basements, server downtime
- 10x less data/battery than always-online
- Industry standard: Tesla, BMW, Apple CarKey, CCC Digital Key 3.0 all do this

**Citations available**: CCC Digital Key 3.0, IEEE 1609.2-2022, V2X SCMS, multiple ACM VANET papers (2008-2009).

### Decision 2: ECDSA P-256 with server-side signing key (vs HMAC shared secret)

**Decision**: Server signs bundles with private key; anchor verifies with cached public key.

**Why**:
- Public-key crypto allows offline verification without sharing secrets
- Compromised anchor cannot forge new bundles (would need server private key)
- Industry standard for digital keys (Tesla uses ECDSA P-256 too)
- Single point of failure: server private key — backup `server_signing_key.pem` is critical

### Decision 3: Permission bitmask scope (UNLOCK + LOCK only)

**Decision**: Minimal permission set for thesis demo: `PERM_UNLOCK = 0x01`, `PERM_LOCK = 0x02`.

**Why**:
- 1-month timeline doesn't allow full permission matrix
- Demonstrates the concept without scope creep
- Easy to extend later (engine start, trunk, valet mode are obvious additions)
- Mention extensibility as "future work" in thesis

### Decision 4: USB Serial bridge (Phone → Tag) instead of BLE direct

**Decision**: Phone communicates with Tag over USB CDC/ACM 115200; Tag does BLE/UWB to Anchor.

**Why**:
- This is **already the existing architecture** in Hiếu's project — don't change it
- Tag is a separate hardware device (key fob style) not the phone
- Phone is just a bridge between cloud and Tag
- Demonstrating UWB + STS encryption requires dedicated UWB hardware (DW3000 on Tag)

### Decision 5: KeyManager namespace separation (Owner vs Friend keys)

**Decision**: Separate namespaces:
- `owner_key_{vehicle_id}` — Owner pairing key (only when user pairs the vehicle)
- `friend_key_{vehicle_id}_{friend_id}` — Friend bundle (multiple per vehicle)

**Why**:
- **BUG in current code**: `KeyManager.savePairingKey(vid, fid, key)` overwrites owner key with friend key (same `key_{vid}` namespace)
- A device can simultaneously be Owner of car A and Friend of car B
- Required for proper multi-vehicle support

### Decision 6: Single-use claim tokens (vs reusable)

**Decision**: After first `GET /friend-sharing/claim/{token}`, server marks token as claimed; subsequent fetches return 410 Gone.

**Why**:
- Prevents token sharing/leakage (one Guest = one claim)
- Owner can detect if Guest's phone was compromised (claim_token already used)
- Standard practice for one-time secrets

### Decision 7: Skip server reporting on local ECDSA fail

**Decision**: When Android local ECDSA verify fails, show error toast and don't save bundle. Do NOT report failure to server.

**Why**:
- Most failures are benign (stale cached pubkey, app outdated, bug) — would flood server with false positives
- Real attackers won't submit forged bundles for logging
- Server-side anchor validate (Sequence 3) is the proper enforcement point
- Privacy: avoid logging `friend_id` to server when not necessary

### Decision 8: 1 Android app with 2 modes (vs 2 separate apps)

**Decision**: Single APK that handles both Owner and Guest roles based on stored credentials.

**Why**:
- Same codebase, less duplication
- Many users will be both Owner of own car + Guest of borrowed car
- UI can hide Owner-only features (like "Manage Shares") when no owner key present
- Demo is cleaner: 1 install per phone

---

## 4. Critical Cross-Component Specs

These MUST match byte-exact across Server, App, Tag, Anchor:

### Canonical Signed Message Format

```
v{version}|{friend_id_hex}|{vehicle_id}|{friend_key_hex}|{issued_at_iso}|{expires_at_iso}|{permissions_decimal}
```

Example:
```
v1|a73a9fc680b792e2|VH001|9ebf37a4b1fb0d138f...|2026-04-25T10:30:00.123456|2026-04-26T10:30:00.123456|1
```

**Rules**:
- `version`: integer (e.g. `1`, not `01`)
- `friend_id`: 16 lowercase hex chars
- `vehicle_id`: ASCII string, no padding
- `friend_key_hex`: 32 lowercase hex chars
- `issued_at`, `expires_at`: ISO 8601 with microseconds (`%Y-%m-%dT%H:%M:%S.%f`)
- `permissions`: decimal integer
- Separator: `|` (pipe)
- Encoding: UTF-8
- Signature: ECDSA-with-SHA256, DER format

### Permission Bitmask

```
PERM_UNLOCK = 0x01  (bit 0)
PERM_LOCK   = 0x02  (bit 1)
```

### Bundle Wire Format (App → Tag over USB)

195 bytes raw, base64-encoded (~328 chars). See `tag_firmware_brief.md` section C for byte layout.

### USB Serial Commands (App → Tag)

```
SET_KEY:<32-hex>\n            (legacy owner mode)
SET_BUNDLE:<base64>\n         (new v3 friend mode)
SET_SERVER_PUBKEY:<base64>\n  (cache server ECDSA pubkey on Tag)
SET_TIME:<unix>\n             (sync RTC since Tag has none)
GET_STATUS\n                  (query state)
DISCONNECT\n
```

### Anchor Authentication Header

For protected endpoints, anchor includes:
- `X-Anchor-Timestamp: <unix_seconds>`
- `X-Anchor-MAC: <base64(HMAC-SHA256(pairing_key, "{vehicle_id}|{timestamp}|{body_sha256_b64}"))>`

Server tolerance: ±5 minutes timestamp window.

---

## 5. Recommended Implementation Order

**Already done**: Server v3 ✅

**Next 3 sessions** (in order):

### Session A: Android App (3-4 days)

1. Update Models for v3 schema
2. Add `BundleVerifier.kt` for ECDSA local verify
3. Add `X-Owner-Key` auth in API service
4. Update `FriendSharingFragment` (permissions UI, TTL spinner, ECDSA verify)
5. Refactor `KeyManager` (separate owner/friend namespaces)
6. Create `OwnerFriendListFragment` (list + revoke)
7. Add USB provisioning in `BluetoothFragment` (SET_SERVER_PUBKEY + SET_TIME + SET_BUNDLE)
8. Test end-to-end with server (Postman + real APK)

### Session B: Tag Firmware (2-3 days)

1. Add bundle data structures + base64 parser
2. Add ECDSA offline verify with mbedTLS
3. Extend serial command parser (SET_BUNDLE, SET_SERVER_PUBKEY, SET_TIME, GET_STATUS)
4. Add permission enforcement in UWB action loop
5. Add backward-compat path (SET_KEY still works)
6. Test with Python USB script + real bundle from server

### Session C: Anchor Firmware (3-5 days, OPTIONAL)

Skip if time-constrained. Demo can work without anchor revocation polling — anchor will only see bundles via BLE handshake from Tag, and Tag does the verification.

If implementing:
1. Component `friend_mgmt` skeleton + NVS schemas
2. ECDSA verify offline (same canonical format)
3. Revocation sync task (5-min poll)
4. Cache miss online validate
5. Activity reporting

---

## 6. Files Created in Sessions So Far

### Source files (in `/mnt/user-data/outputs/`)
- `main.py` — FastAPI server v3 (production-ready)
- `test_flow.py` — End-to-end test script (18/18 scenarios)

### Briefs (paste to Claude Code)
- `README_briefs.md` — Overview + cross-component specs
- `android_app_brief.md` — Android implementation brief
- `tag_firmware_brief.md` — Tag firmware brief
- `anchor_extension_brief.md` — Anchor firmware brief (optional)

### This file
- `CLAUDE.md` — Session memory (you are reading it)

---

## 7. Open Questions / Unresolved

- [ ] **Production deployment URL** — currently `http://10.0.4.64:8000` (LAN). Need HTTPS public URL before demo (Railway/Render).
- [ ] **Tag time sync source** — Tag has no RTC. Currently relies on `SET_TIME` from app each USB connect. Consider fetching from `/health` endpoint server time.
- [ ] **Bundle version migration strategy** — if `bundle_version` increments in future, how do existing Tags handle it? Currently: reject. Future: backward-compat layer.
- [ ] **Multi-vehicle Owner UX** — if user pairs 2+ vehicles, OwnerFriendListFragment needs vehicle selector (Spinner). Decided but not detailed.
- [ ] **Anchor LTE provisioning** — assumed working. May need brief separately for LTE setup if anchor doesn't have it yet.

---

## 8. Defense-Ready Talking Points

When defending the thesis, lead with these:

### Why this design (the 60-second pitch)

> "Hệ thống Friend Sharing được thiết kế theo nguyên tắc **offline-first** chuẩn của automotive industry, dùng ECDSA P-256 cho cryptographic verification và HMAC-SHA256 cho anchor authentication. Trade-off giữa revocation timeliness (5 phút) và offline availability tuân theo chuẩn CCC Digital Key 3.0 và IEEE 1609.2-2022. Defense-in-depth gồm 4 lớp: (1) cryptographic bundle signature, (2) UWB proximity ranging chống relay attack, (3) BLE pairing với HMAC challenge-response, (4) permission bitmask enforcement."

### Key answers to anticipated questions

**Q: Sao không dùng TLS/mTLS giữa anchor và server?**
A: ESP32 + mbedTLS handshake overhead 5-8s trên LTE chậm; HTTP + ECDSA bundle signing đạt cùng mức integrity với overhead 1-2s. Future work: mutual TLS với certificate.

**Q: Sao không check online mỗi lần unlock cho an toàn?**
A: 3 lý do automotive: (1) latency 3-10s vs 300ms, (2) không work trong hầm xe/khi server down, (3) tốn pin và data 10x. Tesla, BMW, CCC chuẩn đều offline-first.

**Q: Khi Owner revoke, Guest còn unlock được không?**
A: Phụ thuộc trạng thái anchor. Online ≤5 phút qua → đã sync → reject. Offline lâu → có revocation gap, bounded bởi chu kỳ poll + fallback validate online khi cache miss.

**Q: Server signing key bị leak thì sao?**
A: Single point of failure trong design hiện tại. Mitigation roadmap: (1) key rotation, (2) HSM/Secure Element, (3) per-vehicle signing key.

**Q: Replay attack trên BLE?**
A: Bundle hiện chưa có nonce, có replay window trong TTL. Mitigation future work: challenge-response với 16-byte nonce từ anchor + HMAC(friend_key, nonce).

---

## 9. Next Steps for User

**Immediate** (this week):
1. Paste round 2 answers back to Claude Code (Android session)
2. Let Claude Code list files to modify, review carefully
3. Approve and let it implement

**This month**:
1. Finish Android implementation, test with deployed server
2. Move to Tag firmware in separate Claude Code session
3. End-to-end test: pair → create share → claim → USB to Tag → BLE to Anchor → unlock
4. Record demo video (10 scenarios from `README_briefs.md`)
5. Write thesis chapter using outline in earlier sessions

**For Ampere job**:
- Internship is concurrent. Friend Sharing project provides strong portfolio piece for embedded security skills.
- Cite this project in interviews as evidence of automotive cybersecurity awareness (UN R155, ISO/SAE 21434).

---

## 10. Update Log

| Date | What changed | Component |
|------|--------------|-----------|
| 2026-04-22 | Initial server v2 written and tested | Server |
| 2026-04-22 | Server v3 with anchor auth + nonce | Server |
| 2026-04-25 | All 3 component briefs written | Briefs |
| 2026-04-25 | Session paused mid-Android-implementation | Android |

---

**End of CLAUDE.md** — When updating, increment date at top and add to Update Log.
