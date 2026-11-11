// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// On plate (canvas "On plate panel"): the part's largest flat faces, to lay
// one on the plate, and the face under the mouse lit in the 3D view.
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>

#include "App.h"
#include "Format.h"
#include "LayFace.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"
#include "view3d/Math.h"

namespace gui::screens::parts {

namespace {

constexpr std::size_t kRows = 5;

std::string areaLine(double area) { return strf("%.0f mm², flat", area); }

// One face as a button: its name and size, and what a click does.
bool faceRow(int index, const RestingFace& f) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 52;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(index);
    const bool clicked = ImGui::InvisibleButton("face", ImVec2(w, h));
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, col::Card, 10);
    dl->AddRect(p, q, hovered ? col::Accent : col::Border, 10, hovered ? 1.5f : 1.0f);
    ImGui::PushFont(fonts().semibold, size::Body);
    dl->AddText(ImVec2(p.x + 12, p.y + 9), col::Text, f.name.c_str());
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Help);
    dl->AddText(ImVec2(p.x + 12, p.y + 29), col::TextMuted, areaLine(f.area).c_str());
    ImGui::PopFont();
    const char* tag = f.on_plate ? "On the plate" : "Lay it here";
    ImGui::PushFont(fonts().semibold, size::Help);
    const ImVec2 ts = ImGui::CalcTextSize(tag);
    dl->AddText(ImVec2(q.x - 12 - ts.x, p.y + (h - ts.y) / 2), f.on_plate ? col::Ok : col::Link, tag);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

bool project(const float* vp, ImVec2 origin, ImVec2 size, const fruit::Vec3& w, ImVec2* out) {
    const float x = vp[0] * w.x + vp[4] * w.y + vp[8] * w.z + vp[12];
    const float y = vp[1] * w.x + vp[5] * w.y + vp[9] * w.z + vp[13];
    const float c = vp[3] * w.x + vp[7] * w.y + vp[11] * w.z + vp[15];
    if (c <= 1e-4f) return false;
    *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
    return true;
}

// The edges of a flat face that only one of its triangles has (its outline), as pairs of the mesh's
// vertices. Kept for the last face drawn: it is the same one frame after frame.
const std::vector<std::array<int32_t, 2>>& outlineOf(const Part& part, int region) {
    static int geometry = -1, kept_region = -1;
    static std::vector<std::array<int32_t, 2>> edges;
    if (geometry == part.geometryId() && kept_region == region) return edges;
    geometry = part.geometryId();
    kept_region = region;
    edges.clear();
    std::map<std::pair<int32_t, int32_t>, int> count;
    const fruit::TriangleMesh& m = part.mesh();
    for (const int32_t t : part.flatFaces().faces[static_cast<std::size_t>(region)].faces)
        for (int k = 0; k < 3; ++k) {
            const int32_t a = m.faces[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)];
            const int32_t b = m.faces[static_cast<std::size_t>(t)][static_cast<std::size_t>((k + 1) % 3)];
            ++count[{std::min(a, b), std::max(a, b)}];
        }
    for (const auto& [e, n] : count)
        if (n == 1) edges.push_back({e.first, e.second});
    return edges;
}

}  // namespace

void onPlateBody(App& app, Document& doc) {
    groupLabel("Lay a face on the plate");
    text("Click a flat face in the view, or pick one here: the model turns so that face lies on the plate.",
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 8));
    const std::vector<RestingFace> faces = restingFaces(doc.part());
    text("Its largest flat faces", fonts().regular, size::Help, col::TextMuted);
    ImGui::Dummy(ImVec2(0, 2));
    if (faces.empty()) {
        text("This model has no flat face to stand it on. Rotate can still turn it.", fonts().regular, size::Body,
             col::TextSecondary, true);
    }
    int lay = -1;
    for (std::size_t i = 0; i < faces.size() && i < kRows; ++i) {
        if (faceRow(static_cast<int>(i), faces[i])) lay = faces[i].region;
        ImGui::Dummy(ImVec2(0, 2));
    }
    // After the loop: laying one changes what the rows are.
    if (lay >= 0) app.layFace(lay);
    if (const App::LaidNote* note = app.laidNote()) {
        ImGui::Dummy(ImVec2(0, 6));
        beginCard("laid", ImVec2(0, 0), 12, col::OkBg, IM_COL32(0x2F, 0x5A, 0x43, 255));
        std::string name = note->name;
        if (!name.empty()) name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
        const std::string line =
            "Laid the " + name + " face on the plate. Supports need making again; undo (" + cmd("Z") + ") puts it back.";
        text(line.c_str(), fonts().regular, size::Body, IM_COL32(0xCD, 0xEB, 0xDB, 255), true);
        endCard();
    }
    ImGui::Dummy(ImVec2(0, 8));
    text("Supports then go under the faces that point down. For a way to stand it with fewer of them, Rotate › Find a "
         "good rotation.",
         fonts().regular, size::Help, col::TextMuted, true);
}

void onPlateOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* vp) {
    const Part& part = doc.part();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= origin.x && mouse.y >= origin.y + 44 && mouse.x < origin.x + size.x &&
                        mouse.y < origin.y + size.y && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
    int region = -1;
    bool by_mouse = false;
    if (inside && !app.placing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        fruit::Vec3 o, d;
        rayThroughPixel(app.camera(), mouse.x, mouse.y, origin.x, origin.y, size.x, size.y, &o, &d);
        region = app.pickRestingFace(o, d);
        by_mouse = region >= 0;
    }
    if (region < 0 && app.hoverFace() >= 0) region = app.hoverFace();
    if (region < 0 || region >= static_cast<int>(part.flatFaces().faces.size())) return;

    const std::vector<RestingFace> faces = restingFaces(part);
    const RestingFace& info = faces[static_cast<std::size_t>(region)];
    const fruit::FlatFace& flat = part.flatFaces().faces[static_cast<std::size_t>(region)];
    const fruit::TriangleMesh& m = part.mesh();
    const fruit::Transform& tf = part.transform();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = (col::Accent & 0x00FFFFFF) | (static_cast<ImU32>(0x61) << 24);
    auto at = [&](int32_t v, ImVec2* out) { return project(vp, origin, size, tf.apply(m.vertices[static_cast<std::size_t>(v)]), out); };
    const ImDrawFlags saved = dl->Flags;
    dl->Flags &= ~ImDrawFlags_AAFill;  // no hairline seams between the triangles
    for (const int32_t t : flat.faces) {
        ImVec2 a, b, c;
        const fruit::Face& f = m.faces[static_cast<std::size_t>(t)];
        if (at(f[0], &a) && at(f[1], &b) && at(f[2], &c)) dl->AddTriangleFilled(a, b, c, fill);
    }
    dl->Flags = saved;
    for (const auto& e : outlineOf(part, region)) {
        ImVec2 a, b;
        if (at(e[0], &a) && at(e[1], &b)) dl->AddLine(a, b, col::Accent, 2.0f);
    }
    if (by_mouse) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // The pill beside it: by the mouse, or by the face's middle when a script lit it.
    ImVec2 anchor;
    if (by_mouse) anchor = ImVec2(mouse.x + 18, mouse.y + 18);
    else if (project(vp, origin, size, tf.apply(flat.centroid), &anchor)) anchor = ImVec2(anchor.x + 24, anchor.y - 20);
    else return;
    const std::string head = info.name + " · " + areaLine(info.area);
    const char* sub = info.on_plate ? "On the plate now" : "Click to lay it on the plate";
    ImGui::PushFont(fonts().semibold, size::Help);
    const ImVec2 hs = ImGui::CalcTextSize(head.c_str());
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Help);
    const ImVec2 ss = ImGui::CalcTextSize(sub);
    ImGui::PopFont();
    const float w = std::max(hs.x, ss.x) + 24, h = hs.y + ss.y + 18;
    ImVec2 p(std::min(anchor.x, origin.x + size.x - w - 8), std::min(anchor.y, origin.y + size.y - h - 8));
    p.x = std::max(p.x, origin.x + 8);
    p.y = std::max(p.y, origin.y + 8);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col::Panel, 10);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), col::Accent, 10);
    ImGui::PushFont(fonts().semibold, size::Help);
    dl->AddText(ImVec2(p.x + 12, p.y + 8), col::Text, head.c_str());
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Help);
    dl->AddText(ImVec2(p.x + 12, p.y + 10 + hs.y), col::TextMuted, sub);
    ImGui::PopFont();
}

}  // namespace gui::screens::parts
