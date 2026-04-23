package com.example.uwb.dataLg

import android.content.Context
import com.google.gson.Gson
import com.google.gson.reflect.TypeToken

data class FriendShareEntry(
    val friendId: String,
    val friendName: String,
    val claimUrl: String,
    val expiresAt: String,
    val vehicleId: String,
    val createdAt: String
)

object FriendShareStore {

    private const val PREFS = "FriendShares"
    private const val KEY_SHARES = "shares"
    private val gson = Gson()

    fun save(context: Context, entry: FriendShareEntry) {
        val list = getAll(context).toMutableList()
        list.removeAll { it.friendId == entry.friendId }
        list.add(0, entry)
        write(context, list)
    }

    fun getAll(context: Context): List<FriendShareEntry> {
        val json = prefs(context).getString(KEY_SHARES, null) ?: return emptyList()
        val type = object : TypeToken<List<FriendShareEntry>>() {}.type
        return gson.fromJson(json, type) ?: emptyList()
    }

    fun remove(context: Context, friendId: String) {
        val list = getAll(context).filter { it.friendId != friendId }
        write(context, list)
    }

    private fun write(context: Context, list: List<FriendShareEntry>) {
        prefs(context).edit().putString(KEY_SHARES, gson.toJson(list)).apply()
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
}
