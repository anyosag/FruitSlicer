// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// fruitslicer — resin support generator CLI.

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

#include "calibration/CalibrationPlates.h"
#include "calibration/Coupons.h"
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
    if (kind != "overlap") {
        std::cerr << "unknown coupon '" << kind << "' (available: overlap)\n";
        return 2;
    }
    const Coupon c = overlapCoupon();
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

struct PointsOpts {
    double density = 1.0;
    double critical_angle = 45.0;
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
    cfg.critical_angle_deg = o.critical_angle;
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
    cfg.critical_angle_deg = popts.critical_angle;
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
                            {"small_islands_skipped", res.stats.tiny_islands_skipped}};
        json w = json::array();
        for (const auto& x : res.warnings) w.push_back({{"code", x.code}, {"message", x.message}});
        r["point_warnings"] = w;
        std::ofstream(o.out + ".points.json") << pointsToJson(pts).dump(1) << "\n";
    }
    TreeConfig tc;
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
    std::cout << r.dump(2) << "\n";
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
    coupon->add_option("kind", coupon_kind, "Coupon kind (overlap)");
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
    points->add_option("--critical-angle", popts.critical_angle, "Surfaces leaning more than this from vertical need support (deg)");
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
    support->add_option("--critical-angle", popts.critical_angle, "Critical overhang angle from vertical (deg)");
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
    orient->add_option("--directions", ocli.directions, "Candidate build directions");
    orient->add_option("--finalists", ocli.finalists, "Candidates evaluated with the full generator");
    orient->add_option("--top", ocli.top, "How many results to print");
    orient->add_option("--visible-from", ocli.visible_from,
                       "Cosmetic hint in the model's own frame: faces seen from this direction (x y z), repeatable")
        ->expected(3, 30);
    orient->add_option("--preview-dir", ocli.preview_dir, "Write a preview PNG for each printed result");

    CLI11_PARSE(app, argc, argv);
    try {
        if (*info) return cmdInfo(in_path);
        if (*slice) return cmdSlice(in_path, place, layer_h, png_dir, png_px, every, json_out);
        if (*coupon) return cmdCoupon(coupon_kind, coupon_out, layer_h, png_dir);
        if (*points) return cmdPoints(in_path, place, layer_h, popts);
        if (*orient) return cmdOrient(in_path, place, layer_h, ocli);
        if (*support) return cmdSupport(in_path, place, layer_h, popts, sopts);
        if (*render) return cmdRender(in_path, place, render_points, render_out, az, el, render_size);
        if (*calibrate) return cmdCalibrate(plate_kind, plate_out, manifest_out, csv_out, seed, replicates);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
