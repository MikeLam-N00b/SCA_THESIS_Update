package com.example.uwb.crypto

import android.util.Base64
import android.util.Log
import com.example.uwb.model.FriendKeyBundle
import java.security.KeyFactory
import java.security.Signature
import java.security.spec.X509EncodedKeySpec

object EcdsaVerifier {

    // Message format must match server _friend_bundle_message():
    // "v{version}|{friend_id}|{vehicle_id}|{friend_key_hex}|{issued_at}|{expires_at}|{permissions}"
    fun verifyFriendBundle(bundle: FriendKeyBundle, serverPubKeyDerB64: String): Boolean {
        return try {
            val keyBytes = Base64.decode(serverPubKeyDerB64, Base64.DEFAULT)
            val publicKey = KeyFactory.getInstance("EC")
                .generatePublic(X509EncodedKeySpec(keyBytes))

            val sig = Signature.getInstance("SHA256withECDSA")
            sig.initVerify(publicKey)

            val message = "v${bundle.bundle_version}|${bundle.friend_id}|${bundle.vehicle_id}" +
                "|${bundle.friend_key_hex}|${bundle.issued_at}|${bundle.expires_at}|${bundle.permissions}"
            sig.update(message.toByteArray(Charsets.UTF_8))

            val sigBytes = Base64.decode(bundle.issuer_sig_b64, Base64.DEFAULT)
            sig.verify(sigBytes)
        } catch (e: Exception) {
            Log.w("EcdsaVerifier", "Verify failed: ${e.javaClass.simpleName}: ${e.message}")
            false
        }
    }
}
