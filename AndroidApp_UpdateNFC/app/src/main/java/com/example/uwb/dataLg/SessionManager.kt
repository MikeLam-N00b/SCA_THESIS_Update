package com.example.uwb.dataLg

import android.content.Context
import android.content.SharedPreferences
import android.provider.Settings

class SessionManager(context: Context) {

    private val pref =
        context.getSharedPreferences("UserSession", Context.MODE_PRIVATE)

    private val editor = pref.edit()

    fun saveLogin(email: String, password: String) {
        editor.putString("email", email)
        editor.putString("password", password)
        editor.putBoolean("isLogin", true)
        editor.apply()
    }

    fun isLoggedIn(): Boolean {
        return pref.getBoolean("isLogin", false)
    }

    fun getEmail(): String? {
        return pref.getString("email", null)
    }

    fun getPassword(): String? {
        return pref.getString("password", null)
    }

    fun clearLogin() {
        editor.clear()
        editor.apply()
    }
}


