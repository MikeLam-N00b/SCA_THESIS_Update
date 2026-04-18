package com.example.uwb.UI

import android.animation.ObjectAnimator
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.animation.LinearInterpolator
import android.widget.Toast
import androidx.fragment.app.Fragment
import android.content.Context
import android.hardware.usb.UsbManager
import com.example.uwb.R
import com.example.uwb.databinding.FragmentPairingLoadingBinding
import com.example.uwb.transport.TransportHolder
import com.example.uwb.transport.UsbTransport
import com.example.uwb.dataLg.KeyManager
import com.example.uwb.dataLg.PairedDeviceStore

class PairingLoadingFragment : Fragment() {

    private var _binding: FragmentPairingLoadingBinding? = null
    private val binding get() = _binding!!

    private lateinit var usbTransport: UsbTransport
    private var carAnimator: ObjectAnimator? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    private var deviceMac: String? = null
    private var deviceName: String? = null

    // Tránh xử lý kết quả nhiều lần (CONNECTED + NOT_FOUND có thể đến liên tiếp)
    private var resultHandled = false

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentPairingLoadingBinding.inflate(inflater, container, false)

        deviceMac  = arguments?.getString("DEVICE_MAC")
        deviceName = arguments?.getString("DEVICE_NAME")

        val existingTransport = TransportHolder.transport
        if (existingTransport != null) {
            usbTransport = existingTransport
            binding.tvDeviceName.text = deviceName ?: ""
            binding.btnCancel.setOnClickListener { cancelAndGoBack() }
            startCarAnimation()
            sendConnectCommand()
            startTimeout()
            listenForResponse()
        } else {
            // BluetoothFragment bị bỏ qua (có savedMac) → tự kết nối USB
            val newTransport = UsbTransport(requireContext())
            val usbManager = requireContext().getSystemService(Context.USB_SERVICE) as UsbManager
            val esp32 = usbManager.deviceList.values.find { it.vendorId == 0x303A }
            if (esp32 == null) {
                Toast.makeText(requireContext(), "Chưa thấy ESP32-S3 — hãy cắm cáp USB", Toast.LENGTH_LONG).show()
                parentFragmentManager.popBackStack()
                return binding.root
            }
            usbTransport = newTransport
            binding.tvDeviceName.text = deviceName ?: ""
            binding.btnCancel.setOnClickListener { cancelAndGoBack() }
            startCarAnimation()
            updateStatus("Đang kết nối USB...")
            newTransport.openDevice(esp32) {
                TransportHolder.transport = newTransport
                usbTransport = newTransport
                sendConnectCommand()
                startTimeout()
                listenForResponse()
            }
        }

        return binding.root
    }

    // ── Gửi key + lệnh scan xuống Tag ─────────────────────────────────────
    private fun sendConnectCommand() {
        // Firmware chỉ nhận SET_KEY: và DISCONNECT — không có lệnh CONNECT:
        val keyHex = KeyManager.getPairingKeyHex()
        if (keyHex == "NOT_SET") {
            Log.e("PairingLoading", "Pairing key chưa có — về VIN để pair lại")
            onPairingFailed("Chưa có pairing key — nhập VIN trước")
            return
        }
        Log.d("PairingKey", "╔══════════════════════════════════╗")
        Log.d("PairingKey", "║   SENDING KEY TO TAG (USB)       ║")
        Log.d("PairingKey", "║ SET_KEY:$keyHex")
        Log.d("PairingKey", "╚══════════════════════════════════╝")
        usbTransport.send("SET_KEY:$keyHex\n".toByteArray())
        updateStatus("Đang quét BLE...")
    }

    // ── Animation xe chạy ngang ────────────────────────────────────────────
    private fun startCarAnimation() {
        binding.roadTrack.post {
            val trackWidth = binding.roadTrack.width.toFloat()
            val carWidth   = binding.ivCar.width.toFloat()
            carAnimator = ObjectAnimator.ofFloat(
                binding.ivCar, "translationX", -carWidth, trackWidth
            ).apply {
                duration        = 1600
                repeatCount     = ObjectAnimator.INFINITE
                interpolator    = LinearInterpolator()
                start()
            }
        }
    }

    // ── Timeout 15 giây ────────────────────────────────────────────────────
    private fun startTimeout() {
        mainHandler.postDelayed({
            if (_binding != null && !resultHandled) {
                onPairingFailed("Hết thời gian chờ — thử lại")
            }
        }, 15_000L)
    }

    // ── Lắng nghe phản hồi từ Tag qua USB ─────────────────────────────────
    // USB CDC có thể gộp nhiều Serial.printf() thành 1 packet.
    // Tách theo '\n' để xử lý từng dòng riêng.
    private fun listenForResponse() {
        usbTransport.receive { data ->
            val raw = String(data)
            if (raw.isBlank()) return@receive

            val lines = raw.split('\n').map { it.trim() }.filter { it.isNotEmpty() }
            Log.d("PairingLoading", "S3: $raw")

            val act = activity ?: return@receive
            act.runOnUiThread {
                if (_binding == null || resultHandled) return@runOnUiThread
                for (line in lines) {
                    if (resultHandled) break
                    when {
                        line.startsWith("KEY_OK")                  -> updateStatus("Đang quét BLE...")
                        line.startsWith("KEY_INVALID")             -> onPairingFailed("Key không hợp lệ")
                        line.startsWith("SCANNING")                -> updateStatus("Đang quét BLE...")
                        line.startsWith("FOUND:")                  -> updateStatus("Đã tìm thấy thiết bị")
                        line.startsWith("[BLE] Connected")
                            || line.startsWith("BLE_CLIENT_CONNECTED")
                                                                   -> updateStatus("Đang xác thực...")
                        line.startsWith("CONNECTED:")
                            || line.startsWith("[BLE notify] AUTH_OK")
                            || line.startsWith("KEY_FORWARDED_TO_ANCHOR:") -> onPairingSuccess()
                        // Firmware đang chạy UWB rồi → vào thẳng UwbFragment
                        // Chỉ match khi UWB đang đo thực sự (có "avg="), KHÔNG match
                        // "[uwbTask] started" hoặc "[uwbTask] UWB disabled" lúc khởi động
                        (line.contains("[uwbTask]") && line.contains("avg="))
                            || line.startsWith("DISTANCE:")
                            || line.startsWith("UNLOCK:")          -> onPairingSuccess()
                        line.startsWith("NOT_FOUND:")              -> onPairingFailed("Không tìm thấy thiết bị")
                        line == "CONNECT_FAILED"                   -> onPairingFailed("Kết nối BLE thất bại")
                    }
                }
            }
        }
    }

    // ── Thành công ─────────────────────────────────────────────────────────
    private fun onPairingSuccess() {
        resultHandled = true
        mainHandler.removeCallbacksAndMessages(null)
        carAnimator?.cancel()

        // Lưu VIN→MAC để lần sau bỏ qua BLE scan
        val vin = KeyManager.getVehicleId()
        if (vin != null && deviceMac != null) {
            PairedDeviceStore.savePairing(vin, deviceMac!!, deviceName ?: "Unknown")
        }

        updateStatus("Kết nối thành công!")

        // Hiển thị thành công 800ms rồi chuyển sang UWB
        mainHandler.postDelayed({
            if (_binding == null) return@postDelayed

            val uwbFragment = UwbFragment().apply {
                arguments = Bundle().also {
                    it.putString("DEVICE_MAC",  deviceMac)
                    it.putString("DEVICE_NAME", deviceName)
                }
            }

            // Pop màn loading ra khỏi back stack, thay bằng UWB
            parentFragmentManager.popBackStack()
            parentFragmentManager.beginTransaction()
                .replace(R.id.fragment_container, uwbFragment)
                .addToBackStack(null)
                .commit()
        }, 800L)
    }

    // ── Thất bại ───────────────────────────────────────────────────────────
    private fun onPairingFailed(reason: String) {
        resultHandled = true
        mainHandler.removeCallbacksAndMessages(null)
        carAnimator?.cancel()

        updateStatus("Kết nối thất bại")
        Toast.makeText(requireContext(), reason, Toast.LENGTH_LONG).show()

        // Quay về Bluetooth sau 1.2s
        mainHandler.postDelayed({
            if (_binding != null) parentFragmentManager.popBackStack()
        }, 1200L)
    }

    // ── Hủy thủ công ───────────────────────────────────────────────────────
    private fun cancelAndGoBack() {
        resultHandled = true
        mainHandler.removeCallbacksAndMessages(null)
        carAnimator?.cancel()
        parentFragmentManager.popBackStack()
    }

    private fun updateStatus(status: String) {
        _binding?.tvStatus?.text = status
    }

    override fun onDestroyView() {
        super.onDestroyView()
        carAnimator?.cancel()
        mainHandler.removeCallbacksAndMessages(null)
        _binding = null
    }
}
