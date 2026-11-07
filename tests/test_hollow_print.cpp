// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// A hollowed, drilled part through the rest of the job: tips kept out of its
// hole and off its insides, its layers joined to its supports', the whole
// checked for islands and written to a print file.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <stdexcept>

#include "formats/GooExport.h"
#include "layers/Drain.h"
#include "mesh/Placement.h"
#include "mesh/Primitives.h"
#include "printer/PrinterProfile.h"
#include "raster/Rasterizer.h"
#include "support/CosmeticField.h"
#include "support/PointGenerator.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

using namespace fruit;
using Catch::Approx;

namespace {

double volume(const LayerStack& ls) {
    double v = 0;
    for (const Paths& s : ls.slices) v += area(s) * ls.layer_height;
    return v;
}

}  // namespace

TEST_CASE("two stacks' layers together", "[hollow]") {
    const LayerStack a = sliceModel(makeBox({0, 0, 0}, {10, 10, 2}), 0.1);
    const LayerStack b = sliceModel(makeBox({5, 0, 0}, {15, 10, 3}), 0.1);
    const LayerStack u = unionLayers(a, b);
    REQUIRE(u.size() == 30);
    CHECK(area(u.slices[5]) == Approx(150).margin(1e-6));   // overlapping where both are
    CHECK(area(u.slices[25]) == Approx(100).margin(1e-6));  // above the lower one, the other alone
    CHECK(u.sample_z[25] == Approx(2.55));
    CHECK(unionLayers(a, LayerStack{}).size() == 20);
    CHECK_THROWS_AS(unionLayers(a, sliceModel(makeBox({0, 0, 0}, {1, 1, 1}), 0.05)), std::invalid_argument);
}

TEST_CASE("a hollowed, drilled part prints: no tips in its hole, nothing floating, less resin", "[hollow]") {
    const TriangleMesh model = makeBox({0, 0, 0}, {20, 20, 20});
    Placement place;
    place.elevation = 5;
    const Transform to_printer = placementTransform(model, place);
    const TriangleMesh placed = placeModel(model, place);
    SupportConfig cfg;
    cfg.layer_height = 0.1;
    const LayerStack solid = sliceModel(placed, cfg.layer_height);
    const HollowResult hollow = hollowInterior(solid, HollowOptions{});
    REQUIRE(hollow.cavities.size() == 1);
    const std::vector<DrainHole> holes = suggestDrainHoles(placed, to_printer, solid, hollow, 3.0);
    REQUIRE(holes.size() == 2);  // a drain hole underneath, an air hole on top
    const std::vector<PlacedHole> cut = placeHoles(holes, to_printer, solid, hollow);
    REQUIRE(cut[0].cavity == 0);
    REQUIRE(cut[1].cavity == 0);
    const auto drilled = std::make_shared<const LayerStack>(drilledLayers(solid, hollow, cut));
    REQUIRE(cavitiesDrain(*drilled, hollow) == std::vector<char>{1});

    // The drain hole opens in the middle of the underside: no tips there,
    // and none inside the part.
    const Vec3 mouth = to_printer.apply(holes[0].at);
    CHECK(mouth.z == Approx(5.0).margin(1e-6));
    CosmeticSpec spec;
    for (size_t i = 0; i < holes.size(); ++i) spec.exclusions.push_back({to_printer.apply(holes[i].at), cut[i].radius + 0.5});
    PointGenOptions opts;
    opts.cosmetic = &spec;
    opts.layers = drilled;
    const PointGenResult pts = generateSupportPoints(placed, cfg, opts);
    REQUIRE(!pts.points.empty());
    for (const SupportPoint& p : pts.points) {
        CHECK(distance(p.pos, mouth) > cut[0].radius);
        CHECK(hollow.cavityAt(solid.layerAt(p.pos.z), p.pos.xy()) == -1);
    }

    // The supports' layers and the drilled part's together: nothing floats.
    TreeConfig tc;
    const SupportTree tree = buildSupportTree(placed, pts.points, tc, PrinterProfile::saturn4Ultra16k().minRaftThickness());
    const LayerStack supports = sliceModel(tree.mesh(tc), cfg.layer_height);
    const LayerStack print = unionLayers(*drilled, supports);
    CHECK(validateLayers(print, cfg.link_grow).floating.empty());

    // Written out, the print holds the shell's resin: the cavity and the
    // holes' channels through the 2 mm floor and roof less than the solid part.
    const double removed = volume(unionLayers(solid, supports)) - volume(print);
    CHECK(removed == Approx(hollow.volume + 2 * kPi * 1.5 * 1.5 * 2.0).margin(25));
    GooHeader h;
    h.layer_height = 0.1f;
    h.print.exposure = 2.5f;
    h.print.bottom_exposure = 25.0f;
    const RasterGrid grid = RasterGrid::fromPrinter(PrinterProfile::saturn4Ultra16k());
    const std::string path = (std::filesystem::temp_directory_path() / "fruit_hollow_print.goo").string();
    const GooExportResult written = exportGooLayers(print, &placed, h, grid, GooExportOptions{}, path);
    CHECK(written.layers == print.size());
    CHECK(written.volume_mm3 == Approx(volume(print)).epsilon(0.02));
    std::filesystem::remove(path);
}
