package com.example.uwb.model

data class FriendShareResponse(
    val friend_id: String,
    val claim_token: String,
    val claim_url: String,
    val expires_at: String
)
