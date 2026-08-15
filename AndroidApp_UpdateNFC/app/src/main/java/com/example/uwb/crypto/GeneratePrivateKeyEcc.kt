package com.example.uwb.crypto

import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.PrivateKey
import java.security.spec.ECGenParameterSpec

object GeneratePrivateKeyEcc {

    private var keyPair: KeyPair? = null

    fun getOrCreateKeyPair(): ByteArray {
        if (keyPair == null) {
            val kpg = KeyPairGenerator.getInstance("EC")
            kpg.initialize(ECGenParameterSpec("secp256r1"))
            keyPair = kpg.generateKeyPair()
        }
        return keyPair!!.public.encoded
    }

    fun getPrivateKey(): PrivateKey = keyPair!!.private

    fun reset() { keyPair = null }
}
