// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The panels of Duplicate, Arrange, Split and Merge, and the buttons pinned
// at the foot of each. The logic is in PartsTools.cpp.
#include <imgui.h>

#include <algorithm>
#include <cctype>
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

bool busyNow(App& app) { return app.placing() || app.partsTools().fusing; }

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
    numberRow("Room between parts", "gap", &st.arrange_gap, "mm", 0, 50);
    ImGui::Checkbox("##arrange_turn", &st.arrange_turn);
    ImGui::SameLine(0, 10);
    text("Turn them to fit", fonts().semibold, size::Body, col::Text);
    text(st.arrange_turn
             ? "Find layouts lays the parts out by their outlines seen from above, with their supports' feet and "
               "rafts, turning them about the vertical only (how each stands does not change), and shows the best "
               "four. Lay out at once packs their rectangles, turning them only when they don't all fit as they are. "
               "Either way the room is between the parts' rafts."
             : "Find layouts lays the parts out by their outlines seen from above, with their supports' feet and "
               "rafts, without turning them, and shows the best four. Lay out at once packs their rectangles. Either "
               "way the room is between the parts' rafts.",
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
        case Tool::Merge:
        case Tool::Arrange: return 150;
        case Tool::Duplicate:
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
                greyed("Find layouts", "Needs two parts.");
            } else if (app.findingLayouts()) {
                // The search's progress, and a way out.
                const JobProgress jp = app.jobs().progress();
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                const float bw = ImGui::GetContentRegionAvail().x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 38), col::Active, 8);
                dl->AddRectFilled(bp, ImVec2(bp.x + bw * static_cast<float>(jp.fraction), bp.y + 38),
                                  IM_COL32(0xFF, 0x8F, 0x3F, 90), 8);
                ImGui::PushFont(fonts().semibold, size::Body);
                const char* label = "Finding layouts…";
                const ImVec2 ts = ImGui::CalcTextSize(label);
                dl->AddText(ImVec2(bp.x + (bw - ts.x) / 2, bp.y + (38 - ts.y) / 2), col::Text, label);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(bw, 38));
                if (secondaryButton("Cancel", ImVec2(-1, 38))) app.jobs().cancel();
            } else {
                if (primaryButton("Find layouts", ImVec2(-1, 38), !working)) app.findLayouts();
                if (secondaryButton(strf("Lay out %zu parts at once", doc.partCount()).c_str(), ImVec2(-1, 38), !working))
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

// ---- Arrange's picker: the layouts found, four across ----

namespace {

const char* layoutWhy(const std::string& title) {
    if (title == "Recommended") return "The best all round: compact, balanced on the plate, and gentle on the peel.";
    if (title == "Most compact") return "Takes the least room on the plate.";
    if (title == "Balanced on the plate") return "Its load is centred, so the plate is pulled evenly at every peel.";
    if (title == "Gentlest peel") return "Its biggest layers are nearest the vat's hinge, where the peel pulls least.";
    if (title == "Fewest turns") return "Turns as few parts as it can.";
    if (title == "Another way") return "A different layout that also scores well.";
    return "";
}

void layoutCard(App& app, int index, const LayoutChoice& c, const LayoutSearch& s, ImVec2 size) {
    const bool first = index == 0;
    beginCard(("layout" + std::to_string(index)).c_str(), size, 16, first ? col::Card : col::Panel,
              first ? col::Accent : col::Border);
    const float w = ImGui::GetContentRegionAvail().x;
    text(c.title.c_str(), fonts().semibold, size::Section + 1, col::Text);
    // What else it is best at: "Also the most compact and balanced on the plate".
    std::string also;
    for (std::size_t i = 0; i < c.also.size(); ++i) {
        std::string a = c.also[i];
        if (!a.empty()) a[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(a[0])));
        if (a == "most compact" || a == "gentlest peel") a = "the " + a;
        also += (i == 0 ? "" : i + 1 == c.also.size() ? " and " : ", ") + a;
    }
    text(also.empty() ? " " : ("Also " + also).c_str(), fonts().regular, size::Help, col::TextSecondary);
    ImGui::Dummy(ImVec2(0, 2));
    // The plate from above.
    const float ph = std::round(w * 320.0f / 568.0f);
    const ImVec2 pp = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pp, ImVec2(pp.x + w, pp.y + ph), col::Viewport, 10);
    if (const unsigned tex = app.layoutPicture(index)) ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(w, ph));
    else ImGui::Dummy(ImVec2(w, ph));
    ImGui::Dummy(ImVec2(0, 6));
    const fruit::ops::NestLayout& L = c.layout;
    const int n = static_cast<int>(s.part_ids.size());
    auto row = [&](const char* label, const std::string& value, ImU32 colour) {
        const float y = ImGui::GetCursorPosY();
        text(label, fonts().regular, size::Body, col::TextSecondary);
        ImGui::PushFont(fonts().semibold, size::Body);
        const float vw = ImGui::CalcTextSize(value.c_str()).x;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - vw, y));
        text(value.c_str(), fonts().semibold, size::Body, colour);
    };
    row("Parts that fit", strf("%d of %d", L.placed, n), L.placed == n ? col::Ok : col::Warn);
    row("Takes", strf("%.0f × %.0f mm", L.width, L.depth), col::Text);
    row("Load from the middle", L.balance < 0.5 ? std::string("centred") : strf("%.0f mm", L.balance), col::Text);
    if (L.peel >= 0) row("Big layers from the hinge", strf("%.0f %% of the way", L.peel * 100), col::Text);
    row("Turned", L.turned == 0 ? std::string("none") : strf("%d of %d", L.turned, n), col::Text);
    ImGui::Dummy(ImVec2(0, 4));
    text(layoutWhy(c.title), fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 16 - 40);
    const bool pressed = first ? primaryButton("Use this", ImVec2(96, 40)) : secondaryButton("Use this", ImVec2(96, 40));
    if (pressed) app.useLayout(index);
    endCard();
}

}  // namespace

void layoutResults(App& app, Document& /*doc*/, ImVec2 size) {
    const LayoutSearch& s = *app.layouts();
    beginPanel("layout_results", ImVec2(0, size.y), col::App, ImVec2(24, 26));
    const std::size_t n = s.choices.size();
    static const char* counts[] = {"No layout found", "One way to lay them out", "Two ways to lay them out",
                                   "Three ways to lay them out", "Four ways to lay them out"};
    const float w = ImGui::GetContentRegionAvail().x;
    const float y0 = ImGui::GetCursorPosY();
    text(counts[std::min<std::size_t>(n, 4)], fonts().bold, size::DialogTitle + 2, col::Text);
    // Test captures leave out the time it took, which differs run to run.
    const std::string took = app.options().deterministic ? std::string() : strf(" in %.1f s", s.seconds);
    ImGui::PushFont(fonts().semibold, size::Body);
    const float bw = ImGui::CalcTextSize("Back to the job").x + 32;
    ImGui::PopFont();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - bw - 24);
    text(strf("Tried %zu layouts%s: every part by its outline seen from above%s, %g mm apart, turned about the "
              "vertical only. Nothing changes until you pick one, and you can undo.",
              s.candidates, took.c_str(),
              s.supports ? ", with its supports' feet and raft" : ", with room for the raft it will have", s.gap)
             .c_str(),
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - bw, y0 + 4));
    if (secondaryButton("Back to the job", ImVec2(bw, 0))) app.closeLayouts();
    ImGui::SetCursorPosY(y0 + 72);
    const float gap = 16, card_w = (w - 3 * gap) / 4;
    const float card_h = std::min(470.0f, ImGui::GetContentRegionAvail().y - 4);
    for (std::size_t i = 0; i < n && i < 4; ++i) {
        layoutCard(app, static_cast<int>(i), s.choices[i], s, ImVec2(card_w, card_h));
        ImGui::SameLine(0, gap);
    }
    endPanel();
}

}  // namespace gui::screens::parts
