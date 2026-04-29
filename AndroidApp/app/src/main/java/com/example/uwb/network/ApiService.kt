package com.example.uwb.network

import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.Header
import retrofit2.http.POST
import retrofit2.http.Path
import com.example.uwb.model.FriendKeyBundle
import com.example.uwb.model.FriendListResponse
import com.example.uwb.model.FriendShareRequest
import com.example.uwb.model.FriendShareResponse
import com.example.uwb.model.PairingRequest
import com.example.uwb.model.PairingResponse
import com.example.uwb.model.ServerPublicKeyResponse

interface ApiService {

    @POST("/owner-pairing")
    suspend fun ownerPairing(
        @Body req: PairingRequest
    ): PairingResponse

    @POST("/friend-sharing/create")
    suspend fun createFriendShare(
        @Header("X-Owner-Key") ownerApiKey: String,
        @Body req: FriendShareRequest
    ): FriendShareResponse

    @GET("/friend-sharing/claim/{token}")
    suspend fun claimFriendShare(
        @Path("token") token: String
    ): FriendKeyBundle

    @DELETE("/friend-sharing/{friend_id}")
    suspend fun revokeFriendShare(
        @Header("X-Owner-Key") ownerApiKey: String,
        @Path("friend_id") friendId: String
    )

    @GET("/friend-sharing/list/{vehicle_id}")
    suspend fun listFriendShares(
        @Header("X-Owner-Key") ownerApiKey: String,
        @Path("vehicle_id") vehicleId: String
    ): FriendListResponse

    @GET("/server-public-key")
    suspend fun getServerPublicKey(): ServerPublicKeyResponse
}
