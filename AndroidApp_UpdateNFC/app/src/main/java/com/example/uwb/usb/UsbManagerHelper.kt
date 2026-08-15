package com.example.uwb.usb

import android.content.Context
import android.hardware.usb.*

class UsbManagerHelper(context: Context) {

    private val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager

    fun findDevice(): UsbDevice? {
        return usbManager.deviceList.values.find {
            it.vendorId == UsbConstants.ESP32_VENDOR_ID
        }
    }

    fun open(device: UsbDevice): UsbDeviceConnection? {
        return usbManager.openDevice(device)
    }
}