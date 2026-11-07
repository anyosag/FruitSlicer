// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Library pieces that used to live only in the CLI: placement, settings
// resolution, the GOO print setup and the orientation verdict.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "formats/GooCompare.h"
#include "formats/GooSetup.h"
#include "mesh/ModelLoad.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "support/SupportSettings.h"

using namespace fruit;
using Catch::Approx;

TEST_CASE("placement: the returned transform maps model vertices to the placed mesh", "[api]") {
    TriangleMesh model = makeBox({10, 20, 30}, {14, 26, 31});
    Placement p;
    p.scale = 2.0;
    p.rotation_deg = {30, -20, 75};
    p.elevation = 4.0;
    const TriangleMesh placed = placeModel(model, p);
    const Transform t = placementTransform(model, p);
    REQUIRE(placed.vertices.size() == model.vertices.size());
    for (size_t i = 0; i < model.vertices.size(); ++i) {
        const Vec3 a = t.apply(model.vertices[i]), b = placed.vertices[i];
        CHECK(a.x == Approx(b.x).margin(1e-9));
        CHECK(a.y == Approx(b.y).margin(1e-9));
        CHECK(a.z == Approx(b.z).margin(1e-9));
    }
    const BBox3 b = placed.bbox();
    CHECK(b.min.z == Approx(4.0));
    CHECK(b.center().x == Approx(0).margin(1e-9));
    CHECK(b.center().y == Approx(0).margin(1e-9));
    p.on_plate = true;
    CHECK(placeModel(model, p).bbox().min.z == Approx(0).margin(1e-12));

    // placeOnPlate reports what it applied.
    TriangleMesh m = model;
    const Transform applied = placeOnPlate(m, {0, 90, 0}, 2.0);
    for (size_t i = 0; i < m.vertices.size(); ++i) CHECK(applied.apply(model.vertices[i]).z == Approx(m.vertices[i].z));
}

TEST_CASE("model loading counts shells and turns inside-out ones around", "[api]") {
    TriangleMesh m = makeBox({0, 0, 0}, {10, 10, 10});
    TriangleMesh flipped = makeBox({20, 0, 0}, {25, 5, 5});
    for (auto& f : flipped.faces) std::swap(f[1], f[2]);
    m.append(flipped);
    const LoadedModel lm = prepareModel(m);
    CHECK(lm.shells == 2);
    CHECK(lm.shells_flipped == 1);
    CHECK(lm.mesh.volume() > 0);
}

TEST_CASE("support settings: defaults, then the resin profile, then the user's choices", "[api]") {
    const nlohmann::json profile = {{"critical_angle_deg", 50.0}};
    SupportChoices c;
    c.layer_height = 0.03;
    c.density = 1.5;
    SupportConfig cfg;
    TreeConfig tree;
    resolveSettings(&profile, c, cfg, &tree);
    CHECK(cfg.layer_height == Approx(0.03));
    CHECK(cfg.coverage.density == Approx(1.5));
    CHECK(cfg.critical_angle_deg == Approx(50.0));  // from the profile
    c.critical_angle = 40;
    resolveSettings(&profile, c, cfg, &tree);
    CHECK(cfg.critical_angle_deg == Approx(40.0));  // the user's choice wins

    c.hinge = parseHinge("+y");
    REQUIRE(c.hinge);
    SupportConfig h;
    resolveSettings(nullptr, c, h, nullptr);
    CHECK(h.peel_axis.y == -1.0);  // the far side is away from the hinge
    CHECK(h.peel_bias == Approx(0.25));
    CHECK(std::string(hingeName(*c.hinge)) == "+y");
    CHECK_FALSE(parseHinge("+z"));

    SupportChoices bad;
    bad.peel_bias = 0.3;
    SupportConfig x;
    CHECK_THROWS_AS(resolveSettings(nullptr, bad, x, nullptr), std::invalid_argument);

    SupportChoices plate;
    plate.on_plate = true;
    SupportConfig y;
    TreeConfig ty;
    resolveSettings(nullptr, plate, y, &ty);
    CHECK(y.on_plate);
    CHECK(ty.raft == RaftStyle::None);

    CHECK(minRaftThickness(6, 6, 0.03) == Approx(0.36));
}

TEST_CASE("GOO print setup: printer defaults without a sample, sample settings with one", "[api]") {
    const PrinterProfile printer = PrinterProfile::saturn4Ultra16k();
    GooPrintRequest r;
    r.layer_height = 0.05;
    CHECK_THROWS_AS(buildGooSetup(printer, r), std::runtime_error);  // no exposure times
    r.exposure = 2.0;
    r.bottom_exposure = 30.0;
    GooPrintSetup s = buildGooSetup(printer, r);
    // Without a sample the first layers still get long exposures.
    CHECK(s.header.print.bottom_layers == static_cast<uint32_t>(printer.bottom_layers));
    CHECK(s.header.print.transition_layers == printer.transition_layers);
    CHECK(s.grid.mirror_y == printer.image_mirror_y);
    CHECK(s.grid.width == printer.res_x);
    r.flip_y = true;
    s = buildGooSetup(printer, r);
    CHECK(s.grid.mirror_y != printer.image_mirror_y);  // the orientation check's correction

    GooHeader sample;
    sample.layer_height = 0.03f;
    sample.print.exposure = 1.5f;
    sample.print.bottom_exposure = 27.5f;
    sample.print.bottom_layers = 6;
    sample.print.transition_layers = 6;
    sample.mirror_y = true;
    GooPrintRequest fs;
    fs.layer_height = 0.05;
    fs.settings_from = &sample;
    const GooPrintSetup t = buildGooSetup(printer, fs);
    CHECK(t.header.print.bottom_layers == 6u);
    CHECK(t.header.print.exposure == 1.5f);
    CHECK(t.header.layer_height == Approx(0.03));
    CHECK_FALSE(t.notes.empty());  // layer height came from the sample
}

TEST_CASE("orientation verdicts", "[api]") {
    GooCompareResult c;
    c.best_iou = 0.9;
    c.best = "identity";
    CHECK(orientationVerdict(c) == OrientationVerdict::Matches);
    c.best = "flip_y";
    CHECK(orientationVerdict(c) == OrientationVerdict::FlipY);
    c.best = "flip_x";
    CHECK(orientationVerdict(c) == OrientationVerdict::FlipX);
    c.best = "flip_xy";
    CHECK(orientationVerdict(c) == OrientationVerdict::Rotated180);
    c.best_iou = 0.2;
    CHECK(orientationVerdict(c) == OrientationVerdict::Undetermined);
    CHECK(std::string(verdictText(OrientationVerdict::FlipY)).find("--flip-y") != std::string::npos);
}
