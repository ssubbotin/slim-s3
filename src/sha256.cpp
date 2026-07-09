#include "sha256.hpp"

#include <algorithm>
#include <cstring>

namespace slims3::detail {
namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Ctx {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint8_t buf[64];
    std::uint64_t totalLen = 0;  // bytes
    std::size_t bufLen = 0;
};

void compress(Ctx& c, const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
               (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    std::uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        std::uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

void update(Ctx& c, const void* data, std::size_t len) {
    auto* p = static_cast<const std::uint8_t*>(data);
    c.totalLen += len;
    if (c.bufLen != 0) {
        std::size_t take = std::min(len, std::size_t(64) - c.bufLen);
        std::memcpy(c.buf + c.bufLen, p, take);
        c.bufLen += take;
        p += take;
        len -= take;
        if (c.bufLen == 64) {
            compress(c, c.buf);
            c.bufLen = 0;
        }
    }
    while (len >= 64) {
        compress(c, p);
        p += 64;
        len -= 64;
    }
    if (len != 0) {
        std::memcpy(c.buf, p, len);
        c.bufLen = len;
    }
}

std::array<std::uint8_t, 32> finish(Ctx& c) {
    std::uint64_t bits = c.totalLen * 8;
    std::uint8_t pad = 0x80;
    update(c, &pad, 1);
    std::uint8_t zero = 0;
    while (c.bufLen != 56) update(c, &zero, 1);
    std::uint8_t lenBe[8];
    for (int i = 0; i < 8; ++i) lenBe[i] = std::uint8_t(bits >> (56 - 8 * i));
    // update() counts these bytes into totalLen, but bits was captured first.
    update(c, lenBe, 8);
    std::array<std::uint8_t, 32> out;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = std::uint8_t(c.h[i] >> (24 - 8 * j));
    return out;
}

}  // namespace

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t len) {
    Ctx c;
    update(c, data, len);
    return finish(c);
}

std::array<std::uint8_t, 32> hmacSha256(const void* key, std::size_t keyLen, const void* msg,
                                        std::size_t msgLen) {
    std::uint8_t k[64] = {0};
    if (keyLen > 64) {
        auto kh = sha256(key, keyLen);
        std::memcpy(k, kh.data(), 32);
    } else {
        std::memcpy(k, key, keyLen);
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    Ctx inner;
    update(inner, ipad, 64);
    update(inner, msg, msgLen);
    auto ih = finish(inner);
    Ctx outer;
    update(outer, opad, 64);
    update(outer, ih.data(), 32);
    return finish(outer);
}

std::string toHex(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i] = d[p[i] >> 4];
        out[2 * i + 1] = d[p[i] & 0xf];
    }
    return out;
}

std::string sha256Hex(std::string_view data) {
    auto h = sha256(data.data(), data.size());
    return toHex(h.data(), h.size());
}

}  // namespace slims3::detail
