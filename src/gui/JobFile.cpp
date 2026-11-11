// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "JobFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "Orient.h"
#include "formats/GooFile.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "support/SupportIO.h"

namespace gui {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// 2: the model can sit anywhere on the plate ("placement"/"offset"). A job
// whose model is centred is still written as 1, which older FruitSlicers
// open; a moved one as 2, which they refuse rather than open it centred
// with its tips where the model no longer is.
// 3: several parts ("parts", each with its own mesh file and placement; tip
// edits and holes say which part they are on), or a part mirrored
// ("placement"/"mirror"): FruitSlicers that read 2 know neither, and would
// open such a job with its parts missing or not mirrored. A job with one
// part that is not mirrored is still written as 1 or 2.
constexpr int kVersion = 3;
constexpr char kMeshMagic[8] = {'F', 'S', 'M', 'E', 'S', 'H', '1', 0};

// The model exactly as the job uses it: doubles, little-endian (every
// platform FruitSlicer builds for).
std::vector<std::uint8_t> meshBytes(const fruit::TriangleMesh& m) {
    std::vector<std::uint8_t> b(8 + 8 + m.vertices.size() * 24 + m.faces.size() * 12);
    std::uint8_t* p = b.data();
    std::memcpy(p, kMeshMagic, 8);
    const std::uint32_t nv = static_cast<std::uint32_t>(m.vertices.size()), nf = static_cast<std::uint32_t>(m.faces.size());
    std::memcpy(p + 8, &nv, 4);
    std::memcpy(p + 12, &nf, 4);
    p += 16;
    for (const fruit::Vec3& v : m.vertices) {
        const double xyz[3] = {v.x, v.y, v.z};
        std::memcpy(p, xyz, 24);
        p += 24;
    }
    for (const fruit::Face& f : m.faces) {
        const std::int32_t abc[3] = {f[0], f[1], f[2]};
        std::memcpy(p, abc, 12);
        p += 12;
    }
    return b;
}

fruit::TriangleMesh meshFrom(const std::vector<std::uint8_t>& b) {
    if (b.size() < 16 || std::memcmp(b.data(), kMeshMagic, 8) != 0) throw std::runtime_error("the model in the job is damaged");
    std::uint32_t nv = 0, nf = 0;
    std::memcpy(&nv, b.data() + 8, 4);
    std::memcpy(&nf, b.data() + 12, 4);
    if (b.size() != 16 + static_cast<std::size_t>(nv) * 24 + static_cast<std::size_t>(nf) * 12)
        throw std::runtime_error("the model in the job is damaged");
    fruit::TriangleMesh m;
    m.vertices.resize(nv);
    m.faces.resize(nf);
    const std::uint8_t* p = b.data() + 16;
    for (auto& v : m.vertices) {
        double xyz[3];
        std::memcpy(xyz, p, 24);
        v = {xyz[0], xyz[1], xyz[2]};
        p += 24;
    }
    for (auto& f : m.faces) {
        std::int32_t abc[3];
        std::memcpy(abc, p, 12);
        for (int k = 0; k < 3; ++k) {
            if (abc[k] < 0 || static_cast<std::uint32_t>(abc[k]) >= nv) throw std::runtime_error("the model in the job is damaged");
            f[k] = abc[k];
        }
        p += 12;
    }
    return m;
}

json vec(const fruit::Vec3& v) { return json::array({v.x, v.y, v.z}); }
fruit::Vec3 vecFrom(const json& j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }

const char* const kEditKinds[] = {"add", "move", "delete", "resize", "pin", "zone"};

// Tip edits and holes name their part by its place in the job's list, and
// only when there are several ("part"); those on a part no longer in the
// job are left out.
int partSlot(const Document& doc, int id) { return doc.partIndex(id); }

json editsJson(const Document& doc, const std::vector<TipEdit>& edits) {
    json a = json::array();
    for (const TipEdit& e : edits) {
        const int slot = partSlot(doc, e.part);
        if (slot < 0) continue;
        json x = {{"kind", kEditKinds[static_cast<int>(e.kind)]}, {"at", vec(e.at)}, {"to", vec(e.to)},
                  {"normal", vec(e.normal)}, {"diameter", e.diameter}, {"radius", e.radius}};
        if (doc.partCount() > 1) x["part"] = slot;
        a.push_back(std::move(x));
    }
    return a;
}

// The part a saved edit or hole is on (an id), or -1 when the job has no such part.
int partFrom(const Document& doc, const json& j) {
    const int slot = j.contains("part") && j["part"].is_number_integer() ? j["part"].get<int>() : 0;
    return slot >= 0 && slot < static_cast<int>(doc.partCount()) ? doc.parts()[static_cast<std::size_t>(slot)].id() : -1;
}

std::vector<TipEdit> editsFrom(const Document& doc, const json& a) {
    std::vector<TipEdit> out;
    for (const json& j : a) {
        TipEdit e;
        const std::string k = j.value("kind", "");
        int kind = -1;
        for (int i = 0; i < 6; ++i)
            if (k == kEditKinds[i]) kind = i;
        if (kind < 0) throw std::runtime_error("the job has a tip edit of an unknown kind: " + k);
        e.kind = static_cast<TipEdit::Kind>(kind);
        e.at = vecFrom(j.at("at"));
        e.to = vecFrom(j.at("to"));
        e.normal = vecFrom(j.at("normal"));
        e.diameter = j.value("diameter", 0.4);
        e.radius = j.value("radius", 0.0);
        e.part = partFrom(doc, j);
        if (e.part < 0) throw std::runtime_error("the job has a tip edit on a part it doesn't have");
        out.push_back(e);
    }
    return out;
}

// What was repaired, and which faces are patches: it travels with the
// job's copy of the mesh, which is the repaired one.
json repairJson(const fruit::RepairReport& r) {
    return {{"open_edges", r.open_edges_before},
            {"faces_turned", r.faces_flipped},
            {"faces_removed", r.faces_removed},
            {"gaps_closed", r.gaps_closed},
            {"widest_gap", r.widest_gap},
            {"holes_filled", r.holes_filled},
            {"largest_hole", r.largest_hole},
            {"patch_area", r.patch_area},
            {"patch_faces", r.patch_faces},
            {"biggest_patch_area", r.biggest_patch_area},
            {"biggest_patch_across", r.biggest_patch_across},
            {"biggest_patch_at", vec(r.biggest_patch_at)},
            {"biggest_patch_facing", vec(r.biggest_patch_facing)}};
}

// A mesh as the job keeps it, with what loading had repaired.
fruit::LoadedModel modelFrom(const std::vector<std::uint8_t>& bytes, const json& model) {
    fruit::LoadedModel m = fruit::prepareModel(meshFrom(bytes));
    if (fruit::RepairReport& r = m.repair; model.contains("repair") && r.watertight() && !r.changed()) {
        const json& rj = model["repair"];
        r.open_edges_before = rj.value("open_edges", std::size_t{0});
        r.faces_flipped = rj.value("faces_turned", std::size_t{0});
        r.faces_removed = rj.value("faces_removed", std::size_t{0});
        r.gaps_closed = rj.value("gaps_closed", std::size_t{0});
        r.widest_gap = rj.value("widest_gap", 0.0);
        r.holes_filled = rj.value("holes_filled", std::size_t{0});
        r.largest_hole = rj.value("largest_hole", 0.0);
        r.patch_area = rj.value("patch_area", 0.0);
        r.patch_faces = std::min(rj.value("patch_faces", std::size_t{0}), m.mesh.faceCount());
        r.biggest_patch_area = rj.value("biggest_patch_area", 0.0);
        r.biggest_patch_across = rj.value("biggest_patch_across", 0.0);
        if (rj.contains("biggest_patch_at")) r.biggest_patch_at = vecFrom(rj["biggest_patch_at"]);
        if (rj.contains("biggest_patch_facing")) r.biggest_patch_facing = vecFrom(rj["biggest_patch_facing"]);
    }
    return m;
}

json placementJson(const fruit::Placement& p) {
    json j = {{"scale", p.scale}, {"rotation", vec(p.rotation_deg)}, {"elevation", p.elevation}, {"on_plate", p.on_plate}};
    if (p.mirrored()) j["mirror"] = {p.mirror_x, p.mirror_y, p.mirror_z};
    if (!p.centred()) j["offset"] = {p.offset.x, p.offset.y};
    return j;
}

fruit::Placement placementFrom(const json& pj) {
    fruit::Placement p;
    p.scale = pj.value("scale", 1.0);
    p.rotation_deg = vecFrom(pj.at("rotation"));
    p.elevation = pj.value("elevation", 5.0);
    p.on_plate = pj.value("on_plate", false);
    if (pj.contains("mirror") && pj["mirror"].is_array() && pj["mirror"].size() == 3) {  // older jobs: not mirrored
        auto flag = [&](int i) { return pj["mirror"][i].is_boolean() && pj["mirror"][i].get<bool>(); };
        p.mirror_x = flag(0), p.mirror_y = flag(1), p.mirror_z = flag(2);
    }
    if (pj.contains("offset") && pj["offset"].is_array() && pj["offset"].size() == 2 && pj["offset"][0].is_number() &&
        pj["offset"][1].is_number()) {  // older jobs: centred
        auto mm = [](double v) { return std::isfinite(v) ? std::clamp(v, -1000.0, 1000.0) : 0.0; };
        p.offset = {mm(pj["offset"][0].get<double>()), mm(pj["offset"][1].get<double>())};
    }
    return p;
}

json orientJson(const OrientState& o) {
    json visible = json::array();
    for (const auto& v : o.visible) visible.push_back(vec(v));
    return {{"visible", visible}, {"decided", o.decided}};
}

void orientFrom(const json& j, OrientState& o) {
    for (const auto& v : j.value("visible", json::array())) o.visible.push_back(vecFrom(v));
    o.decided = j.value("decided", false);
}

struct Zip {
    mz_zip_archive z{};
    bool open = false;
    ~Zip() {
        if (open) mz_zip_reader_end(&z);
    }
    std::vector<std::uint8_t> read(const char* name, bool required) {
        size_t n = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&z, name, &n, 0);
        if (!data) {
            if (required) throw std::runtime_error(std::string("the job has no ") + name);
            return {};
        }
        std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + n);
        mz_free(data);
        return out;
    }
};

}  // namespace

std::uint64_t meshHash(const fruit::TriangleMesh& m) {
    // FNV-1a over the exact bytes.
    std::uint64_t h = 1469598103934665603ULL;
    const auto bytes = meshBytes(m);
    for (std::uint8_t c : bytes) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

void saveJob(const std::string& path, const Document& doc) {
    if (doc.parts().empty()) throw std::runtime_error("the job has no model");
    json j;
    j["format"] = "fruitslicer-job";
    j["name"] = doc.name;
    // The meshes exactly as the job uses them, one file each.
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> meshes;
    auto modelJson = [&](const Part& part, const std::string& file) {
        json m = {{"path", part.path()}, {"file", file}, {"hash", std::to_string(meshHash(part.mesh()))}};
        if (const fruit::RepairReport& r = part.model().repair; r.changed()) m["repair"] = repairJson(r);
        meshes.emplace_back(file, meshBytes(part.mesh()));
        return m;
    };
    if (doc.partCount() == 1) {
        // One part: as before, so older FruitSlicers open it.
        const Part& part = doc.parts().front();
        // Older FruitSlicers would open a moved or mirrored job as if it were not.
        const fruit::Placement& p = part.placement();
        j["version"] = p.mirrored() ? kVersion : p.centred() ? 1 : 2;
        j["model"] = modelJson(part, "model.bin");
        j["placement"] = placementJson(part.placement());
        j["orient"] = orientJson(doc.orientOf(part.id()));
    } else {
        j["version"] = kVersion;
        json parts = json::array();
        for (std::size_t i = 0; i < doc.partCount(); ++i) {
            const Part& part = doc.parts()[i];
            json pj = {{"name", part.name()}};
            pj["model"] = modelJson(part, "part-" + std::to_string(i + 1) + ".bin");
            pj["placement"] = placementJson(part.placement());
            pj["orient"] = orientJson(doc.orientOf(part.id()));
            parts.push_back(std::move(pj));
        }
        j["parts"] = std::move(parts);
        j["selected"] = doc.partIndex(doc.selectedId());
    }
    const SupportChoices& c = doc.supports.choices;
    json sup = {{"choices", {{"usual_tip", c.usual_tip}, {"density", c.density}, {"merge", c.merge},
                             {"advanced", c.advanced}}}};
    // The quick sliders only when off the resin's stops, so a job left at its
    // defaults is saved as it always was.
    if (c.hold != kHoldDefault || c.amount != kAmountDefault || c.sturdy != kSturdyDefault)
        sup["choices"]["quick"] = {{"hold", c.hold}, {"amount", c.amount}, {"sturdy", c.sturdy}};
    if (c.full_raft) sup["choices"]["full_raft"] = true;  // only when on, so older jobs save as they did
    if (c.hinge) sup["choices"]["hinge"] = fruit::hingeName(*c.hinge);
    // The resin's name and a copy of its measured values: the job keeps them.
    if (!c.resin.empty()) sup["choices"]["resin"] = c.resin;
    if (c.profile.is_object()) sup["choices"]["profile"] = c.profile;
    // Tips only for supports made for the job as it is now.
    sup["edits"] = editsJson(doc, doc.supports.edits);
    if (doc.supports.fresh(doc.revision(), doc.supportSettings())) {
        const SupportResult& r = *doc.supports.result;
        sup["tips"] = fruit::pointsToJson(r.planned);  // the planner's; the edits apply on top
        sup["planned_around"] = editsJson(doc, r.plan_edits);  // the edits the planner worked around
        json ws = json::array();
        for (const auto& w : r.plan_warnings)
            ws.push_back({{"code", w.code}, {"message", w.message}, {"layer", w.layer}, {"where", vec(w.where)}});
        sup["warnings"] = ws;
    }
    j["supports"] = sup;
    // The Hollow screen's choices, and the holes placed by hand (in their
    // part's coordinates). The plan itself is made again from them.
    const HollowState& hs = doc.hollow;
    const HollowChoices& hc = hs.choices;
    json holes = json::array();
    for (const fruit::DrainHole& h : hs.manual) {
        const int slot = partSlot(doc, h.part);
        if (slot < 0) continue;
        json x = {{"at", vec(h.at)}, {"axis", vec(h.axis)}, {"diameter", h.diameter}};
        if (doc.partCount() > 1) x["part"] = slot;
        holes.push_back(std::move(x));
    }
    j["hollow"] = {{"applied", hs.applied},
                   {"fix", hc.fix == HollowChoices::Fix::Fill    ? "fill"
                           : hc.fix == HollowChoices::Fix::Leave ? "leave"
                                                                 : "holes"},
                   {"hollow", hc.hollow},
                   {"wall", hc.wall},
                   {"ceiling_lean", hc.ceiling_lean},
                   {"hole_diameter", hc.hole_diameter},
                   {"auto_holes", hc.auto_holes},
                   {"holes", holes}};
    const PrintSettings& s = doc.print.settings;
    j["print"] = {{"exposure", s.exposure},
                  {"bottom_exposure", s.bottom_exposure},
                  {"bottom_layers", s.bottom_layers},
                  {"transition_layers", s.transition_layers},
                  {"layer_height", s.layer_height},
                  {"wait_before_lift", s.wait_before_lift},
                  {"wait_after_lift", s.wait_after_lift},
                  {"wait_after_retract", s.wait_after_retract},
                  {"lift", s.lift},
                  {"light_power", s.light_power},
                  {"density", s.density},
                  {"height_correction", s.height_correction},
                  {"sample_path", s.sample_path}};

    std::vector<std::uint8_t> sample;
    if (s.sample) {
        // The header as SatelLite wrote it, so the job keeps its settings
        // even if that file goes; if it has already gone, the header as read.
        std::ifstream in(s.sample_path, std::ios::binary);
        sample.resize(fruit::kGooHeaderSize);
        if (s.sample_path.empty() ||
            !in.read(reinterpret_cast<char*>(sample.data()), static_cast<std::streamsize>(sample.size())))
            sample = fruit::gooHeaderBytes(*s.sample);
        j["print"]["sample_file"] = "sample-header.bin";
    }

    const std::string text = j.dump(2);
    const std::string tmp = path + ".part";
    std::error_code ec;
    fs::remove(tmp, ec);
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, tmp.c_str(), 0)) throw std::runtime_error("cannot write " + path);
    bool ok = mz_zip_writer_add_mem(&z, "job.json", text.data(), text.size(), MZ_DEFAULT_COMPRESSION);
    for (const auto& [file, bytes] : meshes)
        ok = ok && mz_zip_writer_add_mem(&z, file.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION);
    if (ok && !sample.empty())
        ok = mz_zip_writer_add_mem(&z, "sample-header.bin", sample.data(), sample.size(), MZ_DEFAULT_COMPRESSION);
    ok = mz_zip_writer_finalize_archive(&z) && ok;
    ok = mz_zip_writer_end(&z) && ok;
    if (!ok) {
        fs::remove(tmp, ec);
        throw std::runtime_error("cannot write " + path + " (is the disk full?)");
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("cannot write " + path + ": " + ec.message());
    }
}

OpenedJob openJob(const std::string& path, const fruit::PrinterProfile& printer, const fruit::JobControl& ctl) {
    ctl.step("load", 0);
    Zip zip;
    if (!mz_zip_reader_init_file(&zip.z, path.c_str(), 0)) throw std::runtime_error("this is not a FruitSlicer job");
    zip.open = true;
    const auto text = zip.read("job.json", true);
    json j;
    try {
        j = json::parse(text.begin(), text.end());
    } catch (const std::exception&) {
        throw std::runtime_error("the job's description is damaged");
    }
    if (j.value("format", "") != "fruitslicer-job") throw std::runtime_error("this is not a FruitSlicer job");
    if (j.value("version", 0) > kVersion)
        throw std::runtime_error("this job was saved by a newer FruitSlicer; update to open it");

    OpenedJob out;
    auto doc = std::make_shared<Document>();
    out.doc = doc;
    doc->job_path = path;
    doc->name = j.value("name", fs::path(path).stem().string());
    doc->printer = printer;
    // The parts: one ("model", "placement") or several ("parts").
    struct Saved {
        NewPart part;
        json orient;
    };
    std::vector<Saved> saved;
    auto readPart = [&](const json& model, const json& placement, std::string name) {
        Saved s;
        s.part.path = model.value("path", "");
        s.part.name = name.empty() ? partName(s.part.path, {}) : std::move(name);
        const std::string file = model.value("file", "model.bin");
        s.part.model = makePartModel(modelFrom(zip.read(file.c_str(), true), model));
        // A job keeps no part without triangles (Document::editParts drops it).
        if (s.part.model->model.mesh.empty()) throw std::runtime_error("the model in the job is damaged");
        s.part.placement = placementFrom(placement);
        return s;
    };
    if (j.contains("parts")) {
        if (!j["parts"].is_array() || j["parts"].empty()) throw std::runtime_error("the job has no model");
        for (const json& pj : j["parts"]) {
            Saved s = readPart(pj.at("model"), pj.at("placement"), pj.value("name", ""));
            s.orient = pj.value("orient", json::object());
            saved.push_back(std::move(s));
        }
    } else {
        Saved s = readPart(j.at("model"), j.at("placement"), doc->name);
        s.orient = j.value("orient", json::object());
        saved.push_back(std::move(s));
    }
    ctl.step("load", 0.2);
    std::vector<NewPart> parts;
    for (const Saved& s : saved) parts.push_back(s.part);
    doc->replaceParts(std::move(parts));
    doc->clearHistory();
    if (doc->partCount() != saved.size()) throw std::runtime_error("the model in the job is damaged");
    for (std::size_t i = 0; i < saved.size(); ++i) orientFrom(saved[i].orient, doc->orientOf(doc->parts()[i].id()));
    if (const int sel = j.value("selected", 0); sel >= 0 && sel < static_cast<int>(doc->partCount()))
        doc->select(doc->parts()[static_cast<std::size_t>(sel)].id());
    doc->low_points = fruit::countLowPoints(doc->placed(), doc->placement().on_plate);
    // The first part whose file has gone, to ask where it is now. A part made
    // in the app (cut, split, merged, labelled) has no file to look for.
    for (const Part& part : doc->parts())
        if (!part.path().empty() && !fs::exists(part.path())) {
            out.model_missing = true;
            out.model_path = part.path();
            out.part = part.id();
            break;
        }
    if (!out.model_missing) out.model_path = doc->parts().front().path();

    const json& sj = j.at("supports");
    const json& cj = sj.at("choices");
    SupportChoices& c = doc->supports.choices;
    c.usual_tip = cj.value("usual_tip", 2);
    c.density = cj.value("density", 1.0);
    c.merge = cj.value("merge", true);
    c.full_raft = cj.value("full_raft", false);
    c.advanced = cj.value("advanced", std::map<std::string, double>{});
    if (cj.contains("quick") && cj["quick"].is_object()) {  // jobs from before the sliders have none
        const json& q = cj["quick"];
        c.hold = std::clamp(q.value("hold", kHoldDefault), 1, kQuickStops);
        c.amount = std::clamp(q.value("amount", kAmountDefault), 1, kQuickStops);
        c.sturdy = std::clamp(q.value("sturdy", kSturdyDefault), 1, kQuickStops);
    }
    if (cj.contains("hinge") && cj["hinge"].is_string()) c.hinge = fruit::parseHinge(cj["hinge"].get<std::string>());
    c.resin = cj.value("resin", std::string());
    if (cj.contains("profile") && cj["profile"].is_object()) c.profile = cj["profile"];
    doc->supports.edits = editsFrom(*doc, sj.value("edits", json::array()));
    if (j.contains("hollow")) {
        const json& hj = j["hollow"];
        HollowState& hs = doc->hollow;
        HollowChoices& hc = hs.choices;
        const std::string fix = hj.value("fix", "holes");
        hc.fix = fix == "fill" ? HollowChoices::Fix::Fill : fix == "leave" ? HollowChoices::Fix::Leave : HollowChoices::Fix::Holes;
        hc.hollow = hj.value("hollow", false);
        hc.wall = hj.value("wall", hc.wall);
        hc.ceiling_lean = hj.value("ceiling_lean", hc.ceiling_lean);
        hc.hole_diameter = hj.value("hole_diameter", hc.hole_diameter);
        hc.auto_holes = hj.value("auto_holes", true);
        for (const auto& h : hj.value("holes", json::array())) {
            const int part = partFrom(*doc, h);
            if (part < 0) throw std::runtime_error("the job has a drain hole on a part it doesn't have");
            hs.manual.push_back({vecFrom(h.at("at")), vecFrom(h.at("axis")), h.value("diameter", 3.0), false, part});
        }
        hs.applied = hj.value("applied", false);
    }

    if (j.contains("print")) {
        const json& prj = j["print"];
        PrintSettings& s = doc->print.settings;
        s.exposure = prj.value("exposure", 0.0);
        s.bottom_exposure = prj.value("bottom_exposure", 0.0);
        s.bottom_layers = prj.value("bottom_layers", -1);
        s.transition_layers = prj.value("transition_layers", -1);
        s.layer_height = prj.value("layer_height", 0.0);
        s.wait_before_lift = prj.value("wait_before_lift", -1.0);
        s.wait_after_lift = prj.value("wait_after_lift", -1.0);
        s.wait_after_retract = prj.value("wait_after_retract", -1.0);
        s.lift = prj.value("lift", -1.0);
        s.light_power = prj.value("light_power", -1);
        s.density = prj.value("density", 0.0);
        s.height_correction = prj.value("height_correction", -1.0);
        if (!prj.contains("height_correction")) {
            // Saved while it sat with the placement (for a short while): one of
            // the usual value is the same as none.
            const json& pl = j.contains("parts") && j["parts"].is_array() && !j["parts"].empty()
                                 ? j["parts"][0].value("placement", json::object())
                                 : j.value("placement", json::object());
            const double was = pl.value("height_correction", -1.0);
            if (was >= 0 && std::abs(was - fruit::kDefaultHeightCorrection) > 1e-9)
                s.height_correction = std::min(was, fruit::kMaxHeightCorrection);
        }
        s.sample_path = prj.value("sample_path", "");
        if (prj.contains("sample_file")) {
            const auto bytes = zip.read("sample-header.bin", false);
            if (!bytes.empty()) s.sample = std::make_shared<const fruit::GooHeader>(fruit::parseGooHeader(bytes));
        }
        if (!s.sample) s.sample_path.clear();
    }

    if (sj.contains("tips")) {
        SupportPlan plan;
        plan.points = fruit::pointsFromJson(sj["tips"]);
        plan.edits = editsFrom(*doc, sj.value("planned_around", json::array()));
        for (const auto& w : sj.value("warnings", json::array())) {
            fruit::Warning wn;
            wn.code = w.value("code", "");
            wn.message = w.value("message", "");
            wn.layer = w.value("layer", -1);
            if (w.contains("where")) wn.where = vecFrom(w["where"]);
            plan.warnings.push_back(wn);
        }
        // Planned on the hollowed or drilled model: that is made again, so
        // the layers and the print file use it too.
        fruit::JobControl rest = ctl.sub(0.3, 1.0);
        if (doc->hollow.applied) {
            auto hp = std::make_shared<const HollowPlan>(
                planHollow(*doc, doc->hollow.choices, doc->hollow.manual, kLayerHeight, ctl.sub(0.3, 0.6)));
            doc->hollow.plan = hp;
            doc->hollow.plan_revision = doc->revision();
            if (hp->changes()) plan.hollow = hp;
            rest = ctl.sub(0.6, 1.0);
        }
        auto r = std::make_shared<SupportResult>(
            finishWithEdits(*doc, c, std::move(plan), doc->supports.edits, rest));
        doc->supports.result = std::move(r);
        doc->supports.result_revision = doc->revision();
        doc->supports.result_settings = doc->supportSettings();
    }
    ctl.report("check", 1.0);
    doc->saved = doc->stamp();
    return out;
}

int relinkParts(Document& doc, const std::string& was, const std::string& now, fruit::LoadedModel found) {
    if (was.empty()) return 0;
    const std::uint64_t hash = meshHash(found.mesh);
    std::shared_ptr<const PartModel> model;            // the file's mesh now: one for every part that takes it
    std::map<const PartModel*, bool> same;              // copies share their mesh: hashed once
    int changed = 0;
    doc.editParts([&](PartsEdit& e) {
        for (PartState& s : e.parts()) {
            if (s.path != was || !s.model) continue;
            s.path = now;
            const auto it = same.try_emplace(s.model.get(), false);
            if (it.second) it.first->second = meshHash(s.model->model.mesh) == hash;
            if (it.first->second) continue;
            if (!model) model = makePartModel(std::move(found));
            s.model = model;
            ++changed;
        }
    });
    return changed;
}

std::vector<std::string> changesSince(const JobStamp& a, const JobStamp& b) {
    std::vector<std::string> out;
    if (a.parts != b.parts) {
        std::size_t added = 0, removed = 0;
        for (const int id : b.parts) added += std::find(a.parts.begin(), a.parts.end(), id) == a.parts.end();
        for (const int id : a.parts) removed += std::find(b.parts.begin(), b.parts.end(), id) == b.parts.end();
        if (added) out.push_back(added == 1 ? "added a part" : "added " + std::to_string(added) + " parts");
        if (removed) out.push_back(removed == 1 ? "removed a part" : "removed " + std::to_string(removed) + " parts");
    } else {
        if (a.revision != b.revision)
            out.push_back(b.parts.size() > 1 ? "moved, turned or resized parts" : "moved, turned or resized the model");
        // A path that went ("" now) is a part made in the app, told above as a change to it.
        bool found = false;
        for (std::size_t i = 0; i < a.model_paths.size() && i < b.model_paths.size(); ++i)
            found = found || (a.model_paths[i] != b.model_paths[i] && !b.model_paths[i].empty());
        if (found) out.push_back("found the model in its new place");
    }
    if (a.parts == b.parts && (a.orient_settings != b.orient_settings || a.orient_decided != b.orient_decided))
        out.push_back("changed how it stands");
    if (a.edits != b.edits) {
        const std::size_t n = b.edits.size() > a.edits.size() ? b.edits.size() - a.edits.size() : 0;
        out.push_back(n == 1 ? "edited a tip by hand" : n > 1 ? "edited " + std::to_string(n) + " tips by hand"
                                                              : "undid tip edits");
    }
    const bool resin = a.choices.resin != b.choices.resin || a.choices.profile != b.choices.profile;
    if (resin) out.push_back("changed the resin to " + (b.choices.resin.empty() ? "My resin" : b.choices.resin));
    SupportChoices other = b.choices;  // the rest of the support settings
    other.resin = a.choices.resin;
    other.profile = a.choices.profile;
    other.usual_tip = resin ? a.choices.usual_tip : other.usual_tip;  // a resin sets its usual tip
    if (a.supports != b.supports && a.edits == b.edits)
        out.push_back(a.supports ? "generated the supports again" : "generated supports");
    else if (a.supports == b.supports && !(a.choices == other)) out.push_back("changed support settings");
    if (a.hollow_applied != b.hollow_applied || !(a.hollow == b.hollow) || a.holes != b.holes)
        out.push_back("changed how its hollows print");
    if (a.sample_path != b.sample_path || a.print != b.print)
        out.push_back("changed the print settings");
    return out;
}

std::vector<RecentJob> loadRecentJobs(const std::string& file) {
    std::ifstream in(file);
    if (!in) return {};
    try {
        const json j = json::parse(in);
        std::vector<RecentJob> out;
        for (const auto& e : j.value("jobs", json::array()))
            if (e.is_object() && !e.value("path", "").empty())
                out.push_back({e.value("path", ""), e.value("name", ""), e.value("state", ""), e.value("when", "")});
        return out;
    } catch (const std::exception&) {
        return {};
    }
}

void rememberJob(const std::string& file, const RecentJob& job, std::size_t keep) {
    std::vector<RecentJob> jobs = loadRecentJobs(file);
    std::erase_if(jobs, [&](const RecentJob& r) { return r.path == job.path; });
    jobs.insert(jobs.begin(), job);
    if (jobs.size() > keep) jobs.resize(keep);
    json arr = json::array();
    for (const auto& r : jobs) arr.push_back({{"path", r.path}, {"name", r.name}, {"state", r.state}, {"when", r.when}});
    const fs::path p(file);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << json{{"jobs", arr}}.dump(2) << "\n";
        if (!out) throw std::runtime_error("cannot write " + tmp);
    }
    fs::rename(tmp, file);
}

}  // namespace gui
