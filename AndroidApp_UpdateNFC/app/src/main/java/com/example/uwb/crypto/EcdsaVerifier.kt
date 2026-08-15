package com.example.uwb.crypto

import android.util.Base64
import android.util.Log
import com.example.uwb.model.FriendKeyBundle
import java.io.ByteArrayOutputStream
import java.security.KeyFactory
import java.security.PublicKey
import java.security.Signature
import java.security.spec.X509EncodedKeySpec

object EcdsaVerifier {

    fun verifyBundleBinary(bundle: ByteArray, serverPubKeyDerB64: String): Boolean {
        require(bundle.size == FriendKeyBundle.SIZE)
        return try {
            val keyBytes = Base64.decode(serverPubKeyDerB64, Base64.DEFAULT)
            val publicKey: PublicKey = KeyFactory.getInstance("EC")
                .generatePublic(X509EncodedKeySpec(keyBytes))
            verifyBundleBinary(bundle, publicKey)
        } catch (e: Exception) {
            Log.w("EcdsaVerifier", "Verify failed: ${e.javaClass.simpleName}: ${e.message}")
            false
        }
    }

    fun verifyBundleBinary(bundle: ByteArray, serverPubKey: PublicKey): Boolean {
        require(bundle.size == FriendKeyBundle.SIZE)
        return try {
            val signedPart = bundle.copyOfRange(0, FriendKeyBundle.SIGNED_PART_SIZE)
            val sigR = bundle.copyOfRange(FriendKeyBundle.OFF_SIG, FriendKeyBundle.OFF_SIG + 32)
            val sigS = bundle.copyOfRange(FriendKeyBundle.OFF_SIG + 32, FriendKeyBundle.OFF_SIG + FriendKeyBundle.SIG_SIZE)
            val sigDer = encodeRawToDer(sigR, sigS)

            val verifier = Signature.getInstance("SHA256withECDSA")
            verifier.initVerify(serverPubKey)
            verifier.update(signedPart)
            verifier.verify(sigDer)
        } catch (e: Exception) {
            Log.w("EcdsaVerifier", "Verify failed: ${e.javaClass.simpleName}: ${e.message}")
            false
        }
    }

    /**
     * Encode raw r||s (each 32 bytes, big-endian) to ASN.1 DER format:
     * SEQUENCE { INTEGER r, INTEGER s }
     *
     * Java's Signature.verify() requires DER-encoded ECDSA signatures.
     * Firmware uses mbedtls_ecdsa_verify which accepts raw r/s directly.
     */
    fun encodeRawToDer(r: ByteArray, s: ByteArray): ByteArray {
        val rPadded = padInteger(r)
        val sPadded = padInteger(s)
        val seqLen = 2 + rPadded.size + 2 + sPadded.size
        val out = ByteArrayOutputStream(seqLen + 2)
        out.write(0x30)  // SEQUENCE
        out.write(seqLen)
        out.write(0x02)  // INTEGER
        out.write(rPadded.size)
        out.write(rPadded)
        out.write(0x02)  // INTEGER
        out.write(sPadded.size)
        out.write(sPadded)
        return out.toByteArray()
    }

    // Server đã đảm bảo r, s là 32 byte big-endian. ASN.1 INTEGER chỉ cần
    // prepend 0x00 nếu MSB >= 0x80 (tránh hiểu là số âm). KHÔNG strip leading zeros.
    private fun padInteger(raw: ByteArray): ByteArray {
        require(raw.size == 32) { "ECDSA P-256 component must be 32 bytes" }
        return if (raw[0].toInt() and 0x80 != 0) byteArrayOf(0) + raw else raw
    }
}
