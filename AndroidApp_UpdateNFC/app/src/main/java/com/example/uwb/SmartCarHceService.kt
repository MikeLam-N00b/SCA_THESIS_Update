package com.example.uwb

import android.nfc.cardemulation.HostApduService
import android.os.Bundle
import android.util.Log
import com.example.uwb.dataLg.KeyManager
import java.nio.charset.Charset

class SmartCarHceService : HostApduService() {

    companion object {
        private const val TAG = "SmartCarHceService"
        private val SELECT_AID_COMMAND = byteArrayOf(
            0x00.toByte(), // CLA
            0xA4.toByte(), // INS
            0x04.toByte(), // P1
            0x00.toByte(), // P2
            0x07.toByte(), // Lc (AID length)
            0xF0.toByte(), 0x01.toByte(), 0x02.toByte(), 0x03.toByte(), 0x04.toByte(), 0x05.toByte(), 0x06.toByte() // AID
        )
        private val SUCCESS_SW = byteArrayOf(0x90.toByte(), 0x00.toByte())
        private val FAILURE_SW = byteArrayOf(0x6A.toByte(), 0x82.toByte()) // File not found
    }

    override fun processCommandApdu(commandApdu: ByteArray?, extras: Bundle?): ByteArray? {
        if (commandApdu == null) return FAILURE_SW

        Log.d(TAG, "Command received: ${commandApdu.toHex()}")

        if (commandApdu.contentEquals(SELECT_AID_COMMAND)) {
            Log.d(TAG, "AID selected successfully")
            
            // Initialize KeyManager if not already initialized
            KeyManager.init(this)

            if (!KeyManager.isHceEnabled()) {
                Log.w(TAG, "HCE is currently disabled in app settings")
                return FAILURE_SW
            }

            val vin = KeyManager.getVehicleId()
            val key = KeyManager.getPairingKeyHex()

            if (vin != null && key != "NOT_SET") {
                val responseString = "VIN=$vin;KEY=$key"
                val responseBytes = responseString.toByteArray(Charset.forName("UTF-8"))
                
                Log.d(TAG, "Returning stored credentials (VIN: $vin)")
                return responseBytes + SUCCESS_SW
            } else {
                Log.e(TAG, "No valid VIN or KEY stored")
                return FAILURE_SW
            }
        }

        return FAILURE_SW
    }

    override fun onDeactivated(reason: Int) {
        Log.d(TAG, "HCE Service deactivated: $reason")
    }

    private fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it) }
}
