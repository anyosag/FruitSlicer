// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "GooTools.h"

#include <algorithm>
#include <cmath>

#include "Format.h"

namespace gui {

namespace {

std::string seconds(double s) { return strf("%g s", s); }

double mb(uint64_t bytes) { return static_cast<double>(bytes) / 1e6; }

// The waits of one kind of layer: "0, 0 and 5 s".
std::string waits(float before, float after_lift, float after_retract) {
    return strf("%g, %g and %g s", before, after_lift, after_retract);
}

}  // namespace

std::string durationText(uint32_t s) {
    if (s < 60) return strf("%u s", s);
    if (s < 3600) return strf("%u min %u s", s / 60, s % 60);
    return strf("%u h %02u min", s / 3600, (s % 3600 + 30) / 60 % 60);
}

std::string volumeText(double mm3, int decimals) {
    if (mm3 >= 100) return strf("%.*f ml", decimals, mm3 / 1000);
    return strf("%.1f mm³", mm3);
}

std::string bytesText(uint64_t bytes) {
    if (bytes >= 1'000'000) return strf("%.1f MB", mb(bytes));
    if (bytes >= 1'000) return strf("%.0f KB", static_cast<double>(bytes) / 1e3);
    return strf("%llu bytes", static_cast<unsigned long long>(bytes));
}

std::string mirrorText(bool x, bool y) {
    return x && y ? "both ways" : x ? "left to right" : y ? "top to bottom" : "none";
}

GooSummary summarizeGoo(const fruit::GooHeader& h, uint64_t file_bytes) {
    const fruit::GooPrintSettings& p = h.print;
    GooSummary s;
    std::string when = h.file_time;
    if (when.size() > 16) when.resize(16);  // to the minute
    s.made_by = "Made by " + h.software_name + " " + h.software_version + (when.empty() ? "" : " on " + when) +
                " · for " + h.printer_name;
    s.layers = h.layer_count;
    const fruit::GooLayerDef first = h.layer_count ? fruit::layerDefFor(h, 0) : fruit::GooLayerDef{};
    const fruit::GooLayerDef last = h.layer_count ? fruit::layerDefFor(h, h.layer_count - 1) : fruit::GooLayerDef{};
    s.height_mm = last.z;
    s.print_time_s = h.print_time;
    s.volume_ml = h.volume / 1000.0;
    s.weight_g = h.weight;
    s.resin = h.volume > 0 ? volumeText(h.volume) + " · " + strf(h.weight >= 1 ? "%.1f g" : "%.2f g", h.weight)
                           : std::string("—");
    const std::string version = !h.version.empty() && (h.version[0] == 'V' || h.version[0] == 'v')
                                    ? h.version.substr(1)
                                    : h.version;
    s.groups.push_back({"File",
                        {{"Format", "GOO version " + version},
                         {"Printer", h.printer_name},
                         {"Profile name", h.profile_name.empty() ? "—" : h.profile_name}}});
    s.groups.push_back({"Image",
                        {{"Screen", strf("%u × %u", h.res_x, h.res_y)},
                         {"Mirror", mirrorText(h.mirror_x, h.mirror_y)},
                         {"Anti-aliasing", h.aa_level <= 1 && !h.grayscale_8bit ? "off" : strf("%u levels", h.aa_level)},
                         {"Build size", strf("%.2f × %.2f × %g mm", h.size_x, h.size_y, h.size_z)}}});
    s.groups.push_back({"Exposure",
                        {{"Layer height", strf("%g mm", h.layer_height)},
                         {"Bottom layers", strf("%u × %g s", p.bottom_layers, p.bottom_exposure)},
                         {"Transition", strf("%u layer%s", p.transition_layers, p.transition_layers == 1 ? "" : "s")},
                         {"Normal", seconds(p.exposure)}}});
    GooGroup w{"Waits", {}};
    if (!p.delay_mode_static) {
        w.rows.push_back({"Between layers", strf("light off for %g s", p.turn_off_time)});
    } else {
        w.rows.push_back({"Before lift", seconds(p.wait_before_lift)});
        w.rows.push_back({"After lift", seconds(p.wait_after_lift)});
        w.rows.push_back({"After retract", seconds(p.wait_after_retract)});
        const bool same = p.bottom_wait_before_lift == p.wait_before_lift && p.bottom_wait_after_lift == p.wait_after_lift &&
                          p.bottom_wait_after_retract == p.wait_after_retract;
        w.rows.push_back({"Bottom layers", same ? "the same"
                                                : waits(p.bottom_wait_before_lift, p.bottom_wait_after_lift,
                                                        p.bottom_wait_after_retract)});
    }
    s.groups.push_back(w);
    const bool second = p.lift2_distance != 0 || p.retract2_distance != 0 || p.bottom_lift2_distance != 0 ||
                        p.bottom_retract2_distance != 0;
    s.groups.push_back(
        {"Movement",
         {{"Lift", strf("%g mm, speed %g", p.lift_distance, p.lift_speed)},
          {"Retract", strf("%g mm, speed %g", p.retract_distance, p.retract_speed)},
          {"Second stage", second ? strf("lift %g mm, retract %g mm", p.lift2_distance, p.retract2_distance) : "none"},
          {"Light power", p.bottom_pwm == p.pwm ? strf("%u of 255", p.pwm)
                                                : strf("%u of 255 (bottom %u)", p.pwm, p.bottom_pwm)}}});
    const uint64_t overhead = fruit::kGooHeaderSize + 11;  // the header, and the footer: 3 zero bytes and the magic
    s.groups.push_back({"Layers",
                        {{"First", strf("z %g mm · %g s", first.z, first.exposure)},
                         {"Last", strf("z %g mm · %g s", last.z, last.exposure)},
                         {"Layer data", bytesText(file_bytes > overhead ? file_bytes - overhead : 0)}}});
    return s;
}

std::vector<GooCheckLine> checkLines(const fruit::GooVerifyResult& v, const fruit::GooHeader& h) {
    std::vector<GooCheckLine> out;
    if (v.checksums_ok)
        out.push_back({true, strf("All %u layers read back; every checksum is valid", v.layers)});
    else
        out.push_back({false, strf("Layer %d didn't read back: ", v.first_bad_layer + 1) + v.error});
    if (v.layers != h.layer_count)
        out.push_back({false, strf("The header says %u layers, but the file holds %u", h.layer_count, v.layers)});
    if (v.grey_values == 0)
        out.push_back({true, "Only black and white pixels: anti-aliasing is off"});
    else
        out.push_back({true, strf("%zu grey levels besides black and white: anti-aliased edges", v.grey_values)});
    const double lit = v.volume_mm3, said = h.volume;
    if (said <= 0)
        out.push_back({true, "Lit pixels add up to " + volumeText(lit, 2) + "; the header gives no volume"});
    else
        out.push_back({std::abs(lit - said) <= 0.02 * said,
                       "Lit pixels add up to " + volumeText(lit, 2) + "; the header says " + volumeText(said, 2)});
    out.push_back({v.footer_ok, v.footer_ok ? "The file ends correctly" : "The file's end is missing or damaged"});
    return out;
}

std::vector<unsigned char> previewRgba(const std::vector<uint8_t>& rgb565, int w, int h) {
    if (w <= 0 || h <= 0 || rgb565.size() != static_cast<std::size_t>(w) * h * 2) return {};
    std::vector<unsigned char> out(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
        const unsigned v = static_cast<unsigned>(rgb565[2 * i]) << 8 | rgb565[2 * i + 1];
        out[4 * i] = static_cast<unsigned char>((v >> 11 & 31) * 255 / 31);
        out[4 * i + 1] = static_cast<unsigned char>((v >> 5 & 63) * 255 / 63);
        out[4 * i + 2] = static_cast<unsigned char>((v & 31) * 255 / 31);
        out[4 * i + 3] = 255;
    }
    return out;
}

std::vector<GooDiffRow> diffRows(const std::vector<fruit::GooFieldDiff>& d) {
    struct Name {
        const char* field;
        const char* label;
        bool cures;
        const char* unit;  // "yes/no" and "mode" are words, not units
    };
    static const Name names[] = {
        {"version", "Format version", false, ""},
        {"software", "Software", false, ""},
        {"printer_name", "Printer", false, ""},
        {"printer_type", "Printer type", false, ""},
        {"profile_name", "Profile name", false, ""},
        {"layer_count", "Layers", false, ""},
        {"size_z", "Build height", false, " mm"},
        {"aa_level", "Anti-aliasing levels", true, ""},
        {"grey_level", "Grey level", true, ""},
        {"blur_level", "Blur", true, ""},
        {"grayscale_8bit", "8-bit greys", true, "yes/no"},
        {"res_x", "Screen width", true, " px"},
        {"res_y", "Screen height", true, " px"},
        {"mirror_x", "Mirrored left to right", true, "yes/no"},
        {"mirror_y", "Mirrored top to bottom", true, "yes/no"},
        {"size_x", "Build width", true, " mm"},
        {"size_y", "Build depth", true, " mm"},
        {"layer_height", "Layer height", true, " mm"},
        {"exposure", "Normal exposure", true, " s"},
        {"bottom_exposure", "Bottom exposure", true, " s"},
        {"bottom_layers", "Bottom layers", true, ""},
        {"transition_layers", "Transition layers", true, ""},
        {"delay_mode_static", "Between layers", true, "mode"},
        {"turn_off_time", "Light-off time", true, " s"},
        {"bottom_wait_before_lift", "Bottom wait before lift", true, " s"},
        {"bottom_wait_after_lift", "Bottom wait after lift", true, " s"},
        {"bottom_wait_after_retract", "Bottom wait after retract", true, " s"},
        {"wait_before_lift", "Wait before lift", true, " s"},
        {"wait_after_lift", "Wait after lift", true, " s"},
        {"wait_after_retract", "Wait after retract", true, " s"},
        {"bottom_lift_distance", "Bottom lift", true, " mm"},
        {"bottom_lift_speed", "Bottom lift speed", true, ""},
        {"lift_distance", "Lift", true, " mm"},
        {"lift_speed", "Lift speed", true, ""},
        {"bottom_retract_distance", "Bottom retract", true, " mm"},
        {"bottom_retract_speed", "Bottom retract speed", true, ""},
        {"retract_distance", "Retract", true, " mm"},
        {"retract_speed", "Retract speed", true, ""},
        {"bottom_lift2", "Bottom second lift", true, ""},
        {"lift2", "Second lift", true, ""},
        {"bottom_retract2", "Bottom second retract", true, ""},
        {"retract2", "Second retract", true, ""},
        {"bottom_pwm", "Bottom light power", true, " of 255"},
        {"pwm", "Light power", true, " of 255"},
        {"advance_mode", "Per-layer settings", true, "yes/no"},
    };
    std::vector<GooDiffRow> out;
    for (const fruit::GooFieldDiff& f : d) {
        const Name* n = nullptr;
        for (const Name& x : names)
            if (f.field == x.field) n = &x;
        GooDiffRow r{n ? n->label : f.field, f.a, f.b, n ? n->cures : true};
        const std::string unit = n ? n->unit : "";
        auto word = [&](const std::string& v) {
            const bool on = v != "0" && v != "false";
            if (unit == "yes/no") return std::string(on ? "yes" : "no");
            if (unit == "mode") return std::string(on ? "waits" : "light off");
            return v + unit;
        };
        r.a = word(f.a);
        r.b = word(f.b);
        out.push_back(r);
    }
    // What changes the print first.
    std::stable_sort(out.begin(), out.end(), [](const GooDiffRow& x, const GooDiffRow& y) { return x.cures > y.cures; });
    return out;
}

ImageVerdict imageVerdict(const fruit::GooCompareResult& c) {
    ImageVerdict v;
    double best = 0, runner = 0;
    std::string best_t;
    for (const auto& sc : c.scores) {
        if (sc.iou > best) {
            runner = best;
            best = sc.iou;
            best_t = sc.transform;
        } else if (sc.iou > runner) {
            runner = sc.iou;
        }
    }
    if (best < 0.5) {
        v.headline = "These show different models";
        v.detail = strf("The images overlap by only %.1f %% whichever way they're turned, so orientation can't be "
                        "judged from them. ",
                        100 * best) +
                   "Compare two slices of the same model, like the F test piece.";
        return v;
    }
    if (runner >= best - 0.05) {
        v.headline = "The images match whichever way they're turned";
        v.detail = "The shape looks the same flipped, so it can't show which way the printer's image runs. The F test "
                   "piece of the orientation check can.";
        return v;
    }
    v.clear = true;
    v.headline = best_t == "identity"  ? "The images match as they are"
                 : best_t == "flip_x"  ? "They match once A is flipped left to right"
                 : best_t == "flip_y"  ? "They match once A is flipped top to bottom"
                 : best_t == "flip_xy" ? "They match once A is turned 180°"
                                       : "They match once A is turned";
    v.detail = strf("%.1f %% of the lit area overlaps that way; the next best, %.1f %%.", 100 * best, 100 * runner);
    return v;
}

std::string transformText(const std::string& t) {
    return t == "identity"  ? "As is"
           : t == "flip_x"  ? "Flip left to right"
           : t == "flip_y"  ? "Flip top to bottom"
           : t == "flip_xy" ? "Turn 180°"
                            : t;
}

}  // namespace gui
