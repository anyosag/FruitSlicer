// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Prepare's tools around the 3D view (canvas "FruitSlicer: tools around the
// 3D view"): the tool rail at the view's left, the open tool's panel beside
// it, the model tools along its foot, the view cube at its top right, the
// pills along its top, and Orient's results over it. The Job panel at the
// window's right is in JobPanel.cpp.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Repairs.h"
#include "Theme.h"
#include "ViewCube.h"
#include "Widgets.h"
#include "mesh/ModelCheck.h"
#include "screens/JobParts.h"
#include "view3d/Math.h"

namespace gui::screens::parts {

namespace {

// ---- A floating panel's title row, with a small close button ----

bool floatTitle(const char* title) {
    const float y = ImGui::GetCursorPosY();
    text(title, fonts().semibold, size::Section, col::Text);
    const float x = ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - 26;
    ImGui::SetCursorPos(ImVec2(x, y - 4));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("close", ImVec2(26, 26));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(p, ImVec2(p.x + 26, p.y + 26), col::Active, 7);
    icon::cross(dl, ImVec2(p.x + 7, p.y + 7), 12, hovered ? col::Text : col::TextMuted);
    tooltip("Close the panel");
    ImGui::SetCursorPosY(y + 28);
    return clicked;
}

// ---- The model's own tools: On plate, Move, Rotate's fields, Scale, Repair ----

void onPlateBody(App& app, Document& doc) {
    groupLabel("How it sits on the plate");
    const fruit::Placement& p = doc.placement();
    const std::string raised = strf("%.0f mm above a 1 mm raft that comes off in one piece. Best for most models.",
                                    fruit::Placement{}.elevation);
    if (radioCard("raised", "Raised on supports", raised.c_str(), !p.on_plate)) app.setOnPlate(false);
    ImGui::Dummy(ImVec2(0, 2));
    if (radioCard("flat", "Flat on the plate", "For flat-backed parts. No raft; only overhangs get tips.", p.on_plate))
        app.setOnPlate(true);
}

// The plate seen from above, to scale, with the model's outline (red while
// it doesn't fit) and the dashed line it must stay inside.
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
    const fruit::BBox3 b = doc.placedBox();
    const ImVec2 a = map(b.min.x, b.max.y), c = map(b.max.x, b.min.y);
    dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    dl->AddRectFilled(a, c, doc.fit().fits ? IM_COL32(0xA9, 0xB1, 0xBE, 255) : IM_COL32(0xD8, 0x6A, 0x6A, 255), 2);
    dashedRect(dl, ImVec2(p.x + w / 2, p.y), ImVec2(p.x + w / 2, p.y + h), IM_COL32(0x5A, 0x62, 0x70, 255), 1, 4, 4);
    dashedRect(dl, ImVec2(p.x, p.y + h / 2), ImVec2(p.x + w, p.y + h / 2), IM_COL32(0x5A, 0x62, 0x70, 255), 1, 4, 4);
    const double m = fruit::plateMargin(doc.placement());
    dashedRect(dl, map(-pr.build_x / 2 + m, pr.build_y / 2 - m), map(pr.build_x / 2 - m, -pr.build_y / 2 + m),
               doc.fit().fits ? IM_COL32(0x8A, 0x92, 0xA0, 255) : IM_COL32(0xD8, 0x6A, 0x6A, 255), 1, 5, 4);
    dl->PopClipRect();
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), doc.fit().fits ? col::Border : IM_COL32(0x6B, 0x2A, 0x2A, 255), 8);
}

// Why it doesn't fit where it is, and the fix, as a red card.
void fitCard(App& app, const Document& doc) {
    const fruit::PlateFit& f = doc.fit();
    if (f.fits) return;
    beginCard("fit", ImVec2(0, 0), 12, col::ErrorBg, IM_COL32(0x6B, 0x2A, 0x2A, 255));
    const char* why = f.overEdge()     ? "Over the plate's edge where it is."
                      : f.fits_turned ? "Too big for the plate as it lies. Turned 90° it fits at this size."
                                      : "Too big for the plate at this size, however it is turned.";
    text(why, fonts().regular, size::Body, col::Text, true);
    ImGui::Dummy(ImVec2(0, 2));
    if (f.overEdge()) {
        if (primaryButton("Move it back on", ImVec2(0, 34))) app.slideOntoPlate();
    } else if (f.fits_turned) {
        if (primaryButton("Turn 90°", ImVec2(0, 34))) app.turnQuarter();
    } else if (primaryButton("Scale to fit", ImVec2(0, 34))) {
        app.scaleToFit();
    }
    endCard();
    ImGui::Dummy(ImVec2(0, 4));
}

// Number fields side by side, each under its label; true when one changed.
bool fieldsRow(const char* const* labels, double* const* values, int n, double lo, double hi) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float fw = (w - 8 * static_cast<float>(n - 1)) / static_cast<float>(n);
    const float x0 = ImGui::GetCursorPosX(), y = ImGui::GetCursorPosY();
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        ImGui::SetCursorPos(ImVec2(x0 + static_cast<float>(i) * (fw + 8), y));
        text(labels[i], fonts().regular, size::Help, col::TextMuted);
        ImGui::SetCursorPos(ImVec2(x0 + static_cast<float>(i) * (fw + 8), y + 20));
        if (numberField(labels[i], values[i], fw, "%g", lo, hi)) changed = true;
    }
    ImGui::SetCursorPos(ImVec2(x0, y + 20 + 34 + 8));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

void moveBody(App& app, Document& doc) {
    const fruit::Placement& p = doc.placement();
    const fruit::PlateFit& f = doc.fit();
    fitCard(app, doc);
    groupLabel("Position on the plate");
    text("Its middle, from the plate's centre. Drag the arrows in the view, or use the arrow keys.", fonts().regular,
         size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 2));
    fruit::Vec2 o = p.offset;
    const char* labels[] = {"X mm", "Y mm"};
    double* values[] = {&o.x, &o.y};
    if (fieldsRow(labels, values, 2, -1000, 1000)) app.setPosition(o);
    if (secondaryButton("Centre it", ImVec2(0, 32), !p.centred())) app.centreModel();
    ImGui::Dummy(ImVec2(0, 4));
    plateMap(app, doc);
    text("Top view, to scale. It must stay inside the dashed line: the raft needs room round it.", fonts().regular,
         size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 2));
    keyValue("Footprint", dims2(f.size.x, f.size.y));
    keyValue("Height with lift", strf("%.1f of %.0f mm", f.top, app.printer().build_z));
}

void rotateBody(App& app, Document& doc) {
    const fruit::Placement& p = doc.placement();
    groupLabel("Turn it yourself");
    fruit::Vec3 r = p.rotation_deg;
    const char* labels[] = {"X °", "Y °", "Z °"};
    double* values[] = {&r.x, &r.y, &r.z};
    if (fieldsRow(labels, values, 3, -360, 360)) app.setRotation(r);
    if (secondaryButton("Turn 90°", ImVec2(0, 32))) app.turnQuarter();
    text("A quarter turn about the vertical. Or drag the rings in the view; hold ⇧ for 15° steps.", fonts().regular,
         size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 10));
    orientPanel(app, doc);
}

void scaleBody(App& app, Document& doc) {
    const fruit::Placement& p = doc.placement();
    const fruit::PlateFit& f = doc.fit();
    fitCard(app, doc);
    groupLabel("Size");
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + 8);
        text("Scale", fonts().regular, size::Body, col::TextSecondary);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - 108, y));
        double pct = p.scale * 100;
        if (numberField("scale", &pct, 90, "%g", 1, 100000)) app.setScalePercent(pct);
        ImGui::SameLine(0, 6);
        text("%", fonts().regular, size::Body, col::TextSecondary);
    }
    text(dims(f.size).c_str(), fonts().regular, size::Body, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    const double best = std::max(f.largest, f.largest_turned);
    const std::string label = "Largest that fits (" + percent(best) + (f.largest_turned > f.largest ? ", turned)" : ")");
    if (secondaryButton(label.c_str(), ImVec2(0, 32))) app.scaleToFit();
    const fruit::PrinterProfile& pr = app.printer();
    text(strf("The plate is %.2f × %.2f mm and %.0f mm tall.", pr.build_x, pr.build_y, pr.build_z).c_str(),
         fonts().regular, size::Help, col::TextMuted, true);
}

void repairBody(App& app, const Document& doc) {
    const fruit::MeshCheck& c = doc.check;
    const RepairWords repair = repairWords(c);
    using Level = RepairWords::Level;
    const bool repaired = repair.level == Level::Repaired || repair.level == Level::Check;
    const bool open = repair.level == Level::Open;
    const char* title = repair.level == Level::Check ? "Mesh repaired: check the patch"
                        : repaired                  ? "Mesh repaired"
                        : open                      ? "Not watertight"
                                                    : "Mesh is clean";
    std::string body;
    if (repaired || open) {
        body = repair.brief;
    } else {
        body = (c.shells == 1 ? std::string("One closed shell") : std::to_string(c.shells) + " closed shells") +
               (c.flipped ? strf(", %d turned outward", c.flipped) : std::string(", nothing flipped")) + ".";
    }
    const bool warn = open || repair.level == Level::Check;
    beginCard("health", ImVec2(0, 0), 13, warn ? col::WarnBg : col::Card, warn ? col::WarnBorder : col::Border);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q = ImGui::GetCursorScreenPos();
    if (!warn) icon::check(dl, ImVec2(q.x, q.y + 2), 14, col::Ok);
    else text("!", fonts().bold, size::Body, col::Warn);
    ImGui::SetCursorScreenPos(ImVec2(q.x + 22, q.y));
    ImGui::BeginGroup();
    text(title, fonts().semibold, size::Body, col::Text);
    text(body.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::EndGroup();
    endCard();
    if (c.repair.patch_faces > 0) {
        ImGui::Dummy(ImVec2(0, 2));
        text("The patches are tinted on the model.", fonts().regular, size::Help, col::TextMuted, true);
        if (secondaryButton(c.repair.holes_filled + c.repair.gaps_closed > 1 ? "Show the biggest patch" : "Show the patch",
                            ImVec2(0, 32)))
            app.showPatch();
    }
    ImGui::Dummy(ImVec2(0, 4));
    text("FruitSlicer checks the mesh when the model opens, and repairs holes and inside-out parts.", fonts().regular,
         size::Help, col::TextMuted, true);
}

// ---- The tool rail, and the tip tools while editing ----

constexpr float kRailCardW = 80;

void railCards(const char* top_id, const std::function<void()>& top, const char* tools_id,
               const std::function<void()>& tools, ImVec2 origin) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    beginFloat(top_id, ImVec2(origin.x + kRailX + 8, origin.y + 16), kRailCardW, 0, ImVec2(6, 6));
    top();
    endFloat();
    beginFloat(tools_id, ImVec2(origin.x + kRailX + 8, origin.y + 16 + 72 + 10), kRailCardW, 0, ImVec2(6, 6));
    tools();
    endFloat();
    ImGui::PopStyleVar();
}

void toolRail(App& app, Document* doc, ImVec2 origin) {
    const Tool open = doc && app.toolPanelOpen() ? app.tool() : Tool::None;
    railCards(
        "rail_open",
        [&] {
            if (toolButton("open", "Open", icon::folder, false)) app.openDialog();
            tooltip(doc ? "Open a model in place of this one, or a job." : "Open a model or a job.");
        },
        "rail_tools",
        [&] {
            struct T {
                Tool t;
                const char* id;
                const char* label;
                IconFn ic;
                const char* tip;
            };
            const T tools[] = {{Tool::OnPlate, "on_plate", "On plate", icon::onPlate, "How it sits on the plate (P)"},
                               {Tool::Move, "move", "Move", icon::move, "Move it on the plate (M)"},
                               {Tool::Rotate, "rotate", "Rotate", icon::rotate, "Turn it, or find a good rotation (R)"},
                               {Tool::Scale, "scale", "Scale", icon::scale, "Change its size (S)"}};
            for (const T& t : tools) {
                if (toolButton(t.id, t.label, t.ic, open == t.t, doc != nullptr)) app.openTool(t.t);
                tooltip(doc ? t.tip : "Open a model first.");
            }
        },
        origin);
}

void tipRail(App& app, ImVec2 origin) {
    railCards(
        "rail_done",
        [&] {
            if (toolButton("done", "Done", icon::back, false)) app.editTips(false);
            tooltip("Finish editing tips");
        },
        "rail_tips",
        [&] {
            struct T {
                App::TipTool t;
                const char* id;
                const char* label;
                IconFn ic;
                const char* key;
            };
            const T tools[] = {{App::TipTool::Select, "select", "Select", icon::cursor, "V"},
                               {App::TipTool::Add, "add", "Add tips", icon::addTip, "A"},
                               {App::TipTool::Zone, "zone", "No-tip zone", icon::zone, "Z"}};
            for (const T& t : tools)
                if (toolButton(t.id, t.label, t.ic, app.tipTool() == t.t, true, t.key)) app.tipTool() = t.t;
        },
        origin);
}

// ---- The open tool's panel ----

const char* toolTitle(Tool t) {
    switch (t) {
        case Tool::OnPlate: return "On plate";
        case Tool::Move: return "Move";
        case Tool::Rotate: return "Rotate";
        case Tool::Scale: return "Scale";
        case Tool::Supports: return "Supports";
        case Tool::Hollow: return "Hollow";
        case Tool::Repair: return "Repair";
        case Tool::None: break;
    }
    return "";
}

void toolPanel(App& app, Document& doc, ImVec2 origin, ImVec2 size) {
    const ImVec2 pos(origin.x + kToolPanelX, origin.y + 16);
    if (app.editingTips()) {
        beginFloat("panel_tips", pos, kToolPanelW, size.y - 32);
        text("Editing tips", fonts().semibold, size::Section, col::Text);
        tipEditPanel(app, doc);
        endFloat();
        return;
    }
    if (!app.toolPanelOpen()) return;
    const Tool t = app.tool();
    ImGui::PushID(static_cast<int>(t));
    // The title, then what the tool shows (it scrolls when taller than the
    // view allows), then a footer that stays in sight: Hollow's apply button.
    const bool footer = t == Tool::Hollow;
    beginFloat("panel", pos, kToolPanelW);
    if (floatTitle(toolTitle(t))) app.closeToolPanel();
    const float body_max = size.y - 32 - kToolsBarH - 32 - 36 - (footer ? 96 : 0);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(4000, std::max(120.0f, body_max)));
    ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
    switch (t) {
        case Tool::OnPlate: onPlateBody(app, doc); break;
        case Tool::Move: moveBody(app, doc); break;
        case Tool::Rotate: rotateBody(app, doc); break;
        case Tool::Scale: scaleBody(app, doc); break;
        case Tool::Supports: supportsPanel(app, doc); break;
        case Tool::Hollow: hollowPanel(app, doc); break;
        case Tool::Repair: repairBody(app, doc); break;
        case Tool::None: break;
    }
    ImGui::EndChild();
    if (footer) {
        ImGui::Dummy(ImVec2(0, 2));
        hollowApplyArea(app, doc);
    }
    endFloat();
    ImGui::PopID();
}

// ---- The model tools, along the view's foot ----

void modelTools(App& app, Document* doc, ImVec2 origin, ImVec2 size) {
    if (app.editingTips()) return;
    const Tool open = doc && app.toolPanelOpen() ? app.tool() : Tool::None;
    const float w = 4 * 68 + 3 * 2 + 2 * 8 + 2;
    const ImVec2 pos(origin.x + std::round((size.x - w) / 2), origin.y + size.y - 16 - 72);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
    beginFloat("model_tools", pos, w, 0, ImVec2(8, 6));
    const bool on = doc != nullptr;
    if (toolButton("supports", "Supports", icon::supports, open == Tool::Supports, on)) app.openTool(Tool::Supports);
    tooltip(on ? "Make supports, or change them (T)" : "Open a model first.");
    ImGui::SameLine();
    if (toolButton("hollow", "Hollow", icon::hollow, open == Tool::Hollow, on)) app.openTool(Tool::Hollow);
    tooltip(on ? "Hollow it, and fix hollows that trap resin (H)" : "Open a model first.");
    ImGui::SameLine();
    if (toolButton("holes", "Drain holes", icon::drainHole, open == Tool::Hollow, on)) app.openTool(Tool::Hollow);
    tooltip(on ? "Holes that let resin drain and air in: with Hollow (D)" : "Open a model first.");
    ImGui::SameLine();
    if (toolButton("repair", "Repair", icon::repair, open == Tool::Repair, on)) app.openTool(Tool::Repair);
    tooltip(on ? "What the mesh check found and mended" : "Open a model first.");
    endFloat();
    ImGui::PopStyleVar();
}

// ---- The view cube, with Home and From below under it ----

void viewCube(App& app, ImVec2 origin, ImVec2 size) {
    const float half = 30;
    const ImVec2 c(origin.x + size.x - 16 - kCubeW / 2, origin.y + 16 + 62);
    const std::vector<CubeFace> faces = cubeFaces(app.camera().view(), half);
    // A button over the cube, so a click there turns the view and nothing else.
    ImGui::SetCursorScreenPos(ImVec2(c.x - 56, c.y - 56));
    const bool clicked = ImGui::InvisibleButton("view_cube", ImVec2(112, 112));
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int under = ImGui::IsItemHovered() ? cubeFaceAt(faces, m.x - c.x, m.y - c.y) : -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const CubeFace& f = faces[i];
        ImVec2 pts[4];
        for (int k = 0; k < 4; ++k) pts[k] = ImVec2(c.x + f.x[k], c.y + f.y[k]);
        const float t = std::clamp(f.facing, 0.0f, 1.0f);
        const auto mix = [t](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
        const ImU32 fill = static_cast<int>(i) == under ? IM_COL32(0x4A, 0x52, 0x62, 245)
                                                         : IM_COL32(mix(0x22, 0x3B), mix(0x26, 0x41), mix(0x2E, 0x4D), 240);
        dl->AddConvexPolyFilled(pts, 4, fill);
        dl->AddPolyline(pts, 4, IM_COL32(0x4E, 0x56, 0x66, 255), 1.0f, ImDrawFlags_Closed);
        if (f.facing > 0.3f) {
            const char* label = cubeLabel(f.side);
            ImGui::PushFont(fonts().semibold, f.facing > 0.55f ? size::Label + 0.5f : size::Label - 1);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const ImVec2 mid((pts[0].x + pts[1].x + pts[2].x + pts[3].x) / 4, (pts[0].y + pts[1].y + pts[2].y + pts[3].y) / 4);
            dl->AddText(ImVec2(mid.x - ts.x / 2, mid.y - ts.y / 2), f.facing > 0.55f ? col::Text : col::TextSecondary, label);
            ImGui::PopFont();
        }
    }
    if (under >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        tooltip((std::string("Look from the ") + cubeLabel(faces[static_cast<std::size_t>(under)].side)).c_str());
    }
    if (clicked && under >= 0 && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 9) {
        const CubeSide s = faces[static_cast<std::size_t>(under)].side;
        if (s == CubeSide::Front) app.setView(ViewPreset::Front);
        else if (s == CubeSide::Right) app.setView(ViewPreset::Side);
        else {
            const V3 d = cubeDirection(s);
            app.camera().lookFrom(d);
            app.viewChanged();
        }
    }
    // Home (the start view) and From below, under the cube.
    const float y = c.y + 62;
    ImGui::SetCursorScreenPos(ImVec2(c.x - 64, y));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("home_view", ImVec2(30, 30))) app.setView(ViewPreset::Angled);
        const bool h = ImGui::IsItemHovered();
        dl->AddCircleFilled(ImVec2(p.x + 15, p.y + 15), 15, h ? col::Active : IM_COL32(0x18, 0x1B, 0x21, 240));
        dl->AddCircle(ImVec2(p.x + 15, p.y + 15), 15, col::BorderStrong, 0, 1.0f);
        icon::home(dl, ImVec2(p.x + 7, p.y + 7), 16, app.viewIs(ViewPreset::Angled) ? col::Accent : col::TextSecondary);
        if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        tooltip("Back to the start view");
    }
    ImGui::SetCursorScreenPos(ImVec2(c.x - 28, y));
    {
        const char* label = "From below";
        ImGui::PushFont(fonts().medium, size::Help);
        const float tw = ImGui::CalcTextSize(label).x;
        ImGui::PopFont();
        const ImVec2 sz(tw + 38, 30);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("below_view", sz)) app.setView(ViewPreset::Below);
        const bool h = ImGui::IsItemHovered();
        const bool on = app.viewIs(ViewPreset::Below);
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), h ? col::Active : IM_COL32(0x18, 0x1B, 0x21, 240), 15);
        dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), on ? col::Accent : col::BorderStrong, 15, 1.0f);
        icon::lookUp(dl, ImVec2(p.x + 9, p.y + 7), 16, on ? col::Accent : col::TextSecondary);
        ImGui::PushFont(fonts().medium, size::Help);
        dl->AddText(ImVec2(p.x + 29, p.y + 8), on ? col::Accent : col::TextSecondary, label);
        ImGui::PopFont();
        if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        tooltip("Look up at the underside, where the tips go");
    }
}

// ---- Along the view's top: what it shows, and a line of help ----

void topPill(App& app, Document* doc, ImVec2 origin, ImVec2 size) {
    if (!doc) return;
    const bool supports = app.step() == Step::Supports && !app.editingTips() && doc->supports.result != nullptr;
    const bool hollow = app.step() == Step::Orient && app.orientView() == OrientView::Hollow;
    const bool patched = app.step() == Step::Model && doc->check.repair.patch_faces > 0;
    const char* hint = nullptr;
    if (app.editingTips()) {
        hint = app.tipTool() == App::TipTool::Add    ? "Click the model to add a tip · [ and ] change the selected tip's size"
               : app.tipTool() == App::TipTool::Zone ? "Click the model to keep tips away from there"
                                                     : "Click a tip to select it · drag it to move it · Delete removes it · "
                                                       "[ and ] change its size";
    } else if (app.step() == Step::Supports && !doc->supports.result) {
        hint = "Faces that need tips are tinted: they lean more than 45° from upright.";
    } else if (hollow && !doc->hollow.choices.auto_holes) {
        hint = "Click on the model to put a hole there.";
    }
    if (!supports && !hollow && !patched && !hint) return;
    const bool panel = app.toolPanelOpen() || app.editingTips();
    const float left = origin.x + (panel ? kToolPanelRight : kToolPanelX) + 16;
    const float right = origin.x + size.x - kCubeW - 24;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::max(200.0f, right - left), size.y));
    beginFloat("top_pill", ImVec2(left, origin.y + 16), 0, 0, ImVec2(8, 6));
    if (supports) supportsViewPill(app, *doc);
    if (hollow) {
        if (segmentButton("Cut in half", app.cutAway())) app.setCutAway(true);
        ImGui::SameLine(0, 4);
        if (segmentButton("Whole model", !app.cutAway())) app.setCutAway(false);
    }
    if (patched && !supports && !hollow) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 6, p.y + 9), 4, col::Warn);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 16, p.y + 1));
        text("Patched by FruitSlicer", fonts().regular, size::Help, col::TextSecondary);
    }
    if (hint) {
        if (supports || hollow) ImGui::Dummy(ImVec2(0, 2));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(200.0f, right - left - 20));
        text(hint, fonts().regular, size::Help, col::TextSecondary, true);
        ImGui::PopTextWrapPos();
    }
    endFloat();
}

// ---- Orient's results, over the view ----

void resultsSheet(App& app, Document& doc, ImVec2 origin, ImVec2 size) {
    const ImVec2 pos(origin.x + kToolPanelX, origin.y + 16);
    const ImVec2 sz(size.x - kToolPanelX - 16, size.y - 32);
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::BeginChild("orient_sheet", sz, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    orientResults(app, doc, ImVec2(0, sz.y - 16));
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

}  // namespace

void workspaceOverlay(App& app, Document* doc, ImVec2 origin, ImVec2 size, const float* vp) {
    // The place's own marks in the view first, under the floating tools.
    if (doc) {
        Document& d = *doc;
        switch (app.step()) {
            case Step::Model:
                if (app.handlesShown()) placeOverlay(app, d, origin, size, vp);
                break;
            case Step::Orient:
                if (app.orientView() == OrientView::Hollow) hollowOverlay(app, d, origin, size, vp);
                else if (app.orientView() == OrientView::Settings) placeOverlay(app, d, origin, size, vp);
                break;
            case Step::Supports:
                if (app.editingTips()) tipEditOverlay(app, d, origin, size, vp);
                else supportsOverlay(app, d, origin, size, vp);
                break;
            case Step::Layers:
            case Step::Print: break;
        }
    }
    viewCube(app, origin, size);
    topPill(app, doc, origin, size);
    if (doc && app.editingTips()) tipRail(app, origin);
    else toolRail(app, doc, origin);
    if (doc) toolPanel(app, *doc, origin, size);
    modelTools(app, doc, origin, size);
    if (doc && app.step() == Step::Orient && app.orientView() == OrientView::Results && doc->orient.results)
        resultsSheet(app, *doc, origin, size);
}

}  // namespace gui::screens::parts
