package com.example.uwb.UI

import android.graphics.Color
import android.nfc.NfcAdapter
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.fragment.app.Fragment
import com.example.uwb.R
import com.example.uwb.dataLg.KeyManager
import com.example.uwb.dataLg.SessionManager

import com.example.uwb.databinding.FragmentWelcomeBinding

class WelcomeFragment : Fragment() {

    private var _binding: FragmentWelcomeBinding? = null
    private val binding get() = _binding!!

    private lateinit var session: SessionManager

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentWelcomeBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        session = SessionManager(requireContext())

        binding.btnOwpa.setOnClickListener {
            if (session.isLoggedIn()) {
                goToEnterVIN()
            } else {
                goToLogin()
            }
        }

        binding.btnFriendSharing.setOnClickListener {
            requireActivity().supportFragmentManager.beginTransaction()
                .replace(R.id.fragment_container, FriendSharingFragment())
                .addToBackStack(null)
                .commit()
        }

        updateHceUi()

        binding.btnUnlockNfc.setOnClickListener {
            handleHceToggle()
        }
    }

    private fun updateHceUi() {
        val nfcAdapter = NfcAdapter.getDefaultAdapter(requireContext())
        val isSupported = nfcAdapter != null && 
                requireContext().packageManager.hasSystemFeature(android.content.pm.PackageManager.FEATURE_NFC_HOST_CARD_EMULATION)
        
        val hasCredentials = KeyManager.getVehicleId() != null && KeyManager.getPairingKeyHex() != "NOT_SET"
        val isHceActive = KeyManager.isHceEnabled()

        if (!isSupported) {
            binding.btnUnlockNfc.isEnabled = false
            binding.btnUnlockNfc.setCardBackgroundColor(Color.parseColor("#BDBDBD"))
            binding.tvUnlockNfcStatus.text = "HCE unsupported"
            return
        }

        if (!hasCredentials) {
            binding.btnUnlockNfc.isEnabled = false
            binding.btnUnlockNfc.setCardBackgroundColor(Color.parseColor("#BDBDBD"))
            binding.tvUnlockNfcStatus.text = "Missing credentials"
        } else {
            binding.btnUnlockNfc.isEnabled = true
            if (isHceActive) {
                binding.btnUnlockNfc.setCardBackgroundColor(Color.parseColor("#4CAF50"))
                binding.tvUnlockNfcTitle.text = "NFC UNLOCK ACTIVE"
                binding.tvUnlockNfcStatus.text = "Phone is ready for NFC unlock"
            } else {
                binding.btnUnlockNfc.setCardBackgroundColor(Color.parseColor("#2196F3"))
                binding.tvUnlockNfcTitle.text = "UNLOCK NFC"
                binding.tvUnlockNfcStatus.text = "Tap to activate HCE"
            }
        }
    }

    private fun handleHceToggle() {
        val nfcAdapter = NfcAdapter.getDefaultAdapter(requireContext())
        if (nfcAdapter == null) {
            Toast.makeText(requireContext(), "NFC is not supported", Toast.LENGTH_SHORT).show()
            return
        }
        if (!nfcAdapter.isEnabled) {
            Toast.makeText(requireContext(), "Please enable NFC in settings", Toast.LENGTH_SHORT).show()
            return
        }

        val currentState = KeyManager.isHceEnabled()
        KeyManager.setHceEnabled(!currentState)
        updateHceUi()
        
        val message = if (!currentState) "HCE activated" else "HCE deactivated"
        Toast.makeText(requireContext(), message, Toast.LENGTH_SHORT).show()
    }

    private fun goToLogin() {
        requireActivity().supportFragmentManager.beginTransaction()
            .replace(R.id.fragment_container, LoginFragment())
            .addToBackStack(null)
            .commit()
    }

    private fun goToEnterVIN() {
        requireActivity().supportFragmentManager.beginTransaction()
            .replace(R.id.fragment_container, EnterVinFragment())
            .addToBackStack(null)
            .commit()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
