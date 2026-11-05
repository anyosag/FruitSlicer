#include <cmath>
#include <stdexcept>

#include "support/SupportConfig.h"
#include "support/SupportPoint.h"

namespace fruit {

double CoverageModel::radius(double dz) const {
    if (curve.empty()) return 0;
    double r;
    if (dz <= curve.front().first) r = curve.front().second;
    else if (dz >= curve.back().first) r = curve.back().second;
    else {
        r = curve.back().second;
        for (size_t i = 1; i < curve.size(); ++i)
            if (dz <= curve[i].first) {
                const auto& [z0, r0] = curve[i - 1];
                const auto& [z1, r1] = curve[i];
                r = r0 + (r1 - r0) * (dz - z0) / (z1 - z0);
                break;
            }
    }
    return r / std::sqrt(std::max(density, 1e-6));
}

double CapacityModel::capacity(double d) const {
    const double s = d / reference_diameter;
    return reference_capacity * s * s / std::max(safety, 1e-6);
}

const char* categoryName(PointCategory c) {
    switch (c) {
        case PointCategory::Island: return "island";
        case PointCategory::Overhang: return "overhang";
        case PointCategory::Capacity: return "capacity";
        case PointCategory::Stabilization: return "stabilization";
        case PointCategory::Manual: return "manual";
    }
    return "?";
}

PointCategory categoryFromName(const std::string& s) {
    if (s == "island") return PointCategory::Island;
    if (s == "overhang") return PointCategory::Overhang;
    if (s == "capacity") return PointCategory::Capacity;
    if (s == "stabilization") return PointCategory::Stabilization;
    if (s == "manual") return PointCategory::Manual;
    throw std::runtime_error("unknown point category '" + s + "'");
}

uint64_t stablePointId(const Vec3& p, PointCategory c) {
    // FNV-1a over positions quantised to 1 µm: identical across runs and
    // compilers for identical geometry.
    const int64_t q[3] = {std::llround(p.x * 1000), std::llround(p.y * 1000), std::llround(p.z * 1000)};
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ULL;
        }
    };
    for (int64_t v : q) mix(static_cast<uint64_t>(v));
    mix(static_cast<uint64_t>(c));
    return h & 0x7FFFFFFFFFFFFFFFULL;  // fits a JSON integer comfortably
}

}  // namespace fruit
