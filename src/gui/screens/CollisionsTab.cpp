// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Job panel's Collisions tab: every pair of parts that overlap or sit
// closer than a least gap, each with a way to select them. The check itself
// is in Collisions.cpp and runs in the background.
#include <imgui.h>

#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

std::string nameOf(const Document& doc, int id) {
    const Part* p = doc.findPart(id);
    return p ? p->name() : "(a part)";
}

}  // namespace

void collisionsTab(App& app, Document* doc) {
    if (!doc) {
        text("Open a model first.", fonts().regular, size::Body, col::TextMuted);
        return;
    }
    if (doc->partCount() < 2) {
        beginCard("one", ImVec2(0, 0), 10, col::Panel, col::Border);
        text("Not checked: there is one part.", fonts().semibold, size::Body, col::Text, true);
        text("Add another part to see whether they overlap or sit too close.", fonts().regular, size::Body,
             col::TextSecondary, true);
        endCard();
        return;
    }
    const auto gapRow = [&] {
        {
        const float w = ImGui::GetContentRegionAvail().x;
        const float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + 8);
        text("Closer than", fonts().regular, size::Body, col::TextSecondary);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - 100, y));
        double gap = app.collisionGap();
        if (numberField("mingap", &gap, 72, "%g", 0, 20)) app.collisionGap() = gap;
        ImGui::SameLine(0, 6);
        text("mm", fonts().regular, size::Body, col::TextSecondary);
    }
    ImGui::Dummy(ImVec2(0, 2));
    };
    const CollisionReport* r = app.collisions();
    if (!r) {
        text(app.checkingCollisions() ? "Checking the parts…" : "Not checked yet: it starts when the parts are at rest.",
             fonts().regular, size::Body, col::TextMuted, true);
        gapRow();
        return;
    }
    const Document& d = *doc;
    if (r->problems.empty() && r->not_checked.empty()) {
        beginCard("clear", ImVec2(0, 0), 10, col::Card, col::Border);
        text("No parts overlap or touch", fonts().semibold, size::Body, col::Text, true);
        text(strf("%d %s checked, where they are now. None is closer than %g mm.", r->pairs_checked,
                  r->pairs_checked == 1 ? "pair" : "pairs", r->min_gap)
                 .c_str(),
             fonts().regular, size::Body, col::TextSecondary, true);
        endCard();
        ImGui::Dummy(ImVec2(0, 4));
        gapRow();
        return;
    }
    if (!r->problems.empty()) {
        groupLabel(strf("To look at · %zu", r->problems.size()).c_str());
        for (std::size_t i = 0; i < r->problems.size(); ++i) {
            const CollisionPair& p = r->problems[i];
            ImGui::PushID(static_cast<int>(i));
            const bool bad = p.overlaps;
            beginCard("pair", ImVec2(0, 0), 10, bad ? col::ErrorBg : col::WarnBg,
                      bad ? IM_COL32(0x6B, 0x2A, 0x2A, 255) : col::WarnBorder);
            text((nameOf(d, p.a) + " and " + nameOf(d, p.b)).c_str(), fonts().semibold, size::Body, col::Text, true);
            text(("They " + describePair(p) + (bad ? ": they print as one lump." : ".")).c_str(), fonts().regular,
                 size::Body, col::TextSecondary, true);
            // A way to each: selecting it, so the tools (Move…) act on it.
            for (int k = 0; k < 2; ++k) {
                const int id = k == 0 ? p.a : p.b;
                const Part* pt = d.findPart(id);
                if (!pt) continue;
                if (k) ImGui::SameLine(0, 16);
                if (d.selectedId() == id) text("Selected", fonts().semibold, size::Body, col::TextMuted);
                else if (linkButton(("Select " + pt->name()).c_str())) app.selectCollision(id);
            }
            endCard();
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 2));
        }
    }
    if (!r->not_checked.empty()) {
        groupLabel("Not checked");
        for (const PartNotChecked& n : r->not_checked)
            text((nameOf(d, n.part) + ": " + n.why + ".").c_str(), fonts().regular, size::Help, col::Warn, true);
    }
    text("The Arrange tool spaces the parts out again.", fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 4));
    gapRow();
}

}  // namespace gui::screens::parts
