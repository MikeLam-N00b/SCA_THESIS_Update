#pragma once
// anchor_crypto.h — mbedTLS CSPRNG state and crypto utility functions.

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

static mbedtls_entropy_context  entropy;
static mbedtls_ctr_drbg_context ctr_drbg;

static void hexStringToBytes(const char *hex, uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; i++)
        sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
}

static void generateChallenge(uint8_t *challenge, size_t length) {
    mbedtls_ctr_drbg_random(&ctr_drbg, challenge, length);
}

static bool computeHMAC(const uint8_t *key, size_t keyLen,
                        const uint8_t *data, size_t dataLen,
                        uint8_t *output) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return (mbedtls_md_hmac(md, key, keyLen, data, dataLen, output) == 0);
}

// HKDF-SHA256 (RFC 5869) — replaces mbedtls_hkdf() absent in older SDK.
// salt=NULL/0 uses 32 zero bytes (RFC 5869 §2.2). outLen must be <= 32.
static bool hkdfSha256(const uint8_t *salt, size_t saltLen,
                       const uint8_t *ikm,  size_t ikmLen,
                       const uint8_t *info, size_t infoLen,
                       uint8_t *out, size_t outLen) {
    if (outLen > 32) return false;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    uint8_t zeros[32] = {};
    const uint8_t *s  = (salt && saltLen > 0) ? salt : zeros;
    size_t         sl = (salt && saltLen > 0) ? saltLen : 32;
    uint8_t prk[32];
    if (mbedtls_md_hmac(md, s, sl, ikm, ikmLen, prk) != 0) return false;

    uint8_t counter = 0x01;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, 1) != 0) { mbedtls_md_free(&ctx); return false; }
    mbedtls_md_hmac_starts(&ctx, prk, 32);
    mbedtls_md_hmac_update(&ctx, info, infoLen);
    mbedtls_md_hmac_update(&ctx, &counter, 1);
    uint8_t t1[32];
    mbedtls_md_hmac_finish(&ctx, t1);
    mbedtls_md_free(&ctx);

    memcpy(out, t1, outLen);
    return true;
}

static void printHex(const char *label, const uint8_t *data, size_t length) {
    Serial.print(label);
    for (size_t i = 0; i < length; i++) {
        if (data[i] < 0x10) Serial.print("0");
        Serial.print(data[i], HEX);
    }
    Serial.println();
}
