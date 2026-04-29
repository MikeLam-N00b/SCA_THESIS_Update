package com.example.uwb.UI

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.AdapterView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import com.example.uwb.R
import com.example.uwb.adapter.FriendListAdapter
import com.example.uwb.databinding.FragmentOwnerFriendListBinding
import com.example.uwb.dataLg.KeyManager
import com.example.uwb.network.ApiClient
import kotlinx.coroutines.launch

class OwnerFriendListFragment : Fragment() {

    private var _binding: FragmentOwnerFriendListBinding? = null
    private val binding get() = _binding!!

    private lateinit var adapter: FriendListAdapter
    private var vehicleIds: List<String> = emptyList()
    private var selectedVehicleId: String = ""

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentOwnerFriendListBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        adapter = FriendListAdapter { friendId, friendName ->
            showRevokeDialog(friendId, friendName)
        }
        binding.rvFriendList.layoutManager = LinearLayoutManager(requireContext())
        binding.rvFriendList.adapter = adapter

        binding.btnBack.setOnClickListener { parentFragmentManager.popBackStack() }

        setupVehicleSelector()
    }

    private fun setupVehicleSelector() {
        vehicleIds = KeyManager.getAllOwnerVehicleIds()
        if (vehicleIds.isEmpty()) {
            showEmpty()
            return
        }

        if (vehicleIds.size > 1) {
            binding.layoutVehicleSelector.visibility = View.VISIBLE
            val spinnerAdapter = ArrayAdapter(
                requireContext(),
                android.R.layout.simple_spinner_item,
                vehicleIds
            ).also { it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
            binding.spinnerVehicle.adapter = spinnerAdapter
            binding.spinnerVehicle.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>, v: View?, pos: Int, id: Long) {
                    selectedVehicleId = vehicleIds[pos]
                    loadList()
                }
                override fun onNothingSelected(parent: AdapterView<*>) {}
            }
        } else {
            binding.layoutVehicleSelector.visibility = View.GONE
            selectedVehicleId = vehicleIds[0]
            loadList()
        }
    }

    private fun loadList() {
        if (selectedVehicleId.isEmpty()) return
        binding.progressBar.visibility = View.VISIBLE
        binding.rvFriendList.visibility = View.GONE
        binding.layoutEmpty.visibility = View.GONE

        val ownerApiKey = KeyManager.getOwnerApiKeyForVin(selectedVehicleId) ?: ""

        lifecycleScope.launch {
            try {
                val response = ApiClient.api.listFriendShares(ownerApiKey, selectedVehicleId)
                if (response.friend_keys.isEmpty()) {
                    showEmpty()
                } else {
                    adapter.submitList(response.friend_keys)
                    binding.rvFriendList.visibility = View.VISIBLE
                    binding.layoutEmpty.visibility = View.GONE
                }
            } catch (e: Exception) {
                Toast.makeText(requireContext(), "Không tải được danh sách: ${e.message}", Toast.LENGTH_LONG).show()
                showEmpty()
            } finally {
                binding.progressBar.visibility = View.GONE
            }
        }
    }

    private fun showEmpty() {
        binding.progressBar.visibility = View.GONE
        binding.rvFriendList.visibility = View.GONE
        binding.layoutEmpty.visibility = View.VISIBLE
    }

    private fun showRevokeDialog(friendId: String, friendName: String) {
        AlertDialog.Builder(requireContext())
            .setTitle("Thu hồi quyền truy cập")
            .setMessage("Bạn có chắc muốn thu hồi quyền của \"$friendName\"?\nHành động này không thể hoàn tác.")
            .setPositiveButton("Thu hồi") { _, _ -> revokeFriendShare(friendId) }
            .setNegativeButton("Hủy", null)
            .show()
    }

    private fun revokeFriendShare(friendId: String) {
        binding.progressBar.visibility = View.VISIBLE
        val ownerApiKey = KeyManager.getOwnerApiKeyForVin(selectedVehicleId) ?: ""
        lifecycleScope.launch {
            try {
                ApiClient.api.revokeFriendShare(ownerApiKey, friendId)
                Toast.makeText(requireContext(), "Đã thu hồi quyền truy cập", Toast.LENGTH_SHORT).show()
                loadList()
            } catch (e: Exception) {
                binding.progressBar.visibility = View.GONE
                Toast.makeText(requireContext(), "Thu hồi thất bại: ${e.message}", Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }
}
