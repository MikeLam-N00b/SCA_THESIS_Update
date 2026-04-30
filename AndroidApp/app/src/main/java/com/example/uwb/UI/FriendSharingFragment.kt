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
import com.example.uwb.model.FriendShareRequest
import com.example.uwb.network.ApiClient
import com.google.gson.Gson
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
        Log.i("FriendSharing", "════ claimFromUrl START ════")
        Log.i("FriendSharing", "  URL: $url")
        val token = Uri.parse(url).lastPathSegment
        Log.i("FriendSharing", "  token: $token")
        if (token.isNullOrBlank()) {
            Log.e("FriendSharing", "  ABORT — token rỗng, QR không hợp lệ")
            Toast.makeText(requireContext(), "Mã QR không hợp lệ", Toast.LENGTH_SHORT).show()
            return
        }

        setLoading(true)
        lifecycleScope.launch {
            try {
                Log.i("FriendSharing", "  [1] GET /friend-sharing/claim/$token ...")
                val bundle = withContext(Dispatchers.IO) { ApiClient.api.claimFriendShare(token) }
                Log.i("FriendSharing", "  [1] bundle nhận được:")
                Log.i("FriendSharing", "      friend_id   = ${bundle.friend_id}")
                Log.i("FriendSharing", "      vehicle_id  = ${bundle.vehicle_id}")
                Log.i("FriendSharing", "      friend_name = ${bundle.friend_name}")
                Log.i("FriendSharing", "      issued_at   = ${bundle.issued_at}")
                Log.i("FriendSharing", "      expires_at  = ${bundle.expires_at}")
                Log.i("FriendSharing", "      permissions = ${bundle.permissions}")
                Log.i("FriendSharing", "      friend_key_hex length = ${bundle.friend_key_hex.length}")
                Log.i("FriendSharing", "      issuer_sig_b64 length = ${bundle.issuer_sig_b64.length}")

                // ── ECDSA verify ────────────────────────────────────────────
                Log.i("FriendSharing", "  [2] ECDSA verify ...")
                var pubKeyB64 = ServerPublicKeyStore.load(requireContext())

                if (pubKeyB64 == null) {
                    Log.w("FriendSharing", "  [2] ServerPublicKeyStore trống → lazy fetch từ server ...")
                    pubKeyB64 = try {
                        withContext(Dispatchers.IO) {
                            ApiClient.api.getServerPublicKey().server_public_key_b64
                        }.also { ServerPublicKeyStore.save(requireContext(), it) }
                    } catch (e: Exception) {
                        Log.w("FriendSharing", "  [2] Server key fetch FAILED: ${e.message} — skip verify")
                        null
                    }
                } else {
                    Log.i("FriendSharing", "  [2] ServerPublicKeyStore có sẵn (${pubKeyB64.length} chars)")
                }

                if (pubKeyB64 != null) {
                    val valid = withContext(Dispatchers.Default) {
                        EcdsaVerifier.verifyFriendBundle(bundle, pubKeyB64)
                    }
                    if (!valid) {
                        Log.e("FriendSharing", "  [2] ECDSA verify FAILED — chữ ký không khớp server")
                        Log.e("FriendSharing", "      Canonical msg phải là: v{ver}|{friend_id_hex}|{vehicle_id}|{key_hex}|{issued_at}|{expires_at}|{perms}")
                        Toast.makeText(
                            requireContext(),
                            "Mã QR không hợp lệ — chữ ký không khớp với server",
                            Toast.LENGTH_LONG
                        ).show()
                        return@launch
                    }
                    Log.i("FriendSharing", "  [2] ECDSA verify OK ✓")
                } else {
                    Log.w("FriendSharing", "  [2] Không có pubkey → skip local verify (Anchor sẽ verify)")
                }
                // ───────────────────────────────────────────────────────────

                val keyBytes = bundle.friend_key_hex.chunked(2)
                    .map { it.toInt(16).toByte() }
                    .toByteArray()

                val bundleJson = Gson().toJson(bundle)
                Log.i("FriendSharing", "  [3] bundleJson length = ${bundleJson.length} chars")
                KeyManager.saveFriendBundle(bundle.vehicle_id, bundle.friend_id, keyBytes, bundleJson)
                Log.i("FriendSharing", "  [3] KeyManager.saveFriendBundle OK")

                Toast.makeText(
                    requireContext(),
                    "Đã nhận quyền từ ${bundle.friend_name} — xe ${bundle.vehicle_id}",
                    Toast.LENGTH_SHORT
                ).show()

                Log.i("FriendSharing", "  [4] Navigate → BluetoothFragment (FRIEND MODE) với bundleJson")
                Log.i("FriendSharing", "      → Cắm Tag vào USB để provisionFriendBundle chạy tự động")

                // Navigate to Bluetooth với bundle để USB provisioning
                parentFragmentManager.beginTransaction()
                    .replace(
                        R.id.fragment_container,
                        BluetoothFragment().apply {
                            arguments = Bundle().also {
                                it.putString(BluetoothFragment.ARG_FRIEND_BUNDLE_JSON, bundleJson)
                            }
                        }
                    )
                    .addToBackStack(null)
                    .commit()

            } catch (e: Exception) {
                Log.e("FriendSharing", "  claimFromUrl EXCEPTION: ${e.javaClass.simpleName}: ${e.message}")
                Toast.makeText(requireContext(), "Lỗi nhận key: ${e.message}", Toast.LENGTH_LONG).show()
            } finally {
                setLoading(false)
                Log.i("FriendSharing", "════ claimFromUrl END ════")
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
