// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Setup.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "Format.h"
#include "formats/GooExport.h"

namespace gui {

namespace fs = std::filesystem;

namespace {

std::string seconds(double s) { return strf("%g s", std::round(s * 100) / 100); }

}  // namespace

std::vector<SampleFact> sampleFacts(const fruit::GooHeader& h) {
    const fruit::GooPrintSettings& p = h.print;
    std::vector<SampleFact> out;
    out.push_back({"Layer height", strf("%g mm", fruit::gooLayerHeight(h)), "Used for slicing"});
    out.push_back({"Bottom layers", strf("%u × %s", p.bottom_layers, seconds(p.bottom_exposure).c_str()),
                   "Long exposures to stick to the plate"});
    if (p.transition_layers == 0) {
        out.push_back({"Transition", "None", "Straight from bottom to normal exposure"});
    } else {
        // The ramp as the printer runs it, from the file's own rule.
        std::string ramp;
        const unsigned n = p.transition_layers;
        for (unsigned i = 0; i < n; ++i) {
            if (n > 6 && i == 2) {
                ramp += " → …";
                i = n - 2;
                continue;
            }
            if (!ramp.empty()) ramp += " → ";
            ramp += strf("%.2f", fruit::layerDefFor(h, p.bottom_layers + i).exposure);
        }
        out.push_back({"Transition", strf("%u layers", n), ramp + " s"});
    }
    out.push_back({"Normal exposure", seconds(p.exposure), "Every other layer"});
    if (std::abs(p.bottom_wait_after_retract - p.wait_after_retract) < 1e-3)
        out.push_back({"Wait after retract", seconds(p.wait_after_retract), "Bottom and normal layers"});
    else
        out.push_back({"Wait after retract",
                       seconds(p.bottom_wait_after_retract) + " / " + seconds(p.wait_after_retract),
                       "Bottom / normal layers"});
    if (std::abs(p.lift_distance - p.retract_distance) < 1e-4)
        out.push_back({"Lift and retract", strf("%g mm", std::round(p.lift_distance * 1000) / 1000),
                       "As SatelLite set them"});
    else
        out.push_back({"Lift and retract",
                       strf("%g / %g mm", std::round(p.lift_distance * 1000) / 1000,
                            std::round(p.retract_distance * 1000) / 1000),
                       "As SatelLite set them"});
    out.push_back({"Anti-aliasing", h.aa_level <= 1 ? std::string("Off") : strf("%u levels", h.aa_level),
                   h.aa_level <= 1 ? "Pure black and white pixels" : "Grey pixels at the edges"});
    out.push_back({"Image mirror",
                   h.mirror_x && h.mirror_y ? "X and Y"
                   : h.mirror_x             ? "X"
                   : h.mirror_y             ? "Y"
                                            : "None",
                   "From the file's header; the image orientation check confirms it"});
    return out;
}

std::string keepSampleHeader(const std::string& goo, const std::string& dir) {
    loadSample(goo);  // refuses what can't be read and files made for another printer
    std::vector<char> bytes(fruit::kGooHeaderSize);
    {
        std::ifstream in(goo, std::ios::binary);
        if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("The file is shorter than a print file's header.");
    }
    fs::create_directories(dir);
    const fs::path out = fs::path(dir) / fs::path(goo).filename();
    const fs::path tmp = out.string() + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!o) throw std::runtime_error("Couldn't write " + tmp.string());
    }
    fs::rename(tmp, out);
    return out.string();
}

}  // namespace gui
