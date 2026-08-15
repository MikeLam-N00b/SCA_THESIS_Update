package com.example.uwb.UI

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import com.example.uwb.R
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
            .commit()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
