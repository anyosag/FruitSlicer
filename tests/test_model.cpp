// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// 3MF reading, the model check and plate fit.
#include <miniz.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "formats/ThreeMf.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "mesh/Primitives.h"

using namespace fruit;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

// A zip package with the given (name, content) parts.
std::string makeZip(const std::vector<std::pair<std::string, std::string>>& parts) {
    mz_zip_archive z{};
    REQUIRE(mz_zip_writer_init_heap(&z, 0, 0));
    for (const auto& [name, data] : parts)
        REQUIRE(mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION));
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&z, &buf, &size));
    std::string out(static_cast<const char*>(buf), size);
    mz_zip_writer_end(&z);
    return out;
}

const char* kRels =
    R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>)";

// A 10 mm cube with outward-facing triangles, as a 3MF <mesh>.
std::string cubeMesh(double s = 10) {
    const TriangleMesh m = makeBox({0, 0, 0}, {s, s, s});
    std::string x = "<mesh><vertices>";
    for (const Vec3& v : m.vertices)
        x += "<vertex x=\"" + std::to_string(v.x) + "\" y=\"" + std::to_string(v.y) + "\" z=\"" +
             std::to_string(v.z) + "\"/>";
    x += "</vertices><triangles>";
    for (const Face& f : m.faces)
        x += "<triangle v1=\"" + std::to_string(f[0]) + "\" v2=\"" + std::to_string(f[1]) + "\" v3=\"" +
             std::to_string(f[2]) + "\"/>";
    return x + "</triangles></mesh>";
}

std::string model(const std::string& unit, const std::string& resources, const std::string& build) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit=")" + unit + R"(" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
 xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06">
<resources>)" + resources + "</resources><build>" + build + "</build></model>";
}

TriangleMesh read(const std::string& zip, ThreeMfInfo* info = nullptr) {
    return read3mfFromMemory(zip.data(), zip.size(), info);
}

}  // namespace

TEST_CASE("3MF: a cube in millimetres and in inches", "[3mf]") {
    const std::string res = R"(<object id="1" type="model">)" + cubeMesh() + "</object>";
    ThreeMfInfo info;
    const TriangleMesh mm = read(makeZip({{"_rels/.rels", kRels},
                                          {"3D/3dmodel.model", model("millimeter", res, R"(<item objectid="1"/>)")}}),
                                 &info);
    CHECK(mm.faceCount() == 12);
    CHECK(info.meshes == 1);
    CHECK(info.unit == "millimeter");
    CHECK(mm.volume() == Approx(1000.0));
    const TriangleMesh in =
        read(makeZip({{"_rels/.rels", kRels}, {"3D/3dmodel.model", model("inch", res, R"(<item objectid="1"/>)")}}));
    CHECK(in.bbox().size().x == Approx(254.0));
}

TEST_CASE("3MF: item transforms use the row-vector convention", "[3mf]") {
    // Quarter turn about Z (x' = -y, y' = x), then move by (100, 50, 5).
    const std::string res = R"(<object id="1">)" + cubeMesh() + "</object>";
    const std::string item = R"(<item objectid="1" transform="0 1 0 -1 0 0 0 0 1 100 50 5"/>)";
    const TriangleMesh m = read(makeZip({{"3D/3dmodel.model", model("millimeter", res, item)}}));  // no .rels
    const BBox3 b = m.bbox();
    CHECK(b.min.x == Approx(90.0));
    CHECK(b.max.x == Approx(100.0));
    CHECK(b.min.y == Approx(50.0));
    CHECK(b.max.y == Approx(60.0));
    CHECK(b.min.z == Approx(5.0));
    CHECK(m.volume() == Approx(1000.0));
}

TEST_CASE("3MF: components in another part (Production extension) compose transforms", "[3mf]") {
    // Bambu Studio style: the root object only holds components that point
    // into /3D/Objects/object_1.model.
    const std::string part = model("millimeter", R"(<object id="7">)" + cubeMesh() + "</object>", "");
    const std::string root = model(
        "millimeter",
        R"(<object id="2"><components>
             <component p:path="/3D/Objects/object_1.model" objectid="7" transform="2 0 0 0 2 0 0 0 2 0 0 0"/>
             <component p:path="/3D/Objects/object_1.model" objectid="7" transform="1 0 0 0 1 0 0 0 1 30 0 0"/>
           </components></object>)",
        R"(<item objectid="2" transform="1 0 0 0 1 0 0 0 1 0 0 10"/>)");
    ThreeMfInfo info;
    const TriangleMesh m = read(
        makeZip({{"_rels/.rels", kRels}, {"3D/3dmodel.model", root}, {"3D/Objects/object_1.model", part}}), &info);
    CHECK(info.meshes == 2);
    CHECK(m.faceCount() == 24);
    const BBox3 b = m.bbox();
    CHECK(b.min.z == Approx(10.0));   // item lift applies to both
    CHECK(b.max.z == Approx(30.0));   // the scaled copy is 20 mm tall
    CHECK(b.max.x == Approx(40.0));   // the moved copy ends at 30 + 10
    CHECK(m.volume() == Approx(8000.0 + 1000.0));
}

TEST_CASE("3MF: a mirrored item stays outward-facing", "[3mf]") {
    const std::string res = R"(<object id="1">)" + cubeMesh() + "</object>";
    const TriangleMesh m = read(makeZip(
        {{"3D/3dmodel.model", model("millimeter", res, R"(<item objectid="1" transform="-1 0 0 0 1 0 0 0 1 0 0 0"/>)")}}));
    CHECK(m.bbox().min.x == Approx(-10.0));
    CHECK(m.volume() == Approx(1000.0));  // positive: triangles were turned back outward
}

TEST_CASE("3MF: switched-off items and support objects are left out", "[3mf]") {
    const std::string res = R"(<object id="1">)" + cubeMesh() + R"(</object><object id="2" type="support">)" +
                            cubeMesh(5) + "</object>";
    const std::string build =
        R"(<item objectid="1"/><item objectid="2"/><item objectid="1" printable="0" transform="1 0 0 0 1 0 0 0 1 50 0 0"/>)";
    ThreeMfInfo info;
    const TriangleMesh m = read(makeZip({{"3D/3dmodel.model", model("millimeter", res, build)}}), &info);
    CHECK(info.meshes == 1);
    CHECK(info.skipped == 2);
    CHECK(m.bbox().max.x == Approx(10.0));
}

TEST_CASE("3MF: broken files give readable errors", "[3mf]") {
    CHECK_THROWS_WITH(read("not a zip at all"), ContainsSubstring("zip"));
    CHECK_THROWS_WITH(read(makeZip({{"readme.txt", "hi"}})), ContainsSubstring("3dmodel.model"));
    CHECK_THROWS_WITH(read(makeZip({{"3D/3dmodel.model", "<model><resources>"}})), ContainsSubstring("XML"));
    const std::string res = R"(<object id="1">)" + cubeMesh() + "</object>";
    CHECK_THROWS_WITH(read(makeZip({{"3D/3dmodel.model", model("millimeter", res, R"(<item objectid="9"/>)")}})),
                      ContainsSubstring("object 9"));
    CHECK_THROWS_WITH(read(makeZip({{"3D/3dmodel.model", model("furlong", res, R"(<item objectid="1"/>)")}})),
                      ContainsSubstring("unit"));
    const std::string loop = R"(<object id="1"><components><component objectid="1"/></components></object>)";
    CHECK_THROWS_WITH(read(makeZip({{"3D/3dmodel.model", model("millimeter", loop, R"(<item objectid="1"/>)")}})),
                      ContainsSubstring("loop"));
}

TEST_CASE("3MF: numbers read the same under a decimal-comma locale", "[3mf]") {
    const std::string res = R"(<object id="1"><mesh><vertices>
        <vertex x="0" y="0" z="0"/><vertex x="1.5" y="0" z="0"/><vertex x="0" y="2.25" z="0"/><vertex x="0" y="0" z="0.5"/>
      </vertices><triangles>
        <triangle v1="0" v2="2" v3="1"/><triangle v1="0" v2="1" v3="3"/><triangle v1="0" v2="3" v3="2"/><triangle v1="1" v2="2" v3="3"/>
      </triangles></mesh></object>)";
    const std::string zip = makeZip({{"3D/3dmodel.model", model("millimeter", res, R"(<item objectid="1"/>)")}});
    const char* saved = std::setlocale(LC_NUMERIC, nullptr);
    const std::string keep = saved ? saved : "C";
    // Not every machine has these locales; the C locale run below always counts.
    for (const char* loc : {"de_DE.UTF-8", "fr_FR.UTF-8", "C"}) {
        if (!std::setlocale(LC_NUMERIC, loc)) continue;
        const BBox3 b = read(zip).bbox();
        CHECK(b.max.x == 1.5);
        CHECK(b.max.y == 2.25);
        CHECK(b.max.z == 0.5);
    }
    std::setlocale(LC_NUMERIC, keep.c_str());
}

TEST_CASE("loadModel reads .3mf files by extension", "[3mf]") {
    const std::string res = R"(<object id="1">)" + cubeMesh() + "</object>";
    const auto path = std::filesystem::temp_directory_path() / "fruitslicer_test_cube.3mf";
    {
        std::ofstream f(path, std::ios::binary);
        f << makeZip({{"_rels/.rels", kRels}, {"3D/3dmodel.model", model("millimeter", res, R"(<item objectid="1"/>)")}});
    }
    const LoadedModel m = loadModel(path.string());
    std::filesystem::remove(path);
    CHECK(m.shells == 1);
    CHECK(m.mesh.volume() == Approx(1000.0));
}

TEST_CASE("Model check: closed solids, open edges, inches", "[model]") {
    LoadedModel m = prepareModel(makeBox({0, 0, 0}, {30, 20, 4}));
    MeshCheck c = checkMesh(m);
    CHECK(c.closed());
    CHECK(c.shells == 1);
    CHECK(c.triangles == 12);
    CHECK(c.volume_ml == Approx(2.4));
    CHECK_FALSE(c.maybeInches());

    TriangleMesh open = makeBox({0, 0, 0}, {30, 20, 4});
    open.faces.pop_back();  // one triangle missing leaves three edges unshared
    c = checkMesh(prepareModel(open));
    CHECK(c.open_edges == 3);
    CHECK_FALSE(c.closed());

    CHECK(checkMesh(prepareModel(makeBox({0, 0, 0}, {4, 3, 2}))).maybeInches());
}

TEST_CASE("Plate fit: margins, the largest scale, and turning", "[model]") {
    const PrinterProfile pr = PrinterProfile::saturn4Ultra16k();
    const TriangleMesh box = makeBox({0, 0, 0}, {30, 20, 24});
    Placement p;  // raised 5 mm, 3 mm margin all round
    PlateFit f = checkFit(box, p, pr);
    CHECK(f.fits);
    CHECK(f.size.x == Approx(30));
    CHECK(f.top == Approx(29));
    // Depth limits it: (118.37 - 6) / 20 = 5.6185 -> 561 %.
    CHECK(f.largest == Approx(5.61));
    CHECK(f.largest_turned == Approx(3.74));  // (118.37 - 6) / 30

    // 120 mm deep does not fit the 118.37 mm plate, but fits turned.
    const TriangleMesh deep = makeBox({0, 0, 0}, {10, 120, 10});
    f = checkFit(deep, p, pr);
    CHECK_FALSE(f.fits);
    CHECK(f.largest < 1.0);
    CHECK(f.largest_turned > 1.0);
    p.rotation_deg = {0, 0, 90};
    CHECK(checkFit(deep, p, pr).fits);

    // Flat on the plate: no lift, 0.5 mm margin, the full 220 mm of height.
    Placement flat;
    flat.on_plate = true;
    const TriangleMesh tall = makeBox({0, 0, 0}, {10, 10, 218});
    CHECK(checkFit(tall, flat, pr).fits);
    CHECK_FALSE(checkFit(tall, Placement{}, pr).fits);  // 218 + 5 mm lift > 220
}

TEST_CASE("Low points: mid-air spots, not the ones resting on the plate", "[model]") {
    Placement raised;
    const TriangleMesh m = placeModel(makeBox({0, 0, 0}, {10, 10, 10}), raised);
    CHECK(countLowPoints(m, false) == 1);  // the bottom face, as one plateau
    Placement flat;
    flat.on_plate = true;
    CHECK(countLowPoints(placeModel(makeBox({0, 0, 0}, {10, 10, 10}), flat), true) == 0);
}
