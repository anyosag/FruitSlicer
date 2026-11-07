// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Calibrate: the hub, the image orientation check and the Printer page
// (canvas boards "Calibrate", "OrientCheck" and "Printer").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "formats/GooExport.h"
#include "screens/JobParts.h"
#include "screens/Screens.h"

namespace gui::screens {

namespace {

using parts::tooltip;

constexpr float kMaxW = 1120;

void beginPage(const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32, 26));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
}

void endPage() {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

float pageWidth() { return std::min(ImGui::GetContentRegionAvail().x, kMaxW); }

float labelWidth(const char* s, ImFont* f, float sz) {
    ImGui::PushFont(f, sz);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    return w;
}

// A full-width card: title and words on the left, a status and a button on the right.
bool rowCard(const char* id, const char* title, const char* body, const std::string& status, ImU32 status_col,
             const char* button, bool enabled, const char* why_not = nullptr) {
    const float w = pageWidth();
    beginCard(id, ImVec2(w, 0), 16, col::Panel, col::Border);
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    const float bw = labelWidth(button, fonts().semibold, size::Body) + 28;
    const float sw = labelWidth(status.c_str(), fonts().medium, size::Body);
    const float right = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(x0 + right - bw - sw - 40);
    text(title, fonts().semibold, size::Section, col::Text);
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextSecondary);
    ImGui::TextWrapped("%s", body);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    const float bottom = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + right - bw - 14 - sw, y0 + 9));
    text(status.c_str(), fonts().medium, size::Body, status_col);
    ImGui::SetCursorPos(ImVec2(x0 + right - bw, y0 + 2));
    const bool clicked = secondaryButton(button, ImVec2(bw, 34), enabled);
    if (!enabled && why_not) tooltip(why_not);
    ImGui::SetCursorPosY(std::max(bottom, y0 + 40));
    ImGui::Dummy(ImVec2(0, 0));
    endCard();
    return clicked;
}

void stepMark(ImVec2 p, int n, bool done, bool current) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + 13, p.y + 13);
    if (done) {
        dl->AddCircleFilled(c, 13, col::OkBg, 28);
        icon::check(dl, ImVec2(c.x - 7, c.y - 7), 14, col::Ok);
    } else if (current) {
        dl->AddCircleFilled(c, 13, col::Accent, 28);
        ImGui::PushFont(fonts().bold, size::Body);
        const std::string s = std::to_string(n);
        const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col::OnAccent, s.c_str());
        ImGui::PopFont();
    } else {
        dl->AddCircle(c, 12.5f, col::BorderStrong, 28, 1.5f);
        ImGui::PushFont(fonts().semibold, size::Body);
        const std::string s = std::to_string(n);
        const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col::TextMuted, s.c_str());
        ImGui::PopFont();
    }
}

// One step of the check: its mark, title, words, and what to do.
void step(int n, bool done, bool current, const char* title, const char* body, const std::function<void()>& action) {
    ImGui::PushID(n);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    stepMark(p, n, done, current);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 40, p.y + 3));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    text(title, fonts().semibold, size::Section, current || done ? col::Text : col::TextSecondary);
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextSecondary);
    ImGui::TextWrapped("%s", body);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    if (action) action();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::PopID();
}

void picture(App& app, bool ours, const char* caption, float side) {
    const auto& r = app.orientationResult();
    const LayerPicture& p = ours ? r->ours : r->theirs;
    beginCard(ours ? "ours" : "theirs", ImVec2(side + 28, 0), 14, col::Card, col::Border);
    const unsigned tex = app.orientationPicture(ours);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + side, at.y + side), col::Viewport, 6);
    if (tex && p.w > 0 && p.h > 0) {
        const float k = side / static_cast<float>(std::max(p.w, p.h));
        const ImVec2 sz(std::round(p.w * k), std::round(p.h * k));
        ImGui::SetCursorScreenPos(ImVec2(at.x + std::round((side - sz.x) / 2), at.y + std::round((side - sz.y) / 2)));
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), sz);
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + side + 8));
    text(caption, fonts().medium, size::Body, col::TextSecondary);
    endCard();
}

}  // namespace

// ---- Calibrate ----

void calibrate(App& app) {
    beginPage("calibrate");
    const PrinterSetup& ps = app.printerSetup();
    text("Calibrate", fonts().bold, size::PageTitle, col::Text);
    {
        // The resin the plates calibrate: the one new jobs use.
        const Resin& r = app.newJobResin();
        text(("For " + resinLabel(r.name, r.profile) + " ·").c_str(), fonts().regular, size::Body, col::TextMuted);
        ImGui::SameLine(0, 6);
        if (linkButton("Resins")) {
            app.resinSelected() = r.id;
            app.setScreen(Screen::Resins);
        }
    }
    ImGui::Dummy(ImVec2(0, 8));
    const float w = pageWidth();
    {
        beginCard("next", ImVec2(w, 0), 18, IM_COL32(0x2A, 0x1E, 0x14, 255), col::Accent);
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
        const char* what = !ps.orientation_confirmed ? "Check the image orientation. It takes 5 minutes and uses no resin."
                           : !ps.hinge              ? "Set the side the vat hinges on, so tips hold harder where peel pulls most."
                                                    : "Print the resin test plates. They arrive in a later update.";
        text("Next", fonts().semibold, size::Help, col::Accent);
        text(what, fonts().semibold, size::Section + 1, col::Text, true);
        const bool action = !ps.orientation_confirmed || !ps.hinge;
        if (action) {
            const char* label = !ps.orientation_confirmed ? "Start" : "Open";
            const float bw = labelWidth(label, fonts().bold, size::Body) + 34;
            ImGui::SetCursorPos(ImVec2(x0 + ImGui::GetContentRegionAvail().x - bw, y0 + 4));
            if (primaryButton(label, ImVec2(bw, 38)))
                app.setScreen(!ps.orientation_confirmed ? Screen::OrientationCheck : Screen::Printer);
            ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), y0 + 44));
            ImGui::Dummy(ImVec2(0, 0));
        }
        endCard();
    }
    ImGui::Dummy(ImVec2(0, 10));
    groupLabel("Printer checks · once per printer");
    std::string orient = "Not checked";
    ImU32 orient_col = col::Warn;
    if (ps.orientation_confirmed) {
        orient = ps.flip_x && ps.flip_y ? "Confirmed · flips both ways"
                 : ps.flip_x            ? "Confirmed · flips left to right"
                 : ps.flip_y            ? "Confirmed · flips top to bottom"
                                        : "Confirmed · as is";
        orient_col = col::Ok;
    }
    if (rowCard("orientation", "Image orientation",
                "Compares FruitSlicer's and SatelLite's slice of a small F, so text never prints mirrored.", orient,
                orient_col, "Open", true))
        app.setScreen(Screen::OrientationCheck);
    const auto side = hingeChoice(ps.hinge);
    if (rowCard("printer", "Printer", "The side the vat hinges on, and what the print-time estimates use.",
                side ? std::string("Hinge: ") + hingeLabel(*side) : std::string("Hinge side not set"),
                side ? col::TextSecondary : col::TextMuted, "Open", true))
        app.setScreen(Screen::Printer);
    rowCard("overlap", "Overlap test",
            "Only if you export STL to another slicer: checks that it fills spots where parts overlap.",
            "Optional · arrives later", col::TextMuted, "Open", false, "The overlap test arrives in a later update.");
    ImGui::Dummy(ImVec2(0, 10));
    groupLabel("Resin test plates · once per resin");
    text("They replace the borrowed starting values with numbers from your resin. Print them with your usual "
         "settings.",
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 2));
    struct Plate {
        const char* name;
        const char* size;
        const char* measures;
    };
    const Plate plates[] = {
        {"Tips", "48 coupons · 6 mm tall", "How much each tip size holds and which leaves the smallest mark."},
        {"Spacing", "37 coupons · 19 mm tall",
         "How far apart tips can be on tilted faces, and how steep a face can print without tips."},
        {"Islands", "36 coupons · 7 mm tall",
         "How big a spot that starts in mid-air can be before it needs a second or third tip."}};
    const float pw = std::floor((w - 2 * 12) / 3);
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        beginCard("plate", ImVec2(pw, 170), 16, col::Panel, col::Border);
        text(plates[i].name, fonts().semibold, size::Section + 1, col::Text);
        text(plates[i].size, fonts().regular, size::Help, col::TextMuted);
        ImGui::Dummy(ImVec2(0, 2));
        text(plates[i].measures, fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::SetCursorPosY(170 - 16 - 34);
        secondaryButton("Prepare plate", ImVec2(0, 34), false);
        tooltip("The resin test plates arrive in a later update.");
        endCard();
        ImGui::PopID();
        if (i < 2) ImGui::SameLine(0, 12);
    }
    endPage();
}

// ---- Image orientation check ----

void orientationCheck(App& app) {
    beginPage("orientation_check");
    if (linkButton("‹ Calibrate")) app.setScreen(Screen::Calibrate);
    ImGui::Dummy(ImVec2(0, 2));
    text("Image orientation check", fonts().bold, size::PageTitle, col::Text);
    text("5 minutes · no resin", fonts().regular, size::Body, col::TextMuted);
    ImGui::Dummy(ImVec2(0, 10));
    const auto& r = app.orientationResult();
    const JobProgress jp = app.jobs().progress();
    const bool comparing = jp.running && jp.name == "Comparing the F";
    const bool saved = !app.orientationPiece().empty();
    const float w = pageWidth(), lw = 400, gap = 24;

    // Steps.
    ImGui::BeginGroup();
    ImGui::PushItemWidth(lw);
    beginCard("steps", ImVec2(lw, 0), 20, col::Panel, col::Border);
    step(1, saved, !saved, "Save the F test piece",
         "f.stl: an F 10 × 16 mm and 2 mm tall, standing on the plate in its +X, +Y quarter.", [&] {
             ImGui::Dummy(ImVec2(0, 2));
             if (secondaryButton("Save f.stl…", ImVec2(0, 34))) app.saveOrientationPiece();
             if (saved) {
                 ImGui::SameLine(0, 10);
                 ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 9);
                 text(("Saved as " + std::filesystem::path(app.orientationPiece()).filename().string()).c_str(),
                      fonts().regular, size::Help, col::Ok);
             }
         });
    step(2, r != nullptr, saved && !r && !comparing, "Slice it in SatelLite",
         "Open f.stl in SatelLite as it is, with no supports and no scaling, slice it and save the .goo.", [&] {
             ImGui::Dummy(ImVec2(0, 2));
             if (secondaryButton("Choose SatelLite's .goo…", ImVec2(0, 34), !comparing)) app.chooseOrientationSample();
             if (r) {
                 text((std::filesystem::path(r->satellite_path).filename().string() + " · " + r->software).c_str(),
                      fonts().regular, size::Help, col::TextMuted, true);
             }
         });
    step(3, r != nullptr, comparing, "Compare",
         "FruitSlicer slices the same F with that file's settings and lines up the two images.", [&] {
             if (!comparing) return;
             ImGui::Dummy(ImVec2(0, 2));
             const ImVec2 bp = ImGui::GetCursorScreenPos();
             const float bw = ImGui::GetContentRegionAvail().x;
             ImDrawList* dl = ImGui::GetWindowDrawList();
             dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 6), col::Input, 3);
             dl->AddRectFilled(bp, ImVec2(bp.x + bw * static_cast<float>(jp.fraction), bp.y + 6), col::Accent, 3);
             ImGui::Dummy(ImVec2(bw, 10));
         });
    endCard();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);

    // Result.
    ImGui::BeginGroup();
    const float rw = w - lw - gap;
    beginCard("result", ImVec2(rw, 0), 20, col::Panel, col::Border);
    if (!r) {
        text("The comparison", fonts().semibold, size::Section + 1, col::Text);
        text(comparing ? "Cutting the F and lining up the two images…"
                       : "It appears here once you choose SatelLite's .goo of the F.",
             fonts().regular, size::Body, col::TextSecondary, true);
    } else {
        const float side = std::min(210.0f, (rw - 40 - 28 * 2 - 12) / 2);
        std::string ours = "FruitSlicer";
        if (r->flip_x && r->flip_y) ours += ", turned 180°";
        else if (r->flip_x) ours += ", flipped left to right";
        else if (r->flip_y) ours += ", flipped top to bottom";
        picture(app, true, ours.c_str(), side);
        ImGui::SameLine(0, 12);
        picture(app, false, "SatelLite", side);
        ImGui::Dummy(ImVec2(0, 6));
        const bool found = r->verdict != fruit::OrientationVerdict::Undetermined;
        text(verdictTitle(*r).c_str(), fonts().bold, size::Section + 3, found ? col::Text : col::Warn, true);
        text(verdictBody(*r).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 6));
        text("How well the two images overlap", fonts().semibold, size::Body, col::Text);
        for (const auto& s : r->compare.scores) {
            const bool best = s.transform == r->compare.best;
            keyValue((transformLabel(s.transform) + (best ? "  ·  best" : "")).c_str(), strf("%.0f %%", s.iou * 100),
                     best ? col::Ok : col::TextSecondary);
        }
        ImGui::Dummy(ImVec2(0, 8));
        const PrinterSetup& ps = app.printerSetup();
        const bool in_use = ps.orientation_confirmed && ps.flip_x == r->flip_x && ps.flip_y == r->flip_y &&
                            ps.checked_with == std::filesystem::path(r->satellite_path).filename().string();
        if (primaryButton(in_use ? "In use for your printer" : "Use this for my printer", ImVec2(0, 38),
                          found && !in_use))
            app.useOrientationFix();
        if (!found) tooltip("Nothing to use: the two F's don't line up.");
        ImGui::SameLine(0, 14);
        static bool full = false;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
        if (linkButton(full ? "Hide the full comparison" : "See the full comparison")) full = !full;
        if (full) {
            ImGui::Dummy(ImVec2(0, 6));
            text(strf("Layer %u of FruitSlicer's file (%.2f mm) against layer %u of SatelLite's (%.2f mm).",
                      r->compare.layer_a + 1, r->compare.z_a, r->compare.layer_b + 1, r->compare.z_b)
                     .c_str(),
                 fonts().regular, size::Help, col::TextMuted, true);
            if (r->compare.header.empty()) {
                text("Every other header field is the same.", fonts().regular, size::Help, col::TextMuted);
            } else {
                text("Header fields that differ (FruitSlicer · SatelLite):", fonts().regular, size::Help,
                     col::TextMuted);
                for (const auto& d : r->compare.header) keyValue(d.field.c_str(), d.a + " · " + d.b, col::TextSecondary);
            }
        }
    }
    endCard();
    ImGui::EndGroup();
    endPage();
}

// ---- Printer ----

namespace {

bool radioDot(const char* id, const char* label, bool selected) {
    ImGui::PushID(id);
    ImGui::PushFont(fonts().medium, size::Body);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(ts.x + 26, 22);
    const bool clicked = ImGui::InvisibleButton("r", sz);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + 8, p.y + sz.y / 2);
    dl->AddCircle(c, 7.5f, selected ? col::Accent : (hovered ? col::Text : col::TextMuted), 20, 1.5f);
    if (selected) dl->AddCircleFilled(c, 4, col::Accent, 20);
    ImGui::PushFont(fonts().medium, size::Body);
    dl->AddText(ImVec2(p.x + 22, p.y + (sz.y - ts.y) / 2), selected ? col::Text : col::TextSecondary, label);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopID();
    return clicked;
}

float radioWidth(const char* label) { return labelWidth(label, fonts().medium, size::Body) + 26; }

void hingePicker(App& app, float w) {
    const PrinterSetup& ps = app.printerSetup();
    const auto chosen = hingeChoice(ps.hinge);
    const float vw = 220, vh = std::round(vw * 118.37f / 211.68f);
    const ImVec2 o = ImGui::GetCursorPos();
    const float cx = o.x + w / 2;
    // Back above, Front below, Left and Right at the sides of the vat.
    const float top = o.y, vat_y = top + 30, bottom = vat_y + vh + 8;
    auto put = [&](HingeChoice c, float x, float y) {
        ImGui::SetCursorPos(ImVec2(x, y));
        if (radioDot(hingeLabel(c), hingeLabel(c), chosen == c)) app.setHinge(c);
    };
    put(HingeChoice::Back, cx - radioWidth("Back") / 2, top);
    put(HingeChoice::Left, cx - vw / 2 - 16 - radioWidth("Left"), vat_y + vh / 2 - 11);
    put(HingeChoice::Right, cx + vw / 2 + 16, vat_y + vh / 2 - 11);
    put(HingeChoice::Front, cx - radioWidth("Front") / 2, bottom);
    // The vat, seen from above, with the hinge drawn on its side.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 scroll(ImGui::GetScrollX(), ImGui::GetScrollY());
    const ImVec2 a(wp.x + cx - vw / 2 - scroll.x, wp.y + vat_y - scroll.y), b(a.x + vw, a.y + vh);
    dl->AddRectFilled(a, b, col::Viewport, 8);
    dl->AddRect(a, b, col::BorderStrong, 8, 1.5f);
    ImGui::PushFont(fonts().regular, size::Help);
    const char* cap = "vat, seen from above";
    const ImVec2 ts = ImGui::CalcTextSize(cap);
    dl->AddText(ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2), col::TextMuted, cap);
    ImGui::PopFont();
    if (chosen) {
        const float t = 5;
        ImVec2 p0, p1;
        switch (*chosen) {
            case HingeChoice::Back: p0 = ImVec2(a.x + 10, a.y - t / 2); p1 = ImVec2(b.x - 10, a.y + t / 2); break;
            case HingeChoice::Front: p0 = ImVec2(a.x + 10, b.y - t / 2); p1 = ImVec2(b.x - 10, b.y + t / 2); break;
            case HingeChoice::Left: p0 = ImVec2(a.x - t / 2, a.y + 10); p1 = ImVec2(a.x + t / 2, b.y - 10); break;
            case HingeChoice::Right: p0 = ImVec2(b.x - t / 2, a.y + 10); p1 = ImVec2(b.x + t / 2, b.y - 10); break;
        }
        dl->AddRectFilled(p0, p1, col::Accent, 2);
    }
    ImGui::SetCursorPos(ImVec2(o.x, bottom + 30));
    ImGui::Dummy(ImVec2(w, 0));
    if (chosen && linkButton("Clear")) app.setHinge(std::nullopt);
}

}  // namespace

void printer(App& app) {
    beginPage("printer");
    if (linkButton("‹ Calibrate")) app.setScreen(Screen::Calibrate);
    ImGui::Dummy(ImVec2(0, 2));
    text("ELEGOO Saturn 4 Ultra 16K", fonts().bold, size::PageTitle, col::Text);
    text("The one printer FruitSlicer supports today", fonts().regular, size::Body, col::TextMuted);
    ImGui::Dummy(ImVec2(0, 10));
    const fruit::PrinterProfile& pr = app.printer();
    const PrinterSetup& ps = app.printerSetup();
    const float w = pageWidth(), gap = 16, cw = std::floor((w - gap) / 2);

    ImGui::BeginGroup();
    beginCard("the_printer", ImVec2(cw, 0), 18, col::Panel, col::Border);
    text("The printer", fonts().semibold, size::Section + 1, col::Text);
    keyValue("Screen", strf("%d × %d pixels", pr.res_x, pr.res_y));
    keyValue("Pixel size", strf("%.0f × %.0f µm (not square)", pr.pixelX() * 1000, pr.pixelY() * 1000));
    keyValue("Build area", strf("%.2f × %.2f mm", pr.build_x, pr.build_y));
    keyValue("Height", strf("%.0f mm", pr.build_z));
    keyValue("File format", ".goo, version 3");
    keyValue("Release", "tilting vat");
    endCard();
    ImGui::Dummy(ImVec2(0, gap - 8));
    beginCard("estimates", ImVec2(cw, 0), 18, col::Panel, col::Border);
    text("For estimates", fonts().semibold, size::Section + 1, col::Text);
    keyValue("Movement per layer", strf("%.1f s", fruit::GooExportOptions{}.motion_seconds_per_layer));
    text("Added to each layer's exposure and waits for the print-time estimate: the plate's lift and the vat's "
         "tilt.",
         fonts().regular, size::Help, col::TextMuted, true);
    endCard();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);

    ImGui::BeginGroup();
    beginCard("orientation", ImVec2(cw, 0), 18, col::Panel, col::Border);
    text("Image orientation", fonts().semibold, size::Section + 1, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    if (ps.orientation_confirmed) {
        verdictRow("state", Verdict::Ok, "Confirmed",
                   ("Checked against SatelLite's slice of the F" +
                    (ps.checked_with.empty() ? std::string(".") : " (" + ps.checked_with + ").") +
                    " Every print file uses it.")
                       .c_str());
    } else {
        verdictRow("state", Verdict::Warn, "Not confirmed",
                   "FruitSlicer mirrors rows top to bottom, because SatelLite's file marks it that way. The F check "
                   "confirms it.");
    }
    const bool mx = pr.image_mirror_x != (ps.orientation_confirmed && ps.flip_x);
    const bool my = pr.image_mirror_y != (ps.orientation_confirmed && ps.flip_y);
    keyValue("Mirror left to right", mx ? "Yes" : "No");
    keyValue("Mirror top to bottom", my ? "Yes" : "No");
    ImGui::Dummy(ImVec2(0, 2));
    if (secondaryButton(ps.orientation_confirmed ? "Run the check again" : "Run the check", ImVec2(0, 34)))
        app.setScreen(Screen::OrientationCheck);
    endCard();
    ImGui::Dummy(ImVec2(0, gap - 8));
    beginCard("hinge", ImVec2(cw, 0), 18, col::Panel, col::Border);
    text("Which side does the vat hinge on?", fonts().semibold, size::Section + 1, col::Text);
    text("The tilting vat peels hardest on the side away from its hinge, so FruitSlicer can add extra hold there. "
         "Look at the printer from the front and pick the hinge side.",
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 4));
    hingePicker(app, ImGui::GetContentRegionAvail().x);
    ImGui::Dummy(ImVec2(0, 2));
    const bool on = ps.orientation_confirmed && ps.hinge;
    text(on ? "On: up to +25 % hold on the far side. The right amount hasn't been measured."
            : "Up to +25 % hold on the far side. The right amount hasn't been measured, and it stays off until the "
              "image orientation is confirmed.",
         fonts().regular, size::Help, on ? col::TextSecondary : col::TextMuted, true);
    endCard();
    ImGui::EndGroup();
    endPage();
}

}  // namespace gui::screens
