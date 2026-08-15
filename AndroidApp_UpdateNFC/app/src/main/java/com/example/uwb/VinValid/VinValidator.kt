package com.example.uwb.VinValid

object VinValidator {
    private val VIN_REGEX = Regex("^[A-HJ-NPR-Z0-9]{17}$")

    fun isValid(vin: String): Boolean {
        return VIN_REGEX.matches(vin.trim().uppercase())
    }
}