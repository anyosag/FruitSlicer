// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The job: top bar with the step bar, the current step's panels around the
// 3D view, and the status bar. Only the Model step exists yet.
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/Screens.h"
#include "view3d/Math.h"

namespace gui::screens {

namespace {

constexpr float kTopH = 60, kStatusH = 42, kLeftW = 316, kRightW = 297;

// A structural panel: square, one background, border on one side.
// Item spacing is set inside the child only: the parent lays panels out
// edge to edge.
void beginPanel(const char* id, ImVec2 size, ImU32 bg, ImVec2 padding) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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

// ---- Top bar ----

void topBar(App& app) {
    beginPanel("top", ImVec2(0, kTopH), col::Panel, ImVec2(16, 0));
    edge(false, true);
    const auto& doc = app.document();
    ImGui::SetCursorPos(ImVec2(16, 12));
    ImGui::BeginGroup();
    text(doc ? doc->name.c_str() : "New job", fonts().semibold, size::Body + 1, col::Text);
    text("Not saved", fonts().regular, size::Help, col::TextMuted);
    ImGui::EndGroup();

    static const char* kSteps[] = {"Model", "Orient", "Supports", "Layers", "Print"};
    ImGui::SetCursorPos(ImVec2(152, 11));
    for (int i = 0; i < 5; ++i) {
        const bool current = static_cast<int>(app.step()) == i;
        stepPill(i + 1, kSteps[i], current ? StepState::Current : StepState::Upcoming, current);
        if (!current) tooltip("This step arrives in the next update.");
        ImGui::SameLine(0, 8);
    }

    // Right side: the resin in use, then the one accent button.
    const char* next = doc ? "Next: Orient" : "Open model...";
    ImGui::PushFont(fonts().bold, size::Body);
    const float bw = ImGui::CalcTextSize(next).x + 32;
    ImGui::PopFont();
    const float right = ImGui::GetWindowWidth() - 16;
    ImGui::SetCursorPos(ImVec2(right - bw, 10));
    if (doc) {
        primaryButton(next, ImVec2(bw, 38), false);
        tooltip("Orienting arrives in the next update.");
    } else if (primaryButton(next, ImVec2(bw, 38))) {
        app.openDialog();
    }
    ImGui::PushFont(fonts().medium, size::Body);
    const float cw = ImGui::CalcTextSize("My resin · Defaults").x + 40;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(right - bw - 16 - cw, 14));
    chip("resin", "My resin · Defaults", col::TextMuted, col::TextSecondary, col::Panel, col::BorderStrong);
    tooltip("Starting values borrowed from PrusaSlicer. Resin profiles arrive with calibration.");
    endPanel();
}

// ---- Left panel ----

void emptyLeft(App&) {
    sectionTitle("Model");
    ImGui::Dummy(ImVec2(0, 2));
    text("No model on the plate yet. Open a file or drop it on the plate.", fonts().regular, size::Body,
         col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 6));
    groupLabel("Files FruitSlicer reads");
    for (auto [k, v] : {std::pair{"STL", "binary or text"}, std::pair{"OBJ", "shapes only, no colours"},
                        std::pair{"3MF", "every object on the plate"}}) {
        beginCard(k, ImVec2(0, 40), 0, col::Panel, col::Border);
        ImGui::SetCursorPos(ImVec2(12, 12));
        text(k, fonts().semibold, size::Body, col::Text);
        ImGui::PushFont(fonts().regular, size::Body);
        const float vw = ImGui::CalcTextSize(v).x;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 12 - vw, 12));
        text(v, fonts().regular, size::Body, col::TextMuted);
        endCard();
    }
    ImGui::Dummy(ImVec2(0, 6));
    groupLabel("What happens next");
    const char* steps[] = {"FruitSlicer checks the mesh and fixes inside-out parts.",
                           "It checks the size against the plate.",
                           "You choose a scale, then move on to orienting it."};
    for (int i = 0; i < 3; ++i) {
        text(strf("%d.", i + 1).c_str(), fonts().regular, size::Body, col::TextSecondary);
        ImGui::SameLine(0, 6);
        text(steps[i], fonts().regular, size::Body, col::TextSecondary, true);
    }
}

void fileCard(App& app, const Document& doc) {
    beginCard("file", ImVec2(0, 62), 0, col::Card, col::Border);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    dl->AddRectFilled(ImVec2(p.x + 12, p.y + 15), ImVec2(p.x + 44, p.y + 47), col::Viewport, 7);
    const std::string letter(1, static_cast<char>(std::toupper(static_cast<unsigned char>(doc.name.empty() ? '?' : doc.name[0]))));
    ImGui::PushFont(fonts().bold, size::Section);
    const ImVec2 ls = ImGui::CalcTextSize(letter.c_str());
    dl->AddText(ImVec2(p.x + 28 - ls.x / 2, p.y + 31 - ls.y / 2), col::Text, letter.c_str());
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(56, 12));
    ImGui::BeginGroup();
    const std::string file = std::filesystem::path(doc.path).filename().string();
    text(file.c_str(), fonts().semibold, size::Body, col::Text);
    text((thousands(doc.check.triangles) + " triangles · " + (doc.check.closed() ? "closed" : "open")).c_str(),
         fonts().regular, size::Help, col::TextMuted);
    ImGui::EndGroup();
    ImGui::PushFont(fonts().semibold, size::Body);
    const float rw = ImGui::CalcTextSize("Replace").x;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 12 - rw, 22));
    ImGui::PushFont(fonts().semibold, size::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, col::Accent);
    if (ImGui::Selectable("Replace", false, 0, ImVec2(rw, 0))) app.openDialog();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    endCard();
}

void modelLeft(App& app, Document& doc) {
    fileCard(app, doc);
    ImGui::Dummy(ImVec2(0, 8));
    groupLabel("Size");
    const fruit::Placement& p = doc.placement();
    const float w = ImGui::GetContentRegionAvail().x;
    {
        const float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + 8);
        text("Scale", fonts().regular, size::Body, col::TextSecondary);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - 108, y));
        double pct = p.scale * 100;
        if (numberField("scale", &pct, 90, "%g", 1, 100000)) app.setScalePercent(pct);
        ImGui::SameLine(0, 6);  // ImGui aligns text with the field's own text
        text("%", fonts().regular, size::Body, col::TextSecondary);
    }
    text(dims(doc.fit().size).c_str(), fonts().regular, size::Body, col::TextSecondary);
    {
        const fruit::PlateFit& f = doc.fit();
        const double best = std::max(f.largest, f.largest_turned);
        const std::string label = "Largest that fits (" + percent(best) + (f.largest_turned > f.largest ? ", turned)" : ")");
        if (segmentButton(label.c_str(), false)) app.scaleToFit();
    }
    ImGui::Dummy(ImVec2(0, 8));
    groupLabel("Rotation");
    {
        const float fw = (w - 16) / 3;
        const float x0 = ImGui::GetCursorPosX(), y = ImGui::GetCursorPosY();
        const char* axes[] = {"X °", "Y °", "Z °"};
        fruit::Vec3 r = p.rotation_deg;
        double* v[] = {&r.x, &r.y, &r.z};
        bool changed = false;
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorPos(ImVec2(x0 + i * (fw + 8), y));
            text(axes[i], fonts().regular, size::Help, col::TextMuted);
            ImGui::SetCursorPos(ImVec2(x0 + i * (fw + 8), y + 20));
            if (numberField(axes[i], v[i], fw, "%g", -360, 360)) changed = true;
        }
        if (changed) app.setRotation(r);
        ImGui::SetCursorPos(ImVec2(x0, y + 20 + 34 + 8));
    }
    text("Next step finds a good rotation for you.", fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 8));
    groupLabel("How it sits on the plate");
    const std::string raised = strf("%.0f mm above a 1 mm raft that comes off in one piece. Best for most models.",
                                    fruit::Placement{}.elevation);
    if (radioCard("raised", "Raised on supports", raised.c_str(), !p.on_plate)) app.setOnPlate(false);
    ImGui::Dummy(ImVec2(0, 2));
    if (radioCard("flat", "Flat on the plate", "For flat-backed parts. No raft; only overhangs get tips.", p.on_plate))
        app.setOnPlate(true);
}

void leftPanel(App& app, float h) {
    beginPanel("left", ImVec2(kLeftW, h), col::Panel, ImVec2(18, 18));
    edge(true, false);
    if (const auto& doc = app.document()) modelLeft(app, *doc);
    else emptyLeft(app);
    endPanel();
}

// ---- Right panel ----

void plateMap(const App& app, const Document& doc) {
    const fruit::PrinterProfile& pr = app.printer();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = std::round(w * static_cast<float>(pr.build_y / pr.build_x));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col::Viewport, 8);
    const float sx = w / static_cast<float>(pr.build_x), sy = h / static_cast<float>(pr.build_y);
    auto map = [&](double x, double y) {
        return ImVec2(p.x + w / 2 + static_cast<float>(x) * sx, p.y + h / 2 - static_cast<float>(y) * sy);
    };
    const fruit::BBox3 b = doc.placed().bbox();
    const ImVec2 a = map(b.min.x, b.max.y), c = map(b.max.x, b.min.y);
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddRectFilled(a, c, doc.fit().fits ? IM_COL32(0xA9, 0xB1, 0xBE, 255) : IM_COL32(0xD8, 0x6A, 0x6A, 255), 2);
    dashedRect(dl, ImVec2(p.x + w / 2, p.y), ImVec2(p.x + w / 2, p.y + h), IM_COL32(0x5A, 0x62, 0x70, 255), 1, 4, 4);
    dashedRect(dl, ImVec2(p.x, p.y + h / 2), ImVec2(p.x + w, p.y + h / 2), IM_COL32(0x5A, 0x62, 0x70, 255), 1, 4, 4);
    dl->PopClipRect();
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), col::Border, 8);
}

void healthCard(const Document& doc) {
    const fruit::MeshCheck& c = doc.check;
    std::string issues;
    auto add = [&](const std::string& s) { issues += (issues.empty() ? "" : " ") + s; };
    if (!c.closed()) add(thousands(c.open_edges) + " open edges.");
    if (c.cavities) add(strf("%d closed %s inside.", c.cavities, c.cavities == 1 ? "hollow" : "hollows"));
    const bool clean = issues.empty();
    std::string body;
    if (clean) {
        body = (c.shells == 1 ? std::string("One closed shell") : std::to_string(c.shells) + " closed shells") +
               (c.flipped ? strf(", %d turned outward", c.flipped) : std::string(", nothing flipped")) + ", no hollows.";
    } else {
        body = issues;
    }
    beginCard("health", ImVec2(0, 0), 14, clean ? col::Panel : col::WarnBg, clean ? col::Border : col::WarnBorder);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (clean) icon::check(dl, ImVec2(p.x, p.y + 2), 14, col::Ok);
    else text("!", fonts().bold, size::Body, col::Warn);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 22, p.y));
    ImGui::BeginGroup();
    text(clean ? "Mesh is clean" : "Check the mesh", fonts().semibold, size::Body, col::Text);
    text(body.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::EndGroup();
    endCard();
}

void rightPanel(App& app, float h) {
    beginPanel("right", ImVec2(kRightW, h), col::Panel, ImVec2(18, 22));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        dl->AddLine(p, ImVec2(p.x, p.y + h), col::Border);
    }
    const auto& doc = app.document();
    groupLabel("On the plate");
    ImGui::Dummy(ImVec2(0, 2));
    plateMap(app, *doc);
    text("Top view, to scale. Centred automatically.", fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 4));
    const fruit::PlateFit& f = doc->fit();
    const fruit::Placement& p = doc->placement();
    keyValue("Footprint", dims2(f.size.x, f.size.y));
    keyValue("Height with lift", strf("%.1f of %.0f mm", f.top, app.printer().build_z));
    keyValue("Model resin", strf("%.1f ml", doc->check.volume_ml * p.scale * p.scale * p.scale));
    ImGui::Dummy(ImVec2(0, 8));
    healthCard(*doc);
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

void liftMarker(App& app, const Document& doc, const M4& vp, ImVec2 origin, ImVec2 size) {
    const double lift = doc.placement().lift();
    if (lift <= 0) return;
    const fruit::BBox3 b = doc.placed().bbox();
    const V3 top{static_cast<float>(b.max.x + 6), static_cast<float>(b.min.y), static_cast<float>(lift)};
    const V3 bottom{top.x, top.y, 0};
    ImVec2 a, c;
    if (!project(vp, origin, size, top, &a) || !project(vp, origin, size, bottom, &c)) return;
    if (std::abs(c.y - a.y) < 8) return;  // seen edge-on: nothing to show
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 lc = IM_COL32(0xC3, 0xC8, 0xD2, 220);
    dashedRect(dl, a, c, lc, 1.2f, 4, 3);
    dl->AddLine(ImVec2(a.x - 7, a.y), ImVec2(a.x + 7, a.y), lc, 1.2f);
    dl->AddLine(ImVec2(c.x - 7, c.y), ImVec2(c.x + 7, c.y), lc, 1.2f);
    ImGui::PushFont(fonts().medium, size::Help);
    const std::string label = strf("%.0f mm lift", lift);
    dl->AddText(ImVec2(std::max(a.x, c.x) + 14, (a.y + c.y) / 2 - 7), col::TextSecondary, label.c_str());
    ImGui::PopFont();
    (void)app;
}

void taskTray(App& app, ImVec2 vp_max) {
    const JobProgress p = app.jobs().progress();
    if (!p.running) return;
    const ImVec2 size(340, 96);
    ImGui::SetCursorScreenPos(ImVec2(vp_max.x - size.x - 16, vp_max.y - size.y - 16));
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

void viewport(App& app, ImVec2 size) {
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
    if (doc) {
        app.renderer().setModel(doc->model.mesh, doc->geometry_id);
        app.renderer().setModelTransform(doc->transform());
    } else {
        app.renderer().clearModel();
    }
    const unsigned tex =
        app.renderer().render(pw, ph, app.camera(), app.options().deterministic ? 0 : app.options().msaa_samples);
    if (tex) ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), avail, ImVec2(0, 1), ImVec2(1, 0));
    else ImGui::Dummy(avail);
    // Mouse: left drag turns, right/middle (or Shift + left) drag moves, wheel zooms.
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0) app.camera().zoom(io.MouseWheel);
        const bool pan = ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                         (io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
        if (pan) {
            app.camera().pan(io.MouseDelta.x, io.MouseDelta.y, avail.y);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            app.camera().orbit(io.MouseDelta.x, io.MouseDelta.y);
            app.viewChanged();
        }
    }
    const M4 vp = app.camera().proj(avail.x / std::max(1.0f, avail.y)) * app.camera().view();
    if (doc) {
        liftMarker(app, *doc, vp, origin, avail);
        axisGizmo(app, origin, avail);
    }
    // View presets.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 14, origin.y + 10));
    for (auto [label, v] : {std::pair{"Front", ViewPreset::Front}, std::pair{"Side", ViewPreset::Side},
                            std::pair{"Angled", ViewPreset::Angled}, std::pair{"From below", ViewPreset::Below}}) {
        if (segmentButton(label, app.viewIs(v))) app.setView(v);
        ImGui::SameLine(0, 6);
    }
    ImGui::NewLine();
    if (!doc && !app.jobs().busy() && !app.checking()) dropCard(app, origin, avail);
    taskTray(app, ImVec2(origin.x + avail.x, origin.y + avail.y));
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
    if (doc) {
        const bool fits = doc->fit().fits;
        problems = (fits ? 0 : 1) + (doc->check.closed() ? 0 : 1) + (doc->check.cavities ? 1 : 0);
        const ImVec2 wp = ImGui::GetWindowPos();
        dl->AddCircleFilled(ImVec2(wp.x + x + 4, wp.y + cy), 3.5f, fits ? col::Ok : col::Error);
        x += 14;
        const std::string msg = (fits ? "Fits the plate · " : "Too big for the plate · ") + dims(doc->fit().size);
        ImGui::SetCursorPos(ImVec2(x, cy - 8));
        text(msg.c_str(), fonts().regular, size::Body, col::TextSecondary);
    } else {
        ImGui::SetCursorPos(ImVec2(x, cy - 8));
        text("No model", fonts().regular, size::Body, col::TextSecondary);
    }
    ImGui::SameLine(0, 20);
    text(strf("Problems: %d", problems).c_str(), fonts().regular, size::Body,
         problems ? col::Warn : col::TextSecondary);
    ImGui::SameLine(0, 20);
    ImGui::SetCursorPosY(cy - 15);
    chip("orientation", "Image orientation unconfirmed", col::Warn, col::Warn, col::WarnBg, col::WarnBorder);
    tooltip("Until a printed test confirms which way the 16K shows images, text and left/right-handed parts could "
            "print mirrored. The check arrives with calibration.");
    endPanel();
}

}  // namespace

void job(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);  // structural panels are square
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    topBar(app);
    const float body_h = ImGui::GetContentRegionAvail().y - kStatusH;
    leftPanel(app, body_h);
    ImGui::SameLine(0, 0);
    const bool right = static_cast<bool>(app.document());
    viewport(app, ImVec2(ImGui::GetContentRegionAvail().x - (right ? kRightW : 0), body_h));
    if (right) {
        ImGui::SameLine(0, 0);
        rightPanel(app, body_h);
    }
    statusBar(app);
    ImGui::PopStyleVar(2);
}

}  // namespace gui::screens
