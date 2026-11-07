// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once

#include <string>

#include "core/OutFile.h"
#include "mesh/TriangleMesh.h"

namespace fruit {

// Reads binary or ASCII STL (auto-detected) and welds identical vertices.
// Throws std::runtime_error on malformed input.
TriangleMesh readStl(const std::string& path);

// Streaming binary STL writer: triangles are written as they are added, so
// models with millions of support triangles never need to be held twice.
// Data goes to "<path>.part"; close() patches the triangle count and puts the
// file in place. A writer destroyed without close() deletes the partial file.
class StlWriter {
public:
    explicit StlWriter(const std::string& path);
    StlWriter(const StlWriter&) = delete;
    StlWriter& operator=(const StlWriter&) = delete;

    void add(const Vec3& a, const Vec3& b, const Vec3& c);
    void add(const TriangleMesh& mesh);
    void close(bool sync_to_device = false);
    uint32_t triangleCount() const { return count_; }

private:
    OutFile out_;
    uint32_t count_ = 0;
};

void writeStl(const std::string& path, const TriangleMesh& mesh);

}  // namespace fruit
