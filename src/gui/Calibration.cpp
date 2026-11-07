// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Calibration.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "calibration/Coupons.h"
#include "formats/GooExport.h"
#include "formats/GooFile.h"
#include "formats/GooSetup.h"
#include "mesh/StlIO.h"

namespace gui {

using nlohmann::json;

PrinterSetup loadPrinterSetup(const std::string& file) {
    PrinterSetup s;
    std::ifstream in(file);
    if (!in) return s;
    try {
        const json j = json::parse(in);
        if (j.contains("orientation")) {
            const json& o = j["orientation"];
            s.orientation_confirmed = o.value("confirmed", false);
            s.flip_x = o.value("flip_x", false);
            s.flip_y = o.value("flip_y", false);
            s.checked_with = o.value("checked_with", std::string());
        }
        if (j.contains("hinge") && j["hinge"].is_string()) s.hinge = fruit::parseHinge(j["hinge"].get<std::string>());
    } catch (const std::exception&) {
        return PrinterSetup{};
    }
    return s;
}

void savePrinterSetup(const std::string& file, const PrinterSetup& s) {
    json j;
    j["orientation"] = {{"confirmed", s.orientation_confirmed},
                        {"flip_x", s.flip_x},
                        {"flip_y", s.flip_y},
                        {"checked_with", s.checked_with}};
    if (s.hinge) j["hinge"] = fruit::hingeName(*s.hinge);
    const std::filesystem::path p(file);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2) << "\n";
        if (!out) throw std::runtime_error("cannot write " + tmp);
    }
    std::filesystem::rename(tmp, file);
}

const char* hingeLabel(HingeChoice c) {
    switch (c) {
        case HingeChoice::Back: return "Back";
        case HingeChoice::Left: return "Left";
        case HingeChoice::Right: return "Right";
        case HingeChoice::Front: return "Front";
    }
    return "";
}

fruit::HingeSide plateSide(HingeChoice c) {
    switch (c) {
        case HingeChoice::Back: return fruit::HingeSide::PlusY;
        case HingeChoice::Left: return fruit::HingeSide::MinusX;
        case HingeChoice::Right: return fruit::HingeSide::PlusX;
        case HingeChoice::Front: return fruit::HingeSide::MinusY;
    }
    return fruit::HingeSide::PlusX;
}

std::optional<HingeChoice> hingeChoice(const std::optional<fruit::HingeSide>& s) {
    if (!s) return std::nullopt;
    for (HingeChoice c : {HingeChoice::Back, HingeChoice::Left, HingeChoice::Right, HingeChoice::Front})
        if (plateSide(c) == *s) return c;
    return std::nullopt;
}

namespace {

// The lit part of one layer, 1 mm around it, as square cells about
// `target` across, flipped as asked.
LayerPicture picture(const fruit::GooFileInfo& f, uint32_t layer, bool flip_x, bool flip_y, int target = 200) {
    LayerPicture pic;
    if (layer >= f.rle.size()) return pic;
    const int W = f.header.res_x, H = f.header.res_y;
    std::vector<std::uint8_t> px(static_cast<std::size_t>(W) * H);
    fruit::decodeGooRle(f.rle[layer], px);
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y) {
        const std::uint8_t* row = &px[static_cast<std::size_t>(y) * W];
        for (int x = 0; x < W; ++x)
            if (row[x]) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    }
    if (x1 < 0) return pic;
    const double pw = static_cast<double>(f.header.size_x) / W, ph = static_cast<double>(f.header.size_y) / H;
    const double margin = 1.0;
    const int mx = static_cast<int>(std::ceil(margin / pw)), my = static_cast<int>(std::ceil(margin / ph));
    x0 = std::max(0, x0 - mx);
    x1 = std::min(W - 1, x1 + mx);
    y0 = std::max(0, y0 - my);
    y1 = std::min(H - 1, y1 + my);
    const double rw = (x1 - x0 + 1) * pw, rh = (y1 - y0 + 1) * ph;
    const double cell = std::max(rw, rh) / target;
    pic.cell_mm = cell;
    pic.w = std::max(1, static_cast<int>(std::ceil(rw / cell)));
    pic.h = std::max(1, static_cast<int>(std::ceil(rh / cell)));
    std::vector<double> sum(static_cast<std::size_t>(pic.w) * pic.h, 0.0);
    std::vector<int> count(sum.size(), 0);
    for (int y = y0; y <= y1; ++y) {
        const int cy = std::min(pic.h - 1, static_cast<int>(((y - y0) + 0.5) * ph / cell));
        for (int x = x0; x <= x1; ++x) {
            const int cx = std::min(pic.w - 1, static_cast<int>(((x - x0) + 0.5) * pw / cell));
            const std::size_t i = static_cast<std::size_t>(cy) * pic.w + cx;
            sum[i] += px[static_cast<std::size_t>(y) * W + x];
            ++count[i];
        }
    }
    pic.lit.resize(sum.size());
    for (int y = 0; y < pic.h; ++y)
        for (int x = 0; x < pic.w; ++x) {
            const std::size_t src = static_cast<std::size_t>(y) * pic.w + x;
            const int tx = flip_x ? pic.w - 1 - x : x, ty = flip_y ? pic.h - 1 - y : y;
            pic.lit[static_cast<std::size_t>(ty) * pic.w + tx] =
                count[src] ? static_cast<std::uint8_t>(std::lround(sum[src] / count[src])) : 0;
        }
    return pic;
}

}  // namespace

void saveOrientationPiece(const std::string& path) { fruit::writeStl(path, fruit::orientationCoupon().mesh); }

OrientationCheck checkOrientation(const std::string& satellite_goo, const fruit::PrinterProfile& printer,
                                  const fruit::JobControl& ctl) {
    OrientationCheck c;
    c.satellite_path = satellite_goo;
    ctl.step("slice", 0);
    const fruit::GooFileInfo theirs = fruit::readGoo(satellite_goo);
    c.software = theirs.header.software_name;
    if (!theirs.header.software_version.empty()) c.software += " " + theirs.header.software_version;
    // The same F, cut with SatelLite's own settings and FruitSlicer's default
    // orientation: the comparison then says which fix FruitSlicer needs.
    fruit::GooPrintRequest req;
    req.layer_height = fruit::gooLayerHeight(theirs.header);
    req.settings_from = &theirs.header;
    fruit::GooPrintSetup setup;
    try {
        setup = fruit::buildGooSetup(printer, req);
    } catch (const std::exception&) {
        throw std::runtime_error("That file has no exposure times. Choose the .goo SatelLite saved for the F.");
    }
    const auto ours_path = (std::filesystem::temp_directory_path() / "FruitSlicer orientation F.goo").string();
    fruit::exportGoo(fruit::orientationCoupon().mesh, setup.header, setup.grid, fruit::GooExportOptions{}, ours_path,
                     ctl.sub(0, 0.7));
    const fruit::GooFileInfo ours = fruit::readGoo(ours_path);
    std::error_code ec;
    std::filesystem::remove(ours_path, ec);
    ctl.step("compare", 0.7);
    c.compare = fruit::compareGoo(ours, theirs);
    c.verdict = fruit::orientationVerdict(c.compare);
    switch (c.verdict) {
        case fruit::OrientationVerdict::FlipX: c.flip_x = true; break;
        case fruit::OrientationVerdict::FlipY: c.flip_y = true; break;
        case fruit::OrientationVerdict::Rotated180: c.flip_x = c.flip_y = true; break;
        default: break;
    }
    ctl.step("compare", 0.8);
    c.ours = picture(ours, c.compare.layer_a, c.flip_x, c.flip_y);
    c.theirs = picture(theirs, c.compare.layer_b, false, false);
    ctl.report("compare", 1.0);
    return c;
}

void writeStandInSatelliteF(const std::string& path, bool flip_x, bool flip_y) {
    const auto printer = fruit::PrinterProfile::saturn4Ultra16k();
    fruit::GooPrintRequest r;
    r.layer_height = 0.03;
    r.exposure = 1.5;
    r.bottom_exposure = 27.5;
    r.flip_x = flip_x;
    r.flip_y = flip_y;
    const fruit::GooPrintSetup s = fruit::buildGooSetup(printer, r);
    const std::string raw = path + ".raw";
    fruit::exportGoo(fruit::orientationCoupon().mesh, s.header, s.grid, fruit::GooExportOptions{}, raw);
    fruit::GooFileInfo f = fruit::readGoo(raw);
    std::error_code ec;
    std::filesystem::remove(raw, ec);
    f.header.mirror_x = printer.image_mirror_x;  // the exporter records the flips it applied; undo that
    f.header.mirror_y = printer.image_mirror_y;
    f.header.software_name = "SatelLite (stand-in)";
    f.header.software_version = "";
    fruit::GooWriter w(path, f.header);
    for (std::size_t i = 0; i < f.rle.size(); ++i) w.addLayer(f.layers[i], f.rle[i]);
    w.finish(f.header);
}

std::string verdictTitle(const OrientationCheck& c) {
    switch (c.verdict) {
        case fruit::OrientationVerdict::Matches: return "FruitSlicer's image matches SatelLite's";
        case fruit::OrientationVerdict::FlipX: return "FruitSlicer's image is mirrored left to right";
        case fruit::OrientationVerdict::FlipY: return "FruitSlicer's image is mirrored top to bottom";
        case fruit::OrientationVerdict::Rotated180: return "FruitSlicer's image is turned around";
        default: return "The two images don't line up";
    }
}

std::string verdictBody(const OrientationCheck& c) {
    const std::string same = "Compared with SatelLite's slice of the same F, ";
    const std::string fix = " Using this fix makes every future print match SatelLite's.";
    switch (c.verdict) {
        case fruit::OrientationVerdict::Matches:
            return same + "FruitSlicer's layer image is the same way round. Nothing needs changing; using this "
                          "records that it is confirmed.";
        case fruit::OrientationVerdict::FlipX:
            return same + "FruitSlicer's layer image is mirrored. Flipping it left to right makes the two match." + fix;
        case fruit::OrientationVerdict::FlipY:
            return same + "FruitSlicer's layer image is upside down. Flipping it top to bottom makes the two match." +
                   fix;
        case fruit::OrientationVerdict::Rotated180:
            return same + "FruitSlicer's layer image is turned around. Flipping it both ways makes the two match." + fix;
        default: {
            char b[200];
            std::snprintf(b, sizeof b, "Even the best match overlaps only %.0f %%. ", c.compare.best_iou * 100);
            return std::string(b) + "Check that SatelLite sliced f.stl as saved, with no supports and no scaling, "
                                    "then try again.";
        }
    }
}

std::string transformLabel(const std::string& t) {
    if (t == "identity") return "As is";
    if (t == "flip_x") return "Flipped left to right";
    if (t == "flip_y") return "Flipped top to bottom";
    if (t == "flip_xy") return "Turned 180°";
    return t;
}

}  // namespace gui
