package com.example.uwb.UI

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.animation.LinearInterpolator
import android.view.animation.TranslateAnimation
import androidx.fragment.app.Fragment
import com.example.uwb.databinding.FragmentVerifyingVinBinding

class VerifyingVinFragment : Fragment() {

    private var _binding: FragmentVerifyingVinBinding? = null
    private val binding get() = _binding!!

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentVerifyingVinBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        binding.btnBackArrow.setOnClickListener {
            parentFragmentManager.popBackStack()
        }

        startCarAnimation()
    }

    private fun startCarAnimation() {
        val screenWidth = resources.displayMetrics.widthPixels.toFloat()

        val animation = TranslateAnimation(
            -200f, screenWidth + 200f,  // từ trái sang phải
            0f, 0f
        )
        animation.duration = 3000
        animation.repeatCount = TranslateAnimation.INFINITE
        animation.repeatMode = TranslateAnimation.RESTART
        animation.interpolator = LinearInterpolator()

        binding.imgCar.startAnimation(animation)
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
