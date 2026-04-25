# Friend Sharing v3 — Implementation Briefs Overview

This folder contains 3 separate technical briefs for extending the Smart Car Access (SCA) thesis project to support **Friend Sharing**. Each brief is designed to be paste-able into Claude Code as a self-contained task.

## System Architecture

```
                  ┌──────────────────┐
                  │  Cloud Server    │  ← anchor_extension_brief.md
                  │  (FastAPI v3)    │     refers to this
                  │  ✅ DONE          │
                  └────────┬─────────┘
                           │ HTTPS
                           │
              ┌────────────┴────────────┐
              ▼                         ▼
      ┌──────────────┐          ┌──────────────┐
      │ Android App  │          │ ESP32 Anchor │  ← anchor_extension_brief.md
      │ (Phone)      │          │ (in vehicle) │     (separate work)
      │              │          │              │
      │ ⏳ TODO       │          │ ⏳ TODO       │
      └──────┬───────┘          └──────┬───────┘
             │ USB Serial               │ BLE + UWB
             │ (CDC/ACM)                │
             ▼                          │
      ┌──────────────┐                  │
      │ ESP32-S3 Tag │──────────────────┘
      │ (key fob)    │
      │              │
      │ ⏳ TODO       │
      └──────────────┘
```

## Three Briefs in This Folder

| File | Target Component | Lang | Status |
|------|-----------------|------|--------|
| `android_app_brief.md` | Android App (phone) | Kotlin | TODO |
| `tag_firmware_brief.md` | ESP32-S3 Super Mini Tag (key fob) | Arduino C++ | TODO |
| `anchor_extension_brief.md` | ESP32 Anchor (in vehicle) | ESP-IDF C | TODO |

## How to Use With Claude Code

### Recommended Order

Anh khuyên em làm theo thứ tự này (ít risk nhất):

**1. Android App first** (3-4 ngày)
- Update models + API service to v3 schema
- Add ECDSA verify locally (defense-in-depth)
- Add permission UI + Owner key auth
- Server đã có sẵn, em test được ngay với Postman + app

**2. Tag Firmware next** (2-3 ngày)
- Extend serial protocol with SET_BUNDLE
- Add ECDSA offline verify
- Add permission enforcement
- Test bằng Python script đóng vai app, gửi base64 bundle qua USB

**3. Anchor Firmware last** (3-5 ngày, optional cho thesis)
- Most complex, but actually NOT critical for demo
- Anchor revocation polling can be skipped for thesis demo
- Em có thể defense without this if thiếu time

### Per-Brief Workflow

For each brief, in Claude Code:

```
1. Open the project (cd to project root)
2. Run: claude
3. Provide context:
   "Please read the file `android_app_brief.md` (or whichever applies)
    in the project root. Before writing any code, please:
    - Explore existing source code to understand current patterns
    - List the files you plan to modify and create
    - Highlight any ambiguity in the brief
    - Wait for my confirmation before implementing"

4. Review Claude's plan, give green light
5. Let it implement step by step
6. Test each milestone before moving on
```

### Critical Cross-Component Specs

These specs MUST match across all 3 components or things break:

#### Canonical Signed Message Format

Server, App (verify), Tag (verify) must reconstruct identically:

```
v{version}|{friend_id_hex}|{vehicle_id}|{friend_key_hex}|{issued_at_iso}|{expires_at_iso}|{permissions_decimal}
```

Example:
```
v1|a73a9fc680b792e2|VH001|9ebf37a4b1fb0d138f...|2026-04-25T10:30:00.123456|2026-04-26T10:30:00.123456|1
```

Notes:
- `version`: integer, e.g. `1` (not `01`)
- `friend_id`: 16 lowercase hex chars
- `vehicle_id`: ASCII string, no padding
- `friend_key_hex`: 32 lowercase hex chars
- `issued_at`, `expires_at`: ISO 8601 with microseconds, no timezone suffix
- `permissions`: decimal integer (e.g. `1`, not `0x01`)
- Separator: `|` (pipe)
- Encoding: UTF-8

#### Permission Bitmask

```
PERM_UNLOCK = 0x01  (bit 0)
PERM_LOCK   = 0x02  (bit 1)
```

#### Bundle Wire Format (App → Tag over USB Serial)

Base64-encoded binary, 195 bytes raw → ~328 bytes base64.

See `tag_firmware_brief.md` section C for byte layout.

#### Server v3 Endpoints (App uses)

```
POST   /owner-pairing                     → returns owner_api_key + server_signing_pubkey
GET    /pairing-bootstrap                 → server_signing_pubkey (if not paired)
POST   /friend-sharing/create             [X-Owner-Key]  → claim_token, friend_id
GET    /friend-sharing/claim/{token}      → FriendKeyBundle (one-time)
GET    /friend-sharing/list/{vehicle_id}  [X-Owner-Key]  → list of friends
DELETE /friend-sharing/{friend_id}        [X-Owner-Key]  → revoke
GET    /health                            → connectivity check
```

## Test Vectors (Optional)

If em want test vectors to verify implementation correctness, run:

```bash
# Start server
uvicorn main:app --host 127.0.0.1 --port 8000

# Run test_flow_v3.py to print sample bundle + signature
# Capture output as test fixture for unit tests
```

## Out of Scope for This Phase

These items are explicitly NOT covered in any brief:

- ❌ Activity log UI in mobile app (server logs already implemented)
- ❌ Anchor LTE provisioning (assume LTE works)
- ❌ Secure Element integration (future work)
- ❌ Multi-bundle concurrent (only 1 active bundle on Tag)
- ❌ Time sync via NTP/SNTP (Tag uses SET_TIME from app)
- ❌ MQTT real-time notifications (HTTP polling only)
- ❌ Rate limiting on server (deploy-time config)
- ❌ Production HTTPS cert management (use Railway/Render auto-cert)

## Final Demo Scenarios

After all 3 briefs are implemented, demo video should show:

1. **Owner pairs vehicle** → app stores `owner_api_key`
2. **Owner creates friend share** → checks UNLOCK only, 24h TTL → QR appears
3. **Guest scans QR** → app claims bundle, ECDSA verified locally, permissions displayed
4. **Guest plugs in Tag** → app forwards `SET_SERVER_PUBKEY` + `SET_BUNDLE` over USB
5. **Tag verifies bundle offline** → `BUNDLE_OK` echoed back
6. **Guest approaches car** → Tag scans BLE, auth with Anchor, UWB ranging
7. **Tag enters unlock zone** → permission check passes → CAN UNLOCK frame sent → car unlocks
8. **Tag tries lock** → `PERM_DENIED:LOCK` because bundle didn't grant LOCK
9. **Owner revokes friend** in app → DELETE call to server
10. **Guest re-tries scanning QR** → 410 Gone (single-use claim)

## Questions to Ask Claude Code If Stuck

If implementation gets stuck, useful queries:

- "Show me the current canonical message string the server produces for a sample bundle"
- "What's the exact byte sequence the Tag receives in SET_BUNDLE? Print hex dump."
- "Compare ECDSA verify input bytes between server and client. Are they identical?"
- "What error does mbedtls_pk_verify return? Decode the error code."

These questions help isolate whether failures are due to message format mismatch, signature encoding, or cryptographic issues.

---

Good luck Hiếu! Có gì vướng cứ ping anh.
