package com.example.uwb.UI

import android.animation.ObjectAnimator
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Base64
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.animation.LinearInterpolator
import android.widget.Toast
import androidx.fragment.app.Fragment
import com.example.uwb.R
import com.example.uwb.databinding.FragmentFriendLoadingBinding
import com.example.uwb.dataLg.ServerPublicKeyStore
import com.example.uwb.transport.TransportHolder
import com.example.uwb.transport.UsbTransport

class FriendLoadingFragment : Fragment() {

    companion object {
        const val ARG_BUNDLE_BIN = "bundle_bin"
        private const val TAG = "FriendLoading"
    }

    private var _binding: FragmentFriendLoadingBinding? = null
    private val binding get() = _binding!!
    private val mainHandler = Handler(Looper.getMainLooper())

    private var carAnimator: ObjectAnimator? = null
    private var resultHandled = false

    private enum class State { STEP1, STEP2, STEP3, WAITING_ACCESS, DONE }
    private var provState = State.STEP1

    private val lineBuf = StringBuilder()
    private var stepTimeoutRunnable: Runnable? = null

    private lateinit var bundleBin: ByteArray
    private lateinit var serverPubKey: String
    private lateinit var transport: UsbTransport

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentFriendLoadingBinding.inflate(inflater, container, false)

        val bin = arguments?.getByteArray(ARG_BUNDLE_BIN)
        val t = TransportHolder.transport

        if (bin == null || t == null || !t.isConnected) {
            Toast.makeText(requireContext(), "Lỗi: mất kết nối USB hoặc bundle", Toast.LENGTH_LONG).show()
            parentFragmentManager.popBackStack()
            return binding.root
        }

        val key = ServerPublicKeyStore.load(requireContext())
        if (key == null) {
            Toast.makeText(requireContext(), "Chưa có server signing key — pair lại để cập nhật", Toast.LENGTH_LONG).show()
            parentFragmentManager.popBackStack()
            return binding.root
        }

        bundleBin = bin
        serverPubKey = key
        transport = t

        binding.btnBackArrow.setOnClickListener {
            cancelAndGoBack()
        }

        binding.btnCancel.setOnClickListener { cancelAndGoBack() }
        startCarAnimation()
        registerReceiveCallback()
        sendStep1()

        return binding.root
    }

    private fun registerReceiveCallback() {
        transport.receive { data ->
            lineBuf.append(String(data, Charsets.UTF_8))
            var idx = lineBuf.indexOf("\n")
            while (idx >= 0) {
                val line = lineBuf.substring(0, idx).trim()
                lineBuf.delete(0, idx + 1)
                if (line.isNotEmpty()) {
                    Log.v(TAG, "Tag→App: $line")
                    mainHandler.post { onLine(line) }
                }
                idx = lineBuf.indexOf("\n")
            }
        }
    }

    private fun sendStep1() {
        updateStatus("Gửi server key...")
        Log.i(TAG, "Step 1: SET_SERVER_PUBKEY")
        transport.send("SET_SERVER_PUBKEY $serverPubKey\n".toByteArray(Charsets.UTF_8))
        scheduleStepTimeout(5_000L, "Timeout gửi server key")
    }

    private fun sendStep2() {
        updateStatus("Đồng bộ thời gian...")
        val epoch = System.currentTimeMillis() / 1000
        Log.i(TAG, "Step 2: SET_TIME $epoch")
        transport.send("SET_TIME $epoch\n".toByteArray(Charsets.UTF_8))
        scheduleStepTimeout(5_000L, "Timeout đồng bộ thời gian")
    }

    private fun sendStep3() {
        updateStatus("Gửi bundle vào Tag...")
        val bundleB64 = Base64.encodeToString(bundleBin, Base64.NO_WRAP)
        Log.i(TAG, "Step 3: SET_BUNDLE_BIN ${bundleBin.size} bytes")
        transport.send("SET_BUNDLE_BIN $bundleB64\n".toByteArray(Charsets.UTF_8))
        scheduleStepTimeout(10_000L, "Timeout gửi bundle")
    }

    private fun onLine(line: String) {
        if (resultHandled || _binding == null) return
        when (provState) {
            State.STEP1 -> {
                if (line.startsWith("SERVER_PUBKEY_OK")) {
                    cancelStepTimeout()
                    Log.i(TAG, "Step 1 OK")
                    provState = State.STEP2
                    sendStep2()
                } else if (line.startsWith("SERVER_PUBKEY_ERR")) {
                    cancelStepTimeout()
                    onFailed("Tag từ chối server key")
                }
            }
            State.STEP2 -> {
                if (line.startsWith("TIME_")) {
                    cancelStepTimeout()
                    Log.i(TAG, "Step 2 OK: $line")
                    provState = State.STEP3
                    sendStep3()
                }
            }
            State.STEP3 -> {
                if (line.startsWith("BUNDLE_OK")) {
                    cancelStepTimeout()
                    Log.i(TAG, "Step 3 OK: $line")
                    provState = State.WAITING_ACCESS
                    updateStatus("Tag đang tìm Anchor...")
                    scheduleStepTimeout(120_000L, "Không tìm thấy Anchor trong 2 phút")
                } else if (line.startsWith("BUNDLE_ERR")) {
                    cancelStepTimeout()
                    onFailed("Tag từ chối bundle: ${line.removePrefix("BUNDLE_ERR:")}")
                }
            }
            State.WAITING_ACCESS -> {
                when {
                    line == "FRIEND_ACCESS_GRANTED" -> { cancelStepTimeout(); onSuccess() }
                    line.startsWith("FRIEND_ACCESS_DENIED") -> { cancelStepTimeout(); onFailed("Anchor từ chối truy cập") }
                    line == "DISCONNECTED" -> { cancelStepTimeout(); onFailed("Tag ngắt kết nối") }
                }
            }
            State.DONE -> { /* ignore */ }
        }
    }

    private fun scheduleStepTimeout(ms: Long, message: String) {
        cancelStepTimeout()
        val r = Runnable { if (!resultHandled && _binding != null) onFailed(message) }
        stepTimeoutRunnable = r
        mainHandler.postDelayed(r, ms)
    }

    private fun cancelStepTimeout() {
        stepTimeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        stepTimeoutRunnable = null
    }

    private fun onSuccess() {
        resultHandled = true
        provState = State.DONE
        carAnimator?.cancel()
        updateStatus("Kết nối thành công!")
        Log.i(TAG, "FRIEND_ACCESS_GRANTED → UwbFragment")
        mainHandler.postDelayed({
            if (_binding == null) return@postDelayed
            parentFragmentManager.popBackStack()
            parentFragmentManager.beginTransaction()
                .replace(R.id.fragment_container, UwbFragment())
                .addToBackStack(null)
                .commit()
        }, 800L)
    }

    private fun onFailed(reason: String) {
        resultHandled = true
        provState = State.DONE
        carAnimator?.cancel()
        updateStatus("Thất bại")
        Toast.makeText(requireContext(), reason, Toast.LENGTH_LONG).show()
        Log.e(TAG, "Failed: $reason")
        mainHandler.postDelayed({
            if (_binding != null) parentFragmentManager.popBackStack()
        }, 1200L)
    }

    private fun cancelAndGoBack() {
        resultHandled = true
        provState = State.DONE
        cancelStepTimeout()
        mainHandler.removeCallbacksAndMessages(null)
        carAnimator?.cancel()
        parentFragmentManager.popBackStack()
    }

    private fun startCarAnimation() {
        binding.roadTrack.post {
            val trackWidth = binding.roadTrack.width.toFloat()
            val carWidth = binding.ivCar.width.toFloat()
            carAnimator = ObjectAnimator.ofFloat(
                binding.ivCar, "translationX", -carWidth, trackWidth
            ).apply {
                duration = 1600
                repeatCount = ObjectAnimator.INFINITE
                interpolator = LinearInterpolator()
                start()
            }
        }
    }

    private fun updateStatus(status: String) {
        _binding?.tvStatus?.text = status
    }

    override fun onDestroyView() {
        super.onDestroyView()
        cancelStepTimeout()
        mainHandler.removeCallbacksAndMessages(null)
        carAnimator?.cancel()
        _binding = null
    }
}
