package com.example.uwb.usb
import android.hardware.usb.UsbConstants
import android.hardware.usb.*

class UsbConnection(
    private val connection: UsbDeviceConnection,
    private val usbInterface: UsbInterface
) {

    private var epIn: UsbEndpoint? = null
    private var epOut: UsbEndpoint? = null

    fun setup() {
        connection.claimInterface(usbInterface, true)

        for (i in 0 until usbInterface.endpointCount) {
            val ep = usbInterface.getEndpoint(i)

            if (ep.direction == UsbConstants.USB_DIR_IN) {
                epIn = ep
            } else {
                epOut = ep
            }
        }
    }

    fun send(data: ByteArray) {
        epOut?.let {
            connection.bulkTransfer(it, data, data.size, 1000)
        }
    }

    fun receive(): String? {
        val buffer = ByteArray(64)
        val len = epIn?.let {
            connection.bulkTransfer(it, buffer, buffer.size, 1000)
        } ?: -1

        return if (len > 0) String(buffer, 0, len) else null
    }
}