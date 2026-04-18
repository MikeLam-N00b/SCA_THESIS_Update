package com.example.uwb.Bluetooth

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.*
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.*
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.annotation.RequiresApi
import androidx.core.app.ActivityCompat
import androidx.fragment.app.Fragment
import com.example.uwb.databinding.FragmentBluetoothBinding
import com.example.uwb.transport.UsbTransport
import com.example.uwb.transport.TransportHolder
import com.example.uwb.UI.PairingLoadingFragment
import com.example.uwb.dataLg.KeyManager

class BluetoothFragment : Fragment() {

    private var _binding: FragmentBluetoothBinding? = null
    private val binding get() = _binding!!

    private val bluetoothAdapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()
    private val deviceList = mutableListOf<BluetoothDevice>()
    private val deviceNameList = mutableListOf<String>()
    private lateinit var listAdapter: ArrayAdapter<String>

    private lateinit var usbTransport: UsbTransport

    private val ESP32_VENDOR_ID = 0x303A
    private val SCAN_PERIOD_MS = 10000L

    /**
     * Làm sạch tên thiết bị BLE.
     *
     * Hai loại tên rác thường gặp:
     *   1. Chứa ký tự đặc biệt / không in được  → "SZZvX&UF-C{S"
     *   2. Chứa data được encode thành alphanumeric → "5AEA000009KkpcSnZyVF-CL"
     *      (dấu hiệu: đoạn chữ ≥ 8 ký tự CÓ CẢ BA loại: chữ hoa + chữ thường + số)
     *
     * Quy tắc:
     *   1. Lọc ký tự: chỉ giữ A-Z a-z 0-9 space - _ . ( ) / : @ +
     *   2. Nếu ký tự lạ chiếm > 30% → Unknown
     *   3. Nếu tên trông như encoded data (đoạn dài ≥ 8 có upper+lower+digit) → Unknown
     */
    private fun sanitizeBleName(raw: String?): String {
        if (raw.isNullOrEmpty()) return "Unknown"

        // Bước 1: lọc ký tự không hợp lệ
        val allowed = Regex("[A-Za-z0-9 \\-_.()/:@+]")
        val cleaned = raw.filter { allowed.matches(it.toString()) }
        val garbageRatio = 1.0 - cleaned.length.toDouble() / raw.length
        if (cleaned.length < 2 || garbageRatio > 0.3) return "Unknown"

        // Bước 2: phát hiện tên là encoded binary data
        // Dấu hiệu: ít nhất một "từ" (đoạn alphanumeric liên tục) dài ≥ 8 ký tự
        // VÀ chứa cả chữ hoa, chữ thường, chữ số cùng lúc → rất có thể là data encode
        val segments = cleaned.split(Regex("[^A-Za-z0-9]+"))
        val looksEncoded = segments.any { seg ->
            seg.length >= 8 &&
            seg.any { it.isLowerCase() } &&
            seg.any { it.isUpperCase() } &&
            seg.any { it.isDigit() }
        }
        if (looksEncoded) return "Unknown"

        return cleaned.trim()
    }

    // BLE scan callback — chỉ nhận BLE advertisements (không phải Classic Bluetooth)
    private val bleScanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            if (ActivityCompat.checkSelfPermission(
                    requireContext(), Manifest.permission.BLUETOOTH_CONNECT
                ) != PackageManager.PERMISSION_GRANTED
            ) return
            // Bỏ qua nếu đã có trong danh sách
            if (deviceList.none { it.address == device.address }) {
                deviceList.add(device)
                val name = try { sanitizeBleName(device.name) } catch (e: Exception) { "Unknown" }
                deviceNameList.add("$name\n${device.address}")
                requireActivity().runOnUiThread { listAdapter.notifyDataSetChanged() }
            }
        }

        override fun onScanFailed(errorCode: Int) {
            requireActivity().runOnUiThread {
                Toast.makeText(requireContext(), "BLE scan lỗi: $errorCode", Toast.LENGTH_SHORT).show()
            }
        }
    }

    // Receiver: bắt ESP32-S3 cắm vào khi app đang chạy
    private val usbAttachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
                val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                device?.let { connectUsbDevice(it) }
            }
        }
    }

    @RequiresApi(Build.VERSION_CODES.S)
    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentBluetoothBinding.inflate(inflater, container, false)

        listAdapter = ArrayAdapter(requireContext(), android.R.layout.simple_list_item_1, deviceNameList)
        binding.rvBluetooth.adapter = listAdapter

        usbTransport = UsbTransport(requireContext())

        requireContext().registerReceiver(
            usbAttachReceiver,
            IntentFilter(UsbManager.ACTION_USB_DEVICE_ATTACHED)
        )

        // Kết nối ESP32-S3 qua USB nếu đã cắm sẵn
        findAndConnectEsp32()

        // Nhấn vào thiết bị BLE → chuyển sang màn hình pairing loading
        binding.rvBluetooth.setOnItemClickListener { _, _, position, _ ->
            val device = deviceList[position]
            val mac = device.address.lowercase()
            val name = if (ActivityCompat.checkSelfPermission(
                    requireContext(), Manifest.permission.BLUETOOTH_CONNECT
                ) == PackageManager.PERMISSION_GRANTED
            ) sanitizeBleName(device.name) else "Unknown"

            val pairingFragment = PairingLoadingFragment().apply {
                arguments = Bundle().also {
                    it.putString("DEVICE_MAC", mac)
                    it.putString("DEVICE_NAME", name)
                }
            }

            requireActivity().supportFragmentManager.beginTransaction()
                .replace(com.example.uwb.R.id.fragment_container, pairingFragment)
                .addToBackStack(null)
                .commit()
        }

        requestBluetoothPermissions()
        binding.btnRefresh.setOnClickListener { scanBle() }

        return binding.root
    }

    private fun findAndConnectEsp32() {
        val usbManager = requireContext().getSystemService(Context.USB_SERVICE) as UsbManager
        val esp32 = usbManager.deviceList.values.find { it.vendorId == ESP32_VENDOR_ID }
        if (esp32 != null) {
            connectUsbDevice(esp32)
        } else {
            Toast.makeText(requireContext(), "Chưa thấy ESP32-S3 — hãy cắm cáp USB", Toast.LENGTH_LONG).show()
        }
    }

    private fun connectUsbDevice(device: UsbDevice) {
        usbTransport.openDevice(device) {
            // Lưu transport vào holder để UwbFragment dùng lại
            TransportHolder.transport = usbTransport

            // Nhận phản hồi từ S3 — chỉ log, không toast để tránh spam
            // (UWB measurements đến ~2 Hz; PairingLoadingFragment/UwbFragment xử lý UI riêng)
            usbTransport.receive { data ->
                val msg = String(data).trim()
                android.util.Log.d("S3_MSG", msg)
            }
            requireActivity().runOnUiThread {
                Toast.makeText(requireContext(), "Đã kết nối ESP32-S3 qua USB", Toast.LENGTH_SHORT).show()
            }
        }
    }

    /**
     * Quét BLE advertisements trong 10 giây.
     * DevKit V1 sẽ xuất hiện trong danh sách với tên "ESP32-DevKit-BLE".
     */
    private fun scanBle() {
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled) {
            startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            return
        }
        if (ActivityCompat.checkSelfPermission(
                requireContext(), Manifest.permission.BLUETOOTH_SCAN
            ) != PackageManager.PERMISSION_GRANTED
        ) return

        deviceList.clear()
        deviceNameList.clear()
        listAdapter.notifyDataSetChanged()

        bluetoothAdapter.bluetoothLeScanner?.startScan(bleScanCallback)
        Toast.makeText(requireContext(), "Đang quét BLE (10s)...", Toast.LENGTH_SHORT).show()

        // Dừng scan sau 10 giây để tiết kiệm pin
        Handler(Looper.getMainLooper()).postDelayed({
            if (ActivityCompat.checkSelfPermission(
                    requireContext(), Manifest.permission.BLUETOOTH_SCAN
                ) == PackageManager.PERMISSION_GRANTED
            ) {
                bluetoothAdapter.bluetoothLeScanner?.stopScan(bleScanCallback)
            }
        }, SCAN_PERIOD_MS)
    }

    @RequiresApi(Build.VERSION_CODES.S)
    private fun requestBluetoothPermissions() {
        ActivityCompat.requestPermissions(
            requireActivity(),
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.ACCESS_FINE_LOCATION
            ),
            1
        )
    }

    override fun onDestroyView() {
        super.onDestroyView()
        if (ActivityCompat.checkSelfPermission(
                requireContext(), Manifest.permission.BLUETOOTH_SCAN
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            bluetoothAdapter?.bluetoothLeScanner?.stopScan(bleScanCallback)
        }
        requireContext().unregisterReceiver(usbAttachReceiver)
        // Không disconnect USB ở đây — UwbFragment sẽ dùng lại transport này
        _binding = null
    }
}
