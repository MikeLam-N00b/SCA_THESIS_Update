package com.example.uwb.usb

import android.content.Context

class UsbRepository(context: Context) {

    private val helper = UsbManagerHelper(context)
    private var usb: UsbConnection? = null

    fun connect(): Boolean {
        val device = helper.findDevice() ?: return false
        val conn = helper.open(device) ?: return false

        val intf = device.getInterface(0)
        usb = UsbConnection(conn, intf)
        usb?.setup()

        return true
    }

    fun send(msg: String) {
        usb?.send(msg.toByteArray())
    }
}