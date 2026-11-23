#pragma once

// Real SHA-256 for the ham-mode native suite. The suite pins real digests (the
// Public PSK's 0x11 air hash and the #test channel secret), so the
// deterministic mock in test/mocks/SHA256.h cannot be used here. The include
// order in [env:native_ham_mode] puts this header ahead of test/mocks.
//
// HMAC-SHA256 is included because Utils::encryptThenMAC()/MACThenDecrypt()
// use it; keeping both in one small header means the suite links the same real
// crypto the firmware uses rather than a shortcut that always verifies.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint32_t HAM_SHA256_K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

class SHA256 {
  uint32_t _h[8];
  uint8_t _buf[64];
  size_t _len;
  uint64_t _total;
  uint8_t _ipad[64], _opad[64];

  static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
  }

  void compress(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
             ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = _h[0], b = _h[1], c = _h[2], d = _h[3];
    uint32_t e = _h[4], f = _h[5], g = _h[6], hh = _h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + HAM_SHA256_K[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    _h[0] += a; _h[1] += b; _h[2] += c; _h[3] += d;
    _h[4] += e; _h[5] += f; _h[6] += g; _h[7] += hh;
  }

  void finishInto(uint8_t out[32]) {
    uint64_t bits = _total * 8;
    uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (_len != 56) update(&zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(tail, 8);
    for (int i = 0; i < 8; i++) {
      out[4 * i] = (uint8_t)(_h[i] >> 24);
      out[4 * i + 1] = (uint8_t)(_h[i] >> 16);
      out[4 * i + 2] = (uint8_t)(_h[i] >> 8);
      out[4 * i + 3] = (uint8_t)(_h[i]);
    }
  }

  void baseReset() {
    static const uint32_t init[8] = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(_h, init, sizeof(_h));
    _len = 0;
    _total = 0;
  }

public:
  SHA256() { baseReset(); }

  void reset() { baseReset(); }

  void update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    _total += len;
    while (len) {
      size_t take = 64 - _len;
      if (take > len) take = len;
      memcpy(_buf + _len, p, take);
      _len += take;
      p += take;
      len -= take;
      if (_len == 64) { compress(_buf); _len = 0; }
    }
  }

  void finalize(uint8_t* hash, size_t hashLen) {
    uint8_t out[32];
    finishInto(out);
    memcpy(hash, out, hashLen);
  }

  void resetHMAC(const uint8_t* key, size_t keyLen) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (keyLen > sizeof(k)) {
      SHA256 inner;
      inner.update(key, keyLen);
      inner.finalize(k, 32);
    } else {
      memcpy(k, key, keyLen);
    }
    for (size_t i = 0; i < sizeof(k); i++) {
      _ipad[i] = (uint8_t)(k[i] ^ 0x36);
      _opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    baseReset();
    update(_ipad, sizeof(_ipad));
  }

  void finalizeHMAC(const uint8_t* key, size_t keyLen, uint8_t* hash, size_t hashLen) {
    (void)key; (void)keyLen;   // pads were captured by resetHMAC()
    uint8_t inner[32], out[32];
    finishInto(inner);
    baseReset();
    update(_opad, sizeof(_opad));
    update(inner, sizeof(inner));
    finishInto(out);
    memcpy(hash, out, hashLen);
  }
};
