package com.example.uwb.model

data class FriendKeyBundle(val raw: ByteArray) {
    companion object {
        const val SIZE = 106
        const val SIGNED_PART_SIZE = 42
        const val SIG_SIZE = 64
        const val OFF_VERSION = 0
        const val OFF_FRIEND_ID = 1
        const val OFF_VEHICLE_HASH = 9
        const val OFF_FRIEND_KEY = 17
        const val OFF_ISSUED_AT = 33
        const val OFF_EXPIRES_AT = 37
        const val OFF_PERMISSIONS = 41
        const val OFF_SIG = 42
    }

    val friendIdHex: String get() {
        val id = raw.copyOfRange(OFF_FRIEND_ID, OFF_FRIEND_ID + 8)
        return id.joinToString("") { "%02x".format(it.toInt() and 0xFF) }
    }

    val issuedAt: Long get() {
        val b = raw
        return ((b[OFF_ISSUED_AT].toLong() and 0xFF) shl 24) or
               ((b[OFF_ISSUED_AT + 1].toLong() and 0xFF) shl 16) or
               ((b[OFF_ISSUED_AT + 2].toLong() and 0xFF) shl 8) or
               (b[OFF_ISSUED_AT + 3].toLong() and 0xFF)
    }

    val expiresAt: Long get() {
        val b = raw
        return ((b[OFF_EXPIRES_AT].toLong() and 0xFF) shl 24) or
               ((b[OFF_EXPIRES_AT + 1].toLong() and 0xFF) shl 16) or
               ((b[OFF_EXPIRES_AT + 2].toLong() and 0xFF) shl 8) or
               (b[OFF_EXPIRES_AT + 3].toLong() and 0xFF)
    }

    val permissions: Int get() = raw[OFF_PERMISSIONS].toInt() and 0xFF

    val signedPart: ByteArray get() = raw.copyOfRange(0, SIGNED_PART_SIZE)

    val sigR: ByteArray get() = raw.copyOfRange(OFF_SIG, OFF_SIG + 32)

    val sigS: ByteArray get() = raw.copyOfRange(OFF_SIG + 32, OFF_SIG + SIG_SIZE)

    override fun equals(other: Any?): Boolean = other is FriendKeyBundle && raw.contentEquals(other.raw)
    override fun hashCode(): Int = raw.contentHashCode()
}
