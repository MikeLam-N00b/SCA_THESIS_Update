package com.example.uwb

import android.nfc.NfcAdapter
import android.nfc.Tag
import android.nfc.tech.Ndef
import android.os.Bundle
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import com.example.uwb.UI.EnterVinFragment
import com.example.uwb.UI.WelcomeFragment
import com.example.uwb.databinding.ActivityMainBinding
import com.example.uwb.dataLg.PairedDeviceStore
import com.example.uwb.dataLg.KeyManager
import java.nio.charset.Charset

class MainActivity : AppCompatActivity(), NfcAdapter.ReaderCallback {

    private lateinit var binding: ActivityMainBinding
    private var nfcAdapter: NfcAdapter? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        PairedDeviceStore.init(this)
        KeyManager.init(this)

        nfcAdapter = NfcAdapter.getDefaultAdapter(this)
        if (nfcAdapter == null) {
            Log.w("SCA_NFC", "NFC is not available on this device.")
        } else {
            Log.d("SCA_NFC", "NFC reader initialized")
        }

        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction()
                .replace(R.id.fragment_container, WelcomeFragment())
                .commit()
        }
    }

    override fun onResume() {
        super.onResume()
        if (nfcAdapter?.isEnabled == false) {
            android.widget.Toast.makeText(this, "Please enable NFC in settings", android.widget.Toast.LENGTH_LONG).show()
        }
        nfcAdapter?.enableReaderMode(
            this,
            this,
            NfcAdapter.FLAG_READER_NFC_A or NfcAdapter.FLAG_READER_NFC_B or
                    NfcAdapter.FLAG_READER_NFC_F or NfcAdapter.FLAG_READER_NFC_V,
            null
        )
    }

    override fun onPause() {
        super.onPause()
        nfcAdapter?.disableReaderMode(this)
    }

    override fun onTagDiscovered(tag: Tag?) {
        Log.d("SCA_NFC", "NFC tag detected")
        runOnUiThread {
            android.widget.Toast.makeText(this, "NFC Tag Detected", android.widget.Toast.LENGTH_SHORT).show()
        }
        val ndef = Ndef.get(tag)
        if (ndef == null) {
            Log.e("SCA_NFC", "Tag is not NDEF formatted")
            runOnUiThread {
                android.widget.Toast.makeText(this, "Tag is not NDEF formatted", android.widget.Toast.LENGTH_SHORT).show()
            }
            return
        }

        try {
            ndef.connect()
            val ndefMessage = ndef.ndefMessage
            if (ndefMessage == null) {
                Log.e("SCA_NFC", "NDEF message is null")
                return
            }
            Log.d("SCA_NFC", "NDEF message received")

            var vin: String? = null
            var key: String? = null

            for (record in ndefMessage.records) {
                val payload = record.payload
                // Skip the first byte which contains the language code length
                val textEncoding = if ((payload[0].toInt() and 128) == 0) Charset.forName("UTF-8") else Charset.forName("UTF-16")
                val languageCodeLength = payload[0].toInt() and 63
                val text = String(payload, languageCodeLength + 1, payload.size - languageCodeLength - 1, textEncoding)

                // Parse VIN and KEY from text using Regex for better robustness
                val vinRegex = Regex("VIN\\s*[:\\s]\\s*([A-Z0-9]+)", RegexOption.IGNORE_CASE)
                val keyRegex = Regex("KEY\\s*[:\\s]\\s*([a-fA-F0-9]+)", RegexOption.IGNORE_CASE)

                vinRegex.find(text)?.let {
                    vin = it.groupValues[1]
                }
                keyRegex.find(text)?.let {
                    key = it.groupValues[1]
                }
            }

            val finalVin = vin
            val finalKey = key

            if (finalVin != null && finalKey != null) {
                Log.d("SCA_NFC", "VIN and KEY parsed successfully")
                try {
                    val keyBytes = hexToByteArray(finalKey)
                    KeyManager.savePairingKey(finalVin, "NFC_TAG", keyBytes)
                    Log.d("SCA_NFC", "VIN and KEY stored locally")
                    
                    runOnUiThread {
                        android.widget.Toast.makeText(this, "NFC Scanned: VIN & KEY ready", android.widget.Toast.LENGTH_SHORT).show()
                    }
                    handleVinParsed(finalVin)
                } catch (e: Exception) {
                    Log.e("SCA_NFC", "Error saving NFC credentials", e)
                }
            } else {
                if (finalVin == null) Log.e("SCA_NFC", "VIN not found in NFC payload")
                if (finalKey == null) Log.e("SCA_NFC", "KEY not found in NFC payload")
                
                runOnUiThread {
                    android.widget.Toast.makeText(this, "NFC Scan Incomplete", android.widget.Toast.LENGTH_SHORT).show()
                }
                
                finalVin?.let { handleVinParsed(it) }
            }
        } catch (e: Exception) {
            Log.e("SCA_NFC", "Error reading NDEF tag", e)
        } finally {
            try {
                ndef.close()
            } catch (e: Exception) {}
        }
    }

    private fun handleVinParsed(vin: String) {
        val currentFragment = supportFragmentManager.findFragmentById(R.id.fragment_container)
        if (currentFragment is EnterVinFragment) {
            currentFragment.onVinReceived(vin)
        } else {
            Log.d("SCA_NFC", "EnterVinFragment is not visible, ignoring VIN")
        }
    }

    private fun hexToByteArray(s: String): ByteArray {
        val len = s.length
        val data = ByteArray(len / 2)
        var i = 0
        while (i < len) {
            data[i / 2] = ((Character.digit(s[i], 16) shl 4) + Character.digit(s[i + 1], 16)).toByte()
            i += 2
        }
        return data
    }
}
