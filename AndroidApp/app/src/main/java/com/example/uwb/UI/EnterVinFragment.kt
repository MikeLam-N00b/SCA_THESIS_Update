package com.example.uwb.UI

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.uwb.Bluetooth.BluetoothFragment
import com.example.uwb.UI.PairingLoadingFragment
import com.example.uwb.VinValid.VinValidator
import com.example.uwb.databinding.FragmentEnterVinBinding
import com.example.uwb.repository.PairingRepository
import com.example.uwb.dataLg.KeyManager
import com.example.uwb.dataLg.PairedDeviceStore
import kotlinx.coroutines.launch

class EnterVinFragment : Fragment() {

    private var _binding: FragmentEnterVinBinding? = null
    private val binding get() = _binding!!

    private val repo = PairingRepository()

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {

        _binding = FragmentEnterVinBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        binding.btnConfirm.setOnClickListener {

            val vin = binding.edtVin.text.toString().trim()

            if (!VinValidator.isValid(vin)) {
                binding.edtVin.error = "VIN không hợp lệ"
                return@setOnClickListener
            }

            sendVinToServer(vin)
        }
    }

    private fun sendVinToServer(vin: String) {

        // Nếu key cho VIN này đã có trên disk → dùng lại, không gọi /owner-pairing
        // (tránh server sinh key mới → Anchor NVS lỗi thời → AUTH_FAIL)
        if (KeyManager.loadPairingKey(vin)) {
            val keyHex = KeyManager.getPairingKeyHex()
            android.util.Log.d("PairingKey", "╔══════════════════════════════════╗")
            android.util.Log.d("PairingKey", "║   PAIRING KEY (FROM DISK CACHE)  ║")
            android.util.Log.d("PairingKey", "║ VIN       : $vin")
            android.util.Log.d("PairingKey", "║ Key (hex) : $keyHex")
            android.util.Log.d("PairingKey", "╚══════════════════════════════════╝")
            navigateAfterPairing(vin)
            return
        }

        lifecycleScope.launch {

            try {

                val result = repo.pairVehicle(vin)

                val keyHex = result.pairingKey.joinToString("") { "%02x".format(it) }
                android.util.Log.d("PairingKey", "╔══════════════════════════════════╗")
                android.util.Log.d("PairingKey", "║   PAIRING KEY FROM SERVER        ║")
                android.util.Log.d("PairingKey", "╠══════════════════════════════════╣")
                android.util.Log.d("PairingKey", "║ VIN       : $vin")
                android.util.Log.d("PairingKey", "║ PairingID : ${result.pairingId}")
                android.util.Log.d("PairingKey", "║ Key (hex) : $keyHex")
                android.util.Log.d("PairingKey", "╚══════════════════════════════════╝")

                KeyManager.savePairingKey(
                    vId = vin,
                    pId = result.pairingId,
                    key = result.pairingKey
                )

                navigateAfterPairing(vin)

            } catch (e: javax.crypto.AEADBadTagException) {
                android.util.Log.e("EnterVin", "AES-GCM decrypt failed — key mismatch", e)
                binding.edtVin.error = null
                Toast.makeText(requireContext(),
                    "Lỗi giải mã server: kiểm tra kết nối và thử lại",
                    Toast.LENGTH_LONG).show()
            } catch (e: Exception) {
                android.util.Log.e("EnterVin", "pairVehicle failed", e)
                binding.edtVin.error = null
                Toast.makeText(requireContext(),
                    "Lỗi: ${e.message?.take(80)}",
                    Toast.LENGTH_LONG).show()
            }
        }
    }

    /**
     * Nếu VIN này đã từng paired thành công (có MAC lưu sẵn) → bỏ qua BLE scan,
     * vào thẳng màn hình kết nối.
     * Nếu VIN mới → vào BluetoothFragment để scan.
     */
    private fun navigateAfterPairing(vin: String) {
        val savedMac = PairedDeviceStore.getMacForVin(vin)
        val fm = requireActivity().supportFragmentManager

        if (savedMac != null) {
            val savedName = PairedDeviceStore.getNameForVin(vin)
            val pairingFragment = PairingLoadingFragment().apply {
                arguments = Bundle().also {
                    it.putString("DEVICE_MAC", savedMac)
                    it.putString("DEVICE_NAME", savedName)
                }
            }
            fm.beginTransaction()
                .replace(com.example.uwb.R.id.fragment_container, pairingFragment)
                .addToBackStack(null)
                .commit()
        } else {
            fm.beginTransaction()
                .replace(com.example.uwb.R.id.fragment_container, BluetoothFragment())
                .commit()
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}