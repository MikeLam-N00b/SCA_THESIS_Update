package com.example.uwb.Bluetooth

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import java.io.OutputStream
import java.util.UUID

class BluetoothManager {

    private var socket: BluetoothSocket? = null
    private var outputStream: OutputStream? = null

    private val uuid: UUID =
        UUID.fromString("00001101-0000-1000-8000-00805F9B34FB") // SPP

    @androidx.annotation.RequiresPermission(android.Manifest.permission.BLUETOOTH_CONNECT)
    fun connect(device: BluetoothDevice) {
        socket = device.createRfcommSocketToServiceRecord(uuid)
        socket?.connect()
        outputStream = socket?.outputStream
    }

    fun send(data: String) {
        outputStream?.write(data.toByteArray())
    }

    fun close() {
        socket?.close()
    }
}

