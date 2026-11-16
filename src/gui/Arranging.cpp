// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Arranging.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

#include "Document.h"
#include "Format.h"
#include "mesh/ModelCheck.h"
#include "mesh/Placement.h"
#include "support/SupportPoint.h"

namespace gui {

namespace {

// Volume and where it is centred, seen from above (signed tetrahedra from the origin).
void loadOf(const fruit::TriangleMesh& m, double& volume, fruit::Vec2& at) {
    double v = 0, cx = 0, cy = 0;
    for (const fruit::Face& f : m.faces) {
        const fruit::Vec3 &a = m.vertices[f[0]], &b = m.vertices[f[1]], &c = m.vertices[f[2]];
        const double t = (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6;
        v += t;
        cx += t * (a.x + b.x + c.x) / 4;
        cy += t * (a.y + b.y + c.y) / 4;
    }
    volume = std::abs(v);
    if (std::abs(v) > 1e-9) {
        at = {cx / v, cy / v};
    } else {
        const fruit::BBox3 bb = m.bbox();
        at = {(bb.min.x + bb.max.x) / 2, (bb.min.y + bb.max.y) / 2};
    }
}

std::optional<fruit::Vec2> hingeOf(const SupportChoices& c) {
    if (!c.hinge) return std::nullopt;
    switch (*c.hinge) {
        case fruit::HingeSide::PlusX: return fruit::Vec2{1, 0};
        case fruit::HingeSide::MinusX: return fruit::Vec2{-1, 0};
        case fruit::HingeSide::PlusY: return fruit::Vec2{0, 1};
        case fruit::HingeSide::MinusY: return fruit::Vec2{0, -1};
    }
    return std::nullopt;
}

// Where a part goes in a layout.
fruit::Placement placedBy(const Part& p, const fruit::ops::NestPlacement& np) {
    fruit::Placement q = p.placement();
    q.rotation_deg.z = std::remainder(q.rotation_deg.z + np.turn_deg, 360.0);
    q.offset = np.offset;
    return q;
}

// The plate from above with the parts where a layout puts them.
JobPicture drawLayout(const Document& doc, const LayoutSearch& s, const fruit::ops::NestLayout& L) {
    constexpr int W = 568, H = 320;
    JobPicture pic;
    pic.w = W, pic.h = H;
    pic.rgba.assign(static_cast<std::size_t>(W) * H * 4, 255);
    auto put = [&](int x, int y, unsigned rgb) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        std::uint8_t* p = &pic.rgba[4 * (static_cast<std::size_t>(y) * W + x)];
        p[0] = static_cast<std::uint8_t>(rgb >> 16), p[1] = static_cast<std::uint8_t>(rgb >> 8), p[2] = static_cast<std::uint8_t>(rgb);
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) put(x, y, 0x0E1014);
    const double pw = doc.printer.build_x, pd = doc.printer.build_y;
    const double k = std::min((W - 24) / pw, (H - 24) / pd);  // px per mm
    const double ox = W / 2.0, oy = H / 2.0;
    auto px = [&](double x) { return ox + x * k; };
    auto py = [&](double y) { return oy - y * k; };  // the back of the plate is up
    for (int y = static_cast<int>(py(pd / 2)); y <= static_cast<int>(py(-pd / 2)); ++y)
        for (int x = static_cast<int>(px(-pw / 2)); x <= static_cast<int>(px(pw / 2)); ++x) put(x, y, 0x1A1E25);
    if (s.hinge) {
        const auto h = hingeOf(doc.supports.choices);
        for (int t = 0; t < 4 && h; ++t) {
            if (std::abs(h->y) > 0.5) {
                const int y = static_cast<int>(py(h->y * pd / 2)) + (h->y > 0 ? t : -t);
                for (int x = static_cast<int>(px(-pw / 2)); x <= static_cast<int>(px(pw / 2)); ++x) put(x, y, 0xFF8F3F);
            } else {
                const int x = static_cast<int>(px(h->x * pw / 2)) + (h->x > 0 ? -t : t);
                for (int y = static_cast<int>(py(pd / 2)); y <= static_cast<int>(py(-pd / 2)); ++y) put(x, y, 0xFF8F3F);
            }
        }
    }
    static const unsigned kShades[] = {0xAEB6C4, 0x8FB3D9, 0xD9B88F, 0x9FCFA3, 0xC4A6D9, 0xD9CF8F, 0x8FD0D0, 0xD99F9F};
    for (std::size_t i = 0; i < s.part_ids.size() && i < L.parts.size(); ++i) {
        const Part* p = doc.findPart(s.part_ids[i]);
        if (!p) continue;
        const fruit::TriangleMesh m =
            L.parts[i].placed ? fruit::placeModel(p->mesh(), placedBy(*p, L.parts[i])) : p->placed();
        const unsigned rgb = L.parts[i].placed ? kShades[i % 8] : 0xE5484D;  // left where it was: red
        for (const fruit::Face& f : m.faces) {
            double X[3], Y[3];
            for (int c = 0; c < 3; ++c) X[c] = px(m.vertices[f[c]].x), Y[c] = py(m.vertices[f[c]].y);
            const double area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
            if (std::abs(area) < 1e-9) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({X[0], X[1], X[2]}))));
            const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({X[0], X[1], X[2]}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({Y[0], Y[1], Y[2]}))));
            const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({Y[0], Y[1], Y[2]}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double cx = x + 0.5, cy = y + 0.5;
                    const double w0 = ((X[1] - cx) * (Y[2] - cy) - (X[2] - cx) * (Y[1] - cy)) / area;
                    const double w1 = ((X[2] - cx) * (Y[0] - cy) - (X[0] - cx) * (Y[2] - cy)) / area;
                    if (w0 >= 0 && w1 >= 0 && 1 - w0 - w1 >= 0) put(x, y, rgb);
                }
        }
    }
    return pic;
}

}  // namespace

LayoutSearch findLayouts(const Document& doc, double gap, bool turn, const fruit::JobControl& ctl) {
    const auto t0 = std::chrono::steady_clock::now();
    LayoutSearch s;
    s.revision = doc.revision();
    s.gap = gap;
    const SupportState& sup = doc.supports;
    const bool fresh = sup.result && sup.fresh(doc.revision(), doc.supportSettings());
    s.supports = fresh;
    const fruit::TreeConfig tc = fresh ? sup.result->tree_cfg : treeConfig(sup.choices);
    // How far a raft reaches past the foot of a support, at its widest (a frame's too).
    const double past_foot = fruit::raftPastFoot(tc);
    std::vector<fruit::ops::NestPart> parts;
    double margin = 0;
    for (const Part& p : doc.parts()) {
        s.part_ids.push_back(p.id());
        fruit::ops::NestPart np;
        np.mesh = &p.placed();
        loadOf(*np.mesh, np.load, np.load_at);
        const OrientState& o = doc.orientOf(p.id());
        if (o.nowFresh(p.revision())) np.big_layer = o.now.biggest_layer;
        np.may_turn = turn;
        // Without its supports made for it here, a raft reaches past its outline by about a foot's half and the rest.
        if (!fresh && !p.placement().on_plate) np.grow = tc.base_diameter / 2 + past_foot;
        margin = std::max(margin, fruit::plateMargin(p.placement()));
        parts.push_back(std::move(np));
    }
    // Each support's foot and pad, with the part it holds.
    if (fresh) {
        const SupportResult& r = *sup.result;
        for (const fruit::Pillar& P : r.tree.pillars) {
            if (P.on_model || P.merged) continue;
            const int id = doc.partNear({P.xy.x, P.xy.y, P.top});
            const auto it = std::find(s.part_ids.begin(), s.part_ids.end(), id);
            if (it == s.part_ids.end()) continue;
            const double foot = std::max(P.base_diameter > 0 ? P.base_diameter : tc.base_diameter, P.diameter);
            parts[static_cast<std::size_t>(it - s.part_ids.begin())].disks.push_back({P.xy, foot / 2 + past_foot});
        }
    }
    fruit::ops::NestOptions no;
    no.plate_width = doc.printer.build_x;
    no.plate_depth = doc.printer.build_y;
    no.edge_margin = margin;
    no.gap = std::max(0.0, gap);
    no.turn = turn;
    no.hinge = hingeOf(sup.choices);
    s.hinge = no.hinge.has_value();
    const std::vector<fruit::ops::NestLayout> all = fruit::ops::nestLayouts(parts, no, ctl.sub(0.0, 0.85));
    s.candidates = all.size();
    for (fruit::ops::NestPick& pk : fruit::ops::pickLayouts(all))
        s.choices.push_back({pk.title, std::move(pk.also), std::move(pk.layout), {}});
    ctl.step("pictures", 0.85);
    for (LayoutChoice& c : s.choices) c.picture = drawLayout(doc, s, c.layout);
    s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ctl.report("pictures", 1.0);
    return s;
}

ToolReport applyLayout(const Document& doc, PartsEdit& e, const LayoutSearch& s, int choice) {
    ToolReport r;
    if (choice < 0 || choice >= static_cast<int>(s.choices.size())) return r;
    const LayoutChoice& c = s.choices[static_cast<std::size_t>(choice)];
    r.wanted = static_cast<int>(s.part_ids.size());
    std::vector<std::string> left_out;
    int turned = 0;
    for (std::size_t i = 0; i < s.part_ids.size() && i < c.layout.parts.size(); ++i) {
        const Part* p = doc.findPart(s.part_ids[i]);
        PartState* st = e.find(s.part_ids[i]);
        if (!p || !st) continue;
        const fruit::ops::NestPlacement& np = c.layout.parts[i];
        if (!np.placed) {
            left_out.push_back(p->name());
            continue;
        }
        const fruit::Placement q = placedBy(*p, np);
        if (std::abs(std::remainder(q.rotation_deg.z - st->placement.rotation_deg.z, 360.0)) > 1e-6) ++turned;
        if (!(q == st->placement)) r.changed = true;
        st->placement = q;
        ++r.done;
    }
    const std::string turned_words =
        turned == 0 ? "" : turned == 1 ? " 1 was turned." : " " + std::to_string(turned) + " were turned.";
    if (left_out.empty())
        r.message = std::string(c.title == "Recommended" ? "Laid out as recommended"
                                : c.title == "Another way"  ? "Laid out another way"
                                                            : "Laid out the " + c.title + " way") +
                    ": all " + plural(r.wanted, "part", "parts") + " fit." + turned_words;
    else
        r.message = std::to_string(r.done) + " of " + std::to_string(r.wanted) + " parts fit. " + namesOf(left_out) +
                    (left_out.size() == 1 ? " has" : " have") + " no room and stay" + (left_out.size() == 1 ? "s" : "") +
                    " where " + (left_out.size() == 1 ? "it is" : "they are") + "." + turned_words;
    return r;
}

MovedTips moveTips(const Document& before, const Document& after, const SupportResult& r) {
    MovedTips m;
    // How much harder the peel pulls at a point: the planner's own ramp across the plate.
    auto peel = [&](const fruit::Vec3& p) {
        const fruit::SupportConfig& cfg = r.cfg;
        if (cfg.peel_bias <= 0 || (cfg.peel_axis.x == 0 && cfg.peel_axis.y == 0)) return 1.0;
        const fruit::Vec2 a = fruit::normalized(cfg.peel_axis);
        const double half = std::max(std::abs(a.x) * cfg.plate_half.x + std::abs(a.y) * cfg.plate_half.y, 1.0);
        const double t = std::clamp((p.x * a.x + p.y * a.y) / half, -1.0, 1.0);
        return 1.0 + cfg.peel_bias * 0.5 * (t + 1.0);
    };
    for (const Part& p : after.parts()) {
        const Part* b = before.findPart(p.id());
        if (!b) {
            m.why = "a part was added";
            return m;
        }
        const fruit::Placement &q = p.placement(), &o = b->placement();
        if (q.rotation_deg.x != o.rotation_deg.x || q.rotation_deg.y != o.rotation_deg.y || q.scale != o.scale ||
            q.mirror_x != o.mirror_x || q.mirror_y != o.mirror_y || q.mirror_z != o.mirror_z || q.lift() != o.lift() ||
            q.on_plate != o.on_plate || p.geometryId() != b->geometryId()) {
            m.why = p.name() + " changed more than where it stands";
            return m;
        }
    }
    if (before.partCount() != after.partCount()) {
        m.why = "a part was removed";
        return m;
    }
    auto moved = [&](const fruit::Vec3& at, const fruit::Vec3* normal, fruit::Vec3& out, fruit::Vec3* out_n) {
        const int id = before.partNear(at);
        const Part* b = before.findPart(id);
        const Part* a = after.findPart(id);
        if (!a || !b) return false;
        const fruit::Transform t = b->transform().inverse().then(a->transform());
        out = t.apply(at);
        if (normal && out_n) *out_n = fruit::normalized(t.applyLinear(*normal));
        return true;
    };
    for (const fruit::SupportPoint& pt : r.planned) {
        fruit::SupportPoint q = pt;
        if (!moved(pt.pos, &pt.normal, q.pos, &q.normal)) {
            m.why = "a tip is on no part";
            return m;
        }
        if (peel(q.pos) > peel(pt.pos) * (1 + kMovedTipsSlack)) {
            m.why = "some parts moved further from the vat's hinge, where the peel pulls harder than their tips were planned for";
            return m;
        }
        q.id = fruit::stablePointId(q.pos, q.category);
        m.plan.points.push_back(q);
    }
    for (const fruit::Warning& w : r.plan_warnings) {
        fruit::Warning v = w;
        fruit::Vec3 at;
        if (moved(w.where, nullptr, at, nullptr)) v.where = at;
        m.plan.warnings.push_back(v);
    }
    m.plan.edits = r.plan_edits;
    m.plan.held_min = r.held_min;
    m.plan.held_max = r.held_max;
    m.ok = true;
    return m;
}

}  // namespace gui
