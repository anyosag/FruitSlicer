// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Binary output files that report every failure (a full disk or a pulled USB
// stick must not leave a silently truncated file).
//
// With `atomic`, data goes to "<path>.part" and only replaces `path` when
// commit() succeeds; if the object is destroyed first (an exception, a
// cancelled job) the partial file is deleted.

#include <cstdio>
#include <string>

namespace fruit {

class OutFile {
public:
    explicit OutFile(std::string path, bool atomic = false);
    ~OutFile();
    OutFile(const OutFile&) = delete;
    OutFile& operator=(const OutFile&) = delete;

    void write(const void* data, size_t bytes);
    void seek(long offset);  // absolute position
    // Flushes, forces the data onto the device (fsync; F_FULLFSYNC on macOS)
    // and closes. For atomic files, then renames over the final path.
    void commit(bool sync_to_device = false);

    const std::string& path() const { return path_; }

private:
    [[noreturn]] void fail(const char* what);
    void discard() noexcept;

    std::string path_;       // final path
    std::string write_path_; // path being written (".part" when atomic)
    std::FILE* f_ = nullptr;
};

}  // namespace fruit
