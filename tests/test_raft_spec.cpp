// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/Raft.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

using namespace fruit;
using Catch::Approx;

namespace {

// Pillars standing on the raft at `xy`, joined as Builder::raft joins them (a minimum spanning tree).
SupportTree trunksAt(const std::vector<Vec2>& xy, double top = 1.0) {
    SupportTree t;
    t.raft_top = top;
    for (const Vec2& p : xy) {
        Pillar P;
        P.xy = p;
        P.bottom = top + 1.0;
        P.top = 6.0;
        P.diameter = 1.0;
        t.pillars.push_back(P);
    }
    const size_t n = xy.size();
    std::vector<double> best(n, 1e300);
    std::vector<int> parent(n, -1);
    std::vector<char> in(n, 0);
    best[0] = 0;
    for (size_t it = 0; it < n; ++it) {
        int u = -1;
        for (size_t i = 0; i < n; ++i)
            if (!in[i] && (u < 0 || best[i] < best[u])) u = static_cast<int>(i);
        in[u] = 1;
        for (size_t v = 0; v < n; ++v)
            if (!in[v] && norm(xy[u] - xy[v]) < best[v]) best[v] = norm(xy[u] - xy[v]), parent[v] = u;
    }
    for (size_t v = 1; v < n; ++v)
        t.raft_links.push_back({{xy[parent[v]].x, xy[parent[v]].y, top / 2}, {xy[v].x, xy[v].y, top / 2}, top * 0.9});
    return t;
}

// Two groups of trunks, as a part with two feet might have: lobes along every side.
std::vector<Vec2> twoGroups() {
    std::vector<Vec2> v;
    for (double x : {4.0, 9.0, 14.0})
        for (double y : {4.0, 9.333, 14.667, 20.0}) v.push_back({x, y});
    for (double x : {25.5, 30.5, 35.5})
        for (double y : {7.0, 12.0, 17.0}) v.push_back({x, y});
    return v;
}

// The trunks the demo bracket's supports stand on: a dense row of lobes along every side.
std::vector<Vec2> bracketTrunks() {
    return {{0, 11}, {6, -11}, {16, 0}, {-14, -9}, {-14, -6}, {-14, -3}, {-14, 0}, {-14, 3}, {-14, 6}, {-14, 9}, {-12, 9}, {-11, -9}, {-11, -6}, {-11, 1}, {-9, 9}, {-8, -9}, {-8, 0}, {-7, 6}, {-6, 4}, {-6, 9}, {-5, -9}, {-5, -3}, {-5, 9}, {-2, -9}, {-2, -2}, {-2, 9}, {-1, 2}, {1, -9}, {1, 9}, {1, -6}, {2, 9}, {3, 2}, {4, -9}, {4, 6}, {5, -1}, {5, 9}, {7, -9}, {7, 2}, {7, -3}, {7, -2}, {8, 9}, {9, -6}, {10, -9}, {10, 3}, {11, -9}, {11, 9}, {14, -6}, {14, -5}, {14, -2}, {14, 2}, {14, 5}, {14, 6}, {14, -1}, {14, -9}, {14, 9}, {-7.49, 10.95}, {3.5, -11}, {-3.5, 11}, {-6, -11}, {4.5, 11}, {-0, -11}, {-2.5, -11}, {2.25, 11}};
}

TreeConfig withSpec(const RaftSpec& s) {
    TreeConfig tc;
    applyRaftSpec(s, tc);
    return tc;
}

bool sameMesh(const TriangleMesh& a, const TriangleMesh& b) {
    if (a.vertices.size() != b.vertices.size() || a.faces.size() != b.faces.size()) return false;
    for (size_t i = 0; i < a.vertices.size(); ++i)
        if (a.vertices[i].x != b.vertices[i].x || a.vertices[i].y != b.vertices[i].y || a.vertices[i].z != b.vertices[i].z)
            return false;
    for (size_t i = 0; i < a.faces.size(); ++i)
        if (a.faces[i] != b.faces[i]) return false;
    return true;
}

const RaftSpec& preset(const char* name) {
    const RaftPreset* p = builtInRaftPreset(name);
    REQUIRE(p != nullptr);
    return p->spec;
}

bool covers(const Paths& p, const Vec2& at) {
    for (const ExPolygon& e : toExPolygons(p))
        if (e.contains(at)) return true;
    return false;
}

}  // namespace

TEST_CASE("raft presets: five built in, each saved and read back the same", "[raft]") {
    const auto& v = builtInRaftPresets();
    REQUIRE(v.size() == 5);
    CHECK(v[0].name == "Easy release");
    CHECK(builtInRaftPreset(kDefaultRaftPreset)->spec == leastResinSpec());
    for (const RaftPreset& p : v) CHECK(raftSpecFromJson(raftSpecToJson(p.spec)) == p.spec);
    CHECK(raftSpecFromJson(nlohmann::json::object()) == leastResinSpec());
    CHECK(builtInRaftPreset("Copy of Least resin") == nullptr);
}

TEST_CASE("raft side: the base tucks in under 90°, sticks out over it", "[raft]") {
    CHECK(sideInset(1.0, kPadsSideAngle) == -0.4);
    CHECK(sideInset(1.0, kFramedPadsSideAngle) == 0.4);
    CHECK(sideInset(2.0, 60) == Approx(1.1547).margin(1e-4));
    CHECK(sideInset(1.0, 135) == -1.0);
    CHECK(sideInset(2.0, 90) == 0.0);
}

TEST_CASE("raft: Least resin builds the old pads exactly", "[raft]") {
    const SupportTree t = trunksAt(twoGroups());
    const TreeConfig tc = withSpec(preset("Least resin"));
    CHECK_FALSE(tc.raft_spec.has_value());
    CHECK(tc.raft == RaftStyle::Pads);
    CHECK(sameMesh(t.mesh(tc), t.mesh(TreeConfig{})));
    CHECK(raftPastFoot(tc) == raftPastFoot(TreeConfig{}));
}

TEST_CASE("raft: Plain base builds the old Full slab exactly", "[raft]") {
    const SupportTree t = trunksAt(twoGroups());
    const TreeConfig tc = withSpec(preset("Plain base"));
    CHECK_FALSE(tc.raft_spec.has_value());
    TreeConfig old;
    old.raft = RaftStyle::Full;
    CHECK(tc.raft == RaftStyle::Full);
    CHECK(tc.full_raft_margin == old.full_raft_margin);
    CHECK(sameMesh(t.mesh(tc), t.mesh(old)));
}

TEST_CASE("raft: an old job's own raft values come back as the same raft", "[raft]") {
    const SupportTree t = trunksAt(twoGroups(), 1.5);
    for (bool full : {false, true}) {
        TreeConfig old;
        old.raft = full ? RaftStyle::Full : RaftStyle::Pads;
        old.raft_thickness = 1.5;
        old.link_width = 2.0;
        old.full_raft_margin = 2.0;
        old.full_raft_gap = 6.0;
        TreeConfig now;
        applyRaftSpec(legacyRaftSpec(full, old), now);
        CHECK_FALSE(now.raft_spec.has_value());
        CHECK(now.raft == old.raft);
        CHECK(now.raft_thickness == old.raft_thickness);
        CHECK(now.pad_brim == Approx(old.pad_brim).margin(1e-12));
        if (full) CHECK(now.full_raft_margin == Approx(old.full_raft_margin).margin(1e-12));
        else CHECK(now.link_width == old.link_width);
        CHECK(now.full_raft_gap == old.full_raft_gap);
        INFO("full: " << full);
        CHECK(sameMesh(t.mesh(now), t.mesh(old)));
    }
}

TEST_CASE("raft: a copy with one thing changed is built from its settings", "[raft]") {
    RaftSpec s = preset("Least resin");
    s.rounded = true;
    CHECK(withSpec(s).raft_spec.has_value());
    RaftSpec f = preset("Plain base");
    f.lip_height = 0.5;
    CHECK(withSpec(f).raft_spec.has_value());
    TreeConfig none;
    none.raft = RaftStyle::None;
    applyRaftSpec(preset("Easy release"), none);
    CHECK(none.raft == RaftStyle::None);
    CHECK_FALSE(none.raft_spec.has_value());
}

TEST_CASE("raft: Easy release tucks its base in, has a lip and windows at lobe tips", "[raft]") {
    const RaftSpec& s = preset("Easy release");
    const SupportTree t = trunksAt(twoGroups(), s.thickness);
    const TreeConfig tc = withSpec(s);
    REQUIRE(tc.raft_spec.has_value());
    const RaftShape shape = specRaftShape(t, tc);
    CHECK(shape.pieces == 1);
    CHECK(shape.height == Approx(s.thickness + s.lip_height));
    // The base is inside the top edge by the side's inset, all round.
    CHECK(area(shape.base) < area(shape.top));
    const double in = sideInset(s.thickness, s.side_angle);
    CHECK(area(shape.base) < area(offset(shape.top, -(in - 0.15))));
    CHECK(area(shape.base) > area(offset(shape.top, -(in + 0.15))) * 0.98);
    // Windows: at lobe tips, one of the trunk's disc, and at the plate the base is open there.
    REQUIRE(shape.windows.size() >= 4);
    for (const RaftWindow& w : shape.windows) {
        CHECK(w.radius == Approx(tc.base_diameter / 2 + s.reach));
        const Vec2 mouth = w.trunk + w.dir * (w.radius - in - 0.5);
        CHECK_FALSE(covers(shape.base, mouth));
    }
    CHECK(s.thickness - windowHeight(s) >= kMinWindowRoof);
    const TriangleMesh m = t.mesh(tc);
    CHECK(m.volume() > 0);
    CHECK(m.bbox().max.z >= s.thickness + s.lip_height - 1e-6);
    CHECK(m.bbox().min.z == Approx(0.0).margin(1e-9));
    CHECK(raftPastFoot(tc) == Approx(s.reach));
}

TEST_CASE("raft: fewer windows for Few than for every lobe tip", "[raft]") {
    RaftSpec s = preset("Easy release");
    const SupportTree t = trunksAt(twoGroups(), s.thickness);
    s.windows = RaftWindows::EveryTip;
    const size_t every = specRaftShape(t, withSpec(s)).windows.size();
    s.windows = RaftWindows::Few;
    const size_t few = specRaftShape(t, withSpec(s)).windows.size();
    s.windows = RaftWindows::None;
    CHECK(specRaftShape(t, withSpec(s)).windows.empty());
    CHECK(few >= 1);
    CHECK(few < every);
}

TEST_CASE("raft: Pads with frame is one piece inside its frame", "[raft]") {
    const RaftSpec& s = preset("Pads with frame");
    const SupportTree t = trunksAt(twoGroups(), s.thickness);
    const TreeConfig tc = withSpec(s);
    REQUIRE(tc.raft_spec.has_value());
    const RaftShape shape = specRaftShape(t, tc);
    CHECK(shape.pieces == 1);
    CHECK_FALSE(shape.top.empty());
    CHECK_FALSE(shape.windows.empty());
    CHECK(shape.height == Approx(s.frame_height + s.lip_height));
    // The frame is outside every pad.
    for (const Vec2& p : t.pillars.empty() ? std::vector<Vec2>{} : std::vector<Vec2>{t.pillars[0].xy})
        CHECK(covers(shape.top, p));
    CHECK(raftPastFoot(tc) == Approx(s.reach + s.frame_gap + s.frame_width));
    const TriangleMesh m = t.mesh(tc);
    CHECK(m.volume() > 0);
}

TEST_CASE("raft: plain pads without bars between them are separate pieces", "[raft]") {
    RaftSpec s = preset("Least resin");
    s.rounded = true;  // built from its settings
    SupportTree t = trunksAt({{0, 0}, {20, 0}});
    CHECK(specRaftShape(t, withSpec(s)).pieces == 1);  // the bar joins them
    t.raft_links.clear();
    CHECK(specRaftShape(t, withSpec(s)).pieces == 2);
}

TEST_CASE("raft: the edge's cut shape starts at the plate and reaches the lip's top", "[raft]") {
    const RaftSpec& s = preset("Easy release");
    const auto p = raftEdgeProfile(s, s.thickness);
    REQUIRE(p.size() > 4);
    CHECK(p.front().y == 0.0);
    CHECK(p.front().x == Approx(sideInset(s.thickness, s.side_angle)));
    double top = 0;
    for (const Vec2& q : p) top = std::max(top, q.y);
    CHECK(top == Approx(s.thickness + s.lip_height).margin(1e-6));
    const auto f = raftEdgeProfile(preset("Pads with frame"), 1.0);
    CHECK(f.back().x == Approx(3.6));
    CHECK(f.back().y == 0.0);
}

// Pieces of a layer above the first that rest on nothing in the layer below: they would start in mid-air.
int floatingPieces(const RaftShape& shape) {
    int n = 0;
    for (size_t k = 1; k < shape.layers.size(); ++k)
        for (const ExPolygon& e : toExPolygons(shape.layers[k]))
            if (std::abs(area(intersection(e.toPaths(), shape.layers[k - 1]))) < 1e-3) ++n;
    return n;
}

TEST_CASE("raft: every preset's raft starts on the plate, nothing in mid-air", "[raft]") {
    // As built, and with a window at every lobe tip, the edge wider, or both, on two layouts: a window beside a
    // lobe's valley once left a sliver that started 0.3 mm up. Layer by layer, as the builder makes them.
    for (const RaftPreset& p : builtInRaftPresets())
        for (int variant = 0; variant < 4; ++variant) {
            RaftSpec s = p.spec;
            if (variant & 1) s.windows = RaftWindows::EveryTip;
            if (variant & 2) s.reach += 0.7;
            TreeConfig tc = withSpec(s);
            tc.raft_spec = s;  // the old Pads and Full ones too
            for (const auto& layout : {twoGroups(), bracketTrunks()}) {
                const RaftShape shape = specRaftShape(trunksAt(layout, s.thickness), tc);
                INFO(p.name << ", variant " << variant << ", " << layout.size() << " trunks");
                REQUIRE(shape.layers.size() > 1);
                CHECK(floatingPieces(shape) == 0);
            }
        }
    // And once as the job's own check sees it: the raft built, sliced, and its islands looked for.
    RaftSpec s = builtInRaftPreset("Easy release")->spec;
    s.windows = RaftWindows::EveryTip;
    s.reach += 0.7;
    const TriangleMesh m = trunksAt(bracketTrunks(), s.thickness).mesh(withSpec(s));
    const ValidationReport v = validateSupported(TriangleMesh{}, m, 0.05, 0.0);
    for (const auto& f : v.floating)
        UNSCOPED_INFO("floating at z " << f.z << " (" << f.at.x << ", " << f.at.y << "), " << f.area << " mm²");
    CHECK(v.floating.empty());
}
