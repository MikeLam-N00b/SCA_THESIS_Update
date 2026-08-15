package com.example.uwb.transport

interface Transport {
    fun connect()
    fun send(data: ByteArray)
    fun receive(callback: (ByteArray) -> Unit)
    fun disconnect()
}