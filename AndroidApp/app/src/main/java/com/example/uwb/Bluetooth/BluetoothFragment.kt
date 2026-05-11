package com.example.uwb.Bluetooth

import android.Manifest
import android.app.PendingIntent
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
import android.util.Log
import android.view.*
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.annotation.RequiresApi
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import com.example.uwb.R
import com.example.uwb.databinding.FragmentBluetoothBinding
import com.example.uwb.transport.UsbTransport
import com.example.uwb.transport.TransportHolder
import com.example.uwb.UI.PairingLoadingFragment
import com.example.uwb.UI.FriendLoadingFragment

class BluetoothFragment : Fragment() {

    companion object {
        const val ARG_FRIEND_BUNDLE_BIN = "friend_bundle_bin"
        private const val TAG = "BluetoothFragment"
    }

    private var _binding: FragmentBluetoothBinding? = null
    private val binding get() = _binding!!

    private val bluetoothAdapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()
    private val deviceList = mutableListOf<BluetoothDevice>()
    private val deviceNameList = mutableListOf<String>()
    private lateinit var listAdapter: ArrayAdapter<String>

    private lateinit var usbTransport: UsbTransport

    private val ESP32_VENDOR_ID = 0x303A
    private val SCAN_PERIOD_MS = 10000L
    private val ACTION_USB_PERMISSION = "com.example.uwb.USB_PERMISSION"

    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            if (granted && device != null) {
                connectUsbDevice(device)
            } else {
                Toast.makeText(requireContext(), "USB không được cấp quyền", Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun sanitizeBleName(raw: String?): String {
        if (raw.isNullOrEmpty()) return "Unknown"
        val allowed = Regex("[A-Za-z0-9 \\-_.()/:@+]")
        val cleaned = raw.filter { allowed.matches(it.toString()) }
        val garbageRatio = 1.0 - cleaned.length.toDouble() / raw.length
        if (cleaned.length < 2 || garbageRatio > 0.3) return "Unknown"
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

    private val bleScanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            if (ActivityCompat.checkSelfPermission(
                    requireContext(), Manifest.permission.BLUETOOTH_CONNECT
                ) != PackageManager.PERMISSION_GRANTED
            ) return
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

    private val usbAttachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
                val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                device?.let { requestUsbPermissionOrConnect(it) }
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

        val usbIntentFilter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(ACTION_USB_PERMISSION)
        }
        ContextCompat.registerReceiver(requireContext(), usbAttachReceiver, usbIntentFilter, ContextCompat.RECEIVER_NOT_EXPORTED)
        ContextCompat.registerReceiver(requireContext(), usbPermissionReceiver, IntentFilter(ACTION_USB_PERMISSION), ContextCompat.RECEIVER_NOT_EXPORTED)

        val bundleBin = arguments?.getByteArray(ARG_FRIEND_BUNDLE_BIN)
        if (bundleBin != null) {
            Log.i(TAG, "FRIEND MODE — bundle ${bundleBin.size} bytes — waiting for USB Tag")
            binding.rvBluetooth.visibility = View.GONE
            binding.btnRefresh.visibility = View.GONE
            binding.tvTitle.text = "Cắm Tag vào USB..."
        } else {
            Log.i(TAG, "OWNER MODE — no bundle")
        }

        findAndConnectEsp32()

        if (bundleBin == null) {
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
                    .replace(R.id.fragment_container, pairingFragment)
                    .addToBackStack(null)
                    .commit()
            }

            requestBluetoothPermissions()
            binding.btnRefresh.setOnClickListener { scanBle() }
            scanBle()
        }

        return binding.root
    }

    private fun findAndConnectEsp32() {
        if (TransportHolder.transport?.isConnected == true) {
            Log.d(TAG, "USB already connected — skip reconnect")
            return
        }
        val usbManager = requireContext().getSystemService(Context.USB_SERVICE) as UsbManager
        val esp32 = usbManager.deviceList.values.find { it.vendorId == ESP32_VENDOR_ID }
        if (esp32 != null) {
            requestUsbPermissionOrConnect(esp32)
        } else {
            Toast.makeText(requireContext(), "Chưa thấy ESP32-S3 — hãy cắm cáp USB", Toast.LENGTH_LONG).show()
        }
    }

    private fun requestUsbPermissionOrConnect(device: UsbDevice) {
        val usbManager = requireContext().getSystemService(Context.USB_SERVICE) as UsbManager
        if (usbManager.hasPermission(device)) {
            connectUsbDevice(device)
        } else {
            val permIntent = PendingIntent.getBroadcast(
                requireContext(), 0,
                Intent(ACTION_USB_PERMISSION),
                PendingIntent.FLAG_IMMUTABLE
            )
            usbManager.requestPermission(device, permIntent)
        }
    }

    private fun connectUsbDevice(device: UsbDevice) {
        Log.i(TAG, "USB: openDevice vendorId=0x${device.vendorId.toString(16)}")
        val bundleBin = arguments?.getByteArray(ARG_FRIEND_BUNDLE_BIN)
        usbTransport.openDevice(device) {
            TransportHolder.transport = usbTransport
            requireActivity().runOnUiThread {
                if (bundleBin != null) {
                    Log.i(TAG, "USB: connected — friend mode → FriendLoadingFragment")
                    val frag = FriendLoadingFragment().apply {
                        arguments = Bundle().also {
                            it.putByteArray(FriendLoadingFragment.ARG_BUNDLE_BIN, bundleBin)
                        }
                    }
                    parentFragmentManager.beginTransaction()
                        .replace(R.id.fragment_container, frag)
                        .addToBackStack(null)
                        .commit()
                } else {
                    Log.i(TAG, "USB: connected — owner mode")
                    Toast.makeText(requireContext(), "Đã kết nối Tag qua USB", Toast.LENGTH_SHORT).show()
                    usbTransport.receive { data -> Log.d("S3_MSG", String(data).trim()) }
                }
            }
        }
    }

    // ── BLE Scan ────────────────────────────────────────────────────────────

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
        requireContext().unregisterReceiver(usbPermissionReceiver)
        _binding = null
    }
}
