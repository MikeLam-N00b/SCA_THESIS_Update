package com.example.uwb.network

import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Path
import com.example.uwb.model.PairingRequest
import com.example.uwb.model.PairingResponse
import com.example.uwb.model.FriendShareRequest
import com.example.uwb.model.FriendShareResponse
import com.example.uwb.model.FriendKeyBundle

interface ApiService {
    @POST("/owner-pairing")
    suspend fun ownerPairing(
        @Body req: PairingRequest
    ): PairingResponse

    @POST("/friend-sharing/create")
    suspend fun createFriendShare(
        @Body req: FriendShareRequest
    ): FriendShareResponse

    @GET("/friend-sharing/claim/{token}")
    suspend fun claimFriendShare(
        @Path("token") token: String
    ): FriendKeyBundle
}
