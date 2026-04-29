package com.example.uwb.dataLg

import android.content.Context

object ServerPublicKeyStore {

    private const val PREFS_NAME = "ServerKeys"
    private const val KEY_SIGNING_PUBKEY = "signing_pubkey"

    fun save(context: Context, pubKeyB64: String) {
        prefs(context).edit().putString(KEY_SIGNING_PUBKEY, pubKeyB64).apply()
    }

    fun load(context: Context): String? =
        prefs(context).getString(KEY_SIGNING_PUBKEY, null)

    fun clear(context: Context) {
        prefs(context).edit().remove(KEY_SIGNING_PUBKEY).apply()
    }

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
}
