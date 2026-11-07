// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "JobFile.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "formats/GooFile.h"
#include "mesh/ModelCheck.h"
#include "mesh/ModelLoad.h"
#include "support/SupportIO.h"

namespace gui {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kVersion = 1;
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

json editsJson(const std::vector<TipEdit>& edits) {
    json a = json::array();
    for (const TipEdit& e : edits)
        a.push_back({{"kind", kEditKinds[static_cast<int>(e.kind)]}, {"at", vec(e.at)}, {"to", vec(e.to)},
                     {"normal", vec(e.normal)}, {"diameter", e.diameter}, {"radius", e.radius}});
    return a;
}

std::vector<TipEdit> editsFrom(const json& a) {
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
        out.push_back(e);
    }
    return out;
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
    json j;
    j["format"] = "fruitslicer-job";
    j["version"] = kVersion;
    j["name"] = doc.name;
    j["model"] = {{"path", doc.path}, {"file", "model.bin"}, {"hash", std::to_string(meshHash(doc.model.mesh))}};
    const fruit::Placement& p = doc.placement();
    j["placement"] = {{"scale", p.scale}, {"rotation", vec(p.rotation_deg)}, {"elevation", p.elevation},
                      {"on_plate", p.on_plate}};
    json visible = json::array();
    for (const auto& v : doc.orient.visible) visible.push_back(vec(v));
    j["orient"] = {{"visible", visible}, {"decided", doc.orient.decided}};
    const SupportChoices& c = doc.supports.choices;
    json sup = {{"choices", {{"usual_tip", c.usual_tip}, {"density", c.density}, {"merge", c.merge},
                             {"advanced", c.advanced}}}};
    if (c.hinge) sup["choices"]["hinge"] = fruit::hingeName(*c.hinge);
    // Tips only for supports made for the job as it is now.
    sup["edits"] = editsJson(doc.supports.edits);
    if (doc.supports.fresh(doc.revision(), doc.orient.settings)) {
        const SupportResult& r = *doc.supports.result;
        sup["tips"] = fruit::pointsToJson(r.planned);  // the planner's; the edits apply on top
        sup["planned_around"] = editsJson(r.plan_edits);  // the edits the planner worked around
        json ws = json::array();
        for (const auto& w : r.plan_warnings)
            ws.push_back({{"code", w.code}, {"message", w.message}, {"layer", w.layer}, {"where", vec(w.where)}});
        sup["warnings"] = ws;
    }
    j["supports"] = sup;
    const PrintSettings& s = doc.print.settings;
    j["print"] = {{"exposure", s.exposure}, {"bottom_exposure", s.bottom_exposure}, {"bottom_layers", s.bottom_layers},
                  {"transition_layers", s.transition_layers}, {"sample_path", s.sample_path}};

    std::vector<std::uint8_t> sample;
    if (s.sample && !s.sample_path.empty()) {
        // The header as SatelLite wrote it, so the job keeps its settings
        // even if that file goes.
        std::ifstream in(s.sample_path, std::ios::binary);
        sample.resize(fruit::kGooHeaderSize);
        if (in.read(reinterpret_cast<char*>(sample.data()), static_cast<std::streamsize>(sample.size())))
            j["print"]["sample_file"] = "sample-header.bin";
        else
            sample.clear();
    }

    const std::string text = j.dump(2);
    const std::vector<std::uint8_t> model = meshBytes(doc.model.mesh);
    const std::string tmp = path + ".part";
    std::error_code ec;
    fs::remove(tmp, ec);
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, tmp.c_str(), 0)) throw std::runtime_error("cannot write " + path);
    bool ok = mz_zip_writer_add_mem(&z, "job.json", text.data(), text.size(), MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&z, "model.bin", model.data(), model.size(), MZ_DEFAULT_COMPRESSION);
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
    doc->geometry_id = newGeometryId();
    const json& model = j.at("model");
    out.model_path = model.value("path", "");
    doc->path = out.model_path;
    out.model_missing = out.model_path.empty() || !fs::exists(out.model_path);
    doc->model = fruit::prepareModel(meshFrom(zip.read("model.bin", true)));
    ctl.step("load", 0.2);
    doc->check = fruit::checkMesh(doc->model);

    const json& pj = j.at("placement");
    fruit::Placement p;
    p.scale = pj.value("scale", 1.0);
    p.rotation_deg = vecFrom(pj.at("rotation"));
    p.elevation = pj.value("elevation", 5.0);
    p.on_plate = pj.value("on_plate", false);
    doc->place(p);
    doc->low_points = fruit::countLowPoints(doc->placed(), p.on_plate);

    if (j.contains("orient")) {
        for (const auto& v : j["orient"].value("visible", json::array())) doc->orient.visible.push_back(vecFrom(v));
        doc->orient.decided = j["orient"].value("decided", false);
    }
    const json& sj = j.at("supports");
    const json& cj = sj.at("choices");
    SupportChoices& c = doc->supports.choices;
    c.usual_tip = cj.value("usual_tip", 2);
    c.density = cj.value("density", 1.0);
    c.merge = cj.value("merge", true);
    c.advanced = cj.value("advanced", std::map<std::string, double>{});
    if (cj.contains("hinge") && cj["hinge"].is_string()) c.hinge = fruit::parseHinge(cj["hinge"].get<std::string>());
    doc->supports.edits = editsFrom(sj.value("edits", json::array()));

    if (j.contains("print")) {
        const json& prj = j["print"];
        PrintSettings& s = doc->print.settings;
        s.exposure = prj.value("exposure", 0.0);
        s.bottom_exposure = prj.value("bottom_exposure", 0.0);
        s.bottom_layers = prj.value("bottom_layers", -1);
        s.transition_layers = prj.value("transition_layers", -1);
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
        plan.edits = editsFrom(sj.value("planned_around", json::array()));
        for (const auto& w : sj.value("warnings", json::array())) {
            fruit::Warning wn;
            wn.code = w.value("code", "");
            wn.message = w.value("message", "");
            wn.layer = w.value("layer", -1);
            if (w.contains("where")) wn.where = vecFrom(w["where"]);
            plan.warnings.push_back(wn);
        }
        auto r = std::make_shared<SupportResult>(
            finishWithEdits(*doc, c, std::move(plan), doc->supports.edits, ctl.sub(0.3, 1.0)));
        doc->supports.result = std::move(r);
        doc->supports.result_revision = doc->revision();
        doc->supports.result_settings = doc->orient.settings;
    }
    ctl.report("check", 1.0);
    doc->saved = doc->stamp();
    return out;
}

std::vector<std::string> changesSince(const JobStamp& a, const JobStamp& b) {
    std::vector<std::string> out;
    if (a.revision != b.revision) out.push_back("moved, turned or resized the model");
    if (a.model_path != b.model_path) out.push_back("found the model in its new place");
    if (a.orient_settings != b.orient_settings || a.orient_decided != b.orient_decided)
        out.push_back("changed how it stands");
    if (a.edits != b.edits) {
        const std::size_t n = b.edits.size() > a.edits.size() ? b.edits.size() - a.edits.size() : 0;
        out.push_back(n == 1 ? "edited a tip by hand" : n > 1 ? "edited " + std::to_string(n) + " tips by hand"
                                                              : "undid tip edits");
    }
    if (a.supports != b.supports && a.edits == b.edits)
        out.push_back(a.supports ? "generated the supports again" : "generated supports");
    else if (a.supports == b.supports && !(a.choices == b.choices)) out.push_back("changed support settings");
    if (a.sample_path != b.sample_path || a.exposure != b.exposure || a.bottom_exposure != b.bottom_exposure ||
        a.bottom_layers != b.bottom_layers || a.transition_layers != b.transition_layers)
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
