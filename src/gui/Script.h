// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Scripted runs for screenshots and UI tests. A script is JSON:
//
//   {"size": [1440, 900], "scale": 1,
//    "steps": [{"screen": "home"}, {"shot": "home"},
//              {"open": "@demo"}, {"wait": "jobs"}, {"view": "angled"}, {"shot": "model"}]}
//
// Steps drive the app through the same commands its buttons use:
//   {"open": path | "@demo" | "@demo-big" | "@demo-bar" | "@demo-3mf" | "@demo-job"}  built-in test models, a saved job
//     ("@demo-hollow": a ball with a sealed hollow; "@demo-block": a plain 40 × 24 × 14 mm block; "@demo-worn", "@demo-open", "@demo-loose": faulty
//     meshes for the repair) {"show_patch": true} turns the view to the biggest patch
//   {"add_part": path | "@demo"…} another model beside the others (then "health", as for open)
//   {"select_part": n} {"remove_part": n} (1-based, in the Parts list's order)
//   {"select_at": [x, y, z, dx, dy, dz]} selects the part that ray meets, as a click on it in the view
//   {"save_job": path | "@demo"} {"unsaved": "save" | "discard" | "cancel"}
//   {"relink": "copy" | "cancel" | path | "@demo"} {"remove": path | "@demo"} (test set-up)
//   {"wait": "jobs"}            finish background work
//   {"busy": name} {"cancel_job": true}  a stand-in job that runs (its progress card shows) until cancelled
//   {"health": "full" | "largest" | "custom" (+ "percent") | "inches" | "place" | "cancel"}
//   {"scale": percent} {"rotate": [x, y, z]} {"sits": "raised" | "flat"} {"fit": true}
//   {"move": [x, y]} (mm from the plate's centre) {"nudge": [dx, dy]} {"centre": true} {"turn": 90}
//   {"slide_on": true} {"drag": [handle, amount, snap]} one whole drag of a handle in the 3D view
//     (handle: "move_x" | "move_y" | "turn_x" | "turn_y" | "turn_z"; mm or degrees; snap as with ⇧)
//   {"undo": n} {"redo": n} {"shortcuts": true | false}
//   {"step": "model" | "orient" | "supports" | "layers" | "print"} (any place, any time) {"layer": n (1-based)} {"show_below": bool}
//   {"mirror": "x" | "y" | "z"} the selected part (left to right, front to back, top to bottom)
//   {"duplicate": n} n copies of the selected part {"arrange": gap_mm} every part laid out (gap optional)
//   {"split": true} the selected part into its shells {"merge_pick": [n, ...]} (1-based) only these are joined next
//   {"merge_parts": "join" | "fuse"} {"collisions": "wait" | gap_mm} wait for the check, or set its least gap
//   {"measure": [[x, y, z], [x, y, z]]} clicks, in mm on the plate, each put on the surface along the line to the
//     middle of the part whose middle is nearest it; {"measure": "clear"} starts again
//   {"tool": "on_plate" | "move" | "rotate" | "scale" | "mirror" | "duplicate" | "arrange" | "measure" | "supports" | "hollow" | "repair" |
//     "label" | "combine" | "cut" | "merge" | "split" | "none"} as the tool
//     buttons (again on the open one closes its panel) {"close_tool": true} {"job_tab": "checks" | "hollows" | "collisions"}
//   {"cut_plane": ["z" | "x" | "y", mm]} {"cut_keep": "both" | "above" | "below"} {"cut_apply": true} (then "wait": "jobs"): the Cut tool
//     (z: mm above where the part stands; x, y: mm from the plate's centre; above: top, right or back)
//   {"label_set": {"text": s, "height": mm, "depth": mm, "engrave": bool}} {"label_at": [x, y, z, dx, dy, dz]} (a click
//     on the part along that ray) {"label_apply": true}: the Label tool
//   {"combine_with": n (1-based part)} {"combine_mode": "join" | "subtract" | "overlap"} {"combine_apply": true}: the Combine tool, A = the selected part
//   {"side": "front" | "back" | "left" | "right" | "top" | "bottom"}
//   {"orient": "find" | "settings"} {"use": 1 | 2 | 3, or 0 to keep how it sits}
//   {"supports": "generate" | "now"} ("now": the accent button: shows Supports and makes them) {"tip_size": 0.3 | 0.4 | 0.6} {"spacing": 1 to 2} {"merge": bool}
//   {"supports_view": "supports" | "tips" | "why"} {"select_tip": index} {"problems": bool}
//   {"advanced": "open" | "close" | "reset" | "apply"} {"setting": [key, value]} (keys: advancedSettings())
//   {"exposures": [normal_s, bottom_s]} {"sample": path | "@demo-sample"} (a 0.03 mm print file)
//   {"drive": name} adds a stand-in USB drive (a folder) and picks it
//   {"save": true} as the Save .goo button (asks about orientation once; with no drive, a dialog)
//   {"save_to": path | "@demo"} saves straight to a path, no questions; {"saved": "back"}
//   {"answer": "save_anyway" | "cancel"} the orientation question
//   {"mock_printer": true | "silent"} a printer on loopback (or none answering); {"search": true}
//   {"save_target": "wifi" | "folder"}
//   {"printer": n (1-based)} {"start_print": bool} {"send": "go" | "close"}
//   {"edit_tips": bool} {"tip_tool": "select" | "add" | "zone"} {"tip_select": i} {"tip_delete": i}
//   {"tip_resize": [i, mm]} {"tip_pin": i} {"tip_undo": i | -1 | "all"}
//   {"tip_add_near" | "tip_zone_near" | "tip_move_near": [i, dx, dy]} (up from below the plate, near tip i)
//   {"orientation_piece": path | "@demo"} {"orientation_sample": path | "@stand-in-flip-y" (-flip-x, -as-is)}
//   {"orientation_fix": "use"} {"hinge": "back" | "left" | "right" | "front" | "none"}
//   {"frames": n} renders n frames (lets per-frame work, like background scoring, start)
//   {"screen": "home" | "job" | "calibrate" | "orientation_check" | "printer" | "about"} {"view": "front" | "side" | "angled" | "below" | "top"}
// Shots are written as <dir>/<name>.png once two consecutive frames are
// identical.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gui {

class App;

class Script {
public:
    explicit Script(const std::string& path);
    int width() const { return width_; }
    int height() const { return height_; }
    float scale() const { return scale_; }

    // `render` draws one whole frame offscreen and returns its RGBA pixels.
    // Returns the process exit code.
    int run(App& app, const std::string& shot_dir, const std::function<std::vector<uint8_t>()>& render);

private:
    nlohmann::json steps_;
    int width_ = 1440, height_ = 900;
    float scale_ = 1;
};

}  // namespace gui
