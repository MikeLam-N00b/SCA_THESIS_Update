package com.example.uwb.dataLg

import android.content.Context
import android.content.SharedPreferences

/**
 * Lưu mapping VIN → (mac, name) vào SharedPreferences để tái sử dụng khi
 * người dùng nhập lại cùng mã VIN mà không cần scan BLE lại.
 */
object PairedDeviceStore {

    private lateinit var prefs: SharedPreferences

    fun init(context: Context) {
        prefs = context.applicationContext
            .getSharedPreferences("PairedDevices", Context.MODE_PRIVATE)
    }

    fun savePairing(vin: String, mac: String, name: String) {
        prefs.edit()
            .putString("mac_$vin", mac)
            .putString("name_$vin", name)
            .apply()
    }

    /** null nếu VIN này chưa từng pairing thành công */
    fun getMacForVin(vin: String): String? = prefs.getString("mac_$vin", null)

    fun getNameForVin(vin: String): String? =
        prefs.getString("name_$vin", null) ?: "Unknown"

    fun clearPairing(vin: String) {
        prefs.edit().remove("mac_$vin").remove("name_$vin").apply()
    }
}
