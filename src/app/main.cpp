// fruitslicer — resin support generator CLI.

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

#include "calibration/Coupons.h"
#include "io/PngWriter.h"
#include "layers/IslandGraph.h"
#include "layers/LayerStack.h"
#include "mesh/MeshAnalysis.h"
#include "mesh/Placement.h"
#include "mesh/StlIO.h"
#include "printer/PrinterProfile.h"

using namespace fruit;
using json = nlohmann::json;

namespace {

struct PlacementOpts {
    std::vector<double> rotate{0, 0, 0};
    double elevate = 5.0;
};

void addPlacement(CLI::App* cmd, PlacementOpts& o) {
    cmd->add_option("--rotate", o.rotate, "Rotation in degrees about X, Y, Z (applied in that order)")
        ->expected(3);
    cmd->add_option("--elevate", o.elevate, "Height of the model's lowest point above the plate (mm)");
}

TriangleMesh loadPlaced(const std::string& path, const PlacementOpts& o, json* report = nullptr) {
    TriangleMesh mesh = readStl(path);
    const int flipped = mesh.orientShellsOutward();
    placeOnPlate(mesh, {o.rotate[0], o.rotate[1], o.rotate[2]}, o.elevate);
    if (report) (*report)["shells_flipped"] = flipped;
    return mesh;
}

json bboxJson(const BBox3& b) {
    return {{"min", {b.min.x, b.min.y, b.min.z}}, {"max", {b.max.x, b.max.y, b.max.z}}};
}

int cmdInfo(const std::string& path) {
    TriangleMesh mesh = readStl(path);
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

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"FruitSlicer — resin support generator (Elegoo Saturn 4 Ultra 16K)"};
    app.require_subcommand(1);

    std::string in_path;
    auto* info = app.add_subcommand("info", "Mesh statistics (shells, manifoldness, volume, minima)");
    info->add_option("model", in_path, "STL file")->required()->check(CLI::ExistingFile);

    PlacementOpts place;
    double layer_h = 0.05, png_px = 0.05;
    int every = 1;
    std::string png_dir, json_out;
    auto* slice = app.add_subcommand("slice", "Slice a placed model and report layers and islands");
    slice->add_option("model", in_path, "STL file")->required()->check(CLI::ExistingFile);
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

    CLI11_PARSE(app, argc, argv);
    try {
        if (*info) return cmdInfo(in_path);
        if (*slice) return cmdSlice(in_path, place, layer_h, png_dir, png_px, every, json_out);
        if (*coupon) return cmdCoupon(coupon_kind, coupon_out, layer_h, png_dir);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
