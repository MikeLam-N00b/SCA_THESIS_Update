package com.example.uwb.model

data class FriendListItem(
    val friend_id: String,
    val friend_name: String,
    val expires_at: String,
    val is_revoked: Boolean,
    val created_at: String,
    val status: String  // "active" | "expired" | "revoked"
)

data class FriendListResponse(
    val vehicle_id: String,
    val total: Int,
    val friend_keys: List<FriendListItem>
)
