package com.example.uwb.UI

import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.fragment.app.Fragment
import com.example.uwb.databinding.FragmentUwbBinding
import com.example.uwb.dataLg.KeyManager
import com.example.uwb.transport.UsbTransport
import com.example.uwb.transport.TransportHolder

class UwbFragment : Fragment() {

    private var _binding: FragmentUwbBinding? = null
    private val binding get() = _binding!!

    private var distance = 0.0
    private var isLocked = true
    private lateinit var usbTransport: UsbTransport

    private var deviceMac: String? = null
    private var deviceName: String? = null

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentUwbBinding.inflate(inflater, container, false)

        deviceMac = arguments?.getString("DEVICE_MAC")
        deviceName = arguments?.getString("DEVICE_NAME")

        // Dùng lại transport đã kết nối từ BluetoothFragment
        usbTransport = TransportHolder.transport ?: UsbTransport(requireContext())

        setupUI()
        listenForDistance()

        return binding.root
    }

    private fun setupUI() {
        binding.tvDistance.text = String.format("%.1f m", distance)
        updateLockStatus()
    }

    private fun updateLockStatus() {
        if (isLocked) {
            binding.tvLockStatus.text = "XE ĐƯỢC KHÓA"
            binding.tvLockStatus.setTextColor(Color.parseColor("#D32F2F"))
            binding.ivLockIcon.setImageResource(com.example.uwb.R.drawable.ic_lock_login)
        } else {
            binding.tvLockStatus.text = "XE ĐÃ MỞ KHÓA"
            binding.tvLockStatus.setTextColor(Color.parseColor("#4CAF50"))
            binding.ivLockIcon.setImageResource(com.example.uwb.R.drawable.ic_unlock)
        }
    }

    /**
     * Lấy key đã lưu trong KeyManager rồi gửi xuống S3
     */
    private fun fetchKeyAndSend() {
        val keyHex = KeyManager.getPairingKeyHex()
        if (keyHex == "NOT_SET") {
            Log.e("UwbFragment", "Pairing key not found")
            Toast.makeText(requireContext(), "Chưa có pairing key", Toast.LENGTH_SHORT).show()
            return
        }

        // Gửi key xuống S3
        val keyCmd = "SET_KEY:$keyHex\n"
        usbTransport.send(keyCmd.toByteArray())

        Log.d("UwbFragment", "Key sent to S3: $keyHex")
        Toast.makeText(requireContext(), "✓ Đã gửi key cho S3", Toast.LENGTH_SHORT).show()
    }

    /**
     * Lắng nghe distance từ S3
     */
    private fun listenForDistance() {
        usbTransport.receive { data ->
            val response = String(data).trim()
            Log.d("UwbFragment", "📥 S3: $response")

            when {
                response.startsWith("KEY_FORWARDED_TO_ANCHOR:") -> {
                    val forwardedKey = response.substring(24)
                    Log.d("UwbFragment", "✓ Key forwarded to Anchor: $forwardedKey")
                    requireActivity().runOnUiThread {
                        Toast.makeText(
                            requireContext(),
                            "✓ Tag đã gửi key sang Anchor",
                            Toast.LENGTH_SHORT
                        ).show()
                    }
                }
                response.startsWith("DISTANCE:") -> {
                    val distanceStr = response.substring(9)
                    try {
                        val dist = distanceStr.toDouble()
                        requireActivity().runOnUiThread {
                            distance = dist
                            binding.tvDistance.text = String.format("%.1f m", distance)

                            // Tự động unlock nếu distance < 5m
                            if (distance < 5.0 && isLocked) {
                                isLocked = false
                                updateLockStatus()
                                Toast.makeText(
                                    requireContext(),
                                    "✓ Distance < 5m → Mở khóa tự động",
                                    Toast.LENGTH_SHORT
                                ).show()
                            }
                        }
                    } catch (e: Exception) {
                        Log.e("UwbFragment", "Parse distance error: ${e.message}")
                    }
                }
            }
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        TransportHolder.transport?.disconnect()
        TransportHolder.transport = null
        _binding = null
    }
}
