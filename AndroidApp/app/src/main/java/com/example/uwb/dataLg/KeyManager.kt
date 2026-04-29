package com.example.uwb.dataLg

import android.content.Context
import android.content.SharedPreferences
import android.util.Log

/**
 * KeyManager: lưu owner/friend pairing keys cả in-memory và SharedPreferences.
 *
 * Namespace (SharedPreferences "PairingKeys"):
 *   owner_key_{vin}          — owner pairing key (hex, 32 chars)
 *   owner_pid_{vin}          — owner pairing ID
 *   owner_ts_{vin}           — owner key timestamp
 *   friend_key_{vin}_{fid}   — friend key (hex, 32 chars)
 *   friend_json_{vin}_{fid}  — friend bundle JSON
 *   last_vin                 — VIN được dùng gần nhất
 *   migrated_v2              — flag migration từ namespace cũ (key_*)
 *
 * In-memory session: pairingKey/vehicleId/pairingId giữ key đang active
 * (owner hoặc friend) để BLE auth đọc qua getPairingKey()/getVehicleId().
 */
object KeyManager {

    private var pairingKey: ByteArray? = null
    private var vehicleId: String? = null
    private var pairingId: String? = null
    private var timestamp: Long = 0
    private var ownerApiKey: String? = null

    private var prefs: SharedPreferences? = null

    fun init(context: Context) {
        prefs = context.applicationContext
            .getSharedPreferences("PairingKeys", Context.MODE_PRIVATE)
        migrateIfNeeded()
        val lastVin = prefs?.getString("last_vin", null)
        if (lastVin != null) {
            loadPairingKey(lastVin)
            return
        }
        prefs?.all?.keys
            ?.firstOrNull { it.startsWith("owner_key_") }
            ?.removePrefix("owner_key_")
            ?.let { vin -> loadPairingKey(vin) }
    }

    // key_* → owner_key_* (chạy 1 lần sau khi update)
    private fun migrateIfNeeded() {
        if (prefs?.getBoolean("migrated_v2", false) == true) return
        val editor = prefs?.edit() ?: return
        prefs?.all?.entries
            ?.filter { it.key.startsWith("key_") }
            ?.forEach { (oldKey, _) ->
                val vin = oldKey.removePrefix("key_")
                val hex = prefs?.getString("key_$vin", null) ?: return@forEach
                editor.putString("owner_key_$vin", hex)
                editor.putString("owner_pid_$vin", prefs?.getString("pid_$vin", "") ?: "")
                editor.putLong("owner_ts_$vin", prefs?.getLong("ts_$vin", 0) ?: 0)
                editor.remove("key_$vin")
                editor.remove("pid_$vin")
                editor.remove("ts_$vin")
            }
        editor.putBoolean("migrated_v2", true)
        editor.apply()
        Log.d("KeyManager", "✓ Migrated to v2 namespace")
    }

    // ── Owner pairing ──────────────────────────────────────────────────────

    fun saveOwnerPairingKey(vId: String, pId: String, key: ByteArray) {
        vehicleId = vId
        pairingId = pId
        pairingKey = key
        timestamp = System.currentTimeMillis()
        prefs?.edit()
            ?.putString("owner_key_$vId", key.toHex())
            ?.putString("owner_pid_$vId", pId)
            ?.putLong("owner_ts_$vId", timestamp)
            ?.putString("last_vin", vId)
            ?.apply()
        Log.d("KeyManager", "✓ Owner key saved for $vId")
    }

    /** Backward-compat alias — EnterVinFragment dùng hàm này. */
    fun savePairingKey(vId: String, pId: String, key: ByteArray) =
        saveOwnerPairingKey(vId, pId, key)

    fun saveOwnerApiKey(vId: String, apiKey: String) {
        ownerApiKey = apiKey
        prefs?.edit()?.putString("owner_api_key_$vId", apiKey)?.apply()
        Log.d("KeyManager", "✓ Owner API key saved for $vId")
    }

    fun getOwnerApiKey(): String? = ownerApiKey

    fun getOwnerApiKeyForVin(vId: String): String? =
        prefs?.getString("owner_api_key_$vId", null)

    fun loadPairingKey(vId: String): Boolean {
        val hex = prefs?.getString("owner_key_$vId", null) ?: return false
        if (hex.length != 32) return false
        vehicleId = vId
        pairingId = prefs?.getString("owner_pid_$vId", "") ?: ""
        pairingKey = hex.fromHex()
        timestamp = prefs?.getLong("owner_ts_$vId", 0) ?: 0
        ownerApiKey = prefs?.getString("owner_api_key_$vId", null)
        prefs?.edit()?.putString("last_vin", vId)?.apply()
        Log.d("KeyManager", "✓ Owner key loaded for $vId: $hex")
        return true
    }

    fun hasAnyOwnerKey(): Boolean =
        prefs?.all?.keys?.any { it.startsWith("owner_key_") } == true

    fun getAllOwnerVehicleIds(): List<String> =
        prefs?.all?.keys
            ?.filter { it.startsWith("owner_key_") }
            ?.map { it.removePrefix("owner_key_") }
            ?: emptyList()

    // ── Friend bundles ─────────────────────────────────────────────────────

    fun saveFriendBundle(vId: String, friendId: String, keyBytes: ByteArray, bundleJson: String) {
        prefs?.edit()
            ?.putString("friend_key_${vId}_${friendId}", keyBytes.toHex())
            ?.putString("friend_json_${vId}_${friendId}", bundleJson)
            ?.apply()
        // Activate as current session key for BLE auth
        vehicleId = vId
        pairingId = friendId
        pairingKey = keyBytes
        timestamp = System.currentTimeMillis()
        Log.d("KeyManager", "✓ Friend bundle saved for $vId/$friendId")
    }

    fun loadFriendBundle(vId: String, friendId: String): ByteArray? {
        val hex = prefs?.getString("friend_key_${vId}_${friendId}", null) ?: return null
        return hex.fromHex().also {
            vehicleId = vId
            pairingId = friendId
            pairingKey = it
            timestamp = System.currentTimeMillis()
        }
    }

    fun getFriendBundleJson(vId: String, friendId: String): String? =
        prefs?.getString("friend_json_${vId}_${friendId}", null)

    fun listFriendIds(vId: String): List<String> {
        val prefix = "friend_key_${vId}_"
        return prefs?.all?.keys
            ?.filter { it.startsWith(prefix) }
            ?.map { it.removePrefix(prefix) }
            ?: emptyList()
    }

    fun deleteFriendBundle(vId: String, friendId: String) {
        prefs?.edit()
            ?.remove("friend_key_${vId}_${friendId}")
            ?.remove("friend_json_${vId}_${friendId}")
            ?.apply()
        Log.d("KeyManager", "✓ Friend bundle deleted for $vId/$friendId")
    }

    // ── In-memory session accessors ────────────────────────────────────────

    fun getPairingKey(): ByteArray? = pairingKey
    fun getVehicleId(): String? = vehicleId
    fun getPairingId(): String? = pairingId
    fun getTimestamp(): Long = timestamp
    fun isKeySet(): Boolean = pairingKey != null && vehicleId != null

    fun getPairingKeyHex(): String =
        pairingKey?.toHex() ?: "NOT_SET"

    fun clearKey() {
        vehicleId?.let { vId ->
            prefs?.edit()
                ?.remove("owner_key_$vId")
                ?.remove("owner_pid_$vId")
                ?.remove("owner_ts_$vId")
                ?.apply()
        }
        pairingKey = null
        vehicleId = null
        pairingId = null
        timestamp = 0
        Log.d("KeyManager", "✓ Pairing key cleared")
    }

    // ── Helpers ────────────────────────────────────────────────────────────

    private fun ByteArray.toHex() = joinToString("") { "%02x".format(it) }

    private fun String.fromHex() = ByteArray(length / 2) { i ->
        substring(i * 2, i * 2 + 2).toInt(16).toByte()
    }
}
