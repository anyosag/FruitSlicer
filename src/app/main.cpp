// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// fruitslicer — resin support generator CLI.

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <fstream>
#include <iostream>

#include "calibration/ABPlate.h"
#include "calibration/CalibrationFit.h"
#include "calibration/CalibrationPlates.h"
#include "calibration/Coupons.h"
#include "formats/GooCompare.h"
#include "formats/GooExport.h"
#include "formats/GooFile.h"
#include "io/PngWriter.h"
#include "io/Render.h"
#include "layers/IslandGraph.h"
#include "layers/LayerStack.h"
#include "mesh/MeshAnalysis.h"
#include "mesh/Placement.h"
#include "mesh/ObjIO.h"
#include "mesh/StlIO.h"
#include "printer/PrinterProfile.h"
#include "support/PointGenerator.h"
#include "support/OrientationSearch.h"
#include "support/SupportIO.h"
#include "support/SupportTree.h"
#include "support/SupportValidator.h"

using namespace fruit;
using json = nlohmann::json;

namespace {

struct PlacementOpts {
    std::vector<double> rotate{0, 0, 0};
    double elevate = 5.0;
    double scale = 1.0;
};

void addPlacement(CLI::App* cmd, PlacementOpts& o) {
    cmd->add_option("--rotate", o.rotate, "Rotation in degrees about X, Y, Z (applied in that order)")
        ->expected(3);
    cmd->add_option("--elevate", o.elevate, "Height of the model's lowest point above the plate (mm)");
    cmd->add_option("--scale", o.scale, "Uniform scale applied before rotation (e.g. 1000 for metres)");
}

TriangleMesh loadPlaced(const std::string& path, const PlacementOpts& o, json* report = nullptr) {
    TriangleMesh mesh = readMesh(path);
    int cavities = 0;
    const int flipped = mesh.orientShellsOutward(&cavities);
    if (o.scale != 1.0) {
        Transform s;
        for (int i = 0; i < 3; ++i) s.m[i][i] = o.scale;
        mesh.transform(s);
    }
    placeOnPlate(mesh, {o.rotate[0], o.rotate[1], o.rotate[2]}, o.elevate);
    if (report) {
        (*report)["shells_flipped"] = flipped;
        (*report)["cavities"] = cavities;
    }
    return mesh;
}

json bboxJson(const BBox3& b) {
    return {{"min", {b.min.x, b.min.y, b.min.z}}, {"max", {b.max.x, b.max.y, b.max.z}}};
}

int cmdInfo(const std::string& path) {
    TriangleMesh mesh = readMesh(path);
    int shells = 0;
    mesh.shellIds(&shells);
    const int flipped = mesh.orientShellsOutward();
    const auto minima = findMinima(mesh);
    json r;
    r["file"] = path;
    r["triangles"] = mesh.faceCount();
    r["vertices"] = mesh.vertexCount();
    r["shells"] = shells;
    r["shells_flipped"] = flipped;
    r["closed_manifold"] = mesh.isClosedManifold();
    r["bbox"] = bboxJson(mesh.bbox());
    r["volume_mm3"] = mesh.volume();
    r["surface_area_mm2"] = mesh.surfaceArea();
    r["downward_minima"] = minima.size();
    std::cout << r.dump(2) << "\n";
    return 0;
}

int cmdSlice(const std::string& path, const PlacementOpts& place, double layer_h, const std::string& png_dir,
             double png_px, int every, const std::string& json_out) {
    const PrinterProfile printer = PrinterProfile::saturn4Ultra16k();
    json r;
    TriangleMesh mesh = loadPlaced(path, place, &r);
    SliceStats stats;
    const LayerStack layers = sliceModel(mesh, layer_h, &stats);
    const IslandGraph graph(layers, printer.pixelY());  // ~1 px lateral link tolerance
    const auto islands = graph.islands();

    double max_area = 0;
    size_t max_layer = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        const double a = area(layers.slices[i]);
        if (a > max_area) { max_area = a; max_layer = i; }
    }
    const BBox3 b = mesh.bbox();
    r["file"] = path;
    r["printer"] = printer.name;
    r["layer_height"] = layer_h;
    r["layers"] = layers.size();
    r["open_chains"] = stats.open_chains;
    r["bbox"] = bboxJson(b);
    r["fits_plate"] = printer.fitsXY(b.min.x, b.min.y) && printer.fitsXY(b.max.x, b.max.y) && b.max.z <= printer.build_z;
    r["peak_layer_area_mm2"] = max_area;
    r["peak_layer"] = max_layer;
    r["parts"] = graph.parts().size();
    r["islands"] = islands.size();
    json isl = json::array();
    for (int id : islands) {
        const LayerPart& p = graph.part(id);
        isl.push_back({{"layer", p.layer}, {"z", layers.sample_z[p.layer]}, {"area_mm2", p.area},
                       {"center", {p.box.center().x, p.box.center().y}}});
    }
    r["island_list"] = isl;

    if (!png_dir.empty()) {
        std::filesystem::create_directories(png_dir);
        RasterWindow win;
        win.area.extend({b.min.x - 1, b.min.y - 1});
        win.area.extend({b.max.x + 1, b.max.y + 1});
        win.pixel = png_px;
        for (size_t i = 0; i < layers.size(); i += static_cast<size_t>(std::max(1, every))) {
            int w = 0, h = 0;
            const auto img = rasterize(layers.slices[i], win, w, h);
            char name[64];
            std::snprintf(name, sizeof name, "layer_%05zu.png", i);
            writePngGray((std::filesystem::path(png_dir) / name).string(), w, h, img);
        }
    }
    if (!json_out.empty()) std::ofstream(json_out) << r.dump(2) << "\n";
    // Keep stdout short for large models.
    json brief = r;
    brief.erase("island_list");
    std::cout << brief.dump(2) << "\n";
    return 0;
}

int cmdCoupon(const std::string& kind, const std::string& out, double layer_h, const std::string& png_dir) {
    if (kind != "overlap" && kind != "orientation") {
        std::cerr << "unknown coupon '" << kind << "' (available: overlap, orientation)\n";
        return 2;
    }
    const Coupon c = kind == "overlap" ? overlapCoupon() : orientationCoupon();
    writeStl(out, c.mesh);
    json r;
    r["coupon"] = c.name;
    r["stl"] = out;
    r["triangles"] = c.mesh.faceCount();
    json checks = json::array();
    for (const auto& k : c.checks) checks.push_back({{"z_mm", k.z}, {"expect", k.expect}});
    r["checks"] = checks;
    if (!png_dir.empty()) {
        // Reference slices from FruitSlicer's own (union) slicer, to compare
        // against the third-party slicer preview.
        std::filesystem::create_directories(png_dir);
        RasterWindow win;
        win.area.extend({-16, -16});
        win.area.extend({16, 16});
        win.pixel = 0.05;
        for (const auto& k : c.checks) {
            const double z = (std::floor(k.z / layer_h) + 0.5) * layer_h;
            int w = 0, h = 0;
            const auto img = rasterize(sliceMeshAt(c.mesh, z), win, w, h);
            char name[64];
            std::snprintf(name, sizeof name, "overlap_z%05.2f.png", k.z);
            writePngGray((std::filesystem::path(png_dir) / name).string(), w, h, img);
        }
    }
    std::cout << r.dump(2) << "\n";
    return 0;
}

int cmdCalibrate(const std::string& kind, const std::string& out, const std::string& manifest_path,
                 const std::string& csv_path, int seed, int replicates) {
    const PrinterProfile printer = PrinterProfile::saturn4Ultra16k();
    CalibrationPlate plate;
    auto apply = [&](PlateCommon& c) {
        if (seed >= 0) c.seed = static_cast<uint32_t>(seed);
        if (replicates > 0) c.replicates = replicates;
    };
    if (kind == "tips") {
        TipsPlateOptions o;
        apply(o.common);
        plate = tipsPlate(o, printer);
    } else if (kind == "spacing") {
        SpacingPlateOptions o;
        apply(o.common);
        plate = spacingPlate(o, printer);
    } else if (kind == "islands") {
        IslandsPlateOptions o;
        apply(o.common);
        plate = islandsPlate(o, printer);
    } else {
        std::cerr << "unknown plate '" << kind << "' (available: tips, spacing, islands)\n";
        return 2;
    }
    writeStl(out, plate.mesh);
    const json manifest = plate.manifest();
    const std::string mpath = manifest_path.empty() ? out + ".json" : manifest_path;
    const std::string cpath = csv_path.empty() ? out + ".results.csv" : csv_path;
    std::ofstream(mpath) << manifest.dump(2) << "\n";
    std::ofstream(cpath) << plate.csvTemplate();
    const BBox3 b = plate.mesh.bbox();
    json r{{"plate", kind},
           {"stl", out},
           {"manifest", mpath},
           {"results_csv", cpath},
           {"coupons", plate.coupons.size()},
           {"grid", {{"rows", plate.rows}, {"cols", plate.cols}, {"cell_mm", plate.cell}}},
           {"triangles", plate.mesh.faceCount()},
           {"height_mm", b.max.z}};
    std::cout << r.dump(2) << "\n";
    return 0;
}

// Settings shared by points / support / orient.
struct GenCli {
    std::string profile;
    double critical_angle = -1;  // < 0: profile or built-in default
    std::string hinge;           // side of the vat hinge: +x, -x, +y, -y
    double peel_bias = -1;       // < 0: 0.25 when a hinge is given
    bool on_plate = false;
};

void addGenOptions(CLI::App* cmd, GenCli& g) {
    cmd->add_option("--profile", g.profile, "Resin profile from `fruitslicer fit`");
    cmd->add_option("--critical-angle", g.critical_angle,
                    "Surfaces leaning more than this from vertical need support (deg)");
    cmd->add_option("--hinge", g.hinge, "Side of the tilting vat's hinge on the plate: +x, -x, +y or -y")
        ->check(CLI::IsMember({"+x", "-x", "+y", "-y"}));
    cmd->add_option("--peel-bias", g.peel_bias,
                    "Extra peel demand on the side far from the hinge (default 0.25 with --hinge; UNVERIFIED size)");
    cmd->add_flag("--on-plate", g.on_plate, "Print standing on the build plate: no elevation, no raft");
}

// Resin profile first, then explicit command-line overrides.
void applySettings(const GenCli& g, SupportConfig& cfg, TreeConfig* tc, json& r) {
    if (!g.profile.empty()) {
        std::ifstream in(g.profile);
        if (!in) throw std::runtime_error("cannot open profile " + g.profile);
        std::vector<std::string> notes;
        applyProfile(json::parse(in), cfg, tc, &notes);
        r["profile"] = g.profile;
        if (!notes.empty()) r["profile_notes"] = notes;
    }
    if (g.critical_angle > 0) cfg.critical_angle_deg = g.critical_angle;
    if (!g.hinge.empty()) {
        // Peel stress is highest on the side away from the hinge (Wu 2016).
        const double sgn = g.hinge[0] == '+' ? -1.0 : 1.0;
        cfg.peel_axis = g.hinge[1] == 'x' ? Vec2{sgn, 0} : Vec2{0, sgn};
        cfg.peel_bias = g.peel_bias >= 0 ? g.peel_bias : 0.25;
        r["peel"] = {{"hinge", g.hinge}, {"bias", cfg.peel_bias}};
    } else if (g.peel_bias >= 0) {
        throw std::runtime_error("--peel-bias needs --hinge");
    }
    if (g.on_plate) {
        cfg.on_plate = true;
        if (tc) tc->raft = RaftStyle::None;
        r["on_plate"] = true;
    }
}

struct PointsOpts {
    double density = 1.0;
    GenCli gen;
    std::string cosmetic, points_in, edits, out = "points.json", ply, report;
    bool no_prune = false;
    std::string strategy = "greedy";
};

int cmdPoints(const std::string& path, const PlacementOpts& place, double layer_h, const PointsOpts& o) {
    json r;
    TriangleMesh mesh = loadPlaced(path, place, &r);
    SupportConfig cfg;
    cfg.layer_height = layer_h;
    cfg.coverage.density = o.density;
    applySettings(o.gen, cfg, nullptr, r);
    CosmeticSpec cosmetic;
    if (!o.cosmetic.empty()) cosmetic = CosmeticSpec::fromJson(json::parse(std::ifstream(o.cosmetic)));
    PointGenOptions opts;
    opts.prune = !o.no_prune;
    if (o.strategy == "firstfit") opts.strategy = CoverStrategy::FirstFit;
    else if (o.strategy != "greedy") throw std::runtime_error("--strategy must be greedy or firstfit");
    opts.cosmetic = &cosmetic;
    if (!o.edits.empty()) {
        if (o.points_in.empty()) throw std::runtime_error("--edits needs --points-in (the points.json the ids refer to)");
        const auto previous = pointsFromJson(json::parse(std::ifstream(o.points_in)));
        opts.manual = applyEdits(json::parse(std::ifstream(o.edits)), previous, cosmetic);
    } else if (!o.points_in.empty()) {
        // Keep the user's manual points from a previous run.
        for (const auto& p : pointsFromJson(json::parse(std::ifstream(o.points_in))))
            if (p.category == PointCategory::Manual) opts.manual.push_back(p);
    }
    const PointGenResult res = generateSupportPoints(mesh, cfg, opts);

    json out = pointsToJson(res.points);
    out["placement"] = {{"rotate", place.rotate}, {"elevate", place.elevate}, {"layer_height", layer_h}};
    out["source"] = path;
    std::ofstream(o.out) << out.dump(1) << "\n";
    if (!o.ply.empty()) writePointsPly(o.ply, res.points);

    const auto& st = res.stats;
    json counts = json::object();
    for (const auto& p : res.points) counts[categoryName(p.category)] = counts.value(categoryName(p.category), 0) + 1;
    double cap = 0;
    for (const auto& p : res.points) cap += cfg.capacity.capacity(p.tip_diameter);
    r["file"] = path;
    r["points"] = res.points.size();
    r["by_category"] = counts;
    r["total_capacity_mm2"] = cap;
    r["stats"] = {{"layers", st.layers},
                  {"parts", st.parts},
                  {"islands", st.islands},
                  {"samples", st.samples},
                  {"uncovered_samples", st.uncovered},
                  {"tiny_islands_skipped", st.tiny_islands_skipped},
                  {"cavity_samples_skipped", st.cavity_samples_skipped},
                  {"suction_cups", st.suction_cups},
                  {"resin_traps", st.resin_traps},
                  {"capacity_deficit_layers", st.capacity_deficits},
                  {"before_pruning", st.greedy_points},
                  {"pruned", st.pruned},
                  {"peak_layer_area_mm2", st.peak_layer_area},
                  {"peak_layer", st.peak_layer},
                  {"seconds", st.seconds}};
    json warnings = json::array();
    for (const auto& w : res.warnings)
        warnings.push_back({{"code", w.code}, {"message", w.message}, {"layer", w.layer},
                            {"at", {w.where.x, w.where.y, w.where.z}}});
    r["warnings"] = warnings;
    r["output"] = o.out;
    if (!o.report.empty()) std::ofstream(o.report) << r.dump(2) << "\n";
    json brief = r;
    if (brief["warnings"].size() > 8) {
        brief["warnings_shown"] = 8;
        brief["warnings"] = json(std::vector<json>(r["warnings"].begin(), r["warnings"].begin() + 8));
    }
    std::cout << brief.dump(2) << "\n";
    return 0;
}

int cmdRender(const std::string& path, const PlacementOpts& place, const std::string& points_path,
              const std::string& out, double az, double el, int size) {
    TriangleMesh mesh = loadPlaced(path, place);
    std::vector<SupportPoint> pts;
    if (!points_path.empty()) pts = pointsFromJson(json::parse(std::ifstream(points_path)));
    RenderView v;
    v.azimuth_deg = az;
    v.elevation_deg = el;
    v.size = size;
    renderPreview(out, mesh, pts, v);
    std::cout << json{{"png", out}, {"points", pts.size()}}.dump() << "\n";
    return 0;
}

struct SupportOpts {
    std::string points_in, out = "supported.stl", supports_only, report, preview, raft = "pads";
    bool no_validate = false;
    bool no_merge = false;
};

int cmdSupport(const std::string& path, const PlacementOpts& place, double layer_h, const PointsOpts& popts,
               const SupportOpts& o) {
    const PrinterProfile printer = PrinterProfile::saturn4Ultra16k();
    json r;
    TriangleMesh mesh = loadPlaced(path, place, &r);
    std::vector<SupportPoint> pts;
    SupportConfig cfg;
    cfg.layer_height = layer_h;
    cfg.coverage.density = popts.density;
    TreeConfig tc;
    applySettings(popts.gen, cfg, &tc, r);
    if (!o.points_in.empty()) {
        pts = pointsFromJson(json::parse(std::ifstream(o.points_in)));
    } else {
        CosmeticSpec cosmetic;
        if (!popts.cosmetic.empty()) cosmetic = CosmeticSpec::fromJson(json::parse(std::ifstream(popts.cosmetic)));
        PointGenOptions opts;
        opts.cosmetic = &cosmetic;
        const PointGenResult res = generateSupportPoints(mesh, cfg, opts);
        pts = res.points;
        r["point_stats"] = {{"uncovered_samples", res.stats.uncovered},
                            {"capacity_deficit_layers", res.stats.capacity_deficits},
                            {"small_islands_skipped", res.stats.tiny_islands_skipped},
                            {"suction_cups", res.stats.suction_cups},
                            {"resin_traps", res.stats.resin_traps}};
        json w = json::array();
        for (const auto& x : res.warnings) w.push_back({{"code", x.code}, {"message", x.message}});
        r["point_warnings"] = w;
        std::ofstream(o.out + ".points.json") << pointsToJson(pts).dump(1) << "\n";
    }
    tc.merge_pillars = !o.no_merge;
    if (o.raft == "none") tc.raft = RaftStyle::None;
    else if (o.raft != "pads") throw std::runtime_error("--raft must be pads or none");
    const SupportTree tree = buildSupportTree(mesh, pts, tc, printer.minRaftThickness());
    const TriangleMesh sup = tree.mesh(tc);
    {
        StlWriter w(o.out);
        w.add(mesh);
        w.add(sup);
    }
    if (!o.supports_only.empty()) writeStl(o.supports_only, sup);

    size_t anchors = 0;
    for (const auto& P : tree.pillars) anchors += P.on_model;
    json counts = json::object();
    for (const auto& w : tree.warnings) counts[w.code] = counts.value(w.code, 0) + 1;
    const BBox3 b = [&] { BBox3 bb = mesh.bbox(); bb.extend(sup.bbox()); return bb; }();
    r["file"] = path;
    r["output"] = o.out;
    r["points"] = pts.size();
    r["heads"] = tree.heads.size();
    r["pillars"] = tree.pillars.size();
    r["trunks_on_raft"] = tree.trunks();
    r["branches"] = tree.branches.size();
    r["pillars_on_model"] = anchors;
    r["bridges"] = tree.bridges.size();
    r["braces"] = tree.braces.size();
    r["braces_skipped_no_trap"] = tree.braces_skipped_trap;
    r["dropped_points"] = tree.dropped_points;
    r["raft_thickness"] = tree.raft_top;
    r["support_triangles"] = sup.faceCount();
    r["support_volume_ml"] = sup.volume() / 1000.0;  // overlapping shells: upper bound
    r["fits_printer"] = printer.fitsXY(b.min.x, b.min.y) && printer.fitsXY(b.max.x, b.max.y) && b.max.z <= printer.build_z;
    r["tree_warnings"] = counts;
    if (!o.no_validate) {
        const ValidationReport v = validateSupported(mesh, sup, layer_h, cfg.link_grow, cfg.absorb_island_area,
                                                     printer.pixelX() * printer.pixelY());
        json fl = json::array();
        for (const auto& f : v.floating)
            if (fl.size() < 20) fl.push_back({{"layer", f.layer}, {"z", f.z}, {"area_mm2", f.area}, {"at", {f.at.x, f.at.y}}});
        r["validation"] = {{"layers", v.layers},
                           {"plate_contacts", v.first_layer_islands},
                           {"floating_islands", v.floating.size()},
                           {"small_floating_islands", v.small_floating},
                           {"subpixel_slivers", v.subpixel},
                           {"first_floating", fl}};
    }
    if (!o.preview.empty()) {
        RenderView view;
        renderPreview(o.preview, mesh, pts, view, &sup);
        r["preview"] = o.preview;
    }
    if (!o.report.empty()) std::ofstream(o.report) << r.dump(2) << "\n";
    std::cout << r.dump(2) << "\n";
    return 0;
}

struct OrientCli {
    int directions = 64, finalists = 6, top = 3;
    std::vector<double> visible_from;  // flattened triples, model frame
    std::string preview_dir;
    GenCli gen;
};

int cmdOrient(const std::string& path, const PlacementOpts& place, double layer_h, const OrientCli& o) {
    TriangleMesh mesh = readMesh(path);
    mesh.orientShellsOutward();
    Transform base;
    for (int i = 0; i < 3; ++i) base.m[i][i] = place.scale;
    base = base.then(Transform::rotationXYZ(place.rotate[0], place.rotate[1], place.rotate[2]));
    mesh.transform(base);
    OrientOptions opt;
    opt.directions = o.directions;
    opt.finalists = o.finalists;
    opt.elevation = place.elevate;
    opt.cfg.layer_height = layer_h;
    json settings_report;
    applySettings(o.gen, opt.cfg, nullptr, settings_report);
    if (o.gen.on_plate) opt.elevation = 0;
    for (size_t i = 0; i + 2 < o.visible_from.size(); i += 3)
        opt.visible_from.push_back({o.visible_from[i], o.visible_from[i + 1], o.visible_from[i + 2]});
    const auto res = searchOrientations(mesh, opt);
    const Transform rot_only = Transform::rotationXYZ(place.rotate[0], place.rotate[1], place.rotate[2]);
    json list = json::array();
    json input;
    int shown = 0;
    for (const auto& c : res) {
        if (!c.evaluated) break;
        // One --rotate that reproduces this orientation from the original file.
        const Vec3 e = eulerXYZ(rot_only.then(c.rotation));
        json j{{"rotate", {std::round(e.x * 100) / 100, std::round(e.y * 100) / 100, std::round(e.z * 100) / 100}},
               {"points", c.points},
               {"island_points", c.islands},
               {"cosmetic_points", c.cosmetic_points},
               {"peak_layer_area_mm2", std::round(c.peak_area)},
               {"height_mm", std::round(c.height * 10) / 10},
               {"uncovered", c.uncovered},
               {"capacity_deficit_layers", c.capacity_deficits},
               {"score", std::round(c.score * 1000) / 1000},
               {"input_orientation", c.is_input}};
        if (c.is_input) input = j;
        if (shown < o.top) {
            list.push_back(j);
            if (!o.preview_dir.empty()) {
                std::filesystem::create_directories(o.preview_dir);
                TriangleMesh m = mesh;
                m.transform(c.rotation);
                placeOnPlate(m, {0, 0, 0}, place.elevate);
                PointGenOptions po;
                const auto pr = generateSupportPoints(m, opt.cfg, po);
                RenderView v;
                const std::string png = (std::filesystem::path(o.preview_dir) / ("orient_" + std::to_string(shown + 1) + ".png")).string();
                renderPreview(png, m, pr.points, v);
                list.back()["preview"] = png;
            }
            ++shown;
        }
    }
    json r{{"file", path}, {"candidates", res.size()}, {"best", list}, {"input", input}};
    if (settings_report.contains("profile_notes")) r["profile_notes"] = settings_report["profile_notes"];
    std::cout << r.dump(2) << "\n";
    return 0;
}

struct ABCli {
    int copies = 8, seed = 1;
    std::string out = "abplate";
};

int cmdABPlate(const std::string& path, const PlacementOpts& place, double layer_h, const PointsOpts& popts,
               const ABCli& o) {
    const PrinterProfile printer = PrinterProfile::saturn4Ultra16k();
    json r;
    TriangleMesh mesh = loadPlaced(path, place, &r);
    SupportConfig cfg;
    cfg.layer_height = layer_h;
    cfg.coverage.density = popts.density;
    TreeConfig tc;
    applySettings(popts.gen, cfg, &tc, r);
    const PointGenResult res = generateSupportPoints(mesh, cfg, {});
    const SupportTree tree = buildSupportTree(mesh, res.points, tc, printer.minRaftThickness());
    const TriangleMesh sup = tree.mesh(tc);
    ABPlateOptions ao;
    ao.copies = o.copies;
    ao.seed = static_cast<uint32_t>(o.seed);
    const ABPlate plate = makeABPlate(mesh, sup, printer, ao);
    writeStl(o.out + "_A.stl", plate.supported);
    writeStl(o.out + "_B.stl", plate.bare);
    std::ofstream(o.out + ".json") << plate.manifest.dump(2) << "\n";
    std::ofstream(o.out + ".results.csv") << plate.csv;
    size_t a = 0;
    for (const auto& c : plate.manifest["copies"]) a += c["arm"] == "A";
    r["file"] = path;
    r["copies"] = o.copies;
    r["arm_A"] = a;
    r["arm_B"] = static_cast<size_t>(o.copies) - a;
    r["points_per_copy"] = res.points.size();
    r["grid"] = {{"rows", plate.rows}, {"cols", plate.cols}};
    r["files"] = {o.out + "_A.stl", o.out + "_B.stl", o.out + ".json", o.out + ".results.csv"};
    std::cout << r.dump(2) << "\n";
    return 0;
}

struct GooCli {
    std::string out = "out.goo", settings_from;
    double exposure = -1, bottom_exposure = -1;
    int bottom_layers = -1, transition_layers = -1, aa = 1;
    bool flip_x = false, flip_y = false;
    unsigned threads = 0;
};

int cmdGoo(const std::string& path, double layer_h, const GooCli& o) {
    const PrinterProfile printer = PrinterProfile::saturn4Ultra16k();
    TriangleMesh mesh = readMesh(path);
    int cavities = 0;
    mesh.orientShellsOutward(&cavities);
    const BBox3 b = mesh.bbox();
    json r;
    r["file"] = path;
    if (b.min.z < -1e-6 || !printer.fitsXY(b.min.x, b.min.y) || !printer.fitsXY(b.max.x, b.max.y))
        r["warning"] = "model extends below the plate or outside the printable area; it is used as is "
                       "(use the STL written by `fruitslicer support`, which is already placed)";
    GooHeader h;
    h.layer_height = static_cast<float>(layer_h);
    if (!o.settings_from.empty()) {
        const GooFileInfo sample = readGoo(o.settings_from);
        copyPrintSettings(sample.header, h);
        r["settings_from"] = o.settings_from;
        if (std::abs(h.layer_height - layer_h) > 1e-6)
            r["layer_height_note"] = "layer height taken from the sample";
    }
    if (o.exposure > 0) h.print.exposure = static_cast<float>(o.exposure);
    if (o.bottom_exposure > 0) h.print.bottom_exposure = static_cast<float>(o.bottom_exposure);
    if (o.bottom_layers >= 0) h.print.bottom_layers = static_cast<uint32_t>(o.bottom_layers);
    if (o.transition_layers >= 0) h.print.transition_layers = static_cast<uint16_t>(o.transition_layers);
    if (h.print.exposure <= 0 || h.print.bottom_exposure <= 0)
        throw std::runtime_error("exposure times are required: pass --exposure and --bottom-exposure (and "
                                 "--bottom-layers), or --settings-from a .goo exported by SatelLite");
    RasterGrid grid = RasterGrid::fromPrinter(printer);
    grid.width = h.res_x;
    grid.height = h.res_y;
    grid.px = h.size_x / h.res_x;
    grid.py = h.size_y / h.res_y;
    grid.x_left = -h.size_x / 2;
    grid.y_top = h.size_y / 2;
    // Orientation: the sample's flags if given, else the printer default
    // (SatelLite-style: Y mirrored); --flip-x / --flip-y invert either.
    if (o.settings_from.empty()) {
        h.mirror_x = printer.image_mirror_x;
        h.mirror_y = printer.image_mirror_y;
    }
    grid.mirror_x = h.mirror_x != o.flip_x;
    grid.mirror_y = h.mirror_y != o.flip_y;
    GooExportOptions eo;
    eo.aa_levels = o.aa;
    eo.threads = o.threads;
    const GooExportResult res = exportGoo(mesh, h, grid, eo, o.out);
    r["output"] = o.out;
    r["layers"] = res.layers;
    r["bytes"] = res.bytes;
    r["volume_ml"] = res.volume_mm3 / 1000.0;
    r["print_time_h"] = res.print_time_s / 3600.0;
    r["seconds"] = res.seconds;
    r["header"] = {{"version", h.version},
                   {"printer_name", h.printer_name},
                   {"anti_aliasing", o.aa <= 1 ? json("off") : json(o.aa)},
                   {"mirror", {grid.mirror_x, grid.mirror_y}},
                   {"layer_height", h.layer_height},
                   {"exposure_s", h.print.exposure},
                   {"bottom_exposure_s", h.print.bottom_exposure},
                   {"bottom_layers", h.print.bottom_layers},
                   {"transition_layers", h.print.transition_layers},
                   {"wait_after_retract_s", h.print.wait_after_retract}};
    r["unverified"] = "image orientation: compare with SatelLite's slice of `fruitslicer coupon orientation` "
                      "using `fruitslicer goo-compare` before printing";
    if (o.aa > 1)
        r["warning_aa"] = "grey (anti-aliased) pixels are untested on the Saturn 4 Ultra 16K; SatelLite's file "
                          "uses pure black/white";
    std::cout << r.dump(2) << "\n";
    return 0;
}

int cmdGooInfo(const std::string& path, bool decode) {
    const GooFileInfo f = readGoo(path);
    const GooHeader& h = f.header;
    const GooPrintSettings& p = h.print;
    json r;
    r["file"] = path;
    r["version"] = h.version;
    r["software"] = h.software_name + " " + h.software_version;
    r["file_time"] = h.file_time;
    r["printer_name"] = h.printer_name;
    r["printer_type"] = h.printer_type;
    r["profile_name"] = h.profile_name;
    r["aa_level"] = h.aa_level;
    r["grey_level"] = h.grey_level;
    r["blur_level"] = h.blur_level;
    r["grayscale_8bit"] = h.grayscale_8bit;
    r["resolution"] = {h.res_x, h.res_y};
    r["mirror"] = {h.mirror_x, h.mirror_y};
    r["size_mm"] = {h.size_x, h.size_y, h.size_z};
    r["layer_height"] = h.layer_height;
    r["layer_count"] = h.layer_count;
    r["layer_offset"] = h.layer_offset;
    r["print_time_s"] = h.print_time;
    r["volume_mm3"] = h.volume;
    r["weight_g"] = h.weight;
    r["print"] = {{"exposure", p.exposure},
                  {"bottom_exposure", p.bottom_exposure},
                  {"bottom_layers", p.bottom_layers},
                  {"transition_layers", p.transition_layers},
                  {"delay_mode_static", p.delay_mode_static},
                  {"turn_off_time", p.turn_off_time},
                  {"bottom_waits", {p.bottom_wait_before_lift, p.bottom_wait_after_lift, p.bottom_wait_after_retract}},
                  {"waits", {p.wait_before_lift, p.wait_after_lift, p.wait_after_retract}},
                  {"bottom_lift", {p.bottom_lift_distance, p.bottom_lift_speed, p.bottom_lift2_distance, p.bottom_lift2_speed}},
                  {"lift", {p.lift_distance, p.lift_speed, p.lift2_distance, p.lift2_speed}},
                  {"bottom_retract", {p.bottom_retract_distance, p.bottom_retract_speed, p.bottom_retract2_distance, p.bottom_retract2_speed}},
                  {"retract", {p.retract_distance, p.retract_speed, p.retract2_distance, p.retract2_speed}},
                  {"pwm", {p.bottom_pwm, p.pwm}},
                  {"advance_mode", p.advance_mode}};
    r["footer_ok"] = f.footer_ok;
    auto def = [](const GooLayerDef& d) {
        return json{{"z", d.z}, {"exposure", d.exposure}, {"lift", {d.lift_distance, d.lift_speed}},
                    {"retract", {d.retract_distance, d.retract_speed}}, {"pwm", d.pwm}, {"pause", d.pause}};
    };
    if (!f.layers.empty()) {
        r["first_layer"] = def(f.layers.front());
        r["last_layer"] = def(f.layers.back());
    }
    uint64_t total = 0;
    size_t mx = 0;
    for (const auto& d : f.rle) {
        total += d.size();
        mx = std::max(mx, d.size());
    }
    r["rle_bytes"] = {{"total", total}, {"max_layer", mx}};
    if (decode) {
        std::vector<uint8_t> px(static_cast<size_t>(h.res_x) * h.res_y);
        double lit = 0;
        std::map<int, uint64_t> greys;
        for (const auto& d : f.rle) {
            decodeGooRle(d, px);
            for (uint8_t v : px)
                if (v) {
                    lit += v / 255.0;
                    ++greys[v];
                }
        }
        const double pa = static_cast<double>(h.size_x) / h.res_x * h.size_y / h.res_y;
        r["decoded"] = {{"checksums_ok", true}, {"volume_mm3", lit * pa * h.layer_height},
                        {"distinct_grey_values", greys.size()}};
    }
    std::cout << r.dump(2) << "\n";
    return 0;
}

int cmdGooCompare(const std::string& a_path, const std::string& b_path, double z) {
    const GooFileInfo a = readGoo(a_path), b = readGoo(b_path);
    const GooCompareResult c = compareGoo(a, b, z);
    json r;
    r["a"] = a_path;
    r["b"] = b_path;
    json hd = json::array();
    for (const auto& d : c.header) hd.push_back({{"field", d.field}, {"a", d.a}, {"b", d.b}});
    r["header_differences"] = hd;
    json sc = json::object();
    for (const auto& s : c.scores) sc[s.transform] = std::round(s.iou * 1000) / 1000;
    r["image"] = {{"layer_a", c.layer_a}, {"z_a", c.z_a}, {"layer_b", c.layer_b}, {"z_b", c.z_b},
                  {"lit_cells_a", c.cells_a}, {"lit_cells_b", c.cells_b}, {"iou", sc}, {"best", c.best}};
    std::string verdict;
    if (c.best_iou < 0.5) verdict = "images differ (different models, scale or layer?); orientation not determined";
    else if (c.best == "identity") verdict = "orientation matches";
    else if (c.best == "flip_x") verdict = "A is mirrored in X relative to B: use --flip-x";
    else if (c.best == "flip_y") verdict = "A is mirrored in Y relative to B: use --flip-y";
    else verdict = "A is rotated 180 degrees relative to B: use --flip-x --flip-y";
    r["verdict"] = verdict;
    std::cout << r.dump(2) << "\n";
    return 0;
}

struct FitCli {
    std::vector<std::string> results;
    std::string out = "resin_profile.json", resin;
    double layer = 0, exposure = 0, temp = 0, fraction = 0.7;
};

int cmdFit(const FitCli& o) {
    std::vector<PlateResults> plates;
    for (const auto& csv : o.results) plates.push_back(loadPlateResults(manifestForResults(csv), csv));
    FitOptions fo;
    fo.resin = o.resin;
    fo.layer_height = o.layer;
    fo.exposure_s = o.exposure;
    fo.vat_temp_c = o.temp;
    fo.capacity_fraction = o.fraction;
    const json prof = fitProfile(plates, fo);
    std::ofstream(o.out) << prof.dump(2) << "\n";
    json brief = prof;
    brief.erase("fits");
    brief["written"] = o.out;
    std::cout << brief.dump(2) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"FruitSlicer — resin support generator (Elegoo Saturn 4 Ultra 16K)"};
    app.require_subcommand(1);

    std::string in_path;
    auto* info = app.add_subcommand("info", "Mesh statistics (shells, manifoldness, volume, minima)");
    info->add_option("model", in_path, "STL or OBJ file")->required()->check(CLI::ExistingFile);

    PlacementOpts place;
    double layer_h = 0.05, png_px = 0.05;
    int every = 1;
    std::string png_dir, json_out;
    auto* slice = app.add_subcommand("slice", "Slice a placed model and report layers and islands");
    slice->add_option("model", in_path, "STL or OBJ file")->required()->check(CLI::ExistingFile);
    addPlacement(slice, place);
    slice->add_option("--layer", layer_h, "Layer height (mm)");
    slice->add_option("--png-dir", png_dir, "Write debug PNGs of the layers here");
    slice->add_option("--png-px", png_px, "Debug PNG pixel size (mm)");
    slice->add_option("--every", every, "Only write every Nth layer PNG");
    slice->add_option("--json", json_out, "Write the full report (including islands) here");

    std::string coupon_kind = "overlap", coupon_out = "overlap_coupon.stl";
    auto* coupon = app.add_subcommand("coupon", "Generate a test print");
    coupon->add_option("kind", coupon_kind, "Coupon kind: overlap | orientation");
    coupon->add_option("-o,--output", coupon_out, "Output STL");
    coupon->add_option("--layer", layer_h, "Layer height for reference slices (mm)");
    coupon->add_option("--png-dir", png_dir, "Write reference slice PNGs here");

    std::string plate_kind, plate_out, manifest_out, csv_out;
    int seed = -1, replicates = 0;
    auto* calibrate = app.add_subcommand("calibrate", "Generate a calibration plate (tips, spacing, islands)");
    calibrate->add_option("kind", plate_kind, "Plate kind: tips | spacing | islands")->required();
    calibrate->add_option("-o,--output", plate_out, "Output STL")->required();
    calibrate->add_option("--manifest", manifest_out, "Manifest JSON (default: <output>.json)");
    calibrate->add_option("--csv", csv_out, "Results CSV template (default: <output>.results.csv)");
    calibrate->add_option("--seed", seed, "Layout shuffle seed");
    calibrate->add_option("--replicates", replicates, "Copies of each parameter combination");

    PointsOpts popts;
    auto* points = app.add_subcommand("points", "Generate support points (M1): coverage + capacity + islands");
    points->add_option("model", in_path, "STL or OBJ file")->required()->check(CLI::ExistingFile);
    addPlacement(points, place);
    points->add_option("--layer", layer_h, "Layer height (mm)");
    points->add_option("--density", popts.density, "Support density (1 = default, >1 denser)");
    addGenOptions(points, popts.gen);
    points->add_option("--cosmetic", popts.cosmetic, "Cosmetic regions JSON (keep_out boxes, visible_from directions)");
    points->add_option("--points-in", popts.points_in, "Previous points.json (keeps its manual points; ids for --edits)");
    points->add_option("--edits", popts.edits, "Edits JSON (add / delete / move / resize)");
    points->add_option("-o,--output", popts.out, "Output points JSON");
    points->add_option("--ply", popts.ply, "Also write a coloured PLY of the points");
    points->add_option("--report", popts.report, "Write the full report JSON here");
    points->add_flag("--no-prune", popts.no_prune, "Skip reverse-delete pruning");
    points->add_option("--strategy", popts.strategy, "greedy (default) or firstfit (PrusaSlicer-style baseline)");

    std::string render_points, render_out = "preview.png";
    double az = 30, el = -35;
    int render_size = 900;
    auto* render = app.add_subcommand("render", "Preview PNG of the placed model and its support points");
    render->add_option("model", in_path, "STL or OBJ file")->required()->check(CLI::ExistingFile);
    addPlacement(render, place);
    render->add_option("--points", render_points, "points.json to draw");
    render->add_option("-o,--output", render_out, "Output PNG");
    render->add_option("--azimuth", az, "View azimuth (deg)");
    render->add_option("--elevation", el, "View elevation (deg, negative = from below)");
    render->add_option("--size", render_size, "Image size (px)");

    SupportOpts sopts;
    auto* support = app.add_subcommand("support", "Generate points, heads, pillars and raft; write model + supports STL");
    support->add_option("model", in_path, "STL or OBJ file")->required()->check(CLI::ExistingFile);
    addPlacement(support, place);
    support->add_option("--layer", layer_h, "Layer height (mm)");
    support->add_option("--density", popts.density, "Support density (1 = default)");
    addGenOptions(support, popts.gen);
    support->add_option("--cosmetic", popts.cosmetic, "Cosmetic regions JSON");
    support->add_option("--points", sopts.points_in, "Use these points (e.g. edited) instead of generating");
    support->add_option("-o,--output", sopts.out, "Output STL: model + supports");
    support->add_option("--supports-only", sopts.supports_only, "Also write the supports alone");
    support->add_option("--raft", sopts.raft, "pads (default) or none");
    support->add_option("--report", sopts.report, "Report JSON");
    support->add_option("--preview", sopts.preview, "Preview PNG (from below)");
    support->add_flag("--no-validate", sopts.no_validate, "Skip the floating-island check");
    support->add_flag("--no-merge", sopts.no_merge, "Keep one pillar per head cluster (no branching trunks)");

    OrientCli ocli;
    auto* orient = app.add_subcommand("orient", "Search build orientations; prints the best --rotate values");
    orient->add_option("model", in_path, "STL or OBJ file")->required()->check(CLI::ExistingFile);
    addPlacement(orient, place);
    orient->add_option("--layer", layer_h, "Layer height used to evaluate finalists (mm)");
    addGenOptions(orient, ocli.gen);
    orient->add_option("--directions", ocli.directions, "Candidate build directions");
    orient->add_option("--finalists", ocli.finalists, "Candidates evaluated with the full generator");
    orient->add_option("--top", ocli.top, "How many results to print");
    orient->add_option("--visible-from", ocli.visible_from,
                       "Cosmetic hint in the model's own frame: faces seen from this direction (x y z), repeatable")
        ->expected(3, 30);
    orient->add_option("--preview-dir", ocli.preview_dir, "Write a preview PNG for each printed result");

    ABCli abcli;
    auto* abplate = app.add_subcommand("abplate", "Same-plate A/B test: FruitSlicer-supported vs bare copies");
    abplate->add_option("model", in_path, "STL or OBJ file")->required()->check(CLI::ExistingFile);
    addPlacement(abplate, place);
    abplate->add_option("--layer", layer_h, "Layer height (mm)");
    addGenOptions(abplate, popts.gen);
    abplate->add_option("--copies", abcli.copies, "Number of copies (split evenly between the arms)");
    abplate->add_option("--seed", abcli.seed, "Arm shuffle seed");
    abplate->add_option("-o,--output", abcli.out, "Output name prefix");

    GooCli gcli;
    auto* goo = app.add_subcommand("goo", "Write an Elegoo .goo print file from a placed, supported model");
    goo->add_option("model", in_path, "STL or OBJ (e.g. the STL written by `fruitslicer support`)")
        ->required()
        ->check(CLI::ExistingFile);
    goo->add_option("-o,--output", gcli.out, "Output .goo");
    goo->add_option("--layer", layer_h, "Layer height (mm)");
    goo->add_option("--exposure", gcli.exposure, "Normal layer exposure (s)");
    goo->add_option("--bottom-exposure", gcli.bottom_exposure, "Bottom layer exposure (s)");
    goo->add_option("--bottom-layers", gcli.bottom_layers, "Number of bottom layers");
    goo->add_option("--transition-layers", gcli.transition_layers, "Layers ramping from bottom to normal exposure");
    goo->add_option("--aa", gcli.aa, "Anti-aliasing grey levels (1 = off, the default, as SatelLite; 8; 256 = full)");
    goo->add_option("--settings-from", gcli.settings_from, "Copy print settings and identity from this .goo (e.g. SatelLite's)")
        ->check(CLI::ExistingFile);
    goo->add_flag("--flip-x", gcli.flip_x, "Flip the image in X relative to the default orientation");
    goo->add_flag("--flip-y", gcli.flip_y, "Flip the image in Y relative to the default orientation");
    goo->add_option("--threads", gcli.threads, "Worker threads (0 = all cores)");

    bool goo_decode = false;
    auto* gooinfo = app.add_subcommand("goo-info", "Show a .goo file's header and layer summary");
    gooinfo->add_option("file", in_path, ".goo file")->required()->check(CLI::ExistingFile);
    gooinfo->add_flag("--decode", goo_decode, "Decode every layer: verify checksums, measure volume and grey levels");

    std::string cmp_a, cmp_b;
    double cmp_z = -1;
    auto* goocmp = app.add_subcommand("goo-compare", "Compare two .goo files: header fields and image orientation");
    goocmp->add_option("a", cmp_a, "First .goo (e.g. FruitSlicer's)")->required()->check(CLI::ExistingFile);
    goocmp->add_option("b", cmp_b, "Second .goo (e.g. SatelLite's)")->required()->check(CLI::ExistingFile);
    goocmp->add_option("--z", cmp_z, "Compare the layers nearest this height (mm); default: half the shorter file");

    FitCli fcli;
    auto* fit = app.add_subcommand("fit", "Fit a resin profile from filled-in calibration result sheets");
    fit->add_option("results", fcli.results, "<plate>.stl.results.csv files (manifests are found next to them)")
        ->required()
        ->check(CLI::ExistingFile);
    fit->add_option("-o,--output", fcli.out, "Output resin profile JSON");
    fit->add_option("--resin", fcli.resin, "Resin name (recorded in the profile)");
    fit->add_option("--layer", fcli.layer, "Layer height the plates were printed at (mm)");
    fit->add_option("--exposure", fcli.exposure, "Normal exposure time (s)");
    fit->add_option("--temp", fcli.temp, "Vat temperature (°C)");
    fit->add_option("--fraction", fcli.fraction, "Usable share of the 50 % failure load (default 0.7)");

    CLI11_PARSE(app, argc, argv);
    try {
        if (*info) return cmdInfo(in_path);
        if (*slice) return cmdSlice(in_path, place, layer_h, png_dir, png_px, every, json_out);
        if (*coupon) return cmdCoupon(coupon_kind, coupon_out, layer_h, png_dir);
        if ((*points || *support || *abplate) && popts.gen.on_plate) place.elevate = 0;
        if (*abplate) return cmdABPlate(in_path, place, layer_h, popts, abcli);
        if (*points) return cmdPoints(in_path, place, layer_h, popts);
        if (*orient) return cmdOrient(in_path, place, layer_h, ocli);
        if (*fit) return cmdFit(fcli);
        if (*goo) return cmdGoo(in_path, layer_h, gcli);
        if (*gooinfo) return cmdGooInfo(in_path, goo_decode);
        if (*goocmp) return cmdGooCompare(cmp_a, cmp_b, cmp_z);
        if (*support) return cmdSupport(in_path, place, layer_h, popts, sopts);
        if (*render) return cmdRender(in_path, place, render_points, render_out, az, el, render_size);
        if (*calibrate) return cmdCalibrate(plate_kind, plate_out, manifest_out, csv_out, seed, replicates);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
