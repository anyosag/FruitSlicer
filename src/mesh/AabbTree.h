#pragma once
// Bounding volume hierarchy over a TriangleMesh for ray casts and
// closest-point queries (tip collision tests, local thickness, projecting
// support points onto the true surface).

#include <optional>
#include <vector>

#include "mesh/TriangleMesh.h"

namespace fruit {

struct RayHit {
    double t = 0;     // distance along the (unit) direction
    int32_t face = -1;
    Vec3 point;
};

struct ClosestPoint {
    double distance = 0;
    int32_t face = -1;
    Vec3 point;
};

class AabbTree {
public:
    AabbTree() = default;
    explicit AabbTree(const TriangleMesh& mesh);
    void build(const TriangleMesh& mesh);

    // Nearest hit with t in (t_min, t_max]. `dir` need not be normalised.
    std::optional<RayHit> raycast(const Vec3& origin, const Vec3& dir, double t_max = 1e30,
                                  double t_min = 1e-9) const;
    bool anyHit(const Vec3& origin, const Vec3& dir, double t_max, double t_min = 1e-9) const;
    ClosestPoint closest(const Vec3& p) const;

    const TriangleMesh* mesh() const { return mesh_; }

private:
    struct Node {
        BBox3 box;
        int32_t left = -1, right = -1;  // children; -1 for leaves
        int32_t begin = 0, end = 0;     // face range in order_ for leaves
    };
    int32_t buildNode(int32_t begin, int32_t end, std::vector<Vec3>& centroids, std::vector<BBox3>& boxes);

    const TriangleMesh* mesh_ = nullptr;
    std::vector<Node> nodes_;
    std::vector<int32_t> order_;
};

}  // namespace fruit
