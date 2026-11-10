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

// A card choosing the usual tip size.
bool tipCard(const char* id, const char* size, const char* note, bool selected, float w) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(w, 62);
    const bool clicked = ImGui::InvisibleButton(id, sz);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + sz.x, p.y + sz.y);
    dl->AddRectFilled(p, q, selected ? IM_COL32(0x2A, 0x1E, 0x14, 255) : (hovered ? col::Active : col::Input), 8);
    dl->AddRect(p, q, selected ? col::Accent : col::BorderStrong, 8, selected ? 1.5f : 1.0f);
    dl->AddCircleFilled(ImVec2(p.x + w / 2, p.y + 14), 4.5f, selected ? col::Accent : col::Text);
    auto centred = [&](const char* s, ImFont* f, float size, ImU32 c, float y) {
        ImGui::PushFont(f, size);
        const ImVec2 ts = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + y), c, s);
        ImGui::PopFont();
    };
    centred(size, fonts().semibold, size::Body, col::Text, 24);
    centred(note, fonts().regular, size::Label, col::TextMuted, 42);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

void statCard(const char* id, const std::string& big, const char* what, const std::string& detail, float w) {
    beginCard(id, ImVec2(w, 104), 12, col::Panel, col::Border);
    text(big.c_str(), fonts().bold, size::DialogTitle, col::Text);
    text(what, fonts().regular, size::Body, col::TextSecondary);
    text(detail.c_str(), fonts().regular, size::Help, col::TextMuted, true);
    endCard();
}

void choicesPanel(App& app, Document& doc) {
    SupportChoices c = doc.supports.choices;
    bool changed = false;
    groupLabel("Usual tip size");
    const float w = ImGui::GetContentRegionAvail().x, cw = (w - 12) / 3;
    struct T {
        int cls;
        const char* size;
        const char* note;
    };
    const T tips[] = {{1, "0.3 mm", "smaller marks"}, {2, "0.4 mm", "default"}, {3, "0.6 mm", "holds more"}};
    for (int i = 0; i < 3; ++i) {
        if (tipCard(tips[i].size, tips[i].size, tips[i].note, c.usual_tip == tips[i].cls, cw)) {
            c.usual_tip = tips[i].cls;
            changed = true;
        }
        if (i < 2) ImGui::SameLine(0, 6);
    }
    const fruit::SupportConfig cfg = supportConfig(doc.placement(), c);
    const double d = cfg.tip_classes[static_cast<std::size_t>(c.usual_tip)].diameter;
    text(strf("Each tip size holds a set area: %.1f mm holds %.1f mm² of layer. FruitSlicer still uses bigger or "
              "smaller tips where they're needed.",
              d, cfg.capacity.capacity(d))
             .c_str(),
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 6));
    {
        const float y = ImGui::GetCursorPosY();
        text("Tip spacing", fonts().semibold, size::Body, col::Text);
        const char* label = c.density <= 1.05 ? "Normal" : (c.density < 1.55 ? "A little closer" : "Closer together");
        ImGui::PushFont(fonts().regular, size::Body);
        const float lw = ImGui::CalcTextSize(label).x;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - lw, y));
        text(label, fonts().regular, size::Body, col::TextSecondary);
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Input);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::Active);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col::Active);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 7);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
    ImGui::SetNextItemWidth(w);
    float dens = static_cast<float>(c.density);
    if (ImGui::SliderFloat("##spacing", &dens, 1.0f, 2.0f, "", ImGuiSliderFlags_AlwaysClamp)) {
        c.density = std::round(dens * 20) / 20;
        changed = true;
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    {
        const float y = ImGui::GetCursorPosY();
        text("Normal", fonts().regular, size::Help, col::TextMuted);
        ImGui::PushFont(fonts().regular, size::Help);
        const float lw = ImGui::CalcTextSize("Closer together").x;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - lw, y));
        text("Closer together", fonts().regular, size::Help, col::TextMuted);
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (radioCard("merge", "Merge into trunks",
                  "Neighbouring supports join, so fewer stand on the raft and they come off in groups.", c.merge)) {
        c.merge = !c.merge;
        changed = true;
    }
    ImGui::Dummy(ImVec2(0, 2));
    const fruit::Placement& p = doc.placement();
    const std::string raft = p.on_plate ? "Flat on the plate, no raft" : strf("Raft, %.0f mm lift", p.lift());
    {
        // A link to where it is set, not a choice here.
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const ImVec2 sz(w, 54);
        const bool clicked = ImGui::InvisibleButton("raft", sz);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(q, ImVec2(q.x + sz.x, q.y + sz.y), hovered ? col::Card : col::Panel, 10);
        dl->AddRect(q, ImVec2(q.x + sz.x, q.y + sz.y), col::Border, 10);
        ImGui::PushFont(fonts().semibold, size::Body);
        dl->AddText(ImVec2(q.x + 12, q.y + 10), col::Text, raft.c_str());
        ImGui::PopFont();
        ImGui::PushFont(fonts().regular, size::Help);
        dl->AddText(ImVec2(q.x + 12, q.y + 30), col::TextMuted, "Set with On plate");
        ImGui::PopFont();
        ImGui::PushFont(fonts().semibold, size::Section);
        dl->AddText(ImVec2(q.x + sz.x - 20, q.y + 17), col::TextMuted, "›");
        ImGui::PopFont();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked) app.openTool(Tool::OnPlate);
    }
    if (changed) app.setSupportChoices(c);
}

void resultPanel(App& app, Document& doc) {
    const SupportResult& r = *doc.supports.result;
    const float w = ImGui::GetContentRegionAvail().x, cw = (w - 8) / 2;
    statCard("tips", thousands(r.tips), "tips",
             strf("%zu island · %zu extra hold", r.island_tips, r.extra_hold), cw);
    ImGui::SameLine(0, 8);
    statCard("trunks", thousands(r.trunks), "trunks on the raft", strf("from %zu pillars", r.pillars), cw);
    statCard("braces", thousands(r.braces), "braces", "none in the snap-off zone below the tips", cw);
    ImGui::SameLine(0, 8);
    statCard("resin", strf("%.1f ml", r.support_ml), "support resin", "at most", cw);
    ImGui::Dummy(ImVec2(0, 6));
    text("Tip sizes used", fonts().semibold, size::Body, col::Text);
    float x = 0;
    for (std::size_t i = 0; i < r.tip_sizes.size(); ++i) {
        const std::string label = strf("%.1f mm × %zu", r.tip_sizes[i].first, r.tip_sizes[i].second);
        ImGui::PushFont(fonts().medium, size::Body);
        const float lw = ImGui::CalcTextSize(label.c_str()).x + 26;
        ImGui::PopFont();
        if (i > 0 && x + lw <= w) ImGui::SameLine(0, 8);
        else x = 0;
        x += lw + 8;
        chip(label.c_str(), label.c_str(), 0, col::Text, col::Panel, col::BorderStrong);
    }
    (void)app;
}

}  // namespace

void supportsPanel(App& app, Document& doc) {
    const bool have = doc.supports.result != nullptr;
    if (have && !running(app)) {
        if (linkButton("Remake supports")) app.generateSupports();
        ImGui::Dummy(ImVec2(0, 2));
        resultPanel(app, doc);
    } else {
        choicesPanel(app, doc);
    }
    // The tip picked in the view, and why it is there.
    const SupportState& s = doc.supports;
    if (s.result && !running(app) && s.selected_tip >= 0 &&
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
    } else if (s.result && !running(app) && app.supportsView() == SupportsView::Why) {
        ImGui::Dummy(ImVec2(0, 4));
        text("Click a tip in the view to see why it is there.", fonts().regular, size::Body, col::TextSecondary, true);
    }
    ImGui::Dummy(ImVec2(0, 8));
    if (have) {
        const bool fresh = doc.supports.fresh(doc.revision(), doc.supportSettings());
        if (secondaryButton("Edit tips by hand", ImVec2(-1, 38), fresh && !running(app))) app.editTips(true);
        if (!fresh) tooltip("Remake the supports first.");
    }
    ImGui::Dummy(ImVec2(0, 4));
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
    }
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
ImVec2 bannerAt(ImVec2 origin, ImVec2 size, ImVec2 bs) {
    const float left = origin.x + kToolPanelRight + 16;
    return ImVec2(std::max(left, origin.x + size.x - 16 - bs.x), origin.y + size.y - kToolsBarH - 8 - bs.y);
}

void staleBanner(App& app, Document& doc, ImVec2 origin, ImVec2 size) {
    const ImVec2 bs(std::min(560.0f, size.x - kToolPanelRight - 32), 64);
    ImGui::SetCursorScreenPos(bannerAt(origin, size, bs));
    beginCard("stale", bs, 14, col::WarnBg, col::WarnBorder);
    const float y = ImGui::GetCursorPosY();
    text("Supports are out of date", fonts().semibold, size::Body + 1, col::Text);
    const char* why = doc.supports.result->choices == doc.supports.choices
                          ? "The model or the picked sides changed after they were made."
                          : "The settings changed after they were made.";
    text(why, fonts().regular, size::Body, col::TextSecondary);
    // Remaking them is the Job panel's main button; here, the way back.
    if (doc.canUndo()) {
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
    const fruit::PlateFit& f = doc.fit();
    const ImVec2 bs(std::min(560.0f, size.x - kToolPanelRight - 32), 64);
    ImGui::SetCursorScreenPos(bannerAt(origin, size, bs));
    beginCard("fit", bs, 14, col::ErrorBg, col::Error);
    const float y = ImGui::GetCursorPosY();
    text(f.overEdge() ? "It's over the plate's edge" : "It doesn't fit the plate", fonts().semibold, size::Body + 1,
         col::Text);
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
        if (f.overEdge()) app.slideOntoPlate();
        else if (f.fits_turned) app.turnQuarter();
        else app.openTool(Tool::Scale);
    }
    endCard();
}

}  // namespace

void supportsOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* vp) {
    const SupportState& s = doc.supports;
    if (running(app)) {
        runningCard(app, origin, size);
        return;
    }
    if (!doc.fit().fits) fitBanner(app, doc, origin, size);
    if (!s.result) return;
    const SupportResult& r = *s.result;
    const bool fresh = s.fresh(doc.revision(), doc.supportSettings());
    if (!fresh && doc.fit().fits) staleBanner(app, doc, origin, size);
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
    const std::pair<const char*, SupportsView> views[] = {
        {"Supports", SupportsView::Supports}, {"Tips only", SupportsView::TipsOnly}, {"Why", SupportsView::Why}};
    bool first = true;
    for (const auto& [label, v] : views) {
        if (!first) ImGui::SameLine(0, 4);
        first = false;
        if (segmentButton(label, app.supportsView() == v)) app.setSupportsView(v);
    }
    legend(app, *doc.supports.result);
}

}  // namespace gui::screens::parts
