package com.example.uwb.UI

import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.uwb.Bluetooth.BluetoothFragment
import com.example.uwb.BuildConfig
import com.example.uwb.R
import com.example.uwb.crypto.EcdsaVerifier
import com.example.uwb.databinding.FragmentFriendSharingBinding
import com.example.uwb.dataLg.FriendShareEntry
import com.example.uwb.dataLg.FriendShareStore
import com.example.uwb.dataLg.KeyManager
import com.example.uwb.dataLg.ServerPublicKeyStore
import com.example.uwb.model.FriendKeyBundle
import com.example.uwb.model.FriendShareRequest
import com.example.uwb.network.ApiClient
import android.util.Base64
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.MultiFormatWriter
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

class FriendSharingFragment : Fragment() {

    private var _binding: FragmentFriendSharingBinding? = null
    private val binding get() = _binding!!

    private val ttlOptions = listOf(1 to "1 giờ", 8 to "8 giờ", 24 to "24 giờ", 72 to "72 giờ", 168 to "7 ngày")

    private val scanLauncher = registerForActivityResult(ScanContract()) { result ->
        val url = result.contents ?: return@registerForActivityResult
        claimFromUrl(url)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentFriendSharingBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        setupTtlSpinner()

        binding.btnBack.setOnClickListener { parentFragmentManager.popBackStack() }
        binding.btnCreateShare.setOnClickListener { requestNewQr() }
        binding.btnNewQr.setOnClickListener { showEmptyState() }
        binding.btnScanQr.setOnClickListener { launchScanner() }
        binding.btnManageShares.setOnClickListener { openManageShares() }
    }

    override fun onResume() {
        super.onResume()
        binding.btnManageShares.visibility =
            if (KeyManager.hasAnyOwnerKey()) View.VISIBLE else View.GONE
    }

    // ── TTL Spinner ─────────────────────────────────────────────────────────

    private fun setupTtlSpinner() {
        val spinnerAdapter = ArrayAdapter(
            requireContext(),
            android.R.layout.simple_spinner_item,
            ttlOptions.map { it.second }
        ).also { it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
        binding.spinnerTtl.adapter = spinnerAdapter
        binding.spinnerTtl.setSelection(2) // Default: 24 giờ
    }

    private fun selectedTtlHours(): Int = ttlOptions[binding.spinnerTtl.selectedItemPosition].first

    // ── Tạo QR (chủ xe) ────────────────────────────────────────────────────

    private fun requestNewQr() {
        val vin = KeyManager.getVehicleId()
        if (vin == null) {
            Toast.makeText(requireContext(), "Chưa pair xe — hãy nhập VIN trước", Toast.LENGTH_SHORT).show()
            return
        }

        val friendName = binding.etFriendName.text?.toString()?.trim() ?: ""
        if (friendName.isEmpty()) {
            binding.etFriendName.error = "Vui lòng nhập tên người được chia sẻ"
            return
        }

        val ownerApiKey = KeyManager.getOwnerApiKeyForVin(vin) ?: ""
        if (ownerApiKey.isEmpty()) {
            Toast.makeText(requireContext(), "Chưa có owner key — hãy pair lại xe", Toast.LENGTH_LONG).show()
            return
        }

        setLoading(true)
        lifecycleScope.launch {
            try {
                val response = ApiClient.api.createFriendShare(
                    ownerApiKey,
                    FriendShareRequest(
                        vehicle_id = vin,
                        friend_name = friendName,
                        ttl_hours = selectedTtlHours()
                    )
                )
                FriendShareStore.save(
                    requireContext(),
                    FriendShareEntry(
                        friendId = response.friend_id,
                        friendName = friendName,
                        claimUrl = response.claim_url,
                        expiresAt = response.expires_at,
                        vehicleId = vin,
                        createdAt = java.time.Instant.now().toString()
                    )
                )
                // Cache server signing key for future guest claim verifications (fire-and-forget)
                prefetchServerPublicKey()

                binding.ivQrCode.setImageBitmap(generateQrBitmap(response.claim_url, 800))
                binding.tvExpiry.text = formatExpiry(response.expires_at)
                showQrState()
            } catch (e: Exception) {
                Toast.makeText(requireContext(), "Không tạo được mã: ${e.message}", Toast.LENGTH_LONG).show()
            } finally {
                setLoading(false)
            }
        }
    }

    // Fetch server pubkey và cache — không block UI, lỗi bị bỏ qua
    private fun prefetchServerPublicKey() {
        if (ServerPublicKeyStore.load(requireContext()) != null) return
        lifecycleScope.launch {
            try {
                val resp = withContext(Dispatchers.IO) { ApiClient.api.getServerPublicKey() }
                ServerPublicKeyStore.save(requireContext(), resp.server_public_key_b64)
                Log.d("FriendSharing", "Server signing key cached")
            } catch (e: Exception) {
                Log.w("FriendSharing", "Could not prefetch server key: ${e.message}")
            }
        }
    }

    // ── Quét QR (bạn bè) ───────────────────────────────────────────────────

    private fun launchScanner() {
        val options = ScanOptions().apply {
            setDesiredBarcodeFormats(ScanOptions.QR_CODE)
            setPrompt("Quét mã QR từ bạn bè")
            setBeepEnabled(false)
            setOrientationLocked(true)
            captureActivity = PortraitCaptureActivity::class.java
        }
        scanLauncher.launch(options)
    }

    private fun claimFromUrl(url: String) {
        val token = Uri.parse(url).lastPathSegment
        if (token.isNullOrBlank()) {
            Toast.makeText(requireContext(), "Mã QR không hợp lệ", Toast.LENGTH_SHORT).show()
            return
        }

        setLoading(true)
        lifecycleScope.launch {
            try {
                val resp = withContext(Dispatchers.IO) { ApiClient.api.claimFriendShare(token) }

                // Decode 106-byte binary bundle
                val bundleBytes = Base64.decode(resp.bundle_b64, Base64.NO_WRAP)
                if (bundleBytes.size != FriendKeyBundle.SIZE) {
                    Toast.makeText(requireContext(), "Bundle size không hợp lệ", Toast.LENGTH_LONG).show()
                    return@launch
                }

                // ── ECDSA verify offline ────────────────────────────────────
                var pubKeyB64 = ServerPublicKeyStore.load(requireContext())

                if (pubKeyB64 == null) {
                    pubKeyB64 = try {
                        withContext(Dispatchers.IO) {
                            ApiClient.api.getServerPublicKey().server_public_key_b64
                        }.also { ServerPublicKeyStore.save(requireContext(), it) }
                    } catch (e: Exception) {
                        Log.w("FriendSharing", "Server key fetch failed — skipping verify: ${e.message}")
                        null
                    }
                }

                if (pubKeyB64 != null) {
                    var valid = withContext(Dispatchers.Default) {
                        EcdsaVerifier.verifyBundleBinary(bundleBytes, pubKeyB64)
                    }
                    if (!valid) {
                        // Key cũ có thể không còn hợp lệ — thử fetch key mới từ server
                        Log.w("FriendSharing", "ECDSA verify failed with cached key — refreshing server key")
                        val freshKey = try {
                            withContext(Dispatchers.IO) {
                                ApiClient.api.getServerPublicKey().server_public_key_b64
                            }.also { ServerPublicKeyStore.save(requireContext(), it) }
                        } catch (e: Exception) {
                            Log.w("FriendSharing", "Server key refresh failed: ${e.message}")
                            null
                        }
                        if (freshKey != null && freshKey != pubKeyB64) {
                            valid = withContext(Dispatchers.Default) {
                                EcdsaVerifier.verifyBundleBinary(bundleBytes, freshKey)
                            }
                        }
                    }
                    if (!valid) {
                        Toast.makeText(
                            requireContext(),
                            "Bundle signature không hợp lệ — không thể xác minh",
                            Toast.LENGTH_LONG
                        ).show()
                        if (BuildConfig.DEBUG) {
                            val bundle = FriendKeyBundle(bundleBytes)
                            Log.w("FriendSharing", "ECDSA verify FAILED for bundle ${bundle.friendIdHex}")
                        }
                        return@launch
                    }
                } else {
                    Log.w("FriendSharing", "No server pubkey — skipping local verify (Anchor will verify)")
                }
                // ───────────────────────────────────────────────────────────

                val bundle = FriendKeyBundle(bundleBytes)
                val friendKey = bundleBytes.copyOfRange(FriendKeyBundle.OFF_FRIEND_KEY, FriendKeyBundle.OFF_FRIEND_KEY + 16)
                KeyManager.saveFriendBundle(resp.vehicle_id, bundle.friendIdHex, friendKey, resp.bundle_b64)

                Toast.makeText(
                    requireContext(),
                    "Đã nhận quyền từ ${resp.friend_name} — xe ${resp.vehicle_id}",
                    Toast.LENGTH_SHORT
                ).show()

                // Navigate to Bluetooth với binary bundle để USB provisioning
                parentFragmentManager.beginTransaction()
                    .replace(
                        R.id.fragment_container,
                        BluetoothFragment().apply {
                            arguments = Bundle().also {
                                it.putByteArray(BluetoothFragment.ARG_FRIEND_BUNDLE_BIN, bundleBytes)
                            }
                        }
                    )
                    .addToBackStack(null)
                    .commit()

            } catch (e: Exception) {
                Toast.makeText(requireContext(), "Lỗi nhận key: ${e.message}", Toast.LENGTH_LONG).show()
            } finally {
                setLoading(false)
            }
        }
    }

    // ── Quản lý chia sẻ ────────────────────────────────────────────────────

    private fun openManageShares() {
        parentFragmentManager.beginTransaction()
            .replace(R.id.fragment_container, OwnerFriendListFragment())
            .addToBackStack(null)
            .commit()
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    private fun generateQrBitmap(content: String, size: Int): Bitmap {
        val hints = mapOf(EncodeHintType.MARGIN to 1)
        val matrix = MultiFormatWriter().encode(content, BarcodeFormat.QR_CODE, size, size, hints)
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.RGB_565)
        for (x in 0 until size) {
            for (y in 0 until size) {
                bitmap.setPixel(x, y, if (matrix[x, y]) Color.BLACK else Color.WHITE)
            }
        }
        return bitmap
    }

    private fun formatExpiry(isoUtc: String): String {
        return try {
            val dt = LocalDateTime.parse(isoUtc.take(19), DateTimeFormatter.ISO_LOCAL_DATE_TIME)
            dt.format(DateTimeFormatter.ofPattern("HH:mm  dd/MM/yyyy")) + " (UTC)"
        } catch (_: Exception) { isoUtc }
    }

    private fun showQrState() {
        binding.layoutEmpty.visibility = View.GONE
        binding.layoutQr.visibility = View.VISIBLE
    }

    private fun showEmptyState() {
        binding.layoutQr.visibility = View.GONE
        binding.layoutEmpty.visibility = View.VISIBLE
    }

    private fun setLoading(loading: Boolean) {
        binding.loadingOverlay.visibility = if (loading) View.VISIBLE else View.GONE
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
