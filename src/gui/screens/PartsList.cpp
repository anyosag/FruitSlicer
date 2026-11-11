// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Job panel's Parts section: every part on the plate and off it, each
// with its size; a click selects one (the tools act on it), the cross
// removes one, and "Add a part…" puts another model beside them.
#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"
#include "view3d/Camera.h"

namespace gui::screens::parts {

namespace {

constexpr float kRowH = 44;
constexpr int kRowsShown = 4;  // more than this and the list scrolls

// The fix for a part that doesn't fit, as the Move and Scale tools offer it.
const char* fixLabel(const fruit::PlateFit& f) {
    return f.overEdge() ? "Move it back on" : f.fits_turned ? "Turn 90°" : "Scale to fit";
}

void applyFix(App& app, const Part& part) {
    const fruit::PlateFit f = part.fit();
    app.selectPart(part.id());  // the fixes act on the selected part
    if (f.overEdge()) app.slideOntoPlate();
    else if (f.fits_turned) app.turnQuarter();
    else app.scaleToFit();
}

// One part on the plate: a click selects it; a double-click or a right-click
// opens its menu (Duplicate…); the cross (with several) removes it.
void partRow(App& app, const Document& doc, const Part& part) {
    ImGui::PushID(part.id());
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool several = doc.partCount() > 1;
    const bool selected = part.id() == doc.selectedId();
    const float cross_w = several ? 28 : 0;
    const bool clicked = ImGui::InvisibleButton("select", ImVec2(w - cross_w, kRowH));
    const bool hovered = ImGui::IsItemHovered();
    tooltip(several ? (selected ? "Selected: the tools act on it. Double-click for its menu (Duplicate…)."
                                : "Select it: the tools act on the selected part. Double-click for its menu.")
                    : "Double-click for its menu: Duplicate, more copies.");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + kRowH), selected ? col::Active : col::Card, 10);
    if (selected && several) dl->AddRect(p, ImVec2(p.x + w, p.y + kRowH), col::BorderStrong, 10);
    icon::cube(dl, ImVec2(p.x + 10, p.y + 12), 20, selected ? col::TextSecondary : col::TextMuted);
    ImGui::PushFont(fonts().medium, size::Body);
    dl->PushClipRect(p, ImVec2(p.x + w - cross_w - 4, p.y + kRowH), true);
    dl->AddText(ImVec2(p.x + 40, p.y + 6), col::Text, part.name().c_str());
    dl->PopClipRect();
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Help);
    dl->AddText(ImVec2(p.x + 40, p.y + 24), col::TextMuted, dims(part.fit().size).c_str());
    ImGui::PopFont();
    if (clicked) app.selectPart(part.id());
    if (hovered && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        app.openPartMenu(part.id(), m.x, m.y);
    }
    if (several) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - 26, p.y + (kRowH - 22) / 2));
        const ImVec2 q = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("remove", ImVec2(22, 22))) app.removePart(part.id());
        const bool h = ImGui::IsItemHovered();
        if (h) dl->AddRectFilled(q, ImVec2(q.x + 22, q.y + 22), col::Panel, 6);
        icon::cross(dl, ImVec2(q.x + 6, q.y + 6), 10, h ? col::Text : col::TextMuted);
        tooltip("Remove it from the plate (undo puts it back)");
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + kRowH));
        ImGui::Dummy(ImVec2(w, 0));
    }
    ImGui::PopID();
}

// One part off the plate: why, and the fix.
void offRow(App& app, const Document& doc, const Part& part) {
    ImGui::PushID(part.id());
    const fruit::PlateFit& f = part.fit();
    const bool selected = doc.partCount() > 1 && part.id() == doc.selectedId();
    beginCard("off", ImVec2(0, 0), 10, col::ErrorBg, selected ? IM_COL32(0xD8, 0x6A, 0x6A, 255) : IM_COL32(0x6B, 0x2A, 0x2A, 255));
    const ImVec2 top = ImGui::GetCursorScreenPos();
    text(part.name().c_str(), fonts().medium, size::Body, col::Text);
    text(dims(f.size).c_str(), fonts().regular, size::Help, col::TextSecondary);
    text(f.overEdge()     ? "Over the plate's edge where it is."
         : f.fits_turned ? "Too big for the plate as it lies. Turned 90° it fits at this size."
                         : "Too big for the plate at this size, however it is turned.",
         fonts().regular, size::Help, col::Text, true);
    if (secondaryButton(fixLabel(f), ImVec2(0, 30))) applyFix(app, part);
    if (doc.partCount() > 1) {
        // Selecting it, and removing it, as on the plate.
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(top.x + w - 22, top.y - 2));
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (ImGui::InvisibleButton("remove", ImVec2(22, 22))) app.removePart(part.id());
        const bool h = ImGui::IsItemHovered();
        if (h) dl->AddRectFilled(q, ImVec2(q.x + 22, q.y + 22), col::Panel, 6);
        icon::cross(dl, ImVec2(q.x + 6, q.y + 6), 10, h ? col::Text : col::TextMuted);
        tooltip("Remove it from the plate (undo puts it back)");
        if (!selected) {
            ImGui::SetCursorScreenPos(ImVec2(top.x + w - 70, top.y - 1));
            if (linkButton("Select")) app.selectPart(part.id());
        }
        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy(ImVec2(0, 0));
    }
    endCard();
    ImGui::PopID();
}

}  // namespace

void partsList(App& app, Document* doc) {
    {
        const float y = ImGui::GetCursorPosY(), x = ImGui::GetCursorPosX();
        text("Parts", fonts().semibold, size::Section, col::Text);
        if (doc) {
            const char* add = "Add a part…";
            ImGui::PushFont(fonts().semibold, size::Body);
            const float aw = ImGui::CalcTextSize(add).x;
            ImGui::PopFont();
            const float after = ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(x + ImGui::GetContentRegionAvail().x - aw, y + 2));
            if (linkButton(add)) app.addPartDialog();
            tooltip("Put another model on the plate, beside these (⇧⌘O)");
            ImGui::SetCursorPosY(after);
        }
    }
    ImGui::Dummy(ImVec2(0, 2));
    std::vector<const Part*> on, off;
    if (doc)
        for (const Part& part : doc->parts()) (part.fit().fits ? on : off).push_back(&part);
    groupLabel(strf("On the plate · %zu", on.size()).c_str());
    if (!doc) {
        text("Nothing yet: open a model, or drop one on the plate.", fonts().regular, size::Help, col::TextMuted, true);
    } else if (!on.empty()) {
        const bool scroll = on.size() > static_cast<std::size_t>(kRowsShown);
        if (scroll) ImGui::BeginChild("on_plate", ImVec2(0, kRowsShown * kRowH + kRowH / 2), ImGuiChildFlags_None);
        // A part selected (in the view, by undo…) scrolls into sight once.
        static const void* shown_doc = nullptr;
        static int shown = -1;
        for (const Part* part : on) {
            if (scroll && part->id() == doc->selectedId() && (shown != part->id() || shown_doc != doc)) {
                ImGui::SetScrollHereY(0.0f);
                shown = part->id();
                shown_doc = doc;
            }
            partRow(app, *doc, *part);
        }
        if (scroll) ImGui::EndChild();
    } else {
        text("Nothing on the plate.", fonts().regular, size::Help, col::TextMuted);
    }
    ImGui::Dummy(ImVec2(0, 2));
    groupLabel(strf("Off the plate · %zu", off.size()).c_str());
    if (!off.empty()) {
        for (const Part* part : off) offRow(app, *doc, *part);
    } else {
        text("Nothing here. A part too big for the plate, or over its edge, moves here.", fonts().regular, size::Help,
             col::TextMuted, true);
    }
}

std::string toolTitleFor(const Document& doc, const char* title, bool on_part) {
    if (!on_part || doc.partCount() < 2) return title;
    constexpr std::size_t kMax = 18;  // bytes; the panel's title row has room for about this much
    std::string name = doc.part().name();
    if (name.size() > kMax) {
        std::size_t cut = kMax - 1;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;  // not inside a character
        name = name.substr(0, cut) + "…";
    }
    return std::string(title) + " · " + name;
}

void partsPick(App& app, Document& doc, ImVec2 origin, ImVec2 size) {
    if (doc.parts().empty() || app.editingTips() || app.placing() || app.hoverHandle() != Handle::None) return;
    // Not while a click there puts a hole in.
    if (app.step() == Step::Orient && app.orientView() == OrientView::Hollow && !doc.hollow.choices.auto_holes) return;
    if (app.step() == Step::Orient && app.orientView() == OrientView::Results) return;
    // Not while a click there picks a point to measure from.
    if (app.step() == Step::Model && app.toolPanelOpen() && app.tool() == Tool::Measure) return;
    // Nor while a click picks where the letters go, or the second part to combine with.
    if (app.step() == Step::Model && app.toolPanelOpen() && (app.tool() == Tool::Label || app.tool() == Tool::Combine))
        return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool inside = m.x >= origin.x && m.y >= origin.y && m.x < origin.x + size.x && m.y < origin.y + size.y &&
                        ImGui::IsWindowHovered();  // not over a panel floating on the view
    if (!inside || ImGui::IsAnyItemHovered()) return;
    // A click (not a drag, which turns the view) selects; a double-click, or
    // a right-click (not a drag, which moves the view), opens the part's menu.
    const bool click = ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9;
    const bool menu = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ||
                      (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[1] < 9);
    if (!menu && !(click && doc.partCount() > 1)) return;
    const float nx = 2 * (m.x - origin.x) / size.x - 1, ny = 1 - 2 * (m.y - origin.y) / size.y;
    V3 o, d;
    app.camera().ray(nx, ny, size.x / std::max(1.0f, size.y), &o, &d);
    const int id = app.pickPart({o.x, o.y, o.z}, {d.x, d.y, d.z});
    if (id < 0) return;
    if (menu) app.openPartMenu(id, m.x, m.y);
    else app.selectPart(id);
}

void partMenu(App& app, Document& doc) {
    if (app.takePartMenu() >= 0) {
        ImGui::SetNextWindowPos(ImVec2(app.partMenuX(), app.partMenuY()));
        ImGui::OpenPopup("##part_menu");
    }
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10);
    if (ImGui::BeginPopup("##part_menu")) {
        const Part* part = doc.findPart(app.partMenuPart());
        if (!part) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 220);
            text(part->name().c_str(), fonts().semibold, size::Help, col::TextMuted);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(220, 2));
            const int id = part->id();
            if (ImGui::Selectable("Duplicate")) app.duplicateOnce(id);
            tooltip("One copy beside it, clear of the other parts");
            if (ImGui::Selectable("More copies…")) {
                app.selectPart(id);
                app.openTool(Tool::Duplicate);
            }
            ImGui::Separator();
            if (ImGui::Selectable("Remove", false, doc.partCount() > 1 ? 0 : ImGuiSelectableFlags_Disabled))
                app.removePart(id);
            tooltip(doc.partCount() > 1 ? "Undo puts it back" : "A job keeps at least one part");
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

}  // namespace gui::screens::parts
