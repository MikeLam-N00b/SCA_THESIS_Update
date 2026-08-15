# Walkthrough - NFC KEY Provisioning and Local Storage

I have successfully extended the NFC implementation to parse VIN and KEY separately, store the KEY locally in `KeyManager`, and use it for the BLE authentication flow, bypassing the server call.

## Changes Made

### [MainActivity.kt](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS%20-%20Update/AndroidApp/app/src/main/java/com/example/uwb/MainActivity.kt)
- **Robust Regex Parsing**: Switched to regular expressions (`vinRegex` and `keyRegex`) to extract the `VIN` and `KEY` from the NFC payload. This allows the app to correctly separate data even if they are on the same line or separated by multiple spaces/colons.
- **Local Storage**: The parsed `KEY` is converted to a `ByteArray` and stored in `KeyManager.savePairingKey()`.
- **UI Feedback**: Fixed an issue where the entire payload was displayed in the VIN field; now only the extracted VIN is passed to the UI. Added Toast notifications for success and error states.

### [EnterVinFragment.kt](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS%20-%20Update/AndroidApp/app/src/main/java/com/example/uwb/UI/EnterVinFragment.kt)
- **NFC Logic Bypass**: Added `isNfcKeyAvailable` flag to track if the current VIN was provided via NFC with a KEY.
- **Confirm Button**: Modified the click listener to bypass the server pairing call if an NFC key is available. It now proceeds directly to the BLE flow.
- **Code Preservation**: The server-based `sendVinToServer` logic has been commented out but preserved as requested, marked with appropriate comments.

## How to Test

### 1. Prepare NFC Tag
Create an NFC tag with an NDEF Text Record containing:
```
VIN 1HGBH41JXMN109186
KEY 9d0b658aa467970a32f315ee018d7307
```

### 2. Run the App
1.  Deploy to a physical device.
2.  Navigate to the **Enter VIN** screen.

### 3. Scan the Tag
1.  Bring the tag close to the phone.
2.  Verify:
    - VIN `1HGBH41JXMN109186` fills the input field.
    - Toast `"TAG - Entered KEY"` appears.
    - If KEY is missing, Toast `"TAG - KEY not found"` appears.

### 4. Confirm and Verify Flow
1.  Press **Confirm**.
2.  The app should immediately navigate to the BLE scanning/loading screen.
3.  Check Logcat (`tag:SCA_NFC`):
    - `SCA_NFC: VIN parsed successfully`
    - `SCA_NFC: NFC KEY parsed successfully`
    - `SCA_NFC: KEY stored locally`
    - `SCA_NFC: TAG key is ready for BLE authentication`

### 5. Persistence
1.  Scan the tag.
2.  Close the app and restart.
3.  The key remains in `KeyManager` (stored in `PairingKeys` SharedPreferences).

You should see logs similar to:
```
D/SCA_NFC: NFC reader initialized
D/SCA_NFC: NFC tag detected
D/SCA_NFC: NDEF message received
D/SCA_NFC: VIN parsed successfully
D/SCA_NFC: KEY parsed successfully
D/SCA_NFC: VIN inserted into vehicle VIN field
```

> [!IMPORTANT]
> The actual `KEY` value is never logged to ensure data sensitivity.
