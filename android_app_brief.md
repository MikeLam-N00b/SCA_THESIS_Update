# Android App Extension — Friend Sharing v3

## Context

This is the **Owner/Guest Android app** for the **Smart Car Access (SCA)** thesis project. The app communicates with two components:

1. **Cloud Server** (FastAPI v3) — over HTTPS for pairing, friend share creation, claim, validation
2. **ESP32-S3 Super Mini Tag** — over USB Serial (CDC/ACM, 115200 baud) — the Tag is a separate hardware device (key fob style), NOT the phone itself

The phone acts as a **bridge** between cloud and Tag. After claiming a friend bundle from the cloud, the app forwards the friend key to the Tag via USB. The Tag then connects to the vehicle Anchor (ESP32 DevKit V1) over BLE + UWB.

## Existing Architecture (DO NOT REWRITE)

```
app/src/main/java/com/example/uwb/
├── MainActivity.kt
├── Bluetooth/
│   ├── BluetoothFragment.kt        # BLE scan UI (existing)
│   └── BluetoothManager.kt
├── UI/
│   ├── WelcomeFragment.kt
│   ├── LoginFragment.kt
│   ├── EnterVinFragment.kt
│   ├── VerifyingVinFragment.kt
│   ├── PairingLoadingFragment.kt
│   ├── VehicleInfoFragment.kt
│   ├── FriendSharingFragment.kt    # ← MAIN TARGET FOR EXTENSION
│   ├── UwbFragment.kt
│   └── PortraitCaptureActivity.kt
├── crypto/
│   ├── AesGcmUtil.kt
│   ├── GeneratePrivateKeyEcc.kt
│   └── HKDF_SHA256.kt
├── dataLg/
│   ├── KeyManager.kt               # stores pairing/friend keys
│   ├── FriendShareStore.kt         # local cache of created shares
│   ├── PairedDeviceStore.kt
│   └── SessionManager.kt
├── model/
│   ├── PairingRequest.kt
│   ├── PairingResponse.kt
│   ├── FriendShareRequest.kt
│   ├── FriendShareResponse.kt
│   └── FriendKeyBundle.kt          # ← MUST UPDATE for v3
├── network/
│   ├── ApiClient.kt                # Retrofit instance
│   └── ApiService.kt               # ← MUST UPDATE for v3 endpoints
├── repository/
│   └── PairingRepository.kt
├── transport/
│   ├── Transport.kt                # interface
│   ├── TransportHolder.kt
│   ├── UsbTransport.kt             # USB CDC/ACM to Tag (DO NOT TOUCH)
│   └── UsbPermissionHelper.kt
├── usb/
│   └── (USB device discovery — DO NOT TOUCH)
└── VinValid/
    └── VinValidator.kt
```

## Goal

Update the app to be compatible with **Server v3** (which adds permissions, ECDSA signing, X-Owner-Key auth, revocation, anchor authentication). The app should:

- Authenticate Owner-only actions with `X-Owner-Key` header
- Allow Owner to specify permissions (UNLOCK / LOCK) when creating friend share
- Verify ECDSA signature on claimed friend bundles (defense-in-depth)
- Display new bundle fields (issued_at, permissions, bundle_version)
- Handle single-use claim tokens (server returns 410 on re-claim)
- Update local data models to match new API
- Forward friend key to Tag via USB Serial unchanged (existing flow)

## Required Changes

### A. Update Models (data classes)

**File: `model/PairingResponse.kt`** — Add new fields from server v3:

```kotlin
package com.example.uwb.model

data class PairingResponse(
    val pairing_id: String,
    val server_public_key_b64: String,
    val encrypted_pairing_key_b64: String,
    val nonce_b64: String,
    // NEW v3 fields:
    val owner_api_key: String,
    val server_signing_public_key_b64: String,
    val bundle_version: Int,
    val max_key_ttl_hours: Int,
)
```

**File: `model/FriendShareRequest.kt`** — Add permissions field:

```kotlin
package com.example.uwb.model

// Permission bitmask constants
const val PERM_UNLOCK = 0x01
const val PERM_LOCK = 0x02
const val PERM_ALL = PERM_UNLOCK or PERM_LOCK

data class FriendShareRequest(
    val vehicle_id: String,
    val friend_name: String = "Friend",
    val ttl_hours: Int = 24,
    val permissions: Int = PERM_ALL,
)
```

**File: `model/FriendShareResponse.kt`** — Add issued_at + permissions:

```kotlin
package com.example.uwb.model

data class FriendShareResponse(
    val friend_id: String,
    val claim_token: String,
    val claim_url: String,
    val issued_at: String,
    val expires_at: String,
    val permissions: Int,
)
```

**File: `model/FriendKeyBundle.kt`** — Replace `owner_sig_b64` with `issuer_sig_b64`, add new fields:

```kotlin
package com.example.uwb.model

data class FriendKeyBundle(
    val bundle_version: Int,
    val friend_id: String,
    val vehicle_id: String,
    val friend_key_hex: String,
    val friend_name: String,
    val permissions: Int,
    val issued_at: String,
    val expires_at: String,
    val issuer_sig_b64: String,    // was owner_sig_b64
)
```

### B. Update API Service

**File: `network/ApiService.kt`** — Add Header support and new endpoints:

```kotlin
package com.example.uwb.network

import com.example.uwb.model.*
import retrofit2.http.*

interface ApiService {
    // === Pairing (existing, no header changes) ===
    @POST("/owner-pairing")
    suspend fun ownerPairing(@Body req: PairingRequest): PairingResponse

    @GET("/pairing-bootstrap")
    suspend fun getPairingBootstrap(): PairingBootstrapResponse

    @GET("/server-public-key")
    suspend fun getServerPublicKey(): ServerPublicKeyResponse

    // === Friend Sharing (Owner — protected) ===
    @POST("/friend-sharing/create")
    suspend fun createFriendShare(
        @Header("X-Owner-Key") ownerKey: String,
        @Body req: FriendShareRequest,
    ): FriendShareResponse

    @GET("/friend-sharing/list/{vehicleId}")
    suspend fun listFriendShares(
        @Header("X-Owner-Key") ownerKey: String,
        @Path("vehicleId") vehicleId: String,
    ): ListFriendsResponse

    @DELETE("/friend-sharing/{friendId}")
    suspend fun revokeFriendShare(
        @Header("X-Owner-Key") ownerKey: String,
        @Path("friendId") friendId: String,
    ): SimpleMessageResponse

    // === Friend Sharing (Guest — public, one-time claim) ===
    @GET("/friend-sharing/claim/{token}")
    suspend fun claimFriendShare(@Path("token") token: String): FriendKeyBundle

    // === Health ===
    @GET("/health")
    suspend fun health(): HealthResponse
}
```

Add the new response models in `model/`:

```kotlin
// model/PairingBootstrapResponse.kt
data class PairingBootstrapResponse(
    val server_signing_public_key_b64: String,
    val bundle_version: Int,
    val max_key_ttl_hours: Int,
    val server_time: String,
)

// model/ServerPublicKeyResponse.kt
data class ServerPublicKeyResponse(
    val server_public_key_b64: String,
    val bundle_version: Int,
)

// model/ListFriendsResponse.kt
data class ListFriendsResponse(
    val vehicle_id: String,
    val total: Int,
    val friend_keys: List<FriendKeyEntry>,
)

data class FriendKeyEntry(
    val friend_id: String,
    val friend_name: String?,
    val permissions: Int,
    val issued_at: String,
    val expires_at: String,
    val is_revoked: Boolean,
    val revoked_at: String?,
    val uses_count: Int,
    val last_used_at: String?,
    val claimed: Boolean,
    val created_at: String,
    val status: String,  // "active", "revoked", "expired"
)

// model/SimpleMessageResponse.kt
data class SimpleMessageResponse(
    val message: String,
    val friend_id: String? = null,
)

// model/HealthResponse.kt
data class HealthResponse(
    val status: String,
    val server_time: String,
    val bundle_version: Int,
)
```

### C. Update KeyManager to store Owner API Key

**File: `dataLg/KeyManager.kt`** — Add functions to store/retrieve owner API key per vehicle. Use **EncryptedSharedPreferences** (androidx.security.crypto) since this is a sensitive credential.

Required new functions (signatures):

```kotlin
fun saveOwnerApiKey(vehicleId: String, apiKey: String)
fun getOwnerApiKey(vehicleId: String): String?
fun saveServerSigningPublicKey(pubKeyB64: String)
fun getServerSigningPublicKey(): String?
fun saveBundleVersion(version: Int)
fun getBundleVersion(): Int
```

Storage layout suggestion:
- Namespace: `sca_owner_keys`
- Key format: `owner_api_key_<vehicle_id>` → API key string
- `server_signing_pubkey` → base64 DER public key
- `bundle_version` → Int

### D. Update PairingRepository

**File: `repository/PairingRepository.kt`** — After successful pairing, save the new fields returned by server v3:

```kotlin
suspend fun pairWithServer(...): PairingResult {
    val response = api.ownerPairing(request)
    // ... existing decryption logic for pairing_key ...
    
    // NEW v3 — save additional fields
    KeyManager.saveOwnerApiKey(response.vehicle_id, response.owner_api_key)
    KeyManager.saveServerSigningPublicKey(response.server_signing_public_key_b64)
    KeyManager.saveBundleVersion(response.bundle_version)
    
    return PairingResult.Success(...)
}
```

### E. Update FriendSharingFragment for Owner Mode

**File: `UI/FriendSharingFragment.kt`** — Update `requestNewQr()` to pass owner key + permissions:

```kotlin
private fun requestNewQr() {
    val vin = KeyManager.getVehicleId() ?: run {
        Toast.makeText(requireContext(), "Chưa pair xe", Toast.LENGTH_SHORT).show()
        return
    }
    
    val ownerKey = KeyManager.getOwnerApiKey(vin) ?: run {
        Toast.makeText(requireContext(), "Không tìm thấy owner key — cần pair lại xe", 
            Toast.LENGTH_LONG).show()
        return
    }
    
    val friendName = binding.etFriendName.text?.toString()?.trim() ?: ""
    if (friendName.isEmpty()) {
        binding.etFriendName.error = "Vui lòng nhập tên"
        return
    }
    
    // Read permissions from UI checkboxes
    var permissions = 0
    if (binding.chkUnlock.isChecked) permissions = permissions or PERM_UNLOCK
    if (binding.chkLock.isChecked) permissions = permissions or PERM_LOCK
    if (permissions == 0) {
        Toast.makeText(requireContext(), "Phải chọn ít nhất 1 quyền", Toast.LENGTH_SHORT).show()
        return
    }
    
    // Read TTL from spinner/input
    val ttlHours = parseTtlInput(binding.spinnerTtl.selectedItemPosition)  // helper
    
    setLoading(true)
    lifecycleScope.launch {
        try {
            val response = ApiClient.api.createFriendShare(
                ownerKey = ownerKey,
                req = FriendShareRequest(
                    vehicle_id = vin,
                    friend_name = friendName,
                    ttl_hours = ttlHours,
                    permissions = permissions,
                )
            )
            // ... save to FriendShareStore + render QR ...
        } catch (e: HttpException) {
            when (e.code()) {
                401 -> Toast.makeText(requireContext(), 
                    "Owner key không hợp lệ — cần pair lại", Toast.LENGTH_LONG).show()
                else -> Toast.makeText(requireContext(), 
                    "Lỗi: ${e.message()}", Toast.LENGTH_LONG).show()
            }
        } catch (e: Exception) {
            Toast.makeText(requireContext(), "Không tạo được mã: ${e.message}", 
                Toast.LENGTH_LONG).show()
        } finally {
            setLoading(false)
        }
    }
}
```

### F. Update FriendSharingFragment for Guest Mode (claim flow)

In `claimFromUrl()`, validate ECDSA signature locally before saving:

```kotlin
private fun claimFromUrl(url: String) {
    val token = Uri.parse(url).lastPathSegment
    if (token.isNullOrBlank()) { /* error */ return }
    
    setLoading(true)
    lifecycleScope.launch {
        try {
            val bundle = ApiClient.api.claimFriendShare(token)
            
            // NEW v3 — verify bundle version
            val expectedVersion = KeyManager.getBundleVersion()
            if (bundle.bundle_version != expectedVersion && expectedVersion != 0) {
                Toast.makeText(requireContext(), 
                    "Bundle version mismatch — cần update app", Toast.LENGTH_LONG).show()
                return@launch
            }
            
            // NEW v3 — verify ECDSA signature locally (defense-in-depth)
            val serverPubB64 = KeyManager.getServerSigningPublicKey()
            if (serverPubB64 != null) {
                val sigValid = BundleVerifier.verifyEcdsa(
                    bundle = bundle,
                    serverPublicKeyB64 = serverPubB64,
                )
                if (!sigValid) {
                    Toast.makeText(requireContext(), 
                        "Chữ ký bundle không hợp lệ — từ chối nhận", 
                        Toast.LENGTH_LONG).show()
                    return@launch
                }
            }
            // If serverPubB64 is null (Guest never paired this car), skip local verify
            // — the Tag/Anchor will verify when used
            
            // Save bundle bytes (key, permissions, signature) for forwarding to Tag
            val keyBytes = ByteArray(bundle.friend_key_hex.length / 2) { i ->
                bundle.friend_key_hex.substring(i * 2, i * 2 + 2).toInt(16).toByte()
            }
            KeyManager.savePairingKey(
                vId = bundle.vehicle_id,
                pId = bundle.friend_id,
                key = keyBytes,
            )
            // NEW v3 — also save permissions for UI display
            KeyManager.saveFriendPermissions(bundle.friend_id, bundle.permissions)
            
            Toast.makeText(requireContext(), 
                "Nhận quyền: ${permissionLabel(bundle.permissions)} — xe ${bundle.vehicle_id}", 
                Toast.LENGTH_SHORT).show()
            
            parentFragmentManager.beginTransaction()
                .replace(R.id.fragment_container, BluetoothFragment())
                .addToBackStack(null)
                .commit()
                
        } catch (e: HttpException) {
            when (e.code()) {
                404 -> showError("Mã QR không tồn tại")
                410 -> showError("Mã QR đã được dùng hoặc hết hạn")
                else -> showError("Lỗi: ${e.message()}")
            }
        } catch (e: Exception) {
            showError("Lỗi nhận key: ${e.message}")
        } finally {
            setLoading(false)
        }
    }
}

private fun permissionLabel(perms: Int): String {
    val parts = mutableListOf<String>()
    if (perms and PERM_UNLOCK != 0) parts.add("Mở khóa")
    if (perms and PERM_LOCK != 0) parts.add("Khóa")
    return parts.joinToString(" + ")
}
```

### G. Create BundleVerifier (new crypto helper)

**File: `crypto/BundleVerifier.kt`** — ECDSA verification on Android using BouncyCastle or built-in `java.security`:

```kotlin
package com.example.uwb.crypto

import android.util.Base64
import com.example.uwb.model.FriendKeyBundle
import java.security.KeyFactory
import java.security.Signature
import java.security.spec.X509EncodedKeySpec

object BundleVerifier {
    
    /**
     * Reconstruct canonical message identical to what server signed.
     * Format MUST match server's _friend_bundle_message():
     *   "v{version}|{friend_id}|{vehicle_id}|{friend_key_hex}|{issued_at}|{expires_at}|{permissions}"
     */
    fun canonicalMessage(bundle: FriendKeyBundle): ByteArray {
        return "v${bundle.bundle_version}|${bundle.friend_id}|${bundle.vehicle_id}|" +
               "${bundle.friend_key_hex}|${bundle.issued_at}|${bundle.expires_at}|" +
               "${bundle.permissions}".toByteArray(Charsets.UTF_8)
    }
    
    /**
     * Verify ECDSA P-256 SHA-256 signature with server public key.
     * @return true if signature is valid
     */
    fun verifyEcdsa(bundle: FriendKeyBundle, serverPublicKeyB64: String): Boolean {
        return try {
            val pubKeyDer = Base64.decode(serverPublicKeyB64, Base64.DEFAULT)
            val keyFactory = KeyFactory.getInstance("EC")
            val pubKey = keyFactory.generatePublic(X509EncodedKeySpec(pubKeyDer))
            
            val sigBytes = Base64.decode(bundle.issuer_sig_b64, Base64.DEFAULT)
            
            val sig = Signature.getInstance("SHA256withECDSA")
            sig.initVerify(pubKey)
            sig.update(canonicalMessage(bundle))
            sig.verify(sigBytes)
        } catch (e: Exception) {
            android.util.Log.e("BundleVerifier", "Verify failed: ${e.message}")
            false
        }
    }
}
```

### H. Update layout `fragment_friend_sharing.xml`

Add UI elements for permissions and TTL selection (Owner mode):

```xml
<!-- Inside layoutEmpty (the form before QR is shown) -->
<TextView
    android:layout_width="match_parent" android:layout_height="wrap_content"
    android:text="Quyền chia sẻ:" />

<CheckBox android:id="@+id/chkUnlock"
    android:layout_width="match_parent" android:layout_height="wrap_content"
    android:text="Mở khóa xe" android:checked="true" />

<CheckBox android:id="@+id/chkLock"
    android:layout_width="match_parent" android:layout_height="wrap_content"
    android:text="Khóa xe" android:checked="true" />

<TextView
    android:layout_width="match_parent" android:layout_height="wrap_content"
    android:text="Thời hạn:" />

<Spinner android:id="@+id/spinnerTtl"
    android:layout_width="match_parent" android:layout_height="wrap_content"
    android:entries="@array/ttl_options" />
```

In `res/values/strings.xml`:
```xml
<string-array name="ttl_options">
    <item>1 giờ</item>
    <item>24 giờ</item>
    <item>7 ngày</item>
    <item>30 ngày</item>
</string-array>
```

### I. Add Friend List Screen for Owner

Create new fragment `OwnerFriendListFragment.kt` to show all created shares with revoke ability:

```kotlin
class OwnerFriendListFragment : Fragment() {
    // RecyclerView showing list from API: GET /friend-sharing/list/{vehicleId}
    // Each item shows: friend_name, permissions, expires_at, status (active/revoked/expired)
    // Click "Thu hồi" → DELETE /friend-sharing/{friend_id}
    
    private fun loadFriends() {
        val vin = KeyManager.getVehicleId() ?: return
        val ownerKey = KeyManager.getOwnerApiKey(vin) ?: return
        
        lifecycleScope.launch {
            try {
                val response = ApiClient.api.listFriendShares(ownerKey, vin)
                adapter.submitList(response.friend_keys)
            } catch (e: Exception) {
                showError(e.message)
            }
        }
    }
    
    private fun revokeFriend(friendId: String) {
        // Show confirmation dialog first
        AlertDialog.Builder(requireContext())
            .setTitle("Xác nhận thu hồi")
            .setMessage("Sau khi thu hồi, bạn này sẽ không mở được xe nữa")
            .setPositiveButton("Thu hồi") { _, _ ->
                lifecycleScope.launch {
                    try {
                        val vin = KeyManager.getVehicleId() ?: return@launch
                        val ownerKey = KeyManager.getOwnerApiKey(vin) ?: return@launch
                        ApiClient.api.revokeFriendShare(ownerKey, friendId)
                        loadFriends()  // refresh
                    } catch (e: Exception) {
                        showError(e.message)
                    }
                }
            }
            .setNegativeButton("Hủy", null)
            .show()
    }
}
```

Add navigation entry from `FriendSharingFragment` (button "Quản lý chia sẻ") → push `OwnerFriendListFragment`.

### J. Add gradle dependencies

**File: `app/build.gradle.kts`** — Ensure these exist:

```kotlin
dependencies {
    // EncryptedSharedPreferences for owner API key
    implementation("androidx.security:security-crypto:1.1.0-alpha06")
    
    // Already present (verify):
    // implementation("com.squareup.retrofit2:retrofit:...")
    // implementation("com.squareup.retrofit2:converter-gson:...")
    // implementation("com.journeyapps:zxing-android-embedded:...")
    
    // BouncyCastle is optional — Android has built-in EC support since API 26
    // Only add if you hit issues with built-in:
    // implementation("org.bouncycastle:bcprov-jdk15on:1.70")
}
```

## Acceptance Criteria

The implementation is complete when:

1. ✅ Owner can pair vehicle and `owner_api_key` is stored in EncryptedSharedPreferences
2. ✅ Creating friend share works with permissions checkboxes (UNLOCK, LOCK)
3. ✅ Server returns 401 if `X-Owner-Key` is missing/wrong → app shows clear error
4. ✅ Guest can scan QR and claim bundle (single-use enforced by server)
5. ✅ App verifies ECDSA signature locally before saving friend bundle (if server pubkey is cached)
6. ✅ Tampered bundles (modified locally for testing) are rejected
7. ✅ Permission info is displayed to Guest (which actions allowed)
8. ✅ Friend key + bundle still forwarded to ESP32 Tag via existing USB Serial flow
9. ✅ Owner can list friends with current status (active/revoked/expired)
10. ✅ Owner can revoke a friend with confirmation dialog
11. ✅ Re-claim of used token returns 410 → app shows "Mã đã được dùng"
12. ✅ Existing Tag/USB flow remains untouched (USB Serial commands, BLE scan)
13. ✅ All HTTP errors handled with localized Vietnamese messages

## Constraints

- **Min SDK**: keep at current (likely 26+)
- **Existing UI patterns**: use Fragment + ViewBinding (already in codebase)
- **Vietnamese UI**: all user-facing strings in Vietnamese (consistent with existing app)
- **Network errors**: handle 401, 404, 410, 500 distinctly with appropriate UX
- **Backward compat**: if server returns v1-format bundle (no `bundle_version` field), parse gracefully — log warning but don't crash
- **EncryptedSharedPreferences**: use it for `owner_api_key` since leaking this = full vehicle compromise
- **Security**: never log `owner_api_key` or `friend_key_hex` to logcat in release builds

## Out of Scope (DO NOT implement)

- Changes to Tag firmware (separate brief)
- Changes to USB Transport layer (`UsbTransport.kt`)
- Changes to BLE scan flow (`BluetoothFragment.kt` — only as navigation target)
- UWB ranging logic (`UwbFragment.kt`)
- Changes to existing pairing handshake (ECDH, HKDF, AES-GCM)
- New Activity classes — keep Fragment-based architecture
- Changes to existing crypto utilities (`AesGcmUtil`, `HKDF_SHA256`, etc.)
- Persistent activity log UI (server-side only for now)

## Code Style

- Kotlin conventions, snake_case for JSON fields (matches server)
- Use `lifecycleScope.launch` for coroutines (existing pattern)
- Use `binding` for view access (ViewBinding already enabled)
- Extract reusable error display to helper: `private fun showError(msg: String?)`
- Vietnamese strings → `strings.xml` (don't hardcode)
- Use `try/catch` with `HttpException` for Retrofit errors

## Testing Approach

1. **Unit test BundleVerifier**: hardcode a bundle + signature from server, verify pass; flip 1 byte, verify fail
2. **Manual integration**:
   - Pair vehicle → check `owner_api_key` saved
   - Create friend share → verify QR generates correctly
   - Scan QR with second device → verify bundle stored + ECDSA verified
   - Try re-scan same QR → must show "đã được dùng"
   - Revoke → verify status changes in list
3. **USB Serial test**: after Guest claim, plug in Tag, verify `SET_KEY:<friend_key>` sent unchanged

## File Summary (what Claude Code should produce)

**Modified files:**
- `model/PairingResponse.kt`
- `model/FriendShareRequest.kt`
- `model/FriendShareResponse.kt`
- `model/FriendKeyBundle.kt`
- `network/ApiService.kt`
- `dataLg/KeyManager.kt`
- `repository/PairingRepository.kt`
- `UI/FriendSharingFragment.kt`
- `res/layout/fragment_friend_sharing.xml`
- `res/values/strings.xml`
- `app/build.gradle.kts`

**New files:**
- `crypto/BundleVerifier.kt`
- `model/PairingBootstrapResponse.kt`
- `model/ServerPublicKeyResponse.kt`
- `model/ListFriendsResponse.kt`
- `model/SimpleMessageResponse.kt`
- `model/HealthResponse.kt`
- `UI/OwnerFriendListFragment.kt`
- `res/layout/fragment_owner_friend_list.xml`
- `res/layout/item_owner_friend.xml`

---

**Please review the existing code (especially `FriendSharingFragment.kt`, `KeyManager.kt`, `PairingRepository.kt`) before making changes so the new code follows the same patterns. Ask clarifying questions if any field name or flow is ambiguous.**
