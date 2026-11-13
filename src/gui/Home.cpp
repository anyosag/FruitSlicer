// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Home.h"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "Document.h"
#include "Format.h"
#include "Plates.h"
#include "formats/GooSetup.h"

namespace gui {

namespace {

std::string capital(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

double round3(double v) { return std::round(v * 1000) / 1000; }

}  // namespace

SetupChecks setupChecks(const fruit::PrinterProfile& printer, const PrinterSetup& setup, const Resin& resin,
                        const fruit::GooHeader* sample) {
    using S = SetupCheck::State;
    using G = SetupCheck::Go;
    SetupChecks out;
    // The printer: the one FruitSlicer is made for.
    out.rows.push_back({S::Ok, "Printer: " + printer.name,
                        strf("%g × %g × %g mm build volume", printer.build_x, printer.build_y, printer.build_z), "",
                        G::None});

    // Print settings: a SatelLite file of the user's (with their own values
    // over it), values typed in, or none yet. A file that has gone counts as none.
    const bool from_file = !resin.sample_path.empty();
    const bool typed = resin.print.exposure > 0 && resin.print.bottom_exposure > 0;
    if ((from_file && sample) || (!from_file && typed)) {
        PrintSettings s;
        static_cast<PrintEdits&>(s) = resin.print;
        if (from_file) s.sample = std::shared_ptr<const fruit::GooHeader>(sample, [](const fruit::GooHeader*) {});
        const fruit::GooHeader h = fruit::printHeader(printer, printRequest(s, printer.layer_height));
        const std::string detail =
            strf("%g mm layers · %u bottom layers at %g s · %g s normal", round3(h.layer_height),
                 static_cast<unsigned>(h.print.bottom_layers), round3(h.print.bottom_exposure), round3(h.print.exposure));
        const char* title = !from_file                     ? "Print settings typed in"
                            : resin.print == PrintEdits{} ? "Print settings from your SatelLite file"
                                                          : "Print settings from your SatelLite file, with your changes";
        out.rows.push_back({S::Ok, title, detail, "View", G::Resins});
    } else if (from_file) {
        const std::string name = resin.sample_name.empty() ? std::string("The SatelLite file") : resin.sample_name;
        out.rows.push_back({S::Review, "Print settings file doesn't read",
                            name + " has moved or no longer reads, so each job asks for print settings until it is "
                                   "picked again.",
                            "Open Resins", G::Resins});
    } else {
        out.rows.push_back({S::Review, "No print settings yet",
                            "Each job asks for them before it saves a print file: a SatelLite file of yours, or "
                            "values typed in.",
                            "Set them", G::Resins});
    }

    // Image orientation.
    if (setup.orientation_confirmed) {
        const std::string with = setup.checked_with.empty() ? std::string("Checked") : "Checked with " + setup.checked_with;
        out.rows.push_back({S::Ok, "Image orientation checked",
                            with + (setup.flip_x || setup.flip_y ? ": print files are flipped to match your printer."
                                                                 : ": layer images come out the right way round."),
                            "", G::None});
    } else {
        out.rows.push_back({S::Review, "Image orientation not checked",
                            "Until it is, text and left- or right-handed parts could print mirrored. 5 minutes, no "
                            "resin.",
                            "Check now", G::OrientationCheck});
    }

    // Support values: measured on the resin by its test plates, or starting ones.
    std::vector<std::string> measured, not_yet;
    for (const char* kind : kPlateKinds) {
        const bool m = plateMeasured(resin.profile, kind);
        out.plates.push_back({capital(kind), m});
        (m ? measured : not_yet).push_back(kind);
    }
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (std::size_t i = 0; i < v.size(); ++i) s += (i == 0 ? "" : i + 1 == v.size() ? " and " : ", ") + v[i];
        return s;
    };
    if (not_yet.empty()) {
        out.rows.push_back({S::Ok, "Support values measured on your resin",
                            "Tips, spacing and islands, from its test plates.", "", G::None});
    } else if (measured.empty()) {
        out.rows.push_back({S::Note, "Supports use starting values",
                            "Borrowed from PrusaSlicer: nothing is measured on your resin yet. Three test plates "
                            "measure it.",
                            "Open calibration", G::Calibrate});
    } else {
        out.rows.push_back({S::Note, "Some support values measured",
                            capital(list(measured)) + " measured on your resin; " + list(not_yet) +
                                " still use starting values, borrowed from PrusaSlicer.",
                            "Open calibration", G::Calibrate});
    }
    for (const SetupCheck& c : out.rows)
        if (c.state == S::Ok) ++out.checked;
    return out;
}

ContinueTo continueTo(const Document& d) {
    if (d.partCount() == 0) return {"Continue: open a model", Step::Model, false};
    if (!d.fits()) return {"Continue: fit it on the plate", Step::Model, false};
    switch (mainButton(&d).action) {
        case MainAction::OpenModel: break;
        case MainAction::MakeSupports: return {"Continue: make supports", Step::Supports, true};
        case MainAction::RemakeSupports: return {"Continue: remake supports", Step::Supports, true};
        case MainAction::Slice: return {"Continue: cut the layers", Step::Layers, false};
        case MainAction::ShowLayers:
            if (markFor(d, Step::Print) == Mark::Done) return {"Back to the job", Step::Print, false};
            return {"Continue: save the print file", Step::Print, false};
    }
    return {"Back to the job", Step::Model, false};
}

std::vector<StepChip> stepChips(const Document& d) {
    const Step next = continueTo(d).step;
    const struct {
        Step step;
        const char* label;
    } places[] = {{Step::Model, "Model"},
                  {Step::Orient, "Orient"},
                  {Step::Supports, "Supports"},
                  {Step::Layers, "Layers"},
                  {Step::Print, "Print"}};
    std::vector<StepChip> out;
    for (const auto& p : places) {
        StepChip c;
        c.label = p.label;
        const bool done = p.step == Step::Model ? d.fits() : markFor(d, p.step) == Mark::Done;
        if (done) {
            c.state = StepChip::State::Done;
            if (p.step == Step::Supports && d.supports.result) {
                const std::size_t n = d.supports.result->tips;
                c.detail = n == 0 ? " · no tips" : n == 1 ? " · 1 tip" : strf(" · %zu tips", n);
            }
        } else if (p.step == next) {
            c.state = StepChip::State::Next;
            c.detail = " · next";
        }
        out.push_back(c);
    }
    return out;
}

std::string openJobLine(const Document& d) {
    std::string s = d.job_path.empty() ? "Not saved yet" : d.unsaved() ? "Changed since it was saved" : "Saved";
    const std::size_t n = d.partCount();
    s += n == 1 ? std::string(" · 1 part") : strf(" · %zu parts", n);
    if (n == 1) s += " · " + dims(d.placedBox().size());
    s += " · " + resinLabel(d.supports.choices.resin, d.supports.choices.profile);
    return s;
}

std::string openJobSupports(const Document& d) {
    const auto& r = d.supports.result;
    if (!r) return "";
    if (!d.supports.fresh(d.revision(), d.supportSettings()))
        return "Supports are out of date: they were made for the job as it was before.";
    if (r->tips == 0) return "Supports are made: no tips needed.";
    return strf("Supports are made: %zu %s, %.1f ml.", r->tips, r->tips == 1 ? "tip" : "tips", r->support_ml);
}

std::string jobState(const RecentJob& j) { return capital(j.state); }

std::string jobFacts(const RecentJob& j) {
    if (j.parts <= 0) return "";
    std::string s = j.parts == 1 ? std::string("1 part") : strf("%d parts", j.parts);
    if (j.on_plate) s += " on the plate";
    if (j.tips == 0) s += " · no tips needed";
    else if (j.tips > 0) {
        s += j.tips == 1 ? std::string(" · 1 tip") : strf(" · %ld tips", j.tips);
        if (j.support_ml >= 0) s += strf(" · %.1f ml supports", j.support_ml);
    }
    return s;
}

std::string whenWords(const std::string& when, std::time_t now) {
    int y = 0, mo = 0, da = 0, h = 0, mi = 0;
    if (std::sscanf(when.c_str(), "%d-%d-%d %d:%d", &y, &mo, &da, &h, &mi) != 5 || mo < 1 || mo > 12 || da < 1 ||
        da > 31)
        return capital(when);
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::tm today{};
    localtime_r(&now, &today);
    // Whole days between the two dates, each taken at noon (no daylight saving trouble).
    std::tm a{};
    a.tm_year = y - 1900, a.tm_mon = mo - 1, a.tm_mday = da, a.tm_hour = 12, a.tm_isdst = -1;
    std::tm b{};
    b.tm_year = today.tm_year, b.tm_mon = today.tm_mon, b.tm_mday = today.tm_mday, b.tm_hour = 12, b.tm_isdst = -1;
    const long days = std::lround(std::difftime(std::mktime(&b), std::mktime(&a)) / 86400.0);
    const std::string time = strf("%02d:%02d", h, mi);
    if (days == 0) return "Today, " + time;
    if (days == 1) return "Yesterday, " + time;
    if (y == today.tm_year + 1900) return strf("%d %s, ", da, kMonths[mo - 1]) + time;
    return strf("%d %s %d, ", da, kMonths[mo - 1], y) + time;
}

}  // namespace gui
