// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Repairs.h"

#include <cctype>
#include <cmath>
#include <vector>

#include "Format.h"

namespace gui {

namespace {

// A length in mm with digits that mean something: "13.2", "0.44", "0.044".
// The health check gives sizes at full size, which for a file drawn in
// metres or inches is small.
std::string length(double mm) {
    if (mm >= 1) return strf("%.1f", mm);
    if (mm >= 0.1) return strf("%.2f", mm);
    return strf("%.2g", mm);
}

// "a, b and c".
std::string list(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i)
        out += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    return out;
}

}  // namespace

RepairWords repairWords(const fruit::MeshCheck& c) {
    const fruit::RepairReport& r = c.repair;
    RepairWords w;
    if (!c.closed()) {
        w.level = RepairWords::Level::Open;
        w.title = "Not watertight";
        w.detail = thousands(c.open_edges) + (c.open_edges == 1 ? " edge is" : " edges are") +
                   " open and FruitSlicer could not close " + (c.open_edges == 1 ? "it" : "them") +
                   ", so some layers may not have a clear inside and outside. The file may be loose surfaces "
                   "with no thickness: fix it in your modelling app.";
        w.brief = thousands(c.open_edges) + (c.open_edges == 1 ? " open edge" : " open edges") +
                  " that could not be closed.";
        return w;
    }
    if (!r.changed()) return w;

    std::vector<std::string> did, brief;
    if (r.holes_filled == 1) {
        did.push_back("closed a hole " + length(r.largest_hole) + " mm across");
        brief.push_back("1 hole closed");
    } else if (r.holes_filled > 1) {
        did.push_back("closed " + thousands(r.holes_filled) + " holes (the largest " + length(r.largest_hole) + " mm across)");
        brief.push_back(thousands(r.holes_filled) + " holes closed");
    }
    const bool hairline = r.widest_gap < 0.005;  // narrower than half a hundredth: no width worth giving
    if (r.gaps_closed == 1) {
        did.push_back(hairline ? std::string("closed a hairline crack") : strf("closed a crack about %.2f mm wide", r.widest_gap));
        brief.push_back("1 crack closed");
    } else if (r.gaps_closed > 1) {
        did.push_back("closed " + thousands(r.gaps_closed) + " cracks" +
                      (hairline ? std::string() : strf(" (up to about %.2f mm wide)", r.widest_gap)));
        brief.push_back(thousands(r.gaps_closed) + " cracks closed");
    }
    if (r.faces_flipped == 1) {
        did.push_back("turned a face that faced the wrong way");
        brief.push_back("1 face turned");
    } else if (r.faces_flipped > 1) {
        did.push_back("turned " + thousands(r.faces_flipped) + " faces that faced the wrong way");
        brief.push_back(thousands(r.faces_flipped) + " faces turned");
    }
    if (r.faces_removed == 1) {
        did.push_back("removed a doubled or loose face");
        brief.push_back("1 stray face removed");
    } else if (r.faces_removed > 1) {
        did.push_back("removed " + thousands(r.faces_removed) + " doubled or loose faces");
        brief.push_back(thousands(r.faces_removed) + " stray faces removed");
    }
    w.detail = "FruitSlicer " + list(did) + ".";
    w.brief = brief.empty() ? std::string() : brief[0];
    for (std::size_t i = 1; i < brief.size(); ++i) w.brief += ", " + brief[i];
    w.brief += ".";
    if (!w.brief.empty()) w.brief[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w.brief[0])));

    const double size = fruit::norm(c.bbox.size());
    const double as_disc = std::sqrt(4 * r.biggest_patch_area / fruit::kPi);
    const bool big_hole = r.holes_filled > 0 && as_disc > 0.1 * size;
    const bool much = r.patch_faces * 20 > c.triangles;
    if (big_hole || much) {
        w.level = RepairWords::Level::Check;
        w.title = big_hole ? "Repaired, with a big patch" : "Repaired, with a lot of patching";
        w.detail += " Each patch is the flattest that fits, which is a guess at what was meant: look at the "
                    "tinted patches on the model before you print.";
    } else {
        w.level = RepairWords::Level::Repaired;
        w.title = "Repaired: now watertight";
        w.detail += r.patch_faces ? " No point of the model was moved, and the patches are tinted on it."
                                  : " No point of the model was moved.";
    }
    return w;
}

}  // namespace gui
