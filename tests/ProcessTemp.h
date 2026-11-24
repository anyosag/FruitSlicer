// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The tests' temporary folder: one of this test process's own. ctest runs the
// cases side by side, each in a process of its own, so a file one case writes
// must never be one that another case is reading, rewriting or deleting
// (Windows refuses to replace or delete a file that is still open, and even
// elsewhere two writers of one name can trip over each other's ".part" file).
// The folder is removed when the process ends.

#include <filesystem>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace testfiles {

// <system temporary folder>/fruit_tests_<process id>, made on first use.
inline const std::filesystem::path& temp() {
    struct Folder {
        std::filesystem::path path;
        Folder() {
#ifdef _WIN32
            const int pid = _getpid();
#else
            const int pid = static_cast<int>(getpid());
#endif
            path = std::filesystem::temp_directory_path() / ("fruit_tests_" + std::to_string(pid));
            std::filesystem::create_directories(path);
        }
        ~Folder() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };
    static const Folder folder;
    return folder.path;
}

}  // namespace testfiles
