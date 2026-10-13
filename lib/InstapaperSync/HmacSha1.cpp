#include "HmacSha1.h"

#include <cstring>

namespace HmacSha1 {
namespace {

constexpr size_t BLOCK_SIZE = 64;

inline uint32_t rotl(const uint32_t x, const int n) { return (x << n) | (x >> (32 - n)); }

struct Sha1Ctx {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t block[BLOCK_SIZE] = {};
  size_t blockLen = 0;
  uint64_t totalLen = 0;

  void processBlock() {
    // 16-word rolling message schedule (64 B) instead of the textbook w[80]
    // (320 B), keeping this frame under the project's 256 B local limit.
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      if (i >= 16) {
        w[i & 15] = rotl(w[(i - 3) & 15] ^ w[(i - 8) & 15] ^ w[(i - 14) & 15] ^ w[i & 15], 1);
      }
      uint32_t f;
      uint32_t k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t temp = rotl(a, 5) + f + e + k + w[i & 15];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  void update(const uint8_t* data, size_t len) {
    totalLen += len;
    while (len > 0) {
      const size_t take = (BLOCK_SIZE - blockLen) < len ? (BLOCK_SIZE - blockLen) : len;
      memcpy(block + blockLen, data, take);
      blockLen += take;
      data += take;
      len -= take;
      if (blockLen == BLOCK_SIZE) {
        processBlock();
        blockLen = 0;
      }
    }
  }

  void finish(uint8_t out[DIGEST_SIZE]) {
    const uint64_t bitLen = totalLen * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (blockLen != 56) update(&zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) lenBytes[i] = static_cast<uint8_t>(bitLen >> (56 - 8 * i));
    update(lenBytes, 8);
    for (int i = 0; i < 5; i++) {
      out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
  }
};

}  // namespace

void sha1(const uint8_t* data, const size_t len, uint8_t out[DIGEST_SIZE]) {
  Sha1Ctx ctx;
  ctx.update(data, len);
  ctx.finish(out);
}

void hmac(const uint8_t* key, size_t keyLen, const uint8_t* msg, const size_t msgLen, uint8_t out[DIGEST_SIZE]) {
  uint8_t keyBlock[BLOCK_SIZE] = {};
  if (keyLen > BLOCK_SIZE) {
    sha1(key, keyLen, keyBlock);
  } else {
    memcpy(keyBlock, key, keyLen);
  }

  uint8_t pad[BLOCK_SIZE];
  for (size_t i = 0; i < BLOCK_SIZE; i++) pad[i] = keyBlock[i] ^ 0x36;
  uint8_t innerDigest[DIGEST_SIZE];
  Sha1Ctx inner;
  inner.update(pad, BLOCK_SIZE);
  inner.update(msg, msgLen);
  inner.finish(innerDigest);

  for (size_t i = 0; i < BLOCK_SIZE; i++) pad[i] = keyBlock[i] ^ 0x5c;
  Sha1Ctx outer;
  outer.update(pad, BLOCK_SIZE);
  outer.update(innerDigest, DIGEST_SIZE);
  outer.finish(out);
}

}  // namespace HmacSha1
