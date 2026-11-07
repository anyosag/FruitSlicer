// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "core/OutFile.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fruit {

OutFile::OutFile(std::string path, bool atomic) : path_(std::move(path)) {
    write_path_ = atomic ? path_ + ".part" : path_;
    f_ = std::fopen(write_path_.c_str(), "wb");
    if (!f_) throw std::runtime_error("cannot write " + path_ + ": " + std::strerror(errno));
}

OutFile::~OutFile() { discard(); }

void OutFile::fail(const char* what) {
    const int e = errno;
    discard();
    throw std::runtime_error(std::string(what) + " " + path_ + ": " + (e ? std::strerror(e) : "I/O error"));
}

void OutFile::discard() noexcept {
    if (!f_) return;
    std::fclose(f_);
    f_ = nullptr;
    std::error_code ec;
    std::filesystem::remove(write_path_, ec);
}

void OutFile::write(const void* data, size_t bytes) {
    if (!f_) throw std::logic_error("write after commit: " + path_);
    if (bytes && std::fwrite(data, 1, bytes, f_) != bytes) fail("cannot write");
}

void OutFile::seek(long offset) {
    if (!f_) throw std::logic_error("seek after commit: " + path_);
    if (std::fseek(f_, offset, SEEK_SET) != 0) fail("cannot seek in");
}

void OutFile::commit(bool sync_to_device) {
    if (!f_) throw std::logic_error("commit twice: " + path_);
    if (std::fflush(f_) != 0) fail("cannot write");
#if !defined(_WIN32)
    if (sync_to_device) {
        const int fd = fileno(f_);
#if defined(F_FULLFSYNC)
        if (fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0) fail("cannot flush");
#else
        if (fsync(fd) != 0) fail("cannot flush");
#endif
    }
#else
    (void)sync_to_device;
#endif
    std::FILE* f = f_;
    f_ = nullptr;
    if (std::fclose(f) != 0) {
        const int e = errno;
        std::error_code ec;
        std::filesystem::remove(write_path_, ec);
        throw std::runtime_error("cannot write " + path_ + ": " + (e ? std::strerror(e) : "I/O error"));
    }
    if (write_path_ != path_) {
        std::error_code ec;
        std::filesystem::rename(write_path_, path_, ec);
        if (ec) {
            std::filesystem::remove(write_path_, ec);
            throw std::runtime_error("cannot replace " + path_ + ": " + ec.message());
        }
    }
}

}  // namespace fruit
