// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The job's name, as the Job screen's header and Home's recent jobs show it:
// a new job's first model's, a saved one's file's, and what becomes of it when
// the parts it was named after go.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

#include "Document.h"
#include "JobFile.h"
#include "mesh/ModelLoad.h"
#include "mesh/Primitives.h"
#include "mesh/StlIO.h"

using namespace gui;
namespace fs = std::filesystem;

namespace {

const fruit::PrinterProfile kPrinter = fruit::PrinterProfile::saturn4Ultra16k();

// Each test's files in a folder of its own: tests may run at the same time.
struct Folder {
    fs::path dir;
    explicit Folder(const std::string& test) : dir(fs::temp_directory_path() / "fruit_job_name_tests" / test) {
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    // A model file `name`.stl: a cube `s` mm across.
    std::string model(const std::string& name, double s) const {
        const std::string path = (dir / (name + ".stl")).string();
        fruit::writeStl(path, fruit::makeBox({0, 0, 0}, {s, s, s}));
        return path;
    }

    // A new job opened from that file, placed as the health dialog leaves it.
    std::shared_ptr<Document> newJob(const std::string& name, double s = 10) const {
        auto doc = loadDocument(model(name, s), kPrinter, fruit::JobControl::none());
        doc->saved = doc->stamp();
        return doc;
    }

    // Adds the part `name` (a file of its own) beside the others, as the app does.
    int add(Document& doc, const std::string& name, double s = 10) const {
        const std::string path = model(name, s);
        fruit::Placement p;
        p.offset = {40.0 * static_cast<double>(doc.partCount()), 0};
        return doc.addPart(NewPart{name, path, makePartModel(fruit::loadModel(path)), p});
    }

    std::string job(const std::string& file) const { return (dir / file).string(); }

    // Saved as the app saves it (App::saveJobTo).
    void saveAs(Document& doc, const std::string& file) const {
        saveJob(job(file), doc);
        doc.savedTo(job(file), doc.stamp());
    }

    std::shared_ptr<Document> open(const std::string& file) const {
        return openJob(job(file), kPrinter, fruit::JobControl::none()).doc;
    }
};

int idOf(const Document& doc, const std::string& part) {
    for (const Part& p : doc.parts())
        if (p.name() == part) return p.id();
    FAIL("no part " << part);
    return -1;
}

// What the Job screen's header reads: the name, then the state under it.
std::string header(const Document& doc) { return doc.name + " · " + doc.savedState(); }

}  // namespace

TEST_CASE("Job name: a new job takes its first model's name and keeps it while that part is there",
          "[gui][jobs][name]") {
    const Folder f("new");
    const auto doc = f.newJob("TheDipper_Front");
    CHECK(header(*doc) == "TheDipper_Front · Not saved");
    f.add(*doc, "TheDipper_Back");
    CHECK(header(*doc) == "TheDipper_Front · Not saved");
    // Another part going leaves it as it is.
    REQUIRE(doc->removePart(idOf(*doc, "TheDipper_Back")));
    CHECK(header(*doc) == "TheDipper_Front · Not saved");
}

TEST_CASE("Job name: an unsaved job whose part is replaced takes the part left", "[gui][jobs][name]") {
    const Folder f("replaced");
    const auto doc = f.newJob("TheDipper_Front");
    f.add(*doc, "TheDipper_LoadingCradle");
    REQUIRE(doc->removePart(idOf(*doc, "TheDipper_Front")));
    CHECK(header(*doc) == "TheDipper_LoadingCradle · Not saved");
    doc->undo();
    CHECK(header(*doc) == "TheDipper_Front · Not saved");
    doc->redo();
    CHECK(header(*doc) == "TheDipper_LoadingCradle · Not saved");
    // Every part at once: the first one's name.
    const std::string path = f.model("Coverslip rack", 12);
    doc->replaceParts({NewPart{"Coverslip rack", path, makePartModel(fruit::loadModel(path)), fruit::Placement{}}});
    CHECK(header(*doc) == "Coverslip rack · Not saved");
}

TEST_CASE("Job name: copies and pieces of the part carry its name", "[gui][jobs][name]") {
    const Folder f("pieces");
    const auto doc = f.newJob("bracket");
    f.add(*doc, "bracket 2");       // a copy
    f.add(*doc, "bracket (top)");   // a piece cut off
    f.add(*doc, "bracket_holder");  // another model: doesn't
    REQUIRE(doc->removePart(idOf(*doc, "bracket")));
    CHECK(header(*doc) == "bracket · Not saved");
    REQUIRE(doc->removePart(idOf(*doc, "bracket 2")));
    CHECK(header(*doc) == "bracket · Not saved");
    REQUIRE(doc->removePart(idOf(*doc, "bracket (top)")));
    CHECK(header(*doc) == "bracket_holder · Not saved");
}

TEST_CASE("Job name: a saved job whose parts are all replaced becomes a new job; undo brings back its file",
          "[gui][jobs][name]") {
    const Folder f("saved");
    // As the dipper was: TheDipper_Front.fruitjob, front and back.
    {
        const auto doc = f.newJob("TheDipper_Front");
        f.add(*doc, "TheDipper_Back");
        f.saveAs(*doc, "TheDipper_Front.fruitjob");
    }
    const auto doc = f.open("TheDipper_Front.fruitjob");
    REQUIRE(doc);
    const std::string file = doc->job_path;
    CHECK(header(*doc) == "TheDipper_Front · Saved");

    f.add(*doc, "TheDipper_LoadingCradle");
    CHECK(header(*doc) == "TheDipper_Front · Edited since saving");
    // The front gone: named after the first part left, and no longer that file, so saving asks where.
    REQUIRE(doc->removePart(idOf(*doc, "TheDipper_Front")));
    CHECK(header(*doc) == "TheDipper_Back · Not saved");
    CHECK(doc->job_path.empty());
    REQUIRE(doc->removePart(idOf(*doc, "TheDipper_Back")));
    CHECK(header(*doc) == "TheDipper_LoadingCradle · Not saved");
    CHECK(doc->unsaved());  // leaving it asks

    // Undo all the way: the job's file and name again, saved as it was.
    doc->undo();
    CHECK(header(*doc) == "TheDipper_Back · Not saved");
    doc->undo();
    CHECK(header(*doc) == "TheDipper_Front · Edited since saving");
    CHECK(doc->job_path == file);
    doc->undo();
    CHECK(header(*doc) == "TheDipper_Front · Saved");
    doc->redo();
    doc->redo();
    doc->redo();
    CHECK(header(*doc) == "TheDipper_LoadingCradle · Not saved");

    // Saved as its own job, then undone: the parts come back, the save stays.
    f.saveAs(*doc, "TheDipper_LoadingCradle.fruitjob");
    CHECK(header(*doc) == "TheDipper_LoadingCradle · Saved");
    doc->undo();
    CHECK(header(*doc) == "TheDipper_LoadingCradle · Edited since saving");
    CHECK(doc->job_path == f.job("TheDipper_LoadingCradle.fruitjob"));
    CHECK(f.open("TheDipper_Front.fruitjob")->partCount() == 2);  // the dipper's job is left as it was
}

TEST_CASE("Job name: a job is named after its file, whatever it was called when saved", "[gui][jobs][name]") {
    const Folder f("file");
    // Saved under a name of its own: that name when opened, kept whatever its parts.
    {
        const auto doc = f.newJob("DishRack_Base");
        f.add(*doc, "DishRack_Top");
        f.saveAs(*doc, "assembly.fruitjob");
        CHECK(header(*doc) == "assembly · Saved");
    }
    const auto doc = f.open("assembly.fruitjob");
    REQUIRE(doc);
    CHECK(header(*doc) == "assembly · Saved");
    REQUIRE(doc->removePart(idOf(*doc, "DishRack_Base")));
    CHECK(header(*doc) == "assembly · Edited since saving");
    CHECK_FALSE(doc->job_path.empty());
    // Home's recent jobs too, remembered before jobs were named so.
    const std::string recent = (f.dir / "recent.json").string();
    rememberJob(recent, {f.job("assembly.fruitjob"), "DishRack_Base", "placed", "today"});
    REQUIRE(loadRecentJobs(recent).size() == 1);
    CHECK(loadRecentJobs(recent).front().name == "assembly");

    // One part (saved as older FruitSlicers read it): the job its file's name, the part its own.
    {
        const auto one = f.newJob("TheDipper_LoadingCradle");
        f.saveAs(*one, "cradle print.fruitjob");
    }
    const auto one = f.open("cradle print.fruitjob");
    REQUIRE(one);
    CHECK(header(*one) == "cradle print · Saved");
    CHECK(one->part().name() == "TheDipper_LoadingCradle");
}
