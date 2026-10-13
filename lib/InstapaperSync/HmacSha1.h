#pragma once
#include <cstddef>
#include <cstdint>

/**
 * SHA-1 and HMAC-SHA1, as required by OAuth 1.0a request signing.
 *
 * Kept dependency-free (no mbedtls/wolfSSL) so the signing path compiles and
 * is unit-tested on the host exactly as it runs on the device. SHA-1 is only
 * used here as the HMAC primitive OAuth 1.0a mandates, not for integrity.
 */
namespace HmacSha1 {

constexpr size_t DIGEST_SIZE = 20;

void sha1(const uint8_t* data, size_t len, uint8_t out[DIGEST_SIZE]);

void hmac(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[DIGEST_SIZE]);

}  // namespace HmacSha1
