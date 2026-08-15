package com.example.uwb.model

data class FriendShareRequest(
    val vehicle_id: String,
    val friend_name: String = "Friend",
    val ttl_hours: Int = 24
)
