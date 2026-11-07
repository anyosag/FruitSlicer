// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// MD5 (RFC 1321). The SDCP upload sends the whole file's MD5 with every
// chunk (field "S-File-MD5") so the printer can verify the transfer; MD5 is
// used here only as that checksum, not for anything security-related.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fruit::net {

class Md5 {
public:
    Md5();
    void update(const void* data, std::size_t size);
    void update(std::string_view s) { update(s.data(), s.size()); }
    // Finishes the digest; the object must not be updated afterwards.
    std::array<std::uint8_t, 16> digest();
    // Lower-case hex, 32 characters.
    std::string hexDigest();

    static std::string hex(std::string_view data);
    // Streams the file; throws std::runtime_error if it cannot be read.
    static std::string hexOfFile(const std::filesystem::path& path);

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 4> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t length_ = 0;  // bytes
    bool finished_ = false;
};

}  // namespace fruit::net
