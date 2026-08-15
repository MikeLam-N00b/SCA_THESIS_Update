package com.example.uwb.model

data class PairingRequest(
    val vehicle_id: String,
    val vehicle_public_key_b64: String
)
