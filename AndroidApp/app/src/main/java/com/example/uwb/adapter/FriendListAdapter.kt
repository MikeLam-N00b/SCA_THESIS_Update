package com.example.uwb.adapter

import android.graphics.drawable.GradientDrawable
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.example.uwb.databinding.ItemFriendShareBinding
import com.example.uwb.model.FriendListItem
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

class FriendListAdapter(
    private val onRevoke: (friendId: String, friendName: String) -> Unit
) : RecyclerView.Adapter<FriendListAdapter.ViewHolder>() {

    private val items = mutableListOf<FriendListItem>()

    fun submitList(newItems: List<FriendListItem>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val binding = ItemFriendShareBinding.inflate(
            LayoutInflater.from(parent.context), parent, false
        )
        return ViewHolder(binding)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        holder.bind(items[position])
    }

    override fun getItemCount() = items.size

    inner class ViewHolder(private val binding: ItemFriendShareBinding) :
        RecyclerView.ViewHolder(binding.root) {

        fun bind(item: FriendListItem) {
            binding.tvFriendName.text = item.friend_name.ifBlank { "Friend" }
            binding.tvExpires.text = "Hết hạn: ${formatExpiry(item.expires_at)}"

            // Status chip
            val (label, color) = when (item.status) {
                "active" -> "Đang hiệu lực" to 0xFF4CAF50.toInt()
                "expired" -> "Hết hạn" to 0xFF9E9E9E.toInt()
                else -> "Đã thu hồi" to 0xFFF44336.toInt()
            }
            binding.tvStatus.text = label
            (binding.tvStatus.background as? GradientDrawable)?.setColor(color)
                ?: binding.tvStatus.setBackgroundColor(color)

            // Only active shares can be revoked
            val canRevoke = item.status == "active"
            binding.btnRevoke.isEnabled = canRevoke
            binding.btnRevoke.alpha = if (canRevoke) 1f else 0.4f
            binding.btnRevoke.setOnClickListener {
                if (canRevoke) onRevoke(item.friend_id, item.friend_name)
            }
        }

        private fun formatExpiry(isoUtc: String): String {
            return try {
                val dt = LocalDateTime.parse(isoUtc.take(19), DateTimeFormatter.ISO_LOCAL_DATE_TIME)
                dt.format(DateTimeFormatter.ofPattern("HH:mm dd/MM/yyyy")) + " UTC"
            } catch (_: Exception) { isoUtc }
        }
    }
}
