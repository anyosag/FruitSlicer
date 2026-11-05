#pragma once
// Support point files.
//
// points.json (written by `fruitslicer points`):
//   {"version": 1, "frame": "printer", "placement": {...},
//    "points": [{"id", "pos", "normal", "tip_diameter", "category", "pinned", "layer"}]}
//
// edits.json (read by --edits): changes that survive regeneration.
//   {"add":    [{"pos": [x,y,z], "tip_diameter": 0.4}],
//    "delete": [id, ...],                     // no auto tip within the old tip's radius + 0.3 mm
//    "move":   [{"id": id, "pos": [x,y,z]}],  // delete + pinned manual point
//    "resize": [{"id": id, "tip_diameter": d}]}
// Ids refer to a previous points.json (pass it with --points-in).

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/CosmeticField.h"
#include "support/SupportPoint.h"

namespace fruit {

nlohmann::json pointsToJson(const std::vector<SupportPoint>& pts);
std::vector<SupportPoint> pointsFromJson(const nlohmann::json& j);

// Applies edits against `previous` points: manual points to add, and
// exclusion zones appended to `cosmetic`. Throws on unknown ids.
std::vector<SupportPoint> applyEdits(const nlohmann::json& edits, const std::vector<SupportPoint>& previous,
                                     CosmeticSpec& cosmetic);

// Coloured point cloud (one small octahedron per tip) for viewing in any
// mesh viewer; colour by category.
void writePointsPly(const std::string& path, const std::vector<SupportPoint>& pts);

}  // namespace fruit
