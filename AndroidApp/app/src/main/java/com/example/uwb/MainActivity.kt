package com.example.uwb

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import com.example.uwb.UI.WelcomeFragment
import com.example.uwb.databinding.ActivityMainBinding
import com.example.uwb.dataLg.PairedDeviceStore
import com.example.uwb.dataLg.KeyManager

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        PairedDeviceStore.init(this)
        KeyManager.init(this)

        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction()
                .replace(R.id.fragment_container, WelcomeFragment())
                .commit()
        }
    }
}
