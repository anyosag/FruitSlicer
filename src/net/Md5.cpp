// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "net/Md5.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace fruit::net {
namespace {

constexpr std::uint32_t kK[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline std::uint32_t rotl(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

std::string toHex(const std::array<std::uint8_t, 16>& d) {
    static const char* digits = "0123456789abcdef";
    std::string out(32, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 0xF];
    }
    return out;
}

}  // namespace

Md5::Md5() : state_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476} {}

void Md5::block(const std::uint8_t* p) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = std::uint32_t(p[4 * i]) | (std::uint32_t(p[4 * i + 1]) << 8) |
               (std::uint32_t(p[4 * i + 2]) << 16) | (std::uint32_t(p[4 * i + 3]) << 24);
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + kK[i] + m[g], kS[i]);
        a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(const void* data, std::size_t size) {
    if (finished_) throw std::logic_error("Md5::update after digest");
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t used = std::size_t(length_ % 64);
    length_ += size;
    if (used) {
        const std::size_t take = std::min(size, 64 - used);
        std::memcpy(buffer_.data() + used, p, take);
        p += take;
        size -= take;
        used += take;
        if (used < 64) return;
        block(buffer_.data());
    }
    while (size >= 64) {
        block(p);
        p += 64;
        size -= 64;
    }
    if (size) std::memcpy(buffer_.data(), p, size);
}

std::array<std::uint8_t, 16> Md5::digest() {
    if (!finished_) {
        const std::uint64_t bits = length_ * 8;
        const std::uint8_t pad0 = 0x80;
        update(&pad0, 1);
        const std::uint8_t zero = 0;
        while (length_ % 64 != 56) update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = std::uint8_t(bits >> (8 * i));
        update(len, 8);
        finished_ = true;
    }
    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = std::uint8_t(state_[i] >> (8 * j));
    return out;
}

std::string Md5::hexDigest() { return toHex(digest()); }

std::string Md5::hex(std::string_view data) {
    Md5 m;
    m.update(data);
    return m.hexDigest();
}

std::string Md5::hexOfFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    Md5 m;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        const auto n = in.gcount();
        if (n > 0) m.update(buf.data(), std::size_t(n));
    }
    if (in.bad()) throw std::runtime_error("read error on " + path.string());
    return m.hexDigest();
}

}  // namespace fruit::net
