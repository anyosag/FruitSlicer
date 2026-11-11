// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "PartsTools.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "Format.h"
#include "mesh/ModelLoad.h"
#include "ops/Arrange.h"
#include "ops/MeshOps.h"

namespace gui {

namespace {

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

fruit::Vec2 centreOf(const fruit::BBox2& b) { return b.center(); }

std::string namesOf(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? (i + 1 == v.size() ? " and " : ", ") : "") + v[i];
    return s;
}

// A placement for a mesh that is already in printer coordinates (joined
// parts): it stays where it is, at the job's lift.
fruit::Placement inPlace(const fruit::TriangleMesh& mesh, const fruit::Placement& job) {
    fruit::Placement p;
    p.elevation = job.elevation;
    p.on_plate = job.on_plate;
    const fruit::BBox3 b = mesh.bbox();
    if (!b.empty()) p.offset = {b.center().x, b.center().y};
    return p;
}

struct Region {
    double x0, y0, x1, y1;  // where a footprint may lie, plate coordinates
    int side;               // 0 left, 1 right, 2 front, 3 back: which side of the others it is on
};

}  // namespace

std::string numberedName(const std::string& base_in, const std::vector<std::string>& taken) {
    std::string base = base_in;
    const std::size_t sp = base.find_last_of(' ');
    if (sp != std::string::npos && sp + 1 < base.size() &&
        std::all_of(base.begin() + static_cast<std::ptrdiff_t>(sp) + 1, base.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; }))
        base.erase(sp);
    const auto used = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!used(n)) return n;
    }
}

ToolReport duplicatePart(const Document& doc, PartsEdit& e, int copies, double gap) {
    ToolReport r;
    copies = std::clamp(copies, 1, kMaxCopies);
    r.wanted = copies;
    const Part& src = doc.part();
    const fruit::PrinterProfile& pr = doc.printer;
    gap = std::max(0.0, gap);
    double margin = 0;
    fruit::BBox2 taken;
    for (const Part& p : doc.parts()) {
        taken.extend(p.footprintBox());
        margin = std::max(margin, fruit::plateMargin(p.placement()));
    }
    const fruit::BBox2 mine = src.footprintBox();
    const fruit::ops::Footprint fp{mine.size().x, mine.size().y};
    const double hx = pr.build_x / 2 - margin, hy = pr.build_y / 2 - margin;
    // The room left, as four strips round everything on the plate: left and
    // right of it (full depth), in front of and behind it (between those).
    // Each is filled by arrange, then its block is pushed up to the others.
    std::vector<Region> regions = {{-hx, -hy, taken.min.x - gap, hy, 0},
                                   {taken.max.x + gap, -hy, hx, hy, 1},
                                   {std::max(-hx, taken.min.x), -hy, std::min(hx, taken.max.x), taken.min.y - gap, 2},
                                   {std::max(-hx, taken.min.x), taken.max.y + gap, std::min(hx, taken.max.x), hy, 3}};
    struct Fill {
        Region reg;
        std::vector<fruit::Vec2> centres;  // absolute, as many as fit
    };
    std::vector<Fill> fills;
    for (const Region& g : regions) {
        const double w = g.x1 - g.x0, d = g.y1 - g.y0;
        if (w < fp.width || d < fp.depth) continue;
        fruit::ops::ArrangeOptions o;
        o.plate_width = w;
        o.plate_depth = d;
        o.gap = gap;
        o.edge_margin = 0;
        const fruit::ops::ArrangeResult a = fruit::ops::arrange(std::vector<fruit::ops::Footprint>(static_cast<std::size_t>(copies), fp), o);
        Fill f{g, {}};
        const fruit::Vec2 mid{(g.x0 + g.x1) / 2, (g.y0 + g.y1) / 2};
        for (const auto& part : a.parts)
            if (part.placed) f.centres.push_back(mid + part.centre);
        if (!f.centres.empty()) fills.push_back(std::move(f));
    }
    std::stable_sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) { return a.centres.size() > b.centres.size(); });
    std::vector<fruit::Vec2> spots;
    for (Fill& f : fills) {
        const std::size_t take = std::min(f.centres.size(), static_cast<std::size_t>(copies) - spots.size());
        if (take == 0) break;
        f.centres.resize(take);
        // Push the block up to what is on the plate, along the middle of it.
        fruit::BBox2 blk;
        for (const fruit::Vec2& c : f.centres) {
            blk.extend(c - fruit::Vec2{fp.width, fp.depth} * 0.5);
            blk.extend(c + fruit::Vec2{fp.width, fp.depth} * 0.5);
        }
        fruit::Vec2 shift{0, 0};
        const fruit::Vec2 mid = centreOf(taken), bm = centreOf(blk);
        const Region& g = f.reg;
        if (g.side == 0) shift = {(taken.min.x - gap) - blk.max.x, mid.y - bm.y};
        else if (g.side == 1) shift = {(taken.max.x + gap) - blk.min.x, mid.y - bm.y};
        else if (g.side == 2) shift = {mid.x - bm.x, (taken.min.y - gap) - blk.max.y};
        else shift = {mid.x - bm.x, (taken.max.y + gap) - blk.min.y};
        // Stay inside the region.
        shift.x = std::clamp(shift.x, g.x0 - blk.min.x, g.x1 - blk.max.x);
        shift.y = std::clamp(shift.y, g.y0 - blk.min.y, g.y1 - blk.max.y);
        for (const fruit::Vec2& c : f.centres) spots.push_back(c + shift);
    }
    std::vector<std::string> names;
    for (const Part& p : doc.parts()) names.push_back(p.name());
    int last = -1;
    for (const fruit::Vec2& c : spots) {
        NewPart n{numberedName(src.name(), names), src.path(), src.shared(), src.placement()};
        n.placement.offset = c;
        names.push_back(n.name);
        last = e.add(std::move(n));
        ++r.done;
    }
    if (last >= 0) e.selected = last;
    r.changed = r.done > 0;
    if (r.done == r.wanted)
        r.message = r.wanted == 1 ? "Made 1 copy. The other parts did not move."
                                  : "Made " + std::to_string(r.done) + " copies. The other parts did not move.";
    else if (r.done == 0)
        r.message = "No copy fitted: there is no room for one beside the other parts. Arrange can fit more, "
                    "because it moves them.";
    else
        r.message = "Made " + std::to_string(r.done) + " of " + std::to_string(r.wanted) +
                    " copies. There was no room for the other " + std::to_string(r.wanted - r.done) +
                    " without moving the other parts. Arrange can fit more.";
    return r;
}

ToolReport arrangeParts(const Document& doc, PartsEdit& e, double gap) {
    ToolReport r;
    r.wanted = static_cast<int>(doc.partCount());
    gap = std::max(0.0, gap);
    double margin = 0;
    std::vector<fruit::ops::Footprint> fps;
    for (const Part& p : doc.parts()) {
        const fruit::BBox2 b = p.footprintBox();
        fps.push_back({b.size().x, b.size().y});
        margin = std::max(margin, fruit::plateMargin(p.placement()));
    }
    fruit::ops::ArrangeOptions o;
    o.plate_width = doc.printer.build_x;
    o.plate_depth = doc.printer.build_y;
    o.gap = gap;
    o.edge_margin = margin;
    fruit::ops::ArrangeResult a = fruit::ops::arrange(fps, o);
    // Centre the layout on the plate.
    fruit::BBox2 all;
    for (std::size_t i = 0; i < a.parts.size(); ++i) {
        if (!a.parts[i].placed) continue;
        const fruit::Vec2 h{fps[i].width / 2, fps[i].depth / 2};
        all.extend(a.parts[i].centre - h);
        all.extend(a.parts[i].centre + h);
    }
    const fruit::Vec2 shift = all.empty() ? fruit::Vec2{0, 0} : fruit::Vec2{0, 0} - centreOf(all);
    std::vector<std::string> left_out;
    for (std::size_t i = 0; i < a.parts.size(); ++i) {
        const Part& p = doc.parts()[i];
        if (!a.parts[i].placed) {
            left_out.push_back(p.name());
            continue;
        }
        PartState* s = e.find(p.id());
        if (!s) continue;
        const fruit::Vec2 c = a.parts[i].centre + shift;
        if (std::abs(s->placement.offset.x - c.x) > 1e-9 || std::abs(s->placement.offset.y - c.y) > 1e-9) {
            s->placement.offset = c;
            r.changed = true;
        }
        ++r.done;
    }
    if (left_out.empty()) {
        r.message = r.changed ? "All " + plural(r.wanted, "part", "parts") + " fit, " + strf("%g", gap) + " mm apart."
                              : "They are laid out like this already.";
    } else {
        r.message = std::to_string(r.done) + " of " + std::to_string(r.wanted) + " parts fit. " +
                    namesOf(left_out) + (left_out.size() == 1 ? " has" : " have") +
                    " no room and stay" + (left_out.size() == 1 ? "s" : "") + " where " +
                    (left_out.size() == 1 ? "it is" : "they are") + ", so may overlap another part.";
    }
    return r;
}

int shellsOf(const Part& part) {
    static std::map<int, int> cache;  // by geometry id: a mesh never changes
    const auto it = cache.find(part.geometryId());
    if (it != cache.end()) return it->second;
    const int n = fruit::ops::countPieces(part.mesh());
    if (cache.size() > 256) cache.clear();
    cache[part.geometryId()] = n;
    return n;
}

ToolReport splitPart(const Document& doc, PartsEdit& e, int part_id) {
    ToolReport r;
    const Part* src = doc.findPart(part_id);
    if (!src) {
        r.message = "That part is not on the plate.";
        return r;
    }
    const int n = shellsOf(*src);
    r.wanted = n;
    if (n < 2) {
        r.message = "It is one shell: there is nothing to split.";
        return r;
    }
    if (n > kMaxPieces) {
        r.message = "It has " + std::to_string(n) + " shells: too many to make into parts (the most is " +
                    std::to_string(kMaxPieces) + ").";
        return r;
    }
    std::vector<fruit::TriangleMesh> pieces = fruit::ops::splitShells(src->mesh());
    if (pieces.size() < 2) {
        r.message = "It is one shell: there is nothing to split.";
        return r;
    }
    std::vector<std::string> names;
    for (const Part& p : doc.parts())
        if (p.id() != part_id) names.push_back(p.name());
    const std::string base = src->name();
    int hand_made = 0;
    for (const TipEdit& t : doc.supports.edits) hand_made += t.part == part_id;
    for (const fruit::DrainHole& h : doc.hollow.manual) hand_made += h.part == part_id;
    e.remove(part_id);
    int first = -1;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        // Each piece stands where it was: its middle where it was in the plate's XY.
        fruit::BBox3 b;
        for (const fruit::Vec3& v : pieces[i].vertices) b.extend(src->transform().apply(v));
        NewPart n2;
        n2.name = base + " " + std::to_string(i + 1);
        if (std::find(names.begin(), names.end(), n2.name) != names.end()) n2.name = numberedName(n2.name, names);
        names.push_back(n2.name);
        n2.model = makePartModel(fruit::prepareModel(std::move(pieces[i]), false));  // no file: made in the app
        n2.placement = src->placement();
        if (!b.empty()) n2.placement.offset = {b.center().x, b.center().y};
        const int id = e.add(std::move(n2));
        if (first < 0) first = id;
        ++r.done;
    }
    e.selected = first;
    r.changed = true;
    r.message = "Split into " + std::to_string(r.done) + " parts, each where it was. Each part now stands at the job's height.";
    if (hand_made > 0)
        r.message += " Tips and drain holes you placed by hand on it are not kept.";
    return r;
}

namespace {

std::vector<const Part*> partsIn(const Document& doc, const std::vector<int>& ids) {
    std::vector<const Part*> chosen;
    for (const Part& p : doc.parts())
        if (std::find(ids.begin(), ids.end(), p.id()) != ids.end()) chosen.push_back(&p);
    return chosen;
}

// The parts `chosen` become one part holding `joined` (their meshes appended,
// or fused): Merge's edit, after the joining.
ToolReport joinAsOne(const Document& doc, PartsEdit& e, const std::vector<const Part*>& chosen,
                     fruit::TriangleMesh joined, bool fuse, ToolReport r) {
    const int pieces = fruit::ops::countPieces(joined);
    // Fusing joins every shell that meets another (overlapping or touching) into one; a part may have
    // several shells of its own, so fewer pieces than all the parts' shells is what tells it happened.
    int shells_before = 0;
    for (const Part* p : chosen) shells_before += shellsOf(*p);
    const std::string name = chosen.front()->name();
    const fruit::Placement place = inPlace(joined, chosen.front()->placement());
    for (const Part* p : chosen) e.remove(p->id());
    // Made in the app: it has no file of its own (finding one part's file again must not bring that back).
    const int id = e.add(NewPart{name, "", makePartModel(fruit::prepareModel(std::move(joined), false)), place});
    e.selected = id;
    r.changed = true;
    r.done = r.wanted;
    if (!fuse) {
        r.message = "Joined " + std::to_string(r.wanted) + " parts as one part, " + name + ". The shells are as they were.";
    } else if (pieces >= shells_before) {
        r.message = "Joined " + std::to_string(r.wanted) + " parts as one part. None of them overlapped or touched, so "
                    "nothing was fused: it still has " + std::to_string(pieces) + " shells.";
    } else {
        r.message = "Fused " + std::to_string(r.wanted) + " parts into one part, " + name + ". It has " +
                    std::to_string(pieces) + (pieces == 1 ? " shell." : " shells.");
    }
    int hand_made = 0;
    for (const Part* p : chosen) {
        for (const TipEdit& t : doc.supports.edits) hand_made += t.part == p->id();
        for (const fruit::DrainHole& h : doc.hollow.manual) hand_made += h.part == p->id();
    }
    if (hand_made > 0) r.message += " Tips and drain holes you placed by hand are not kept.";
    return r;
}

}  // namespace

FuseResult fuseMeshes(const std::vector<std::pair<std::string, fruit::TriangleMesh>>& meshes,
                      const fruit::JobControl& ctl) {
    FuseResult r;
    fruit::ops::Solid sum;
    bool first = true;
    for (const auto& [name, mesh] : meshes) {
        ctl.check();
        fruit::ops::Solid s(mesh);
        if (!s.valid()) {
            r.message = "Can't fuse: " + name + " " + s.message() + ". Join as one part works with any mesh.";
            return r;
        }
        if (first) {
            sum = s;
            first = false;
        } else {
            sum = fruit::ops::unite(sum, s);
            if (!sum.valid()) {
                r.message = "Can't fuse " + name + " with the others: " + sum.message();
                return r;
            }
        }
    }
    ctl.check();
    r.mesh = sum.toMesh();
    if (r.mesh.faces.empty()) {
        r.message = "Fusing left nothing.";
        return r;
    }
    r.ok = true;
    return r;
}

ToolReport mergeParts(const Document& doc, PartsEdit& e, const std::vector<int>& ids, bool fuse) {
    ToolReport r;
    const std::vector<const Part*> chosen = partsIn(doc, ids);
    r.wanted = static_cast<int>(chosen.size());
    if (chosen.size() < 2) {
        r.message = "Pick at least two parts.";
        return r;
    }
    fruit::TriangleMesh joined;
    if (!fuse) {
        for (const Part* p : chosen) joined.append(p->placed());
    } else {
        std::vector<std::pair<std::string, fruit::TriangleMesh>> meshes;
        for (const Part* p : chosen) meshes.emplace_back(p->name(), p->placed());
        FuseResult f = fuseMeshes(meshes, fruit::JobControl::none());
        if (!f.ok) {
            r.message = f.message;
            return r;
        }
        joined = std::move(f.mesh);
    }
    return joinAsOne(doc, e, chosen, std::move(joined), fuse, r);
}

ToolReport mergeFused(const Document& doc, PartsEdit& e, const std::vector<int>& ids, fruit::TriangleMesh fused) {
    ToolReport r;
    const std::vector<const Part*> chosen = partsIn(doc, ids);
    r.wanted = static_cast<int>(chosen.size());
    if (chosen.size() != ids.size() || chosen.size() < 2) {
        r.message = "The parts changed while they were being fused, so nothing was changed. Try it again.";
        return r;
    }
    return joinAsOne(doc, e, chosen, std::move(fused), true, r);
}

}  // namespace gui
