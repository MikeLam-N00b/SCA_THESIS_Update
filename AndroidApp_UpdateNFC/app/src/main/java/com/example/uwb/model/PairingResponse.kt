package com.example.uwb.model

data class PairingResponse(
    val pairing_id: String,
    val server_public_key_b64: String,
    val encrypted_pairing_key_b64: String,
    val nonce_b64: String,
    val owner_api_key: String? = null,
    val server_signing_public_key_b64: String? = null,
    val bundle_version: Int = 1,
    val max_key_ttl_hours: Int = 168,
)
