# Implementation Plan - NFC KEY Provisioning and Local Storage

Extend the existing NFC implementation to parse VIN and KEY separately, store the KEY locally, and use it for BLE authentication instead of fetching it from the server.

## User Review Required

> [!IMPORTANT]
> The server-based KEY retrieval will be temporarily disabled in `EnterVinFragment`.
> The `KeyManager` will be used to store the NFC KEY persistently in `SharedPreferences`.
> A Toast "TAG - Entered KEY" will be shown upon successful NFC KEY storage.

## Proposed Changes

### Core Logic & Storage

#### [MODIFY] [MainActivity.kt](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS%20-%20Update/AndroidApp/app/src/main/java/com/example/uwb/MainActivity.kt)
- Update `onTagDiscovered` parser to extract `VIN` and `KEY` separately.
- Save the parsed `KEY` into `KeyManager.savePairingKey(vin, "NFC_TAG", keyBytes)`.
- Show Toast `"TAG - Entered KEY"`.
- Update `handleVinParsed` to pass a flag indicating an NFC key was received.
- Add Logcat logs: `SCA_NFC: VIN parsed successfully`, `SCA_NFC: NFC KEY parsed successfully`, `SCA_NFC: KEY stored locally`.

#### [MODIFY] [EnterVinFragment.kt](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS%20-%20Update/AndroidApp/app/src/main/java/com/example/uwb/UI/EnterVinFragment.kt)
- Add `isNfcKeyAvailable` flag.
- Update `onVinReceived` to set `isNfcKeyAvailable = true`.
- Modify `btnConfirm.setOnClickListener` to:
    - Skip `sendVinToServer` if `isNfcKeyAvailable` is true.
    - Comment out the `sendVinToServer` logic that calls the server, preserving it for future use.
    - Directly call `navigateAfterPairing(vin)` when `isNfcKeyAvailable` is true.

## Verification Plan

### Automated Tests
- None (requires physical NFC).

### Manual Verification
1.  **Prepare Tag**: Use a tag with:
    ```
    VIN 1HGBH41JXMN109186
    KEY 9d0b658aa467970a32f315ee018d7307
    ```
2.  **Scan Tag**:
    - Verify VIN fills the input field.
    - Verify Toast `"TAG - Entered KEY"` appears.
3.  **Confirm**:
    - Press "Confirm".
    - Verify it skips the server call (no "PAIRING KEY FROM SERVER" log).
    - Verify it navigates to BLE scan/loading.
4.  **Check Logs**:
    - Verify `SCA_NFC: TAG key is ready for BLE authentication` (if added) or similar logs.
    - Verify the actual KEY is NOT logged.
5.  **Restart App**:
    - Verify the KEY remains available (KeyManager persists to SharedPreferences).
