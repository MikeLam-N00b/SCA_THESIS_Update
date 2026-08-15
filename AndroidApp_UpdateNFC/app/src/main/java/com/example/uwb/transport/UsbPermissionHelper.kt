package com.example.uwb.transport

import android.app.PendingIntent
import android.content.*
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.widget.Toast
import androidx.core.content.ContextCompat

class UsbPermissionHelper(private val context: Context) {

    companion object {
        const val ACTION_USB_PERMISSION = "com.example.uwb.USB_PERMISSION"
    }

    private var usbReceiver: BroadcastReceiver? = null

    fun requestPermission(device: UsbDevice, onGranted: () -> Unit) {
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager

        // Hủy receiver cũ nếu có
        unregisterReceiver()

        usbReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                if (granted) {
                    Toast.makeText(context, "USB được cấp quyền", Toast.LENGTH_SHORT).show()
                    onGranted()
                } else {
                    Toast.makeText(context, "USB không được cấp quyền", Toast.LENGTH_SHORT).show()
                }
                unregisterReceiver()
            }
        }

        val filter = IntentFilter(ACTION_USB_PERMISSION)
        ContextCompat.registerReceiver(
            context,
            usbReceiver,
            filter,
            ContextCompat.RECEIVER_NOT_EXPORTED
        )

        val permissionIntent = PendingIntent.getBroadcast(
            context,
            0,
            Intent(ACTION_USB_PERMISSION),
            PendingIntent.FLAG_IMMUTABLE
        )

        usbManager.requestPermission(device, permissionIntent)
    }

    fun unregisterReceiver() {
        usbReceiver?.let {
            try {
                context.unregisterReceiver(it)
            } catch (_: Exception) {}
            usbReceiver = null
        }
    }
}