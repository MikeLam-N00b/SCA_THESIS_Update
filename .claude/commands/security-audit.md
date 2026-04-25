# Security Audit — SCA Project

Bạn là security specialist. Audit bảo mật cho SCA project theo tiêu chuẩn embedded IoT security.

## Threat Model SCA

**Attack surfaces:**
1. BLE channel (Anchor ↔ Tag/Phone) — trong range ~10m
2. UWB channel — relay attack
3. CAN bus — vật lý
4. HTTP server — network
5. NVS flash — vật lý access
6. Android SharedPreferences — rooted device

**Critical assets:**
- `pairingKey` (16 bytes) — master key, unlock xe
- `friend_key` — limited access key
- `claim_token` — one-time token
- Server ECDSA signing key

## Security Checklist

### Firmware (embedded + secure-code-guardian skill)
- [ ] HMAC-SHA256: `mbedtls_md_hmac()` — đúng
- [ ] Challenge 16 bytes random: `mbedtls_ctr_drbg_random()` — đúng
- [ ] `memcmp` cho HMAC compare → có thể timing attack (nên dùng `mbedtls_ct_memcmp`)
- [ ] STS mode 1 UWB → chống relay (chỉ accept nếu STS quality OK)
- [ ] `connectionGen` → chống stale session replay
- [ ] BLE: GATT không có encryption/pairing → key trong NVS là defense
- [ ] NVS: `ble-keys/bleKey` → không encrypted trong flash
- [ ] Watchdog timer? (chưa thấy trong code)
- [ ] Friend bundle: ECDSA verify → cần implement

### Server (security-reviewer skill)
- [ ] HTTP không HTTPS → plain text trên network
- [ ] SQLite: parameterized queries ✅
- [ ] `secrets.token_hex()` ✅
- [ ] Rate limiting `/owner-pairing` → không có → brute force risk
- [ ] ECDSA signing key: lưu file `server_signing_key.pem` → protect file permissions
- [ ] Error responses: có expose internal details không?
- [ ] `expires_at` format: ISO string → timezone issue?
- [ ] `is_revoked` flag: check ✅

### Android (secure-code-guardian skill)
- [ ] SharedPreferences không encrypted → rooted device risk
- [ ] Pairing key in Logcat? → kiểm tra `Log.d` calls trong KeyManager
- [ ] Retrofit: HTTP không HTTPS → MITM risk
- [ ] BLE data validate trước khi xử lý?
- [ ] Certificate pinning → không có

## Format output
Với mỗi issue tìm thấy:
- **Severity**: Critical / High / Medium / Low
- **Location**: file, function, line
- **Description**: mô tả lỗi
- **Impact**: có thể dùng để tấn công gì
- **Fix**: code/config cụ thể để fix

## Phạm vi audit (để trống = audit toàn bộ):
$ARGUMENTS
