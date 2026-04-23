package com.example.uwb.model

data class FriendKeyBundle(
    val friend_id: String,
    val vehicle_id: String,
    val friend_key_hex: String,
    val friend_name: String,
    val expires_at: String,
    val owner_sig_b64: String
)
