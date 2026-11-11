// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The panels of Duplicate, Arrange, Split and Merge, and the buttons pinned
// at the foot of each. The logic is in PartsTools.cpp.
#include <imgui.h>

#include <algorithm>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "PartsTools.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

// A labelled number field on one row.
bool numberRow(const char* label, const char* id, double* v, const char* unit, double lo, double hi) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y + 8);
    text(label, fonts().regular, size::Body, col::TextSecondary);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - 100, y));
    const bool changed = numberField(id, v, *unit ? 72 : 90, "%g", lo, hi);
    if (*unit) {
        ImGui::SameLine(0, 6);
        text(unit, fonts().regular, size::Body, col::TextSecondary);
    }
    return changed;
}

// What the last tool did, while the job is as it left it.
void reportCard(App& app, const Document& doc, Tool t) {
    const PartsToolState& st = app.partsTools();
    if (!sameJob(st.last_doc, app.document()) || st.last_revision != doc.revision() || st.last_tool != static_cast<int>(t) ||
        st.last.message.empty())
        return;
    ImGui::Dummy(ImVec2(0, 6));
    const bool ok = st.last.changed && (st.last.wanted == 0 || st.last.done == st.last.wanted);
    beginCard("report", ImVec2(0, 0), 12, ok ? col::Card : col::WarnBg, ok ? col::Border : col::WarnBorder);
    text(st.last.message.c_str(), fonts().regular, size::Body, ok ? col::TextSecondary : col::Text, true);
    endCard();
}

bool busyNow(App& app) { return app.placing(); }

// A greyed button with the reason under it.
void greyed(const char* label, const std::string& why) {
    primaryButton(label, ImVec2(-1, 38), false);
    text(why.c_str(), fonts().regular, size::Help, col::TextMuted, true);
}

}  // namespace

void duplicatePanel(App& app, Document& doc) {
    PartsToolState& st = app.partsTools();
    groupLabel("Copies");
    double n = st.copies;
    if (numberRow("How many copies", "copies", &n, "", 1, kMaxCopies)) st.copies = std::clamp(static_cast<int>(n), 1, kMaxCopies);
    text(strf("Each copy goes in the room beside the other parts, %g mm clear of them. The other parts do not move. "
              "Supports need making again.",
              kPartGap)
             .c_str(),
         fonts().regular, size::Help, col::TextMuted, true);
    reportCard(app, doc, Tool::Duplicate);
}

void arrangePanel(App& app, Document& doc) {
    PartsToolState& st = app.partsTools();
    groupLabel("Lay them out");
    numberRow("Gap between parts", "gap", &st.arrange_gap, "mm", 0, 50);
    text("Every part is moved, biggest first, with this much room between them, and the whole layout is centred on "
         "the plate. Parts are not turned. Their rafts and the supports at their edges need room: 5 mm is a good gap.",
         fonts().regular, size::Help, col::TextMuted, true);
    if (doc.partCount() < 2) {
        ImGui::Dummy(ImVec2(0, 6));
        text("There is one part: nothing to lay out. Add another with Add a part, in the Job panel.", fonts().regular, size::Body,
             col::TextSecondary, true);
    }
    reportCard(app, doc, Tool::Arrange);
}

void splitPanel(App& app, Document& doc) {
    const int n = shellsOf(doc.part());
    groupLabel("Shells");
    beginCard("shells", ImVec2(0, 0), 12);
    text(n == 1 ? "It has 1 shell." : strf("It has %d shells.", n).c_str(), fonts().semibold, size::Body, col::Text, true);
    text(n < 2 ? "It is all one piece, so there is nothing to split."
         : n > kMaxPieces
             ? strf("That is more than the most that can be made into parts (%d).", kMaxPieces).c_str()
             : "Each shell becomes a part of its own, standing where it was.",
         fonts().regular, size::Body, col::TextSecondary, true);
    endCard();
    ImGui::Dummy(ImVec2(0, 6));
    text("A hollow inside a shell stays with it. Tips and drain holes placed by hand are not kept. Supports need "
         "making again.",
         fonts().regular, size::Help, col::TextMuted, true);
    reportCard(app, doc, Tool::Split);
}

void mergePanel(App& app, Document& doc) {
    if (doc.partCount() < 2) {
        text("There is one part: nothing to join. Add another part first.", fonts().regular, size::Body,
             col::TextSecondary, true);
        reportCard(app, doc, Tool::Merge);
        return;
    }
    groupLabel("Parts to join");
    const std::vector<int> chosen = app.mergeChosen();
    for (const Part& part : doc.parts()) {
        ImGui::PushID(part.id());
        const bool on = std::find(chosen.begin(), chosen.end(), part.id()) != chosen.end();
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("pick", ImVec2(w, 30));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 30), col::Card, 8);
        const ImVec2 b(p.x + 6, p.y + 7);
        dl->AddRectFilled(b, ImVec2(b.x + 16, b.y + 16), on ? col::Accent : col::Panel, 4);
        dl->AddRect(b, ImVec2(b.x + 16, b.y + 16), on ? col::Accent : col::BorderStrong, 4);
        if (on) icon::check(dl, ImVec2(b.x + 2, b.y + 2), 12, col::OnAccent);
        ImGui::PushFont(fonts().medium, size::Body);
        dl->PushClipRect(p, ImVec2(p.x + w - 4, p.y + 30), true);
        dl->AddText(ImVec2(p.x + 32, p.y + 6), col::Text, part.name().c_str());
        dl->PopClipRect();
        ImGui::PopFont();
        if (clicked) app.mergePick(part.id(), !on);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 6));
    groupLabel("Two ways");
    text("Join as one part keeps the shells just as they are: the parts become one in the list, and Split gets them "
         "back apart.",
         fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 4));
    text("Fuse where they overlap makes the overlapping shells one solid. It needs closed meshes and takes a moment on "
         "big parts.",
         fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 4));
    text("Supports need making again, and tips or drain holes placed by hand on these parts are not kept.",
         fonts().regular, size::Help, col::TextMuted, true);
    reportCard(app, doc, Tool::Merge);
}

float partsToolFooterHeight(Tool t) {
    switch (t) {
        case Tool::Merge: return 150;
        case Tool::Duplicate:
        case Tool::Arrange:
        case Tool::Split: return 96;
        default: return 0;
    }
}

void partsToolFooter(App& app, Document& doc, Tool t) {
    ImGui::Dummy(ImVec2(0, 4));
    const bool working = busyNow(app);
    switch (t) {
        case Tool::Duplicate: {
            const int n = app.partsTools().copies;
            if (primaryButton(n == 1 ? "Make 1 copy" : strf("Make %d copies", n).c_str(), ImVec2(-1, 38), !working))
                app.duplicateParts();
            break;
        }
        case Tool::Arrange: {
            if (doc.partCount() < 2) {
                greyed("Arrange", "Needs two parts.");
            } else if (primaryButton(strf("Arrange %zu parts", doc.partCount()).c_str(), ImVec2(-1, 38), !working)) {
                app.arrangeAll();
            }
            break;
        }
        case Tool::Split: {
            const int n = shellsOf(doc.part());
            if (n < 2) greyed("Split", "Needs two shells or more.");
            else if (n > kMaxPieces) greyed("Split", strf("It has %d shells: too many to make into parts.", n));
            else if (primaryButton(strf("Split into %d parts", n).c_str(), ImVec2(-1, 38), !working)) app.splitSelected();
            break;
        }
        case Tool::Merge: {
            const std::size_t n = app.mergeChosen().size();
            if (doc.partCount() < 2) {
                greyed("Join as one part", "Needs two parts.");
            } else if (n < 2) {
                primaryButton("Join as one part", ImVec2(-1, 38), false);
                secondaryButton("Fuse where they overlap", ImVec2(-1, 38), false);
                text("Pick at least two parts above.", fonts().regular, size::Help, col::TextMuted, true);
            } else {
                if (primaryButton("Join as one part", ImVec2(-1, 38), !working)) app.mergeParts(false);
                if (secondaryButton("Fuse where they overlap", ImVec2(-1, 38), !working)) app.mergeParts(true);
            }
            break;
        }
        default: break;
    }
}

}  // namespace gui::screens::parts
