// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Parts.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>

namespace gui {

int newGeometryId();  // Document.cpp

std::shared_ptr<const PartModel> makePartModel(fruit::LoadedModel m) {
    auto p = std::make_shared<PartModel>();
    p->check = fruit::checkMesh(m);
    p->model = std::move(m);
    p->geometry_id = newGeometryId();
    return p;
}

const fruit::FlatFaces& PartModel::flatFaces() const {
    std::call_once(flat_once_, [this] { flat_ = fruit::findFlatFaces(model.mesh); });
    return flat_;
}

void Part::update(const fruit::PrinterProfile& printer) {
    const fruit::TriangleMesh& m = mesh();
    transform_ = fruit::placementTransform(m, s_.placement);
    box_ = fruit::BBox3{};
    for (const fruit::Vec3& v : m.vertices) box_.extend(transform_.apply(v));
    fit_ = fruit::checkFit(m, s_.placement, printer);
    placed_.reset();
    footprint_.reset();
}

const fruit::TriangleMesh& Part::placed() const {
    if (!placed_) {
        auto m = std::make_shared<fruit::TriangleMesh>(mesh());
        m->transform(transform_);
        placed_ = std::move(m);
    }
    return *placed_;
}

namespace {

// Andrew's monotone chain: the convex hull, counter-clockwise, no repeats.
std::vector<fruit::Vec2> hull(std::vector<fruit::Vec2> pts) {
    std::sort(pts.begin(), pts.end(), [](const fruit::Vec2& a, const fruit::Vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    std::vector<fruit::Vec2> h(2 * pts.size());
    std::size_t k = 0;
    auto turn = [](const fruit::Vec2& o, const fruit::Vec2& a, const fruit::Vec2& b) {
        return fruit::cross(a - o, b - o);
    };
    for (const fruit::Vec2& p : pts) {
        while (k >= 2 && turn(h[k - 2], h[k - 1], p) <= 0) --k;
        h[k++] = p;
    }
    for (std::size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && turn(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    return h;
}

}  // namespace

const std::vector<fruit::Vec2>& Part::footprint() const {
    if (!footprint_) {
        const fruit::TriangleMesh& m = placed();
        std::vector<fruit::Vec2> pts;
        pts.reserve(m.vertices.size());
        for (const fruit::Vec3& v : m.vertices) pts.push_back({v.x, v.y});
        footprint_ = std::make_shared<const std::vector<fruit::Vec2>>(hull(std::move(pts)));
    }
    return *footprint_;
}

fruit::BBox2 Part::footprintBox() const {
    fruit::BBox2 b;
    if (!box_.empty()) {
        b.extend(fruit::Vec2{box_.min.x, box_.min.y});
        b.extend(fruit::Vec2{box_.max.x, box_.max.y});
    }
    return b;
}

PartState* PartsEdit::find(int id) {
    for (PartState& p : parts_)
        if (p.id == id) return &p;
    return nullptr;
}

int PartsEdit::add(NewPart p) {
    PartState s;
    s.id = next_id_++;
    s.name = std::move(p.name);
    s.path = std::move(p.path);
    s.model = std::move(p.model);
    s.placement = p.placement;
    parts_.push_back(std::move(s));
    return parts_.back().id;
}

bool PartsEdit::remove(int id) {
    const auto n = parts_.size();
    std::erase_if(parts_, [id](const PartState& p) { return p.id == id; });
    return parts_.size() != n;
}

const fruit::Transform* PartFrames::find(int part) const {
    for (const auto& [id, t] : frames_)
        if (id == part) return &t;
    return all_ ? &*all_ : nullptr;
}

fruit::Vec2 freeSpot(const std::vector<fruit::BBox2>& taken, fruit::Vec2 size, const fruit::PrinterProfile& printer,
                     double margin, double gap) {
    fruit::BBox2 all;
    for (const fruit::BBox2& b : taken) all.extend(b);
    if (all.empty()) return {0, 0};
    const fruit::Vec2 mid = all.center();
    // How far the middle may go from the plate's centre, within the margins.
    const double rx = (printer.build_x - 2 * margin - size.x) / 2, ry = (printer.build_y - 2 * margin - size.y) / 2;
    const auto clear = [&](const fruit::Vec2& c) {
        fruit::BBox2 b;
        b.extend(c - size * 0.5);
        b.extend(c + size * 0.5);
        for (const fruit::BBox2& t : taken)
            if (t.overlaps(b, gap - 1e-9)) return false;
        return true;
    };
    std::optional<fruit::Vec2> best;
    double best_d = 0;
    if (rx >= 0 && ry >= 0) {
        // Every whole millimetre the middle can stand at; the nearest free
        // one wins, the one further right (then further forward) on a tie.
        // Front to back counts double, so parts go side by side in a row
        // along the plate's long side before they start another.
        constexpr double kStep = 1.0;
        for (double y = -std::floor(ry / kStep) * kStep; y <= ry + 1e-9; y += kStep)
            for (double x = -std::floor(rx / kStep) * kStep; x <= rx + 1e-9; x += kStep) {
                const fruit::Vec2 c{x, y};
                const double d = std::hypot(x - mid.x, 2 * (y - mid.y));
                const bool better = !best || d < best_d - 1e-9 ||
                                    (std::abs(d - best_d) <= 1e-9 && (x > best->x || (x == best->x && y < best->y)));
                if (better && clear(c)) {
                    best = c;
                    best_d = d;
                }
            }
    }
    if (best) return *best;
    return {all.max.x + gap + size.x / 2, mid.y};
}

std::string partName(const std::string& path, const std::vector<std::string>& taken) {
    std::string base = std::filesystem::path(path).stem().string();
    if (base.empty()) base = "Part";
    const auto used = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
    if (!used(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!used(n)) return n;
    }
}

}  // namespace gui
