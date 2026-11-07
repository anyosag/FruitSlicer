// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The A/B test: the files it saves, what FruitSlicer keeps of it, and the
// copies it offers.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
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

namespace {

// Scores for a test with `per_side` copies a side: A's marks and times
// from `a`, B's from `b` (every copy whole).
fruit::ABSummary summaryOf(int per_side, std::vector<std::pair<int, double>> a, std::vector<std::pair<int, double>> b) {
    const fruit::ABPlate p = plateOf(2 * per_side);
    std::vector<fruit::ABCopyResult> scores;
    std::size_t ia = 0, ib = 0;
    for (const auto& c : p.manifest["copies"]) {
        fruit::ABCopyResult r;
        r.id = c["id"].get<int>();
        r.recorded = r.ok = true;
        const auto& src = c.at("arm").get<std::string>() == "A" ? a[ia++] : b[ib++];
        r.scar = src.first;
        r.removal_s = src.second;
        scores.push_back(r);
    }
    return fruit::summarizeAB(p.manifest, scores);
}

}  // namespace

TEST_CASE("the verdict names a winner only when chance is an unlikely reason", "[ab]") {
    using W = ABVerdict::Winner;
    // 4 a side, cleanly apart on marks and clean-up: FruitSlicer's did better.
    const ABVerdict win = abVerdict(summaryOf(4, {{1, 40}, {2, 45}, {1, 41}, {2, 44}}, {{3, 90}, {3, 95}, {4, 92}, {3, 99}}));
    CHECK(win.winner == W::A);
    CHECK(win.headline == "On this model, FruitSlicer's supports did better.");
    CHECK(win.detail ==
          "Every copy stayed on either way. FruitSlicer's left smaller marks: 1.5 against 3.2. FruitSlicer's cleaned "
          "up faster: 42 s against 94 s.");
    CHECK(win.caveat == "4 copies a side is a small test. Repeat it, or try another model, before trusting the difference.");

    // The same pattern with 2 a side can't show anything: even this sweep is 1 in 3.
    const ABVerdict two = abVerdict(summaryOf(2, {{1, 40}, {2, 45}}, {{3, 90}, {4, 95}}));
    CHECK(two.winner == W::None);
    CHECK(two.headline == "No clear winner yet.");
    CHECK_THAT(two.detail, Catch::Matchers::EndsWith("With this few copies, that could easily be chance."));
    CHECK(two.caveat == "With 2 copies a side, even a clean sweep could be chance: print at least 4 a side for a "
                        "difference to be able to show.");

    // Each side better at one thing.
    const ABVerdict mixed = abVerdict(summaryOf(4, {{1, 90}, {1, 95}, {2, 92}, {1, 99}}, {{3, 40}, {4, 45}, {3, 41}, {4, 44}}));
    CHECK(mixed.winner == W::Mixed);
    CHECK(mixed.headline == "Mixed: each side did better at something.");

    // SatelLite's better.
    const ABVerdict lose = abVerdict(summaryOf(4, {{3, 90}, {3, 95}, {4, 92}, {3, 99}}, {{1, 40}, {2, 45}, {1, 41}, {2, 44}}));
    CHECK(lose.winner == W::B);
    CHECK(lose.headline == "On this model, SatelLite's supports did better.");

    // Identical results.
    const ABVerdict same = abVerdict(summaryOf(4, {{2, 60}, {2, 60}, {2, 60}, {2, 60}}, {{2, 60}, {2, 60}, {2, 60}, {2, 60}}));
    CHECK(same.winner == W::None);
    CHECK(same.headline == "No difference between them on this model.");
    CHECK_THAT(same.caveat, Catch::Matchers::StartsWith("With 4 copies a side, only a large difference can show"));

    // Overlapping 4 a side: leaning, but not sure.
    const ABVerdict lean = abVerdict(summaryOf(4, {{1, 50}, {3, 60}, {2, 55}, {2, 70}}, {{2, 60}, {3, 65}, {3, 58}, {2, 72}}));
    CHECK(lean.winner == W::None);
    CHECK(lean.headline == "No clear winner yet.");

    // Nothing scored on one side yet.
    fruit::ABSummary half;
    half.a.scored = 3;
    CHECK(abVerdict(half).winner == W::NotYet);
    CHECK(abVerdict(half).headline == "Score both sides to compare them.");
}

TEST_CASE("the tiles put the two sides side by side", "[ab]") {
    const fruit::ABSummary s = summaryOf(4, {{1, 40}, {2, 45}, {1, 41}, {2, 44}}, {{3, 90}, {3, 95}, {4, 92}, {3, 99}});
    const auto tiles = abTiles(s, 47);
    REQUIRE(tiles.size() == 4);
    CHECK(tiles[0].label == "Stayed on");
    CHECK(tiles[0].a == "4 of 4");
    CHECK(tiles[1].a == "1.5");
    CHECK(tiles[1].b == "3.2");
    CHECK(tiles[2].a == "42 s");
    CHECK(tiles[3].a == "47");
    CHECK(tiles[3].b == "from SatelLite");
    const auto empty = abTiles(fruit::ABSummary{}, -1);
    CHECK(empty[0].a == "—");
    CHECK(empty[3].a == "—");
}
