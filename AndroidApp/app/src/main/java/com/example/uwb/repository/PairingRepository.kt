package com.example.uwb.repository

import android.util.Log
import com.example.uwb.crypto.AesGcmUtil
import com.example.uwb.crypto.GeneratePrivateKeyEcc
import com.example.uwb.crypto.HKDF_SHA256
import com.example.uwb.model.PairingRequest
import com.example.uwb.network.ApiClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.security.KeyFactory
import java.security.spec.X509EncodedKeySpec
import java.util.Base64
import javax.crypto.KeyAgreement

data class PairingResult(
    val pairingId: String,
    val pairingKey: ByteArray
)

class PairingRepository {

    suspend fun pairVehicle(vin: String): PairingResult = withContext(Dispatchers.IO) {

        // 1. Tạo EC key pair (SECP256R1) và lấy public key dạng DER → Base64
        GeneratePrivateKeyEcc.reset()
        val pubBytes = GeneratePrivateKeyEcc.getOrCreateKeyPair()
        val pubB64 = Base64.getEncoder().encodeToString(pubBytes)
        Log.d("PairingCrypto", "client_pub_key_der: ${pubBytes.toHex()}")

        // 2. Gửi VIN + public key lên server → POST /owner-pairing
        val resp = ApiClient.api.ownerPairing(PairingRequest(vin, pubB64))
        Log.d("PairingCrypto", "server_pub_key_b64 len: ${resp.server_public_key_b64.length}")
        Log.d("PairingCrypto", "nonce_b64: ${resp.nonce_b64}")
        Log.d("PairingCrypto", "cipher_b64 len: ${resp.encrypted_pairing_key_b64.length}")

        // 3. Parse server public key (DER format)
        val serverPubBytes = Base64.getDecoder().decode(resp.server_public_key_b64)
        Log.d("PairingCrypto", "server_pub_key_der: ${serverPubBytes.toHex()}")
        val serverPublicKey = KeyFactory.getInstance("EC")
            .generatePublic(X509EncodedKeySpec(serverPubBytes))

        // 4. ECDH: tính shared secret
        val keyAgreement = KeyAgreement.getInstance("ECDH")
        keyAgreement.init(GeneratePrivateKeyEcc.getPrivateKey())
        keyAgreement.doPhase(serverPublicKey, true)
        val sharedSecret = keyAgreement.generateSecret()
        Log.d("PairingCrypto", "shared_secret (${sharedSecret.size}B): ${sharedSecret.toHex()}")

        // 5. HKDF-SHA256 → KEK 16 bytes
        //    salt = 32 zero bytes (khớp với server: salt=None theo RFC 5869)
        //    info = "owner-pairing-kek"
        val kek = HKDF_SHA256.hkdf(sharedSecret)
        Log.d("PairingCrypto", "kek (${kek.size}B): ${kek.toHex()}")

        // 6. AES-GCM decrypt → pairing key 16 bytes
        val nonce = Base64.getDecoder().decode(resp.nonce_b64)
        val cipherText = Base64.getDecoder().decode(resp.encrypted_pairing_key_b64)
        Log.d("PairingCrypto", "nonce: ${nonce.toHex()}")
        Log.d("PairingCrypto", "cipherText (${cipherText.size}B): ${cipherText.toHex()}")
        val pairingKey = AesGcmUtil.decrypt(kek, nonce, cipherText)
        Log.d("PairingCrypto", "pairingKey: ${pairingKey.toHex()}")

        PairingResult(
            pairingId = resp.pairing_id,
            pairingKey = pairingKey
        )
    }

    private fun ByteArray.toHex(): String =
        joinToString("") { "%02x".format(it) }
}
