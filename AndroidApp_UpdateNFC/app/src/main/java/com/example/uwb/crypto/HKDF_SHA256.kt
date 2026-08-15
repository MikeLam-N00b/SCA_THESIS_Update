package com.example.uwb.crypto

import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

object HKDF_SHA256 {

    /**
     * HKDF-SHA256 theo RFC 5869.
     * salt = 32 zero bytes (tương đương salt=None trong Python cryptography lib)
     * info = "owner-pairing-kek"
     * length = 16 bytes
     */
    fun hkdf(sharedSecret: ByteArray): ByteArray {
        val salt = ByteArray(32) // 32 zero bytes = RFC 5869 default khi salt=None
        val info = "owner-pairing-kek".toByteArray()
        val length = 16

        // Extract: PRK = HMAC-SHA256(salt, IKM)
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(salt, "HmacSHA256"))
        val prk = mac.doFinal(sharedSecret)

        // Expand: T(1) = HMAC-SHA256(PRK, info || 0x01)
        val result = ByteArray(length)
        var t = ByteArray(0)
        var offset = 0
        var counter = 1

        while (offset < length) {
            mac.init(SecretKeySpec(prk, "HmacSHA256"))
            mac.update(t)
            mac.update(info)
            mac.update(counter.toByte())
            t = mac.doFinal()

            val toCopy = minOf(t.size, length - offset)
            System.arraycopy(t, 0, result, offset, toCopy)
            offset += toCopy
            counter++
        }

        return result
    }
}
