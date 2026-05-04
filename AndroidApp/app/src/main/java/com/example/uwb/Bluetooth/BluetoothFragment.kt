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
import android.util.Base64
import android.util.Log
import android.view.*
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.annotation.RequiresApi
import androidx.core.app.ActivityCompat
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.uwb.databinding.FragmentBluetoothBinding
import com.example.uwb.dataLg.ServerPublicKeyStore
import com.example.uwb.transport.UsbTransport
import com.example.uwb.transport.TransportHolder
import com.example.uwb.UI.PairingLoadingFragment
import com.example.uwb.dataLg.KeyManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

class BluetoothFragment : Fragment() {

    companion object {
        const val ARG_FRIEND_BUNDLE_BIN = "friend_bundle_bin"
        private const val TAG = "FriendProvision"
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
        requireContext().registerReceiver(usbAttachReceiver, usbIntentFilter)
        requireContext().registerReceiver(usbPermissionReceiver, IntentFilter(ACTION_USB_PERMISSION))

        val bundleBin = arguments?.getByteArray(ARG_FRIEND_BUNDLE_BIN)
        if (bundleBin != null) {
            Log.i(TAG, "FRIEND MODE — bundle ${bundleBin.size} bytes — waiting for USB Tag")
            binding.rvBluetooth.visibility = android.view.View.GONE
            binding.btnRefresh.visibility = android.view.View.GONE
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
                    .replace(com.example.uwb.R.id.fragment_container, pairingFragment)
                    .addToBackStack(null)
                    .commit()
            }

            requestBluetoothPermissions()
            binding.btnRefresh.setOnClickListener { scanBle() }
        }

        return binding.root
    }

    private fun findAndConnectEsp32() {
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
        Log.i(TAG, "USB: openDevice vendorId=0x${device.vendorId.toString(16)} productId=0x${device.productId.toString(16)}")
        val bundleBin = arguments?.getByteArray(ARG_FRIEND_BUNDLE_BIN)
        if (bundleBin != null) {
            _binding?.tvTitle?.text = "Đang kết nối USB..."
        }
        usbTransport.openDevice(device) {
            TransportHolder.transport = usbTransport

            requireActivity().runOnUiThread {
                if (bundleBin != null) {
                    Log.i(TAG, "USB: connected — bundleBin present (${bundleBin.size} bytes) → provisionFriendBundle")
                    _binding?.tvTitle?.text = "Đang nạp bundle..."
                    lifecycleScope.launch { provisionFriendBundle(bundleBin) }
                } else {
                    Log.i(TAG, "USB: connected — owner log mode")
                    Toast.makeText(requireContext(), "Đã kết nối ESP32-S3 qua USB", Toast.LENGTH_SHORT).show()
                    restoreLogCallback()
                }
            }
        }
    }

    // ── USB Provisioning ────────────────────────────────────────────────────

    private suspend fun provisionFriendBundle(bundle: ByteArray) {
        val ctx = requireContext()
        Log.i(TAG, "provisionFriendBundle: START — bundle=${bundle.size} bytes")

        val serverPubKey = withContext(Dispatchers.IO) { ServerPublicKeyStore.load(ctx) }
        if (serverPubKey == null) {
            withContext(Dispatchers.Main) {
                Toast.makeText(ctx, "Chưa có server signing key — không thể nạp bundle vào Tag", Toast.LENGTH_LONG).show()
            }
            restoreLogCallback()
            return
        }

        // Line-buffered response channel — receive callback chạy trên IO thread của SerialInputOutputManager
        val lineChannel = Channel<String>(Channel.UNLIMITED)
        val buf = StringBuilder()

        usbTransport.receive { data ->
            buf.append(String(data, Charsets.UTF_8))
            var idx = buf.indexOf("\n")
            while (idx >= 0) {
                val line = buf.substring(0, idx).trim()
                buf.delete(0, idx + 1)
                if (line.isNotEmpty()) {
                    Log.v(TAG, "Tag→App: $line")
                    lineChannel.trySend(line)
                }
                idx = buf.indexOf("\n")
            }
        }

        // Send command, wait up to timeoutMs for a response line starting with expectedPrefix.
        // Lines not matching the prefix (e.g. BLE/UWB debug output) are discarded silently.
        suspend fun sendCmd(cmd: String, expectedPrefix: String = "", timeoutMs: Long = 3_000L): String? {
            val displayCmd = if (cmd.length > 80) "${cmd.take(80)}...(${cmd.length} chars)" else cmd
            Log.i(TAG, "App→Tag: $displayCmd")
            usbTransport.send("$cmd\n".toByteArray(Charsets.UTF_8))
            val result = withTimeoutOrNull(timeoutMs) {
                var line: String
                do { line = lineChannel.receive() }
                while (expectedPrefix.isNotEmpty() && !line.startsWith(expectedPrefix))
                line
            }
            if (result == null) {
                Log.e(TAG, "  TIMEOUT (${timeoutMs}ms) — không nhận được response bắt đầu bằng '$expectedPrefix'")
            } else {
                Log.i(TAG, "  Response: $result")
            }
            return result
        }

        var oldFirmware = false

        // Step 1: SET_SERVER_PUBKEY
        Log.i(TAG, "─── Step 1: SET_SERVER_PUBKEY ───")
        _binding?.tvTitle?.text = "Gửi server key... (1/3)"
        when (val r = sendCmd("SET_SERVER_PUBKEY $serverPubKey", "SERVER_PUBKEY_")) {
            null -> {
                Log.w(TAG, "Step 1 TIMEOUT → firmware cũ hoặc Tag chưa sẵn sàng")
                oldFirmware = true
            }
            else -> {
                if (r.startsWith("SERVER_PUBKEY_OK")) {
                    Log.i(TAG, "Step 1 OK ✓")
                } else {
                    Log.e(TAG, "Step 1 FAIL: Tag trả về '$r'")
                    _binding?.tvTitle?.text = "Lỗi server key"
                    Toast.makeText(ctx, "Tag từ chối server key: $r", Toast.LENGTH_SHORT).show()
                }
            }
        }

        // Step 2: SET_TIME
        if (!oldFirmware) {
            Log.i(TAG, "─── Step 2: SET_TIME ───")
            _binding?.tvTitle?.text = "Đồng bộ thời gian... (2/3)"
            val epoch = System.currentTimeMillis() / 1000
            Log.i(TAG, "  epoch=$epoch")
            when (val r = sendCmd("SET_TIME $epoch", "TIME_")) {
                null -> {
                    Log.w(TAG, "Step 2 TIMEOUT → firmware cũ")
                    oldFirmware = true
                }
                else -> Log.i(TAG, "Step 2 OK ✓ → $r")
            }
        }

        // Step 3: SET_BUNDLE_BIN — binary 106 bytes, base64-encoded → 144 chars → fits 256-byte RX buffer
        if (!oldFirmware) {
            Log.i(TAG, "─── Step 3: SET_BUNDLE_BIN ───")
            _binding?.tvTitle?.text = "Gửi bundle... (3/3)"
            val bundleB64 = Base64.encodeToString(bundle, Base64.NO_WRAP)
            Log.i(TAG, "  bundle=${bundle.size} bytes → base64=${bundleB64.length} chars")
            // ECDSA P-256 verify trên ESP32-S3 (mbedTLS) mất ~150-300ms → 10s timeout an toàn
            when (val r = sendCmd("SET_BUNDLE_BIN $bundleB64", "BUNDLE_", timeoutMs = 10_000L)) {
                null -> {
                    Log.e(TAG, "Step 3 TIMEOUT (10s) — firmware cũ hoặc Tag crash")
                    oldFirmware = true
                }
                else -> {
                    if (r.startsWith("BUNDLE_OK")) {
                        val friendId = r.split(":").getOrNull(1) ?: ""
                        Log.i(TAG, "Step 3 OK ✓ → BUNDLE_OK friendId=$friendId")
                        _binding?.tvTitle?.text = "Bundle OK — Tag đang kết nối Anchor"
                        Toast.makeText(ctx, "Bundle đã nạp vào Tag ($friendId)", Toast.LENGTH_SHORT).show()
                    } else {
                        val reason = r.removePrefix("BUNDLE_ERR:")
                        Log.e(TAG, "Step 3 FAIL: BUNDLE_ERR:$reason")
                        _binding?.tvTitle?.text = "Bundle lỗi: $reason"
                        Toast.makeText(ctx, "Tag từ chối bundle: $reason", Toast.LENGTH_LONG).show()
                    }
                }
            }
        }

        if (oldFirmware) {
            Log.e(TAG, "provisionFriendBundle: ABORT — firmware Tag cũ, cần update")
            _binding?.tvTitle?.text = "Firmware cũ — cần update Tag"
            Toast.makeText(
                ctx,
                "Vui lòng update Tag firmware để dùng tính năng Friend Sharing",
                Toast.LENGTH_LONG
            ).show()
        }

        Log.i(TAG, "provisionFriendBundle: END")
        lineChannel.close()
        restoreLogCallback()
    }

    private fun restoreLogCallback() {
        usbTransport.receive { data ->
            Log.d("S3_MSG", String(data).trim())
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
