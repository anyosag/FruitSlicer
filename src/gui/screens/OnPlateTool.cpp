// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// On plate (canvas "On plate panel"), as Select Base in other slicers: any
// surface of the part, flat or curved, turned to face the plate, by a click in
// the 3D view (an orange arrow shows which way) or from its largest flat faces.
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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
// vertices. Kept for the last face drawn (`key` names it): it is the same one frame after frame.
const std::vector<std::array<int32_t, 2>>& outlineOf(const Part& part, const fruit::FlatFace& face, int key) {
    static int geometry = -1, kept_key = -1;
    static std::vector<std::array<int32_t, 2>> edges;
    if (geometry == part.geometryId() && kept_key == key) return edges;
    geometry = part.geometryId();
    kept_key = key;
    edges.clear();
    std::map<std::pair<int32_t, int32_t>, int> count;
    const fruit::TriangleMesh& m = part.mesh();
    for (const int32_t t : face.faces)
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
    groupLabel("Turn a surface to the plate");
    text("Click any surface of the model, flat or curved: it turns so that surface faces the plate. The orange "
         "arrow shows which way.",
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 8));
    const std::vector<RestingFace> faces = restingFaces(doc.part());
    text("Flat faces it can rest on", fonts().regular, size::Help, col::TextMuted);
    ImGui::Dummy(ImVec2(0, 2));
    if (faces.empty()) {
        text("None: no flat face has the whole model behind it. Clicking a surface still turns it to the plate.",
             fonts().regular, size::Body, col::TextSecondary, true);
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
        const std::string undo = " Supports need making again; undo (" + cmd("Z") + ") puts it back.";
        std::string line;
        switch (note->kind) {
            case SurfacePick::Kind::Flat:
                line = "Turned the " + name + " face to the plate. Other parts of the model reach lower, so it stands on "
                       "those." + undo;
                break;
            case SurfacePick::Kind::Curved: line = "Turned the spot you clicked to face the plate." + undo; break;
            default: line = "Laid the " + name + " face on the plate." + undo; break;
        }
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
    SurfacePick s;
    bool by_mouse = false;
    if (inside && !app.placing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        fruit::Vec3 o, d;
        rayThroughPixel(app.camera(), mouse.x, mouse.y, origin.x, origin.y, size.x, size.y, &o, &d);
        s = app.surfaceUnder(o, d);
        by_mouse = s.kind != SurfacePick::Kind::None;
    }
    const fruit::FlatFaces& flat = part.flatFaces();
    const fruit::Transform& tf = part.transform();
    if (s.kind == SurfacePick::Kind::None && app.hoverFace() >= 0 &&
        app.hoverFace() < static_cast<int>(flat.faces.size())) {  // a script lit one of the listed faces
        const fruit::FlatFace& f = flat.faces[static_cast<std::size_t>(app.hoverFace())];
        s = surfaceAt(part, f.faces.front(), tf.apply(f.centroid));
    }
    if (s.kind == SurfacePick::Kind::None && app.hoverPoint()) s = app.surfaceNear(*app.hoverPoint());
    if (s.kind == SurfacePick::Kind::None) return;

    const fruit::TriangleMesh& m = part.mesh();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = (col::Accent & 0x00FFFFFF) | (static_cast<ImU32>(0x61) << 24);
    auto at = [&](int32_t v, ImVec2* out) { return project(vp, origin, size, tf.apply(m.vertices[static_cast<std::size_t>(v)]), out); };
    const fruit::Vec3 n = printerDirection(part.placement(), s.normal);  // outward, printer directions
    const fruit::BBox3& box = part.box();
    const double diag = fruit::norm(box.max - box.min);
    if (s.kind == SurfacePick::Kind::Curved) {
        // A spot on a curved surface: a small disc lying on it.
        const double r = std::clamp(diag * 0.05, 1.5, 5.0);
        const fruit::Vec3 a = std::abs(n.z) < 0.9 ? fruit::Vec3{0, 0, 1} : fruit::Vec3{1, 0, 0};
        const fruit::Vec3 u = fruit::normalized(fruit::cross(n, a)), w = fruit::cross(n, u);
        std::vector<ImVec2> ring;
        for (int k = 0; k < 28; ++k) {
            const double t = 2 * 3.14159265358979 * k / 28;
            ImVec2 q;
            if (project(vp, origin, size, s.point + n * 0.05 + (u * std::cos(t) + w * std::sin(t)) * r, &q)) ring.push_back(q);
        }
        if (ring.size() == 28) {
            dl->AddConvexPolyFilled(ring.data(), static_cast<int>(ring.size()), fill);
            dl->AddPolyline(ring.data(), static_cast<int>(ring.size()), col::Accent, 2.0f, ImDrawFlags_Closed);
        }
    } else {
        const bool resting = s.kind == SurfacePick::Kind::Resting;
        const fruit::FlatFace& f = resting ? flat.faces[static_cast<std::size_t>(s.region)] : flat.others[static_cast<std::size_t>(s.region)];
        const ImDrawFlags saved = dl->Flags;
        dl->Flags &= ~ImDrawFlags_AAFill;  // no hairline seams between the triangles
        for (const int32_t t : f.faces) {
            ImVec2 a, b, c;
            const fruit::Face& tri = m.faces[static_cast<std::size_t>(t)];
            if (at(tri[0], &a) && at(tri[1], &b) && at(tri[2], &c)) dl->AddTriangleFilled(a, b, c, fill);
        }
        dl->Flags = saved;
        for (const auto& e : outlineOf(part, f, (resting ? 0 : 1 << 20) + s.region)) {
            ImVec2 a, b;
            if (at(e[0], &a) && at(e[1], &b)) dl->AddLine(a, b, col::Accent, 2.0f);
        }
    }
    // The arrow: from outside the surface into it, the way that surface will face (down, to the plate).
    const double len = std::clamp(diag * 0.4, 8.0, 30.0);
    ImVec2 tail, tip;
    if (project(vp, origin, size, s.point + n * len, &tail) && project(vp, origin, size, s.point, &tip)) {
        const float dx = tip.x - tail.x, dy = tip.y - tail.y, l = std::sqrt(dx * dx + dy * dy);
        if (l > 6) {
            const float ux = dx / l, uy = dy / l, head = std::min(14.0f, l * 0.45f);
            const ImVec2 base(tip.x - ux * head, tip.y - uy * head);
            dl->AddLine(tail, base, col::Panel, 6.0f);
            dl->AddLine(tail, base, col::Accent, 3.0f);
            const ImVec2 l1(base.x - uy * head * 0.55f, base.y + ux * head * 0.55f), r1(base.x + uy * head * 0.55f, base.y - ux * head * 0.55f);
            dl->AddTriangleFilled(tip, l1, r1, col::Accent);
            dl->AddTriangle(tip, l1, r1, col::Panel, 1.0f);
        }
    }
    if (by_mouse) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // The pill beside it: by the mouse, or by the arrow's tail when a script lit it.
    ImVec2 anchor;
    if (by_mouse) anchor = ImVec2(mouse.x + 18, mouse.y + 18);
    else if (project(vp, origin, size, s.point + n * len, &anchor)) anchor = ImVec2(anchor.x + 16, anchor.y - 20);
    else return;
    std::string head;
    const char* sub = "";
    switch (s.kind) {
        case SurfacePick::Kind::Resting:
            head = s.name + " · " + areaLine(s.area);
            sub = s.facing_down ? "On the plate now" : "Click to lay it on the plate";
            break;
        case SurfacePick::Kind::Flat:
            head = s.name + " · " + areaLine(s.area);
            sub = s.facing_down ? "Faces the plate now" : "Click to turn it to face the plate";
            break;
        default:
            head = "Curved surface";
            sub = s.facing_down ? "Faces the plate here now" : "Click to turn this spot to face the plate";
            break;
    }
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
