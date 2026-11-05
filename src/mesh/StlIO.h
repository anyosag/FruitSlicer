#pragma once

#include <cstdio>
#include <string>

#include "mesh/TriangleMesh.h"

namespace fruit {

// Reads binary or ASCII STL (auto-detected) and welds identical vertices.
// Throws std::runtime_error on malformed input.
TriangleMesh readStl(const std::string& path);

// Streaming binary STL writer: triangles are written as they are added, so
// models with millions of support triangles never need to be held twice.
class StlWriter {
public:
    explicit StlWriter(const std::string& path);
    ~StlWriter();
    StlWriter(const StlWriter&) = delete;
    StlWriter& operator=(const StlWriter&) = delete;

    void add(const Vec3& a, const Vec3& b, const Vec3& c);
    void add(const TriangleMesh& mesh);
    void close();  // patches the triangle count; called by the destructor
    uint32_t triangleCount() const { return count_; }

private:
    std::FILE* file_ = nullptr;
    uint32_t count_ = 0;
};

void writeStl(const std::string& path, const TriangleMesh& mesh);

}  // namespace fruit
