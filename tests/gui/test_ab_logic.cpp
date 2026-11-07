// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The A/B test: the files it saves, what FruitSlicer keeps of it, and the
// copies it offers.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "ABTest.h"
#include "mesh/Primitives.h"

using namespace gui;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const std::string& name) {
    const auto d = fs::temp_directory_path() / "fruit_ab_logic_tests" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

fruit::ABPlate plateOf(int copies) {
    fruit::ABPlateOptions o;
    o.copies = copies;
    o.model = "box";
    return fruit::makeABPlate(fruit::makeBox({-5, -5, 5}, {5, 5, 8}), fruit::makeBox({-6, -6, 0}, {6, 6, 5}),
                              fruit::PrinterProfile::saturn4Ultra16k(), o);
}

}  // namespace

TEST_CASE("the B file is named after the A file", "[ab]") {
    CHECK(abFileB("/tmp/x/bracket AB_A.stl") == "/tmp/x/bracket AB_B.stl");
    CHECK(abFileB("bracket.stl") == "bracket_B.stl");
    CHECK(abFileB("/a/b/test_A.STL") == "/a/b/test_B.stl");
}

TEST_CASE("the copies offered are even, at most 8, and fit", "[ab]") {
    CHECK(defaultABCopies(0) == 0);
    CHECK(defaultABCopies(1) == 0);
    CHECK(defaultABCopies(2) == 2);
    CHECK(defaultABCopies(3) == 2);
    CHECK(defaultABCopies(5) == 4);
    CHECK(defaultABCopies(8) == 8);
    CHECK(defaultABCopies(9) == 8);
    CHECK(defaultABCopies(40) == 8);
}

TEST_CASE("the test's record reads back, and a damaged one is none", "[ab]") {
    const fs::path d = freshDir("record");
    ABRecord r;
    r.model = "bracket";
    r.resin = "My resin";
    r.file_a = "/Volumes/NO NAME/bracket AB_A.stl";
    r.file_b = "/Volumes/NO NAME/bracket AB_B.stl";
    r.copies = 4;
    r.scored = 1;
    saveABRecord((d / "test.json").string(), r);
    CHECK(loadABRecord((d / "test.json").string()) == r);
    CHECK(loadABRecord((d / "missing.json").string()) == ABRecord{});
    std::ofstream(d / "bad.json") << "{not json";
    CHECK(loadABRecord((d / "bad.json").string()) == ABRecord{});
}

TEST_CASE("a new test never overwrites the last one's scores", "[ab]") {
    const fs::path d = freshDir("keep");
    const std::string m = (d / "ab.json").string(), sheet = (d / "ab.results.csv").string();
    const fruit::ABPlate first = plateOf(4);
    keepABTest(first.manifest, m, sheet);
    REQUIRE(fs::exists(m));
    REQUIRE(fs::exists(sheet));
    {
        std::ifstream in(sheet);
        std::string head;
        std::getline(in, head);
        CHECK(head == "id,row,col,result,scar,removal_s,notes");  // blind: no arm column
    }
    CHECK(fruit::loadABResults(m, sheet).size() == 4);

    // Nothing scored: the next test simply replaces it.
    const fruit::ABPlate second = plateOf(6);
    keepABTest(second.manifest, m, sheet);
    CHECK_FALSE(fs::exists(d / "ab.earlier.results.csv"));
    CHECK(fruit::loadABResults(m, sheet).size() == 6);

    // Scored: set aside, with its manifest, before the next one.
    std::vector<fruit::ABCopyResult> scores(1);
    scores[0].id = 2;
    scores[0].recorded = scores[0].ok = true;
    std::ofstream(sheet) << fruit::blindABCsv(second.manifest, scores);
    keepABTest(first.manifest, m, sheet);
    REQUIRE(fs::exists(d / "ab.earlier.results.csv"));
    REQUIRE(fs::exists(d / "ab.earlier.json"));
    const auto earlier = fruit::loadABResults((d / "ab.earlier.json").string(), (d / "ab.earlier.results.csv").string());
    CHECK(earlier.size() == 6);
    CHECK(earlier[1].recorded);
    CHECK(fruit::loadABResults(m, sheet).size() == 4);
}
