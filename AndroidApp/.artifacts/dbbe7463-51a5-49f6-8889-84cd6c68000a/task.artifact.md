# Task List - NFC KEY Provisioning

- `[x]` Refine NFC Parsing and KEY Storage
    - `[x]` Update `MainActivity.kt` parser for separate VIN/KEY extraction
    - `[x]` Save KEY to `KeyManager` in `MainActivity.kt`
    - `[x]` Show Toast "TAG - Entered KEY"
    - `[x]` Add Logcat security logs
- `[x]` Update EnterVinFragment Flow
    - `[x]` Implement `isNfcKeyAvailable` logic
    - `[x]` Bypass and comment out server KEY retrieval in `btnConfirm`
- `[x]` Verification
    - `[x]` Build check
    - `[x]` Log verification
