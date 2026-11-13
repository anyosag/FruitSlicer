// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Supports: choices before generating, the run with its stages, the
// result with its checks, the problems drawer, and the out-of-date state
// (canvas boards "SupportsEmpty", "SupportsRunning", "Supports",
// "SupportsStale" and "Problems").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

bool running(App& app) { return app.jobs().busy() && app.jobs().progress().name == "Generating supports"; }

// The word for a slider's stop, on the right of its label: accent when it is not the resin's.
void rightText(const char* s, ImFont* f, float size, ImU32 c, float y) {
    ImGui::PushFont(f, size);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - w, y));
    text(s, f, size, c);
}

// A slider with a stop at each whole number from `lo` to `hi`: a thin track, a
// round grip and a dot for each stop. True when it moved. `activated` and
// `released` tell when a drag starts and ends. Left and Right move it a stop
// while it has the focus.
bool stopSlider(const char* id, int* v, int lo, int hi, bool* activated = nullptr, bool* released = nullptr) {
    const float w = ImGui::GetContentRegionAvail().x, h = 22, r = 9;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (activated) *activated = ImGui::IsItemActivated();
    if (released) *released = ImGui::IsItemDeactivated();
    const int n = hi - lo + 1;
    const float x0 = p.x + r, x1 = p.x + w - r;
    int next = *v;
    if (active) {
        const float t = (ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0);
        next = std::clamp(lo + static_cast<int>(std::lround(t * (n - 1))), lo, hi);
    }
    if (ImGui::IsItemFocused() || hovered) {
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) next = std::max(lo, *v - 1);
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) next = std::min(hi, *v + 1);
    }
    const bool moved = next != *v;
    *v = next;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + h / 2;
    dl->AddRectFilled(ImVec2(p.x, cy - 2), ImVec2(p.x + w, cy + 2), IM_COL32(0x2E, 0x34, 0x3E, 255), 2);
    const float gx = x0 + (x1 - x0) * (n > 1 ? static_cast<float>(*v - lo) / (n - 1) : 0.0f);
    dl->AddCircleFilled(ImVec2(gx, cy), r, hovered || active ? col::AccentHover : col::Accent, 24);
    dl->AddCircle(ImVec2(gx, cy), r - 1.5f, col::Panel, 24, 3);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    // The dots, under the track.
    for (int i = 0; i < n; ++i) {
        const float x = x0 + (x1 - x0) * (n > 1 ? static_cast<float>(i) / (n - 1) : 0.0f);
        dl->AddCircleFilled(ImVec2(x, p.y + h + 3), 2, IM_COL32(0x4A, 0x51, 0x60, 255));
    }
    ImGui::Dummy(ImVec2(0, 4));
    return moved;
}

// One quick support slider: label and word, the slider, the end words and a
// caption of what the stop sets.
void quickSlider(App& app, const SupportChoices& choices, QuickSlider which) {
    const QuickInfo q = quickInfo(which, choices);
    ImGui::PushID(q.label);
    const float y = ImGui::GetCursorPosY();
    text(q.label, fonts().semibold, size::Body, col::Text);
    rightText(q.word.c_str(), fonts().semibold, size::Help, q.stop == q.normal ? col::TextSecondary : col::Accent, y);
    int v = q.stop;
    if (stopSlider("##stop", &v, 1, kQuickStops)) {
        SupportChoices c = choices;
        quickStop(c, which) = v;
        app.setSupportChoices(c);
    }
    const float ey = ImGui::GetCursorPosY();
    text(q.left, fonts().regular, size::Label, col::TextMuted);
    rightText(q.right, fonts().regular, size::Label, col::TextMuted, ey);
    text(q.caption.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    ImGui::PopID();
}

// With no raft: the height added under the model. It is a print setting of
// the resin (next to its bottom layers), so here it is only shown.
void heightCorrection(App& app, const Document& doc) {
    const PrintEdits& e = doc.print.settings;
    const double mm = heightCorrection(e);
    const int layers = baseLayers(doc, kLayerHeight);
    const float y = ImGui::GetCursorPosY();
    text("Height correction", fonts().semibold, size::Body, col::Text);
    rightText(strf("%g mm", mm).c_str(), fonts().semibold, size::Help, col::TextSecondary, y);
    std::string what =
        layers > 0 ? strf("Adds %g mm (%d %s) under the model, because the printer squashes the first layers.",
                          layers * kLayerHeight, layers, layers == 1 ? "layer" : "layers")
                   : std::string("Adds nothing under the model, so it may print a little short: the printer squashes "
                                 "the first layers.");
    what += e.height_correction >= 0 ? " Your value, from the resin's print settings."
                                     : " 0.10 mm is a typical value, not measured on your printer.";
    text(what.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    text("It's one of the resin's print settings, next to Bottom layers.", fonts().regular, size::Help,
         col::TextSecondary, true);
    if (linkButton("Change it on the Resins page")) {
        std::string id = app.newJobResin().id;
        for (const Resin& r : app.resins())
            if (r.name == doc.supports.choices.resin) id = r.id;
        app.resinSelected() = id;
        app.setScreen(Screen::Resins);
    }
}

// The raft and its lift: a job-wide placement choice (every part).
void onThePlate(App& app, Document& doc) {
    const fruit::Placement& p = doc.placement();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2));
    groupLabel("On the plate");
    ImGui::Dummy(ImVec2(0, 2));
    bool raft = !p.on_plate;
    if (ImGui::Checkbox("##raft", &raft)) app.setOnPlate(!raft);
    ImGui::SameLine(0, 10);
    ImGui::BeginGroup();
    text("Raft", fonts().semibold, size::Body, col::Text);
    // The raft's real thickness: the built one when the supports are up to date, else what the settings will give.
    const SupportState& made = doc.supports;
    const double thick =
        made.result && made.result->choices == made.choices && made.result->tree.raft_top > 0
            ? made.result->tree.raft_top
            : std::max(treeConfig(made.choices).raft_thickness,
                       fruit::minRaftThickness(doc.printer.bottom_layers, doc.printer.transition_layers, kLayerHeight));
    // With no raft the model's first layers print as bottom layers (those left after the height correction's own).
    const double held = std::round(modelInBottomLayers(doc) * 100) / 100;
    const std::string raftText =
        raft ? strf("A %g mm raft under the supports, which comes off in one piece. Best for most models.",
                    std::round(thick * 100) / 100)
        : held > 0
            ? strf("Off: the model sits flat on the plate. For flat-backed parts; only overhangs get tips, and none "
                   "in its first %g mm: that prints in the bottom layers, whose long exposures should cure it onto "
                   "the plate. Not measured.",
                   held)
            : std::string("Off: the model sits flat on the plate. For flat-backed parts; only overhangs get tips.");
    text(raftText.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    ImGui::EndGroup();
    if (raft) {
        // The kind of raft: a pad under each support, or one solid base.
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Indent(28);
        ImGui::PushID("raft_type");
        const bool full = made.choices.full_raft;
        bool pick = full;
        if (segmentButton("Pads", !full)) pick = false;
        ImGui::SameLine(0, 4);
        if (segmentButton("Full", full)) pick = true;
        ImGui::PopID();
        if (pick != full) {
            SupportChoices c = made.choices;
            c.full_raft = pick;
            app.setSupportChoices(c);
        }
        text(full ? "One solid base with a sloped edge for the scraper. Holds small and thin parts best."
                  : "Small pads joined by bars. Uses the least resin.",
             fonts().regular, size::Help, col::TextSecondary, true);
        ImGui::Unindent(28);
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (!raft) {
        heightCorrection(app, doc);
        return;
    }
    ImGui::BeginDisabled(!raft);
    const int now = static_cast<int>(std::lround(p.elevation));
    const bool differs = raft && std::abs(p.elevation - fruit::Placement{}.elevation) > 1e-9;
    const float y = ImGui::GetCursorPosY();
    text("Lift height", fonts().semibold, size::Body, col::Text);
    rightText(raft ? strf("%d mm", now).c_str() : "None", fonts().semibold, size::Help,
              differs ? col::Accent : col::TextSecondary, y);
    int v = now;
    bool begin = false, end = false;
    if (stopSlider("##lift", &v, 1, 10, &begin, &end) || begin) {
        if (begin) app.beginLift();
        app.previewLift(v);
    }
    if (end) app.endLift();
    const float ey = ImGui::GetCursorPosY();
    text("1 mm", fonts().regular, size::Label, col::TextMuted);
    rightText("10 mm", fonts().regular, size::Label, col::TextMuted, ey);
    text(raft ? strf("%d mm between the raft and the model's lowest point. Higher is easier to snap off; lower uses "
                     "less resin.",
                     now)
                    .c_str()
              : "No lift: the model stands on the plate.",
         fonts().regular, size::Help, col::TextSecondary, true);
    ImGui::EndDisabled();
}

// Why supports that were made no longer count, and whether undo is the way back.
struct Stale {
    const char* why;
    bool undo;
};
Stale whyStale(const Document& doc) {
    // With no raft no tip goes where the bottom layers print the model: a print setting, not an edit.
    if (doc.supportsOutdatedByBottomLayers())
        return {"The resin's bottom layers or height correction changed after they were made.", false};
    return {doc.supports.result->choices == doc.supports.choices
                ? "The model or the picked sides changed after they were made."
                : "The settings changed after they were made.",
            true};
}

// Under the controls: nothing made, what was made, or that it is out of date.
void statusArea(App& app, Document& doc) {
    const SupportState& s = doc.supports;
    if (!s.result) {
        beginCard("nothing", ImVec2(0, 0), 10, col::Card, col::Border);
        text("Nothing made yet. Make supports uses these settings.", fonts().regular, size::Help,
             col::TextSecondary, true);
        endCard();
        return;
    }
    if (!s.fresh(doc.revision(), doc.supportSettings()) && !running(app)) {
        beginCard("stale", ImVec2(0, 0), 10, col::WarnBg, col::WarnBorder);
        text("Supports are out of date", fonts().semibold, size::Body, col::Text);
        text(whyStale(doc).why, fonts().regular, size::Help, col::TextSecondary, true);
        endCard();
        return;
    }
    const SupportResult& r = *s.result;
    beginCard("made", ImVec2(0, 0), 10, col::Card, col::Border);
    const float w = ImGui::GetContentRegionAvail().x / 3;
    auto stat = [&](const std::string& big, const char* what, int i) {
        if (i > 0) ImGui::SameLine(w * i);
        ImGui::BeginGroup();
        text(big.c_str(), fonts().bold, size::DialogTitle - 2, col::Text);
        text(what, fonts().regular, size::Label, col::TextMuted);
        ImGui::EndGroup();
    };
    stat(thousands(r.tips), "tips", 0);
    stat(thousands(r.trunks), "trunks", 1);
    stat(strf("%.1f ml", r.support_ml), "support resin", 2);
    endCard();
    (void)app;
}

void controlsPanel(App& app, Document& doc) {
    const SupportChoices c = doc.supports.choices;
    const fruit::Placement& p = doc.placement();
    const bool differs = quickChanged(c) || p.on_plate || std::abs(p.elevation - fruit::Placement{}.elevation) > 1e-9;
    {
        const float y = ImGui::GetCursorPosY();
        groupLabel("Quick support");
        if (differs) {
            ImGui::PushFont(fonts().semibold, size::Body);
            const float lw = ImGui::CalcTextSize("Reset to the resin's values").x;
            ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - lw, y - 2));
            if (linkButton("Reset to the resin's values")) app.resetQuickSupport();
        }
    }
    ImGui::Dummy(ImVec2(0, 4));
    for (const QuickSlider q : {QuickSlider::Hold, QuickSlider::Amount, QuickSlider::Sturdiness}) {
        // Each slider reads the choices as they are now, so a move earlier in the frame shows.
        quickSlider(app, doc.supports.choices, q);
        ImGui::Dummy(ImVec2(0, 4));
    }
    onThePlate(app, doc);
}

}  // namespace

void supportsPanel(App& app, Document& doc) {
    controlsPanel(app, doc);
    // The tip picked in the view, and why it is there.
    const SupportState& s = doc.supports;
    // (Not in the Risk view: its marks are the places that may not print, and no tip is ringed there.)
    if (s.result && !running(app) && s.selected_tip >= 0 && app.supportsView() != SupportsView::Risk &&
        s.selected_tip < static_cast<int>(s.result->points.size())) {
        ImGui::Dummy(ImVec2(0, 6));
        groupLabel("Selected tip");
        const SupportResult& r = *s.result;
        const fruit::SupportPoint& p = r.points[static_cast<std::size_t>(s.selected_tip)];
        beginCard("tip", ImVec2(0, 0), 13, col::Card, col::Accent);
        text(tipKind(p).c_str(), fonts().semibold, size::Body, col::Text);
        text(tipWhy(p).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 2));
        text(tipNumbers(p, r.cfg, r.tree_cfg).c_str(), fonts().regular, size::Help, col::TextMuted, true);
        endCard();
    } else if (s.result && !running(app) && app.supportsView() == SupportsView::Risk) {
        ImGui::Dummy(ImVec2(0, 4));
        text("The view shows what may not print. Each numbered place is listed under Checks.", fonts().regular,
             size::Body, col::TextSecondary, true);
    } else if (s.result && !running(app) && app.supportsView() == SupportsView::Why) {
        ImGui::Dummy(ImVec2(0, 4));
        text("Click a tip in the view to see why it is there.", fonts().regular, size::Body, col::TextSecondary, true);
    }
}

// Pinned under the panel's body: always in sight, however far it scrolls.
void supportsFooter(App& app, Document& doc) {
    const bool have = doc.supports.result != nullptr;
    ImGui::Dummy(ImVec2(0, 2));
    statusArea(app, doc);
    ImGui::Dummy(ImVec2(0, 6));
    if (have) {
        const bool fresh = doc.supports.fresh(doc.revision(), doc.supportSettings());
        if (secondaryButton("Edit tips by hand", ImVec2(-1, 34), fresh && !running(app))) app.editTips(true);
        if (!fresh) tooltip("Remake the supports first.");
        ImGui::Dummy(ImVec2(0, 2));
    }
    if (linkButton("Advanced settings")) app.showAdvanced(true);
    std::size_t changed = 0;
    for (const AdvancedSetting& a : advancedSettings()) changed += advancedChanged(a, doc.supports.choices);
    if (changed) {
        ImGui::SameLine(0, 8);
        text(strf("%zu changed", changed).c_str(), fonts().regular, size::Help, col::Accent);
    }
}

namespace {

void checkRow(const char* id, const SupportCheck& c) {
    beginCard(id, ImVec2(0, 0), 12, col::Panel, c.ok ? col::Border : IM_COL32(0x6B, 0x2A, 0x2A, 255));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 9, p.y + 9), 9, c.ok ? col::OkBg : col::ErrorBg, 20);
    if (c.ok) icon::check(dl, ImVec2(p.x + 3, p.y + 3), 12, col::Ok);
    else icon::cross(dl, ImVec2(p.x + 4, p.y + 4), 10, col::Error);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 28, p.y));
    ImGui::BeginGroup();
    text(c.title.c_str(), fonts().semibold, size::Body, col::Text);
    if (!c.detail.empty()) text(c.detail.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    ImGui::EndGroup();
    endCard();
}

void problemCard(App& app, const SupportProblem& p, int i) {
    const bool review = p.level == SupportProblem::Level::Review, blocks = p.level == SupportProblem::Level::Blocks;
    ImGui::PushID(i);
    beginCard("p", ImVec2(0, 0), 14, blocks ? col::ErrorBg : (review ? col::WarnBg : col::Panel),
              blocks ? IM_COL32(0x6B, 0x2A, 0x2A, 255) : (review ? col::WarnBorder : col::Border));
    const float w = ImGui::GetContentRegionAvail().x;
    const float y = ImGui::GetCursorPosY();
    text(p.title.c_str(), fonts().semibold, size::Body, col::Text);
    const std::string n = thousands(p.count);
    ImGui::PushFont(fonts().semibold, size::Body);
    const float nw = ImGui::CalcTextSize(n.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x + w - nw, ImGui::GetWindowPos().y + y),
        blocks ? col::Error : (review ? col::Warn : col::TextMuted), n.c_str());
    ImGui::PopFont();
    text(p.body.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    if (p.has_where) {
        if (secondaryButton("Show", ImVec2(0, 0))) app.focusOn(p.where);
    }
    if (p.code == "suction_cup" || p.code == "resin_trap" || p.code == "cavity_ceiling") {
        if (p.has_where) ImGui::SameLine(0, 8);
        if (secondaryButton("Open Hollows", ImVec2(0, 0))) app.showHollow();
    }
    if (!p.engine.empty()) {
        ImGui::PushFont(fonts().regular, size::Help);
        ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
        if (ImGui::TreeNodeEx("Engine message", ImGuiTreeNodeFlags_SpanAvailWidth)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(p.engine.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TreePop();
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    endCard();
    ImGui::PopID();
}

}  // namespace

void supportChecks(App& app, Document& doc) {
    const SupportState& s = doc.supports;
    if (!s.result) return;
    const SupportResult& r = *s.result;
    for (std::size_t i = 0; i < r.checks.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        checkRow("check", r.checks[i]);
        ImGui::PopID();
        // Right after "No part starts in mid-air": what else may not print, from replaying the print.
        if (i == 0) riskChecks(app, doc);
    }
    if (r.checks.empty()) riskChecks(app, doc);
    if (r.problems.empty()) return;
    ImGui::Dummy(ImVec2(0, 6));
    int i = 0;
    auto section = [&](SupportProblem::Level level, const char* label) {
        const std::size_t n = r.count(level);
        if (n == 0) return;
        groupLabel(strf("%s · %zu", label, n).c_str());
        for (const auto& p : r.problems)
            if (p.level == level) problemCard(app, p, i++);
        ImGui::Dummy(ImVec2(0, 4));
    };
    section(SupportProblem::Level::Blocks, "Blocks saving");
    section(SupportProblem::Level::Review, "Review");
    section(SupportProblem::Level::Note, "Notes");
}

// ---- Over the 3D view ----

namespace {

// The legend, left to right from the cursor, on one line.
void legend(App& app, const SupportResult& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    float x = at.x;
    const float y = at.y + 14;
    if (app.supportsView() == SupportsView::Risk) {
        // What the paint and the tips' colours mean, or that the replay is not done yet.
        ImGui::Dummy(ImVec2(riskLegend(app, at), 28));
        return;
    }
    ImGui::PushFont(fonts().regular, size::Help);
    auto item = [&](const char* label, ImU32 c, bool line) {
        if (line) dl->AddLine(ImVec2(x, y), ImVec2(x + 12, y), c, 2);
        else dl->AddCircleFilled(ImVec2(x + 5, y), 4, c);
        x += line ? 18 : 14;
        dl->AddText(ImVec2(x, y - 7), col::TextSecondary, label);
        x += ImGui::CalcTextSize(label).x + 14;
    };
    if (app.supportsView() == SupportsView::Why) {
        item("Island", col::Accent, false);
        item("Overhang", IM_COL32(0x5B, 0x9B, 0xD5, 255), false);
        item("Extra hold", IM_COL32(0xB4, 0x8E, 0xF0, 255), false);
        item("Steadying", IM_COL32(0x5B, 0xD6, 0x9A, 255), false);
        item("By hand", IM_COL32(0xF2, 0xF4, 0xF7, 255), false);
    } else {
        for (const auto& t : r.tip_sizes) item(strf("%.1f mm", t.first).c_str(), IM_COL32(0xF2, 0xF4, 0xF7, 255), false);
        if (app.supportsView() == SupportsView::Supports) item("Support", IM_COL32(0xE2, 0xE6, 0xED, 255), true);
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(x - at.x, 28));
}

void runningCard(App& app, ImVec2 origin, ImVec2 size) {
    const JobProgress p = app.jobs().progress();
    const ImVec2 cs(340, 286);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - cs.x - 16, origin.y + size.y - cs.y - 16));
    beginCard("running", cs, 16, col::Panel, col::BorderStrong);
    text("Generating supports", fonts().semibold, size::Body + 1, col::Text);
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 5), col::Input, 3);
    dl->AddRectFilled(bp, ImVec2(bp.x + bw * static_cast<float>(p.fraction), bp.y + 5), col::Accent, 3);
    ImGui::Dummy(ImVec2(bw, 10));
    const int at = supportStage(p.stage, p.fraction);
    const auto& stages = supportStages();
    for (int i = 0; i < static_cast<int>(stages.size()); ++i) {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const ImVec2 c(q.x + 8, q.y + 9);
        if (i < at) {
            dl->AddCircleFilled(c, 8, col::OkBg, 20);
            icon::check(dl, ImVec2(c.x - 5, c.y - 5), 10, col::Ok);
        } else if (i == at) {
            dl->AddCircleFilled(c, 8, col::Accent, 20);
        } else {
            dl->AddCircle(c, 7.5f, col::BorderStrong, 20);
        }
        ImGui::SetCursorScreenPos(ImVec2(q.x + 24, q.y));
        text(stages[static_cast<std::size_t>(i)].c_str(), fonts().regular, size::Body,
             i <= at ? col::Text : col::TextMuted);
        ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + 24));
    }
    ImGui::SetCursorPosY(cs.y - 16 - 34);
    if (secondaryButton("Cancel", ImVec2(0, 34))) app.jobs().cancel();
    endCard();
}

// Banners sit in the free space right of the tool panel, above the model tools.
ImVec2 bannerSize(ImVec2 size) { return ImVec2(std::min(560.0f, size.x - kToolPanelRight - 32), 64); }
ImVec2 bannerAt(ImVec2 origin, ImVec2 size, ImVec2 bs) {
    const float left = origin.x + kToolPanelRight + 16;
    return ImVec2(std::max(left, origin.x + size.x - 16 - bs.x), origin.y + size.y - kToolsBarH - 8 - bs.y);
}

void staleBanner(App& app, Document& doc, ImVec2 origin, ImVec2 size) {
    const ImVec2 bs = bannerSize(size);
    ImGui::SetCursorScreenPos(bannerAt(origin, size, bs));
    beginCard("stale", bs, 14, col::WarnBg, col::WarnBorder);
    const float y = ImGui::GetCursorPosY();
    text("Supports are out of date", fonts().semibold, size::Body + 1, col::Text);
    const Stale stale = whyStale(doc);
    text(stale.why, fonts().regular, size::Body, col::TextSecondary);
    // Remaking them is the Job panel's main button; here, the way back.
    if (stale.undo && doc.canUndo()) {
        ImGui::PushFont(fonts().semibold, size::Body);
        const float uw = ImGui::CalcTextSize("Undo the change").x + 32;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(bs.x - 14 - uw, y + 1));
        if (secondaryButton("Undo the change", ImVec2(uw, 34))) app.undo();
    }
    endCard();
}

// Supports are made for the model where it stands, and only when it fits there.
void fitBanner(App& app, Document& doc, ImVec2 origin, ImVec2 size) {
    const Part* off = doc.partOff();
    if (!off) return;
    const int id = off->id();
    const fruit::PlateFit f = off->fit();
    const ImVec2 bs = bannerSize(size);
    ImGui::SetCursorScreenPos(bannerAt(origin, size, bs));
    beginCard("fit", bs, 14, col::ErrorBg, col::Error);
    const float y = ImGui::GetCursorPosY();
    const std::string title = doc.partCount() > 1
                                  ? off->name() + (f.overEdge() ? " is over the plate's edge" : " doesn't fit the plate")
                                  : (f.overEdge() ? "It's over the plate's edge" : "It doesn't fit the plate");
    text(title.c_str(), fonts().semibold, size::Body + 1, col::Text);
    text(f.overEdge() ? "Supports are made for it where it stands, inside the dashed line Move shows."
         : f.fits_turned ? "Too big as it lies; turned 90° it fits at this size."
                         : "Too big at this size, however it is turned.",
         fonts().regular, size::Body, col::TextSecondary);
    const char* fix = f.overEdge() ? "Move it back on" : f.fits_turned ? "Turn 90°" : "Scale it";
    ImGui::PushFont(fonts().semibold, size::Body);
    const float rw = ImGui::CalcTextSize(fix).x + 32;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(bs.x - 14 - rw, y + 1));
    if (secondaryButton(fix, ImVec2(rw, 34))) {
        app.selectPart(id);  // the fixes act on the selected part
        if (f.overEdge()) app.slideOntoPlate();
        else if (f.fits_turned) app.turnQuarter();
        else app.openTool(Tool::Scale);
    }
    endCard();
}

}  // namespace

bool supportsBanner(App& app, const Document& doc, ImVec2 origin, ImVec2 size, ImVec2* min, ImVec2* max) {
    if (app.step() != Step::Supports || app.editingTips() || running(app)) return false;
    const SupportState& s = doc.supports;
    const bool fits = doc.fits();
    if (fits && !(s.result && !s.fresh(doc.revision(), doc.supportSettings()))) return false;
    const ImVec2 bs = bannerSize(size);
    *min = bannerAt(origin, size, bs);
    *max = ImVec2(min->x + bs.x, min->y + bs.y);
    return true;
}

void supportsOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* vp) {
    const SupportState& s = doc.supports;
    if (running(app)) {
        runningCard(app, origin, size);
        return;
    }
    if (!doc.fits()) fitBanner(app, doc, origin, size);
    if (!s.result) return;
    const SupportResult& r = *s.result;
    const bool fresh = s.fresh(doc.revision(), doc.supportSettings());
    if (!fresh && doc.fits()) staleBanner(app, doc, origin, size);
    // The Risk view has its own marks: the places that may not print, by number.
    if (app.supportsView() == SupportsView::Risk) {
        if (fresh) riskOverlay(app, doc, origin, size, vp);
        return;
    }
    // Tips: the selected one is ringed; a click (not a drag) selects the nearest.
    auto project = [&](const fruit::Vec3& w, ImVec2* out) {
        const float x = vp[0] * w.x + vp[4] * w.y + vp[8] * w.z + vp[12];
        const float y = vp[1] * w.x + vp[5] * w.y + vp[9] * w.z + vp[13];
        const float c = vp[3] * w.x + vp[7] * w.y + vp[11] * w.z + vp[15];
        if (c <= 1e-4f) return false;
        *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
        return true;
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (fresh && s.selected_tip >= 0) {
        ImVec2 q;
        if (project(r.points[static_cast<std::size_t>(s.selected_tip)].pos, &q)) {
            dl->AddCircle(q, 9, col::Accent, 24, 2.5f);
            dl->AddCircle(q, 12, IM_COL32(0xFF, 0x8F, 0x3F, 90), 24, 2.0f);
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool inside = m.x >= origin.x && m.y >= origin.y + 50 && m.x < origin.x + size.x && m.y < origin.y + size.y &&
                        ImGui::IsWindowHovered();
    if (fresh && inside && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9 &&
        !ImGui::IsAnyItemHovered()) {
        int best = -1;
        float best_d = 14 * 14;
        for (std::size_t i = 0; i < r.points.size(); ++i) {
            ImVec2 q;
            if (!project(r.points[i].pos, &q)) continue;
            const float d = (q.x - m.x) * (q.x - m.x) + (q.y - m.y) * (q.y - m.y);
            if (d < best_d) {
                best_d = d;
                best = static_cast<int>(i);
            }
        }
        app.selectTip(best);
    }
}

// Over the view's top: which supports to show, and what the marks mean.
void supportsViewPill(App& app, Document& doc) {
    if (!doc.supports.result || running(app)) return;
    const std::pair<const char*, SupportsView> views[] = {{"Supports", SupportsView::Supports},
                                                          {"Tips only", SupportsView::TipsOnly},
                                                          {"Why", SupportsView::Why},
                                                          {"Risk", SupportsView::Risk}};
    bool first = true;
    for (const auto& [label, v] : views) {
        if (!first) ImGui::SameLine(0, 4);
        first = false;
        if (segmentButton(label, app.supportsView() == v)) app.setSupportsView(v);
    }
    legend(app, *doc.supports.result);
}

}  // namespace gui::screens::parts
