# Review Android — SCA Project

Bạn là senior Kotlin/Android engineer. Review đoạn code Android Kotlin dưới đây.

## Context
- Pattern: Fragment-based UI, Repository pattern, Retrofit2
- Storage: KeyManager (SharedPreferences), PairedDeviceStore, FriendShareStore
- Network: ApiService với suspend functions
- BLE: Android BLE API (GATT client)
- Package: com.example.uwb

## Checklist Review

### Kotlin Quality (kotlin-specialist skill)
- [ ] Null safety: không `!!` trừ khi có comment giải thích?
- [ ] `sealed class` / `sealed interface` cho state modeling?
- [ ] `suspend` functions cho async operations?
- [ ] Scope functions dùng đúng (`let`, `apply`, `also`, `with`, `run`)?
- [ ] `Flow` cho reactive streams?
- [ ] Coroutine cancellation handled (cancel parent scope on teardown)?
- [ ] KDoc cho public APIs?

### Coroutines & Threading (kotlin-specialist skill)
- [ ] KHÔNG `GlobalScope.launch` → dùng `viewModelScope`/`lifecycleScope`?
- [ ] KHÔNG `runBlocking` trong production?
- [ ] Network calls trên `Dispatchers.IO`?
- [ ] UI updates trên `Dispatchers.Main`?
- [ ] `withContext(Dispatchers.IO)` cho blocking I/O?
- [ ] Structured concurrency — không fire-and-forget?

### KeyManager Pattern (SCA-specific)
- [ ] Check `KeyManager.loadPairingKey(vin)` TRƯỚC khi gọi `/owner-pairing`?
- [ ] `KeyManager.savePairingKey()` sau khi nhận key từ server?
- [ ] `PairedDeviceStore` update sau successful pairing?

### BLE Handling
- [ ] BLUETOOTH_CONNECT permission check (Android API 31+)?
- [ ] BLE callback chạy trên đúng thread (dùng `Handler`/`runOnUiThread`)?
- [ ] GATT connection close sau khi disconnect?
- [ ] Retry logic cho BLE scan?

### Network & Error Handling
- [ ] `try-catch` cho mọi Retrofit suspend call?
- [ ] Error message không expose sensitive data?
- [ ] Timeout config cho Retrofit client?
- [ ] Loading state trong UI khi đang gọi API?

### Security (secure-code-guardian skill)
- [ ] Pairing key không log ra Logcat (production build)?
- [ ] SharedPreferences → cân nhắc Android Keystore cho sensitive data?
- [ ] BLE data validate length trước khi xử lý?

## Code cần review:
$ARGUMENTS
