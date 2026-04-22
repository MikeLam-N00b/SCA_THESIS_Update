package com.example.uwb.UI

import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.example.uwb.Bluetooth.BluetoothFragment
import com.example.uwb.R
import com.example.uwb.databinding.FragmentFriendSharingBinding
import com.example.uwb.dataLg.FriendShareEntry
import com.example.uwb.dataLg.FriendShareStore
import com.example.uwb.dataLg.KeyManager
import com.example.uwb.model.FriendShareRequest
import com.example.uwb.network.ApiClient
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.MultiFormatWriter
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import kotlinx.coroutines.launch
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

class FriendSharingFragment : Fragment() {

    private var _binding: FragmentFriendSharingBinding? = null
    private val binding get() = _binding!!

    // ── ZXing scanner launcher ──────────────────────────────────────────────
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

        binding.btnBack.setOnClickListener { parentFragmentManager.popBackStack() }
        binding.btnCreateShare.setOnClickListener { requestNewQr() }
        binding.btnNewQr.setOnClickListener { showEmptyState() }
        binding.btnScanQr.setOnClickListener { launchScanner() }
    }

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

        setLoading(true)
        lifecycleScope.launch {
            try {
                val response = ApiClient.api.createFriendShare(
                    FriendShareRequest(vehicle_id = vin, friend_name = friendName, ttl_hours = 24)
                )
                FriendShareStore.save(
                    requireContext(),
                    FriendShareEntry(
                        friendId   = response.friend_id,
                        friendName = friendName,
                        claimUrl   = response.claim_url,
                        expiresAt  = response.expires_at,
                        vehicleId  = vin,
                        createdAt  = java.time.Instant.now().toString()
                    )
                )
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
                val bundle = ApiClient.api.claimFriendShare(token)

                // Lưu friend key vào KeyManager như pairing key bình thường
                val keyBytes = ByteArray(bundle.friend_key_hex.length / 2) { i ->
                    bundle.friend_key_hex.substring(i * 2, i * 2 + 2).toInt(16).toByte()
                }
                KeyManager.savePairingKey(
                    vId = bundle.vehicle_id,
                    pId = bundle.friend_id,
                    key = keyBytes
                )

                Toast.makeText(
                    requireContext(),
                    "Đã nhận quyền từ ${bundle.friend_name} — xe ${bundle.vehicle_id}",
                    Toast.LENGTH_SHORT
                ).show()

                // Chuyển sang Bluetooth để kết nối xe
                parentFragmentManager.beginTransaction()
                    .replace(R.id.fragment_container, BluetoothFragment())
                    .addToBackStack(null)
                    .commit()

            } catch (e: Exception) {
                Toast.makeText(requireContext(), "Lỗi nhận key: ${e.message}", Toast.LENGTH_LONG).show()
            } finally {
                setLoading(false)
            }
        }
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
