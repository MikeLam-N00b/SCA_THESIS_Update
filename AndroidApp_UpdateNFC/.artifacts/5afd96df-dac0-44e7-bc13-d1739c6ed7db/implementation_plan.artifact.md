# Implementation Plan - VIN Persistence and EnterVIN UI Update

Modify the NFC Owner Pairing flow to store the VIN locally alongside the KEY, and update the `EnterVinFragment` to automatically proceed if both are already provisioned.

## User Review Required

> [!IMPORTANT]
> The VIN will be stored in `SharedPreferences` under the key `last_vin`, which is already managed by the existing `KeyManager`.
> I will create a new vector asset `ic_check_green.xml` for the status checkmarks.
> The "Confirm" button will remain visible even when VIN/KEY are auto-filled, as per the requirement to "allow the user to press the existing 'Confirm' button immediately".

## Proposed Changes

### Assets

#### [NEW] [ic_check_green.xml](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS - Update/AndroidApp_UpdateNFC/app/src/main/res/drawable/ic_check_green.xml)
- Simple green checkmark vector drawable.

### UI Components

#### [MODIFY] [fragment_enter_vin.xml](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS - Update/AndroidApp_UpdateNFC/app/src/main/res/layout/fragment_enter_vin.xml)
- Wrap the existing `TextInputLayout` in a `LinearLayout` with ID `layoutVinInput`.
- Add a new `LinearLayout` with ID `layoutStatusDisplay` containing two status rows:
    - "Vehicle VIN Entered ✓"
    - "Tag - KEY Entered ✓"
- Both containers will have their visibility toggled programmatically.

### Core Logic

#### [MODIFY] [MainActivity.kt](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS - Update/AndroidApp_UpdateNFC/app/src/main/java/com/example/uwb/MainActivity.kt)
- Update `onTagDiscovered` to call `KeyManager.savePairingKey` **before** notifying the fragment via `handleVinParsed`. This ensures the fragment immediately sees the persisted VIN/KEY state.

#### [MODIFY] [EnterVinFragment.kt](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS - Update/AndroidApp_UpdateNFC/app/src/main/java/com/example/uwb/UI/EnterVinFragment.kt)
- Add `updateUiState()` to toggle between input and status display based on `KeyManager.isKeySet()`.
- Call `updateUiState()` in `onViewCreated` and `onVinReceived`.
- Update `btnConfirm` click listener to use the stored VIN from `KeyManager` if available, bypassing manual input validation.

## Verification Plan

### Automated Tests
- Build the project to ensure no compilation errors.

### Manual Verification
1. **NFC Pairing**:
    - Open "Enter VIN" screen.
    - Scan an NFC tag containing VIN/KEY.
    - Verify the UI immediately hides the input field and shows the "Entered ✓" status rows with green checkmarks.
2. **Persistence**:
    - Restart the application.
    - Navigate to the "Enter VIN" screen.
    - Verify it still shows the "Entered ✓" status (loaded from `SharedPreferences`).
3. **Navigation**:
    - Press "Confirm" on the status screen.
    - Verify it navigates directly to the BLE scanning screen without requiring VIN input.
