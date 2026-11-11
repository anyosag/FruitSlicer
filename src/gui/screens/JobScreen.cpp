// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The job: the top bar (Prepare or Layers), the 3D view with the tools
// around it and the Job panel, or the layers; and the status bar.
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Repairs.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"
#include "screens/Screens.h"
#include "view3d/Math.h"

namespace gui::screens::parts {

// A structural panel: square, one background, border on one side.
// Item spacing is set inside the child only: the parent lays panels out
// edge to edge.
void beginPanel(const char* id, ImVec2 size, ImU32 bg, ImVec2 padding, bool scroll) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding,
                      scroll ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
}

void endPanel() {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void edge(bool right, bool bottom) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    if (right) dl->AddLine(ImVec2(p.x + s.x - 1, p.y), ImVec2(p.x + s.x - 1, p.y + s.y), col::Border);
    if (bottom) dl->AddLine(ImVec2(p.x, p.y + s.y - 1), ImVec2(p.x + s.x, p.y + s.y - 1), col::Border);
}

void tooltip(const char* s) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Card);
    ImGui::BeginTooltip();
    text(s, fonts().regular, size::Help, col::TextSecondary);
    ImGui::EndTooltip();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// A card floating over the 3D view: rounded, bordered, as tall as what is in
// it (up to `max_h`, then it scrolls); `w` 0 makes it as wide as what is in it.
void beginFloat(const char* id, ImVec2 pos, float w, float max_h, ImVec2 padding) {
    ImGui::SetCursorScreenPos(pos);
    // A soft shadow under it, on the view.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x18, 0x1B, 0x21, 248));
    ImGui::PushStyleColor(ImGuiCol_Border, col::Border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    if (max_h > 0) ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w > 0 ? w : 4000.0f, max_h));
    ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY;
    if (w <= 0) flags |= ImGuiChildFlags_AutoResizeX;
    ImGui::BeginChild(id, ImVec2(w, 0), flags, max_h > 0 ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
}

void endFloat() {
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

}  // namespace gui::screens::parts

namespace gui::screens {

namespace {

using namespace parts;

// ---- Top bar ----

void iconButton(const char* id, IconFn ic, bool enabled, const char* tip, const std::function<void()>& run) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(id, ImVec2(34, 34)) && enabled) run();
    const bool h = enabled && ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + 34, p.y + 34), h ? col::Active : col::Panel, 8);
    dl->AddRect(p, ImVec2(p.x + 34, p.y + 34), col::Border, 8);
    ic(dl, ImVec2(p.x + 8, p.y + 8), 18, enabled ? col::TextSecondary : IM_COL32(0x4B, 0x52, 0x60, 255));
    if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    tooltip(tip);
}

void topBar(App& app) {
    beginPanel("top", ImVec2(0, kTopH), col::Panel, ImVec2(16, 0));
    edge(false, true);
    const auto& doc = app.document();
    ImGui::SetCursorPos(ImVec2(16, 10));
    ImGui::BeginGroup();
    text(doc ? doc->name.c_str() : "New job", fonts().semibold, size::Body + 1, col::Text);
    const char* state = !doc || doc->job_path.empty() ? "Not saved" : doc->unsaved() ? "Edited since saving" : "Saved";
    text(state, fonts().regular, size::Help, col::TextMuted);
    if (doc) tooltip(doc->job_path.empty() ? "Save the job with ⌘S." : doc->job_path.c_str());
    ImGui::EndGroup();

    // Prepare or Layers, in the middle. Layers opens once there are up-to-date
    // supports to slice; the Job panel's Slice button gets there too.
    {
        const bool layers = doc && inLayersMode(app.step());
        const MainButton mb = mainButton(doc.get());
        const bool can_layers = doc && (mb.action == MainAction::Slice || mb.action == MainAction::ShowLayers);
        const float tw = 104, w = tw * 2 + 6;
        const float x = std::round((ImGui::GetWindowWidth() - w) / 2);
        ImGui::SetCursorPos(ImVec2(x, 10));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 36), col::App, 10);
        dl->AddRect(p, ImVec2(p.x + w, p.y + 36), col::Border, 10);
        const char* labels[] = {"Prepare", "Layers"};
        for (int i = 0; i < 2; ++i) {
            const ImVec2 q(p.x + 3 + i * tw, p.y + 3);
            const bool on = (i == 1) == layers;
            const bool enabled = i == 0 ? doc != nullptr : can_layers;
            ImGui::SetCursorScreenPos(q);
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("mode", ImVec2(tw, 30));
            const bool h = enabled && ImGui::IsItemHovered();
            ImGui::PopID();
            if (on) dl->AddRectFilled(q, ImVec2(q.x + tw, q.y + 30), col::Active, 8);
            ImGui::PushFont(on ? fonts().semibold : fonts().medium, size::Body);
            const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
            dl->AddText(ImVec2(q.x + (tw - ts.x) / 2, q.y + (30 - ts.y) / 2),
                        on ? col::Text : enabled ? (h ? col::Text : col::TextSecondary) : IM_COL32(0x5A, 0x61, 0x6E, 255),
                        labels[i]);
            ImGui::PopFont();
            if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (i == 1 && !enabled) tooltip(doc ? "Nothing sliced yet: make supports, then Slice." : "Open a model first.");
            if (clicked && enabled && !on) {
                if (i == 0) app.goToPrepare();
                else app.goToStep(Step::Layers);
            }
        }
    }

    // Undo and redo, at the right.
    {
        const float right = ImGui::GetWindowWidth() - 16;
        ImGui::SetCursorPos(ImVec2(right - 34 - 6 - 34, 11));
        iconButton("undo", icon::undo, doc && doc->canUndo(), "Undo (⌘Z)", [&app] { app.undo(); });
        ImGui::SameLine(0, 6);
        iconButton("redo", icon::redo, doc && doc->canRedo(), "Redo (⇧⌘Z)", [&app] { app.redo(); });
    }
    endPanel();
}

// ---- Viewport ----

// Screen position of printer point `w`; false when behind the camera.
bool project(const M4& vp, ImVec2 origin, ImVec2 size, V3 w, ImVec2* out) {
    const float* m = vp.m;
    const float x = m[0] * w.x + m[4] * w.y + m[8] * w.z + m[12];
    const float y = m[1] * w.x + m[5] * w.y + m[9] * w.z + m[13];
    const float c = m[3] * w.x + m[7] * w.y + m[11] * w.z + m[15];
    if (c <= 1e-4f) return false;
    *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
    return true;
}

void axisGizmo(App& app, ImVec2 origin, ImVec2 size) {
    const M4 v = app.camera().view();
    const ImVec2 c(origin.x + 58, origin.y + size.y - 58);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* names[] = {"X", "Y", "Z"};
    ImGui::PushFont(fonts().medium, size::Label);
    for (int i = 0; i < 3; ++i) {
        // Direction of the world axis on screen (view rotation only).
        const float dx = v.m[i * 4 + 0], dy = v.m[i * 4 + 1];
        const ImVec2 e(c.x + dx * 30, c.y - dy * 30);
        dl->AddLine(c, e, IM_COL32(0x8A, 0x92, 0xA0, 255), 1.2f);
        const ImVec2 ts = ImGui::CalcTextSize(names[i]);
        dl->AddText(ImVec2(e.x + dx * 9 - ts.x / 2, e.y - dy * 9 - ts.y / 2), col::TextMuted, names[i]);
    }
    ImGui::PopFont();
}

// How far the model sits above the plate: an arrow from the bottom corner
// nearest the camera down to a ring on the plate under it, and a label. With
// supports in the way it stands beside the corner, tied to the model and the
// plate by dashed lines. When the label would run under a panel or off the
// view, it goes to the next bottom corner along; it stays above Supports'
// banner.
void liftMarker(App& app, const Document& doc, const M4& vp, ImVec2 origin, ImVec2 size) {
    const double lift = doc.placement().lift();
    if (lift <= 0) return;
    const fruit::BBox3 b = doc.placedBox();
    const V3 eye = app.camera().eye();
    const float cx = static_cast<float>((b.min.x + b.max.x) / 2), cy = static_cast<float>((b.min.y + b.max.y) / 2);
    const float z = static_cast<float>(lift);
    if (eye.z < z) return;  // looking up from below: the plate hides the gap
    ImVec2 mid;
    if (!project(vp, origin, size, {cx, cy, z}, &mid)) return;
    const bool aside = doc.supports.result != nullptr && app.step() == Step::Supports;
    ImGui::PushFont(fonts().semibold, size::Help);
    const std::string label = strf("%.0f mm lift", lift);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    ImGui::PopFont();
    const ImVec2 ps(ts.x + 22, 24);
    // The free part of the view: right of the rail (and an open tool panel),
    // left of the Job panel.
    const float room_l = origin.x + (app.toolPanelOpen() || app.editingTips() ? kToolPanelRight : kToolPanelX) + 8;
    const float room_r = origin.x + size.x - 8;
    // The corner nearest the camera first, then the two beside it.
    const float nx = static_cast<float>(eye.x >= cx ? b.max.x : b.min.x);
    const float ny = static_cast<float>(eye.y >= cy ? b.max.y : b.min.y);
    const float fx = static_cast<float>(eye.x >= cx ? b.min.x : b.max.x);
    const float fy = static_cast<float>(eye.y >= cy ? b.min.y : b.max.y);
    const V3 corners[3] = {{nx, ny, 0}, {fx, ny, 0}, {nx, fy, 0}};
    const float reach = (aside ? 30 : 0) + 10 + ps.x;  // from the corner to the label's far end
    ImVec2 a, c;
    float out = 0;
    bool found = false, roomy = false;
    for (const V3& k : corners) {
        ImVec2 ka, kc;
        if (!project(vp, origin, size, {k.x, k.y, z}, &ka) || !project(vp, origin, size, {k.x, k.y, 0}, &kc)) continue;
        if (kc.y - ka.y < 14) continue;  // seen edge-on: nothing to show
        const float ko = ka.x >= mid.x ? 1.0f : -1.0f;  // away from the model on screen
        const float far = ka.x + ko * reach;
        const bool fits = std::min(ka.x, far) >= room_l && std::max(ka.x, far) <= room_r;
        if (!found || fits) a = ka, c = kc, out = ko, found = true, roomy = fits;
        if (fits) break;
    }
    if (!found) return;
    if (!roomy) out = -out;  // no room outside any of them: the label goes inside, over the model
    const ImU32 lc = col::Text, dc = IM_COL32(0xC3, 0xC8, 0xD2, 200);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lx = aside ? a.x + out * 30 : a.x;
    if (aside) {
        dashedRect(dl, ImVec2(a.x + out * 3, a.y), ImVec2(lx + out * 7, a.y), dc, 1.2f, 3, 3);
        dashedRect(dl, ImVec2(c.x + out * 3, c.y), ImVec2(lx + out * 7, c.y), dc, 1.2f, 3, 3);
    } else {
        dl->AddEllipse(c, ImVec2(8, 3), lc, 0, 20, 1.3f);
    }
    const float bx = aside ? lx : c.x;  // bottom end: under the corner
    dl->AddLine(ImVec2(lx, a.y + 8), ImVec2(bx, c.y - 8), lc, 1.6f);
    dl->AddTriangleFilled(ImVec2(lx, a.y + 1), ImVec2(lx + 4, a.y + 9), ImVec2(lx - 4, a.y + 9), lc);
    dl->AddTriangleFilled(ImVec2(bx, c.y - 1), ImVec2(bx + 4, c.y - 9), ImVec2(bx - 4, c.y - 9), lc);
    const float px = out > 0 ? lx + 10 : lx - 10 - ps.x;
    float py = std::round((a.y + c.y) / 2 - ps.y / 2);
    ImVec2 b0, b1;
    if (supportsBanner(app, doc, origin, size, &b0, &b1) && px < b1.x && px + ps.x > b0.x && py + ps.y > b0.y - 6)
        py = b0.y - 6 - ps.y;  // above Supports' banner, not under it
    ImGui::PushFont(fonts().semibold, size::Help);
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + ps.x, py + ps.y), IM_COL32(0x18, 0x1B, 0x21, 240), 12);
    dl->AddRect(ImVec2(px, py), ImVec2(px + ps.x, py + ps.y), col::BorderStrong, 12);
    dl->AddText(ImVec2(px + 11, py + (ps.y - ts.y) / 2), col::Text, label.c_str());
    ImGui::PopFont();
}

void taskTray(App& app, ImVec2 origin, ImVec2 avail) {
    const JobProgress p = app.jobs().progress();
    if (!p.running) return;
    // The main button shows these two itself, and Cut, Label and Combine show theirs under their panel.
    if (p.name == "Generating supports" || p.name == "Cutting layers") return;
    const Tool open = app.toolPanelOpen() ? app.tool() : Tool::None;
    if (app.shapeBusy() && (open == Tool::Cut || open == Tool::Label || open == Tool::Combine)) return;
    const ImVec2 size(340, 96);
    ImVec2 at(origin.x + avail.x - size.x - 16, origin.y + avail.y - size.y - 16);
    // Above the model tools when it would cover them, and above a banner there.
    const ToolsBar bar = toolsBarLayout(origin, avail);
    if (!app.editingTips() && at.x < bar.pos.x + bar.w + 8) at.y = bar.pos.y - 8 - size.y;
    ImVec2 b0, b1;
    if (const auto& d = app.document(); d && supportsBanner(app, *d, origin, avail, &b0, &b1) && at.x < b1.x &&
                                        at.y + size.y > b0.y - 8)
        at.y = b0.y - 8 - size.y;
    ImGui::SetCursorScreenPos(at);
    beginCard("tray", size, 14, col::Panel, col::BorderStrong);
    text(p.name.c_str(), fonts().semibold, size::Body, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 6), col::Input, 3);
    dl->AddRectFilled(bp, ImVec2(bp.x + bw * static_cast<float>(p.fraction), bp.y + 6), col::Accent, 3);
    ImGui::Dummy(ImVec2(bw, 12));
    if (secondaryButton("Cancel")) app.jobs().cancel();
    endCard();
}
void dropCard(App& app, ImVec2 origin, ImVec2 avail) {
    const ImVec2 cs(580, 246);
    const ImVec2 p(origin.x + std::round((avail.x - cs.x) / 2), origin.y + std::round((avail.y - cs.y) / 2) - 20);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + cs.x, p.y + cs.y), IM_COL32(0x11, 0x13, 0x17, 245), 14);
    dashedRect(dl, p, ImVec2(p.x + cs.x, p.y + cs.y), IM_COL32(0x5A, 0x62, 0x70, 255), 1.5f, 7, 5);
    icon::upload(dl, ImVec2(p.x + cs.x / 2 - 18, p.y + 40), 36, col::TextSecondary);
    auto centred = [&](const char* s, ImFont* f, float sz, ImU32 c, float y) {
        ImGui::PushFont(f, sz);
        const ImVec2 ts = ImGui::CalcTextSize(s);
        dl->AddText(ImVec2(p.x + (cs.x - ts.x) / 2, p.y + y), c, s);
        ImGui::PopFont();
    };
    centred("Drop an STL, OBJ or 3MF file", fonts().semibold, size::Section + 3, col::Text, 96);
    const fruit::PrinterProfile& pr = app.printer();
    centred(strf("The plate is %.2f × %.2f mm and %.0f mm tall.", pr.build_x, pr.build_y, pr.build_z).c_str(),
            fonts().regular, size::Body, col::TextSecondary, 132);
    ImGui::PushFont(fonts().bold, size::Body);
    const float bw = ImGui::CalcTextSize("Open model...").x + 34;
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + (cs.x - bw) / 2, p.y + 166));
    if (primaryButton("Open model...", ImVec2(bw, 40))) app.openDialog();
}

void viewport(App& app, ImVec2 size, const Overlay& overlay = {}) {
    app.syncScene();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Viewport);
    ImGui::BeginChild("viewport", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImGuiIO& io = ImGui::GetIO();
    const int pw = static_cast<int>(avail.x * io.DisplayFramebufferScale.x);
    const int ph = static_cast<int>(avail.y * io.DisplayFramebufferScale.y);
    const auto& doc = app.document();
    {
        // A panel open over the view's left: keep the model in the middle of
        // what it leaves, sliding there rather than jumping.
        const bool panel = doc && (app.toolPanelOpen() || app.editingTips());
        const float want = panel ? kToolPanelRight / std::max(1.0f, avail.x) : 0.0f;
        const float now = app.camera().shift();
        const float next = app.options().deterministic || std::abs(want - now) < 0.002f ? want : now + (want - now) * 0.25f;
        app.camera().setShift(next);
    }
    const unsigned tex =
        app.renderer().render(pw, ph, app.camera(), app.viewSamples());
    if (tex) ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), avail, ImVec2(0, 1), ImVec2(1, 0));
    else ImGui::Dummy(avail);
    // Mouse: left drag turns, right/middle (or Shift + left) drag moves, wheel
    // zooms; not while a handle or a tip is held, or over a handle.
    if (!app.handlesShown()) app.hoverHandle() = Handle::None;  // left where the handles were
    if (app.mouseSpent() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) app.mouseSpent() = false;
    const bool holding = app.placing() || app.draggingTip() >= 0 || app.hoverHandle() != Handle::None ||
                         app.mouseSpent();
    if (ImGui::IsItemHovered() && holding && io.MouseWheel != 0) app.camera().zoom(io.MouseWheel);
    if (ImGui::IsItemHovered() && !holding) {
        if (io.MouseWheel != 0) app.camera().zoom(io.MouseWheel);
        const bool pan = ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                         (io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
        if (pan) {
            app.camera().pan(io.MouseDelta.x, io.MouseDelta.y, avail.y);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && app.draggingTip() < 0) {
            app.camera().orbit(io.MouseDelta.x, io.MouseDelta.y);
            app.viewChanged();
        }
    }
    const M4 vp = app.camera().proj(avail.x / std::max(1.0f, avail.y)) * app.camera().view();
    if (doc) {
        liftMarker(app, *doc, vp, origin, avail);
        axisGizmo(app, origin, avail);
    }
    if (overlay) overlay(origin, avail, vp.m);
    if (!doc && !app.jobs().busy() && !app.checking()) dropCard(app, origin, avail);
    taskTray(app, origin, avail);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ---- Status bar ----

void statusBar(App& app) {
    beginPanel("status", ImVec2(0, kStatusH), col::Panel, ImVec2(16, 0));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        dl->AddLine(p, ImVec2(p.x + ImGui::GetWindowWidth(), p.y), col::Border);
    }
    const auto& doc = app.document();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = 16;
    const float cy = kStatusH / 2;
    int problems = 0;
    std::string totals;
    const bool results = doc && app.step() == Step::Orient && app.orientView() == OrientView::Results &&
                         doc->orient().results;
    if (results) {
        bool all = true;
        for (const auto& op : doc->orient().results->options) all = all && op.fits;
        const ImVec2 wp = ImGui::GetWindowPos();
        dl->AddCircleFilled(ImVec2(wp.x + x + 4, wp.y + cy), 3.5f, all ? col::Ok : col::Warn);
        x += 14;
        ImGui::SetCursorPos(ImVec2(x, cy - 8));
        text(all ? "Every option fits the plate" : "An option does not fit the plate however it is turned", fonts().regular,
             size::Body, col::TextSecondary);
    } else if (doc && inLayersMode(app.step()) && doc->layers.fresh(doc->supports.result)) {
        const LayerResult& lr = *doc->layers.result;
        const ImVec2 wp = ImGui::GetWindowPos();
        dl->AddCircleFilled(ImVec2(wp.x + x + 4, wp.y + cy), 3.5f, col::Ok);
        x += 14;
        ImGui::SetCursorPos(ImVec2(x, cy - 8));
        text(("Fits the plate · " + dims(lr.bounds.size()) + " with supports").c_str(), fonts().regular, size::Body,
             col::TextSecondary);
        problems = static_cast<int>(lr.cups() + lr.traps());
        totals = strf("%s layers · %.1f ml of resin", thousands(lr.layers->size()).c_str(), lr.resin_ml);
        {
            const auto plan = app.printPlan();
            if (plan && plan->ready) {
                const long m = std::max(1L, std::lround(plan->print_time_s / 60));
                totals = strf("%s layers · %.1f ml of resin · about %s", thousands(plan->layers).c_str(), lr.resin_ml,
                              (m >= 60 ? strf("%ld h %ld min", m / 60, m % 60) : strf("%ld min", m)).c_str());
            }
        }
    } else if (doc && app.step() == Step::Supports && doc->supports.result &&
               doc->supports.fresh(doc->revision(), doc->supportSettings())) {
        const SupportResult& r = *doc->supports.result;
        const ImVec2 wp = ImGui::GetWindowPos();
        const bool ok = r.count(SupportProblem::Level::Blocks) == 0;
        dl->AddCircleFilled(ImVec2(wp.x + x + 4, wp.y + cy), 3.5f, ok ? col::Ok : col::Error);
        x += 14;
        ImGui::SetCursorPos(ImVec2(x, cy - 8));
        text(("Fits the plate · " + dims(r.size_with_supports) + " with supports").c_str(), fonts().regular, size::Body,
             col::TextSecondary);
        problems = static_cast<int>(r.count(SupportProblem::Level::Review) + r.count(SupportProblem::Level::Blocks));
        totals = strf("%zu tips · %.1f ml of supports at most", r.tips, r.support_ml);
        if (!doc->supports.edits.empty()) {
            totals = strf("%zu tips · %s by you", r.tips, describeCounts(countEdits(doc->supports.edits)).c_str());
        }
    } else if (doc) {
        const bool fits = doc->fits();
        std::size_t off = 0, open = 0;
        for (const Part& part : doc->parts()) {
            off += part.fit().fits ? 0 : 1;
            open += part.check().closed() ? 0 : 1;
        }
        problems = static_cast<int>(off + open) + (hollowsToFix(*doc) ? 1 : 0);
        const ImVec2 wp = ImGui::GetWindowPos();
        dl->AddCircleFilled(ImVec2(wp.x + x + 4, wp.y + cy), 3.5f, fits ? col::Ok : col::Error);
        x += 14;
        const std::size_t n = doc->partCount();
        const std::string msg =
            n > 1 ? (fits ? strf("Fits the plate · %zu parts · ", n) + dims(doc->placedBox().size())
                          : strf("%zu of %zu parts %s fit the plate", off, n, off == 1 ? "doesn't" : "don't"))
                  : (fits                     ? "Fits the plate · "
                     : doc->fit().overEdge() ? "Over the plate's edge · "
                                             : "Too big for the plate · ") +
                        dims(doc->fit().size);
        ImGui::SetCursorPos(ImVec2(x, cy - 8));
        text(msg.c_str(), fonts().regular, size::Body, col::TextSecondary);
    } else {
        ImGui::SetCursorPos(ImVec2(x, cy - 8));
        text("No model", fonts().regular, size::Body, col::TextSecondary);
    }
    ImGui::SameLine(0, 20);
    text(strf("Problems: %d", problems).c_str(), fonts().regular, size::Body,
         problems ? col::Warn : col::TextSecondary);
    if (!app.printerSetup().orientation_confirmed) {
        ImGui::SameLine(0, 20);
        ImGui::SetCursorPosY(cy - 15);
        if (chip("orientation", "Image orientation unconfirmed", col::Warn, col::Warn, col::WarnBg, col::WarnBorder))
            app.setScreen(Screen::OrientationCheck);
        tooltip("Until the check confirms which way the 16K shows images, text and left- or right-handed parts could "
                "print mirrored. Click to run it: 5 minutes, no resin.");
    }
    if (!totals.empty()) {  // right end of the bar
        ImGui::PushFont(fonts().regular, size::Body);
        const float tw = ImGui::CalcTextSize(totals.c_str()).x;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 16 - tw, cy - 8));
        text(totals.c_str(), fonts().regular, size::Body, col::TextSecondary);
    }
    endPanel();
}

// The advanced support settings, as a sheet over the workspace.
void advancedSheet(App& app, Document& doc, ImVec2 origin, ImVec2 area) {
    // Over everything behind it, so clicks land on the sheet or nowhere.
    ImGui::SetCursorScreenPos(origin);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(6, 7, 9, 158));
    ImGui::BeginChild("advanced_dim", area, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::InvisibleButton("dim", area);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    const ImVec2 sz(std::min(1160.0f, area.x - 80), area.y - 48);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + std::round((area.x - sz.x) / 2), origin.y + 24));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::BeginChild("advanced_sheet", sz, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    supportsAdvanced(app, doc, ImVec2(0, sz.y - 16));
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

}  // namespace

void parts::viewport3d(App& app, ImVec2 size, const char* hint, const Overlay& overlay) {
    (void)hint;
    viewport(app, size, overlay);
}

void job(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);  // structural panels are square
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    topBar(app);
    const float body_h = ImGui::GetContentRegionAvail().y - kStatusH;
    const ImVec2 body_at = ImGui::GetCursorScreenPos();
    const float body_w = ImGui::GetContentRegionAvail().x;
    const auto& doc = app.document();
    if (doc && inLayersMode(app.step())) {
        layersMode(app, *doc, ImVec2(0, body_h));
    } else {
        Document* d = doc.get();
        viewport(app, ImVec2(body_w - kJobW, body_h),
                 [&app, d](ImVec2 o, ImVec2 s, const float* vp) { workspaceOverlay(app, d, o, s, vp); });
        ImGui::SameLine(0, 0);
        jobPanel(app, body_h);
        if (doc && app.advancedOpen()) advancedSheet(app, *doc, body_at, ImVec2(body_w, body_h));
    }
    statusBar(app);
    ImGui::PopStyleVar(2);
}

}  // namespace gui::screens
