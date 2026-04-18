package com.example.uwb.dataLg

import android.content.Context
import android.content.SharedPreferences
import android.util.Log

/**
 * KeyManager: lưu pairing key cả in-memory và SharedPreferences.
 *
 * Tại sao cần persist?
 *   - Server sinh key mới mỗi lần /owner-pairing được gọi.
 *   - Nếu app bị kill rồi mở lại, key in-memory mất → app gọi /owner-pairing lại
 *     → key mới trong DB → Anchor vẫn dùng key cũ trong NVS → AUTH_FAIL.
 *   - Persist key theo VIN để chỉ gọi /owner-pairing một lần duy nhất,
 *     sau đó tái sử dụng key đã lưu (cùng key mà Anchor đang giữ trong NVS).
 *
 * Cách dùng:
 *   1. Gọi KeyManager.init(context) trong Application.onCreate() hoặc MainActivity.
 *   2. Trước khi gọi /owner-pairing, kiểm tra loadPairingKey(vin) != null.
 */
object KeyManager {

    private var pairingKey: ByteArray? = null
    private var vehicleId: String? = null
    private var pairingId: String? = null
    private var timestamp: Long = 0

    private var prefs: SharedPreferences? = null

    fun init(context: Context) {
        prefs = context.applicationContext
            .getSharedPreferences("PairingKeys", Context.MODE_PRIVATE)
    }

    /** Lưu key vào memory VÀ SharedPreferences theo VIN. */
    fun savePairingKey(vId: String, pId: String, key: ByteArray) {
        vehicleId  = vId
        pairingId  = pId
        pairingKey = key
        timestamp  = System.currentTimeMillis()

        prefs?.edit()
            ?.putString("key_$vId",  key.joinToString("") { "%02x".format(it) })
            ?.putString("pid_$vId",  pId)
            ?.putLong(  "ts_$vId",   timestamp)
            ?.apply()

        Log.d("KeyManager", "✓ Key saved for $vId: ${key.joinToString("") { "%02x".format(it) }}")
    }

    /**
     * Tải key từ SharedPreferences cho VIN cho trước.
     * Trả về true nếu có key → không cần gọi /owner-pairing lại.
     */
    fun loadPairingKey(vId: String): Boolean {
        val hex = prefs?.getString("key_$vId", null) ?: return false
        if (hex.length != 32) return false
        val bytes = ByteArray(16) { i ->
            hex.substring(i * 2, i * 2 + 2).toInt(16).toByte()
        }
        vehicleId  = vId
        pairingId  = prefs?.getString("pid_$vId", "") ?: ""
        pairingKey = bytes
        timestamp  = prefs?.getLong("ts_$vId", 0) ?: 0
        Log.d("KeyManager", "✓ Key loaded from disk for $vId: $hex")
        return true
    }

    fun getPairingKey(): ByteArray? = pairingKey

    fun getVehicleId(): String? = vehicleId

    fun getPairingId(): String? = pairingId

    fun getTimestamp(): Long = timestamp

    fun getPairingKeyHex(): String =
        pairingKey?.joinToString("") { "%02x".format(it) } ?: "NOT_SET"

    fun clearKey() {
        vehicleId?.let { vId ->
            prefs?.edit()
                ?.remove("key_$vId")
                ?.remove("pid_$vId")
                ?.remove("ts_$vId")
                ?.apply()
        }
        pairingKey = null
        vehicleId  = null
        pairingId  = null
        timestamp  = 0
        Log.d("KeyManager", "✓ Pairing key cleared")
    }

    fun isKeySet(): Boolean = pairingKey != null && vehicleId != null
}
