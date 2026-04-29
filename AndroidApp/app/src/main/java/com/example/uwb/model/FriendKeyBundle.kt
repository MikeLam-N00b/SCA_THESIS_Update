package com.example.uwb.model

data class FriendKeyBundle(
    val bundle_version: Int,
    val friend_id: String,
    val vehicle_id: String,
    val friend_key_hex: String,
    val friend_name: String,
    val permissions: Int,
    val issued_at: String,
    val expires_at: String,
    val issuer_sig_b64: String
)
