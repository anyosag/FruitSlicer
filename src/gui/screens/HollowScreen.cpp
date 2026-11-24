// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Orient · Hollows: the model's own hollows that hold resin and how to
// fix them, hollowing a solid model, drain and air holes, and what then
// prints (canvas board "Hollow").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "App.h"
#include "Format.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

ImU32 rgbCol(unsigned rgb, int alpha = 255) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

// One row per hollow: a dot in its colour in the view, what it is, its grade
// or state, how much and where. Pressing it chooses it.
void cavityRow(App& app, const HollowPlan& plan, bool applied, std::size_t i) {
    const fruit::Cavity& c = plan.closed.cavities[i];
    const bool selected = app.hollowSelected() == static_cast<int>(i);
    const float w = ImGui::GetContentRegionAvail().x, h = 46;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(static_cast<int>(i));
    const bool pressed = ImGui::InvisibleButton("row", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered) app.hoverHollow(static_cast<int>(i));
    if (pressed) app.selectHollow(selected ? -1 : static_cast<int>(i));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool problem = !plan.isAccepted(i) && !(applied && plan.ownFixed(i)) &&
                         (!c.opens_up || plan.grade(i) == fruit::CupGrade::Problem);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), selected ? col::Active : hovered ? col::Input : col::Card, 8);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), selected ? col::BorderStrong : col::Border, 8);
    dl->AddCircleFilled(ImVec2(p.x + 16, p.y + 15), 5, rgbCol(hollowColour(plan, i, applied)), 16);
    const std::string kind = cavityKind(c, false);
    ImGui::PushFont(fonts().semibold, size::Body);
    dl->AddText(ImVec2(p.x + 30, p.y + 7), col::Text, kind.c_str());
    const std::string grade = cavityGradeText(plan, i, applied).substr(kind.size());
    const std::string chipText = grade.size() > 3 ? grade.substr(3) : std::string();  // after " · "
    const float gw = ImGui::CalcTextSize(chipText.c_str()).x;
    dl->AddText(ImVec2(p.x + w - gw - 12, p.y + 7), problem ? col::Warn : col::TextSecondary, chipText.c_str());
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Help);
    const std::string facts = volumeText(c.volume) + " · " + cavityHeights(c, plan.layer_height);
    dl->AddText(ImVec2(p.x + 30, p.y + 25), col::TextMuted, facts.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
}

// How hard each height of a cup pulls: one bar a layer (or the hardest of
// several, when there are more layers than room), the cup's bottom at the
// bottom, with the note and problem lines.
void pullChart(const fruit::Cavity& c, double layer_height, const fruit::CupLimits& l) {
    const std::vector<float>& by = c.cup.by_layer;
    if (by.empty()) return;
    const float w = ImGui::GetContentRegionAvail().x;
    const float label_w = 58, top_pad = 16, chart_h = std::clamp(static_cast<float>(by.size()) * 2.0f, 48.0f, 96.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, chart_h + top_pad + 4));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = p.x + label_w, x1 = p.x + w - 4, y0 = p.y + top_pad, y1 = y0 + chart_h;
    float top = 0;
    for (float v : by) top = std::max(top, v);
    const double scale = std::max({static_cast<double>(top), l.problem_from * 1.15, 0.1});
    auto xAt = [&](double share) { return x0 + static_cast<float>(std::min(share / scale, 1.0)) * (x1 - x0); };
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col::Viewport, 4);
    const int rows = std::max(1, static_cast<int>(chart_h));
    const std::size_t n = by.size();
    for (int r = 0; r < rows; ++r) {
        // Row r from the bottom: the layers it covers, the hardest of them.
        const std::size_t a = n * static_cast<std::size_t>(r) / static_cast<std::size_t>(rows);
        const std::size_t b = std::max(a + 1, n * static_cast<std::size_t>(r + 1) / static_cast<std::size_t>(rows));
        float v = 0;
        for (std::size_t k = a; k < b && k < n; ++k) v = std::max(v, by[k]);
        const ImU32 colour = v >= l.problem_from ? rgbCol(0xF2B84B) : v >= l.note_below ? rgbCol(0xE3D27C) : rgbCol(0x7FA7D6);
        const float yb = y1 - static_cast<float>(r) * chart_h / rows, yt = y1 - static_cast<float>(r + 1) * chart_h / rows;
        if (v > 0) dl->AddRectFilled(ImVec2(x0, yt), ImVec2(std::max(xAt(v), x0 + 1), yb), colour);
    }
    ImGui::PushFont(fonts().regular, size::Label);
    // The note line's label to its left, the problem line's to its right: apart however close the lines are.
    for (const double at : {l.note_below, l.problem_from}) {
        const float x = xAt(at);
        dashedRect(dl, ImVec2(x, y0), ImVec2(x, y1), col::TextMuted, 1.0f, 3, 3);
        const std::string t = strf("%.0f %%", 100 * at);
        const float tw = ImGui::CalcTextSize(t.c_str()).x;
        const float tx = at == l.note_below ? std::max(x0, x - tw - 3) : std::min(x + 3, x1 - tw);
        dl->AddText(ImVec2(tx, p.y), col::TextMuted, t.c_str());
    }
    if (top > l.problem_from * 1.15) {  // where the bars end
        const std::string t = strf("%.0f %%", 100.0 * top);
        const float tw = ImGui::CalcTextSize(t.c_str()).x;
        if (x1 - tw > xAt(l.problem_from) + 40) dl->AddText(ImVec2(x1 - tw, p.y), col::TextMuted, t.c_str());
    }
    const std::string lo = strf("%.1f mm", c.first_layer * layer_height), hi = strf("%.1f mm", (c.last_layer + 1) * layer_height);
    dl->AddText(ImVec2(p.x, y1 - 12), col::TextMuted, lo.c_str());
    dl->AddText(ImVec2(p.x, y0), col::TextMuted, hi.c_str());
    ImGui::PopFont();
    // Under the pointer: that height and how hard it pulls.
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (ImGui::IsWindowHovered() && m.x >= x0 && m.x <= x1 && m.y >= y0 && m.y < y1) {
        const auto k = std::min(n - 1, static_cast<std::size_t>((y1 - m.y) / chart_h * static_cast<float>(n)));
        ImGui::SetTooltip("At %.2f mm: %s", (c.first_layer + static_cast<int>(k) + 0.5) * layer_height,
                          strf("%.0f %%", 100.0 * by[k]).c_str());
    }
}

// The tilt that would shrink a cup: found when asked, then one button to use it.
void tiltFix(App& app, const Document& doc, const HollowPlan& plan, bool fresh, std::size_t i) {
    const HollowState& hs = doc.hollow;
    text("Tilt the model", fonts().semibold, size::Body, col::Text);
    const int part = doc.partNear(cavityLabelAt(plan.closed, i, plan.layer_height));
    const CupTilt* t = hs.tilt && hs.tilt_revision == doc.revision() && hs.tilt->part == part ? hs.tilt.get() : nullptr;
    const Part* on = doc.findPart(part);
    const bool on_plate = on && on->placement().on_plate;
    if (!t) {
        text("Tilted, a cup's resin runs out over its low side as each layer prints, and its openings get smaller. "
             "The fix PreForm and Lychee suggest first.",
             fonts().regular, size::Body, col::TextSecondary, true);
        if (app.findingTilt()) primaryButton("Trying angles…", ImVec2(0, 32), false);
        else if (secondaryButton("Find a tilt", ImVec2(0, 32), fresh && !app.jobs().busy())) app.findCupTilt(i);
        return;
    }
    text(tiltWords(t->now).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    for (const CupTiltTry& k : t->tries) text(tiltWords(k).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    if (t->pick >= 0) {
        const CupTiltTry& k = t->tries[static_cast<std::size_t>(t->pick)];
        const std::string label = strf("Tilt it %.0f°", std::abs(k.deg));
        if (primaryButton(label.c_str(), ImVec2(0, 32), !app.jobs().busy())) app.useCupTilt(k);
        tooltip(strf("Turns it %.0f° about the plate's %s axis. Then it stands on supports, and Supports makes "
                     "them again.",
                     k.deg, k.axis == 0 ? "X" : "Y")
                    .c_str());
        if (on_plate)
            text("It then stands on supports, not on the plate.", fonts().regular, size::Help, col::TextMuted, true);
    } else {
        text(strf("Up to %.0f° no tilt takes every cup under the problem line.", cupTiltAngles().back()).c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
    }
}

// A vent into a cup: shown in the view before it is added.
void ventFix(App& app, const HollowPlan& plan, bool fresh, bool applied, std::size_t i) {
    text("Add a vent", fonts().semibold, size::Body, col::Text);
    std::size_t vents = 0;
    for (std::size_t k = 0; k < plan.holes.size() && k < plan.into_own.size(); ++k)
        vents += plan.holes[k].vent && plan.into_own[k] == static_cast<int>(i) ? 1 : 0;
    if (vents) {
        const bool drains = i < plan.own_drains.size() && plan.own_drains[i];
        const std::string said = (vents == 1 ? std::string("A vent goes in") : strf("%zu vents go in", vents)) +
                                 (drains ? std::string(": resin gets in under it, so it no longer pulls.")
                                         : std::string(", but it still pulls: the vent doesn't reach its lowest point.")) +
                                 (applied ? "" : " Nothing changes until you apply it below.");
        text(said.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        if (linkButton("Take it out") && fresh) app.removeVents(i);
        return;
    }
    if (i < plan.vent_preview.size() && plan.vent_preview[i]) {
        const fruit::PlacedHole& v = *plan.vent_preview[i];
        const double len = fruit::norm(v.to - v.from);
        text(strf("A %.1f mm hole from its lowest point to the outside, about %.0f mm long, shown in blue in the "
                  "view, which cuts along it. It goes through the model's surface where it comes out, and into the "
                  "cup's floor: check it spoils nothing that matters.",
                  2 * v.radius, len)
                 .c_str(),
             fonts().regular, size::Body, col::Warn, true);
        if (secondaryButton("Add this vent", ImVec2(0, 32), fresh)) app.addVent(i);
        return;
    }
    text("No way out from its lowest point that doesn't end on the plate, which would seal it. Tilt it, or place a "
         "hole by hand.",
         fonts().regular, size::Body, col::TextSecondary, true);
}

// The hollow chosen, opened up: what it does, where and how hard it pulls,
// and the fixes that fit it.
void cavityDetail(App& app, const Document& doc, const HollowPlan& plan, bool fresh, std::size_t i) {
    const fruit::Cavity& c = plan.closed.cavities[i];
    const HollowState& hs = doc.hollow;
    const bool accepted = plan.isAccepted(i);
    const bool fixed = hs.applied && plan.ownFixed(i);
    const fruit::CupLimits limits = cupLimits(doc);
    beginCard("detail", ImVec2(0, 0), 14, col::Card, col::BorderStrong);
    text(cavityMeaning(c, false).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    if (c.opens_up) {
        ImGui::Dummy(ImVec2(0, 4));
        groupLabel("How hard it pulls, by height");
        pullChart(c, plan.layer_height, limits);
        text(cavityPull(c, plan.layer_height, limits).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        text(cavityGradeWhy(c, plan.grade(i), limits).c_str(), fonts().regular, size::Help, col::TextMuted, true);
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (accepted) {
        text("Accepted for this job: it prints as it is, not counted as a problem. Holes and filling leave it.",
             fonts().regular, size::Help, col::TextMuted, true);
        if (linkButton("Take it back") && fresh) app.acceptHollow(i, false);
    } else if (c.opens_up) {
        if (!fixed) {
            tiltFix(app, doc, plan, fresh, i);
            ImGui::Dummy(ImVec2(0, 6));
        }
        ventFix(app, plan, fresh, hs.applied, i);
        ImGui::Dummy(ImVec2(0, 6));
        if (secondaryButton("Accept for this model", ImVec2(0, 0), fresh)) app.acceptHollow(i, true);
        tooltip("Prints it as it is. Saved with this job: not counted as a problem. If the hollow changes, it shows "
                "again.");
    } else {
        const bool filling = plan.filling() && plan.filled.size() > i && plan.filled[i];
        const std::string fate = plan.hollowing() ? "It goes with the rest of the hollowed inside."
                                 : filling        ? "Printed solid, as chosen below."
                                 : plan.drilling() ? (i < plan.own_drains.size() && plan.own_drains[i]
                                                          ? "Drain holes go in, as chosen below: a drain at its lowest point, an air hole at its top."
                                                          : "Drain holes are chosen below, but none reaches its lowest point.")
                                                   : "Left as it is, as chosen below: a problem before saving.";
        text(fate.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 2));
        if (secondaryButton("Accept for this model", ImVec2(0, 0), fresh)) app.acceptHollow(i, true);
        tooltip("Prints it as it is. Saved with this job: not counted as a problem. Holes and filling leave it. If "
                "the hollow changes, it shows again.");
    }
    endCard();
    ImGui::Dummy(ImVec2(0, 4));
}

// A label on the left, and on the right of the panel a field with its unit.
bool fieldRow(const char* label, const char* id, double* v, const char* format, const char* unit, double lo,
              double hi) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y + 8);
    text(label, fonts().regular, size::Body, col::TextSecondary);
    ImGui::PushFont(fonts().regular, size::Body);
    const float uw = ImGui::CalcTextSize(unit).x;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - uw - 6 - 72, y));
    const bool changed = numberField(id, v, 72, format, lo, hi);
    ImGui::SameLine(0, 6);  // on the field's text line
    text(unit, fonts().regular, size::Body, col::TextSecondary);
    return changed;
}

// A label on the left, a choice on the right; returns the one picked.
int choiceRow(const char* label, const char* id, const char* const* items, int n, int current) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y + 8);
    text(label, fonts().regular, size::Body, col::TextSecondary);
    ImGui::PushFont(fonts().regular, size::Body);
    float iw = 0;
    for (int i = 0; i < n; ++i) iw = std::max(iw, ImGui::CalcTextSize(items[i]).x);
    const float cw = iw + 52;
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - cw, y));
    ImGui::SetNextItemWidth(cw);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Viewport);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::Viewport);
    ImGui::PushStyleColor(ImGuiCol_Button, col::Viewport);  // the arrow's box: part of the field
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::Viewport);
    ImGui::PushStyleColor(ImGuiCol_Text, col::Text);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Card);
    int picked = current;
    if (ImGui::BeginCombo(id, items[current])) {
        for (int i = 0; i < n; ++i)
            if (ImGui::Selectable(items[i], i == current)) picked = i;
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    return picked;
}

}  // namespace

void hollowPanel(App& app, Document& doc) {
    const HollowState& hs = doc.hollow;
    HollowChoices c = hs.choices;
    bool changed = false;
    // The model's own hollows depend only on how it sits, not on the choices.
    const HollowPlan* plan = hs.plan && hs.plan_revision == doc.revision() ? hs.plan.get() : nullptr;
    const HollowPlan* fresh = hs.planFresh(doc.revision()) ? hs.plan.get() : nullptr;

    text("Hollows in this model", fonts().semibold, size::Section + 1, col::Text);
    std::size_t traps = 0, cups = 0;
    if (!plan) {
        text(app.planningHollow() ? "Looking for hollows…" : "Waiting for the task running now…", fonts().regular,
             size::Body, col::TextMuted, true);
    } else if (plan->closed.cavities.empty()) {
        beginCard("none", ImVec2(0, 0), 14, col::Card, col::Border);
        text("No hollows that hold resin", fonts().semibold, size::Body, col::Text);
        text("It is solid, or its hollows open towards the plate and drain as it prints.", fonts().regular,
             size::Body, col::TextSecondary, true);
        endCard();
    } else {
        const std::size_t n = plan->closed.cavities.size();
        for (std::size_t i = 0; i < n; ++i) (plan->isCup(i) ? cups : traps) += 1;
        // Which is chosen, and a step to the next or the one before.
        const int sel = app.hollowSelected();
        const std::string where = sel >= 0 ? strf("Hollow %d of %zu", sel + 1, n)
                                           : (n == 1 ? std::string("Painted on the model") : strf("%zu, painted on the model", n));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
        text(where.c_str(), fonts().regular, size::Help, col::TextMuted);
        if (n > 1) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 92);
            if (secondaryButton("<", ImVec2(40, 26))) app.selectHollow(sel <= 0 ? static_cast<int>(n) - 1 : sel - 1);
            ImGui::SameLine(0, 6);
            if (secondaryButton(">", ImVec2(40, 26))) app.selectHollow(sel < 0 || sel + 1 >= static_cast<int>(n) ? 0 : sel + 1);
        }
        ImGui::Dummy(ImVec2(0, 2));
        for (std::size_t i = 0; i < n; ++i) {
            cavityRow(app, *plan, hs.applied, i);
            if (app.hollowSelected() == static_cast<int>(i)) cavityDetail(app, doc, *plan, fresh != nullptr, i);
        }
    }
    bool own = plan && !plan->closed.cavities.empty();
    if (!plan)
        for (const Part& part : doc.parts()) own = own || part.check().cavities > 0;
    // The choice below is for resin traps: suction cups have theirs above.
    const bool trap_choice = plan ? traps > 0 : own;

    // One choice: how its own hollows print, or the whole model hollowed.
    ImGui::Dummy(ImVec2(0, 8));
    text(trap_choice ? (traps == 1 ? "The resin trap" : "The resin traps") : "Hollow the model", fonts().semibold,
         size::Body, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    using F = HollowChoices::Fix;
    auto option = [&](const char* id, const char* title, const char* body, bool selected, bool hollow, F f) {
        if (radioCard(id, title, body, selected) && !selected) {
            c.hollow = hollow;
            c.fix = f;
            changed = true;
        }
        ImGui::Dummy(ImVec2(0, 2));
    };
    if (trap_choice) {
        option("fix_holes", "Drain holes", "One at the lowest point to drain, one higher up to let air in.",
               !c.hollow && c.fix == F::Holes, false, F::Holes);
        option("fix_fill", "Fill it", "Print it solid: more resin, no trap. Suction cups are never filled.",
               !c.hollow && c.fix == F::Fill, false, F::Fill);
        option("fix_leave", "Leave it", "Shown as a problem before saving, unless you accept it above.",
               !c.hollow && c.fix == F::Leave, false, F::Leave);
        option("fix_hollow", "Hollow the whole model",
               "Take the inside out, keeping a wall all round. A resin trap goes with the rest.", c.hollow, true,
               c.fix);
    } else {
        option("keep_solid", "Keep it solid", "It prints as it is.", !c.hollow, false, c.fix);
        option("hollow_it", "Hollow it", "Take the inside out to save resin on a big part, keeping a wall all round.",
               c.hollow, true, c.fix);
    }
    if (c.hollow) {
        ImGui::Dummy(ImVec2(0, 2));
        if (fieldRow("Wall", "wall", &c.wall, "%.1f", "mm", 0.8, 10)) changed = true;
        ImGui::Dummy(ImVec2(0, 2));
        if (fieldRow("Ceilings lean up to", "lean", &c.ceiling_lean, "%.0f", "°", 20, 89)) changed = true;
        text("A ceiling inside can't have tips, so it is shaped to hold itself up. That costs room: a round hollow "
             "keeps about three quarters of itself.",
             fonts().regular, size::Help, col::TextMuted, true);
    }

    const bool trap_holes = c.hollow || (trap_choice && c.fix == F::Holes);
    if (trap_holes || !hs.vents.empty()) {
        ImGui::Dummy(ImVec2(0, 8));
        groupLabel("Holes");
        ImGui::Dummy(ImVec2(0, 2));
        if (fieldRow("Diameter", "hole_d", &c.hole_diameter, "%.1f", "mm", 1, 10)) changed = true;
        ImGui::Dummy(ImVec2(0, 2));
    }
    if (trap_holes) {
        static const char* kPlace[] = {"Automatically", "Click on the model"};
        const int place = choiceRow("Place", "##place", kPlace, 2, c.auto_holes ? 0 : 1);
        if ((place == 0) != c.auto_holes) {
            c.auto_holes = place == 0;
            changed = true;
        }
        if (!c.auto_holes) {
            ImGui::Dummy(ImVec2(0, 2));
            if (hs.manual.empty())
                text("No holes yet. Click on the model where one should go in: it goes in square to the surface.",
                     fonts().regular, size::Help, col::TextMuted, true);
            // Those on a part off the job (removed, until undo) are left out,
            // here and in the plan.
            const PartFrames frames = doc.frames();
            std::size_t k = 0;  // into the plan's holes
            for (std::size_t i = 0; i < hs.manual.size(); ++i) {
                if (!frames.find(hs.manual[i].part)) continue;
                const bool missed = fresh && k < fresh->placed.size() && fresh->placed[k].cavity < 0;
                ++k;
                text(strf("Hole %zu · %.1f mm", i + 1, hs.manual[i].diameter).c_str(), fonts().regular, size::Body,
                     col::Text);
                if (missed) {
                    ImGui::SameLine(0, 8);
                    text("reaches no hollow", fonts().regular, size::Body, col::Warn);
                }
                ImGui::SameLine(0, 10);
                ImGui::PushID(static_cast<int>(i));
                if (linkButton("Remove")) app.removeHole(static_cast<int>(i));
                ImGui::PopID();
            }
        }
    }
    if (changed) app.setHollowChoices(c);
}

// Nothing changes what prints until this is pressed: at the panel's foot.
void hollowApplyArea(App& app, Document& doc) {
    const HollowState& hs = doc.hollow;
    const HollowPlan* fresh = hs.planFresh(doc.revision()) ? hs.plan.get() : nullptr;
    if (!hs.applied && app.planningHollow()) {
        primaryButton("Finding hollows…", ImVec2(-1, 38), false);
    } else if (!hs.applied && fresh && fresh->changes()) {
        if (primaryButton(applyLabel(*fresh).c_str(), ImVec2(-1, 38))) app.applyHollow();
        text(("Nothing changes until you press " + applyLabel(*fresh) + ".").c_str(), fonts().regular, size::Help,
             col::TextMuted, true);
    } else if (hs.applied) {
        text("Applied: what prints follows these choices as you change them.", fonts().regular, size::Help,
             col::TextMuted, true);
    }
}

void hollowOutcomesList(App& app, Document& doc) {
    const HollowState& hs = doc.hollow;
    const HollowPlan* plan = hs.planFresh(doc.revision()) ? hs.plan.get() : nullptr;
    groupLabel(plan && !plan->holes.empty() ? "After the holes"
               : plan && plan->hollowing()  ? "After hollowing"
                                            : "What prints");
    ImGui::Dummy(ImVec2(0, 2));
    if (!plan) {
        text(app.planningHollow() ? "Working it out…" : "Open Hollow to check this model's hollows.", fonts().regular,
             size::Body, col::TextMuted, true);
        return;
    }
    std::vector<HollowOutcome> out = hollowOutcomes(*plan);
    if (out.empty()) out.push_back({true, "It prints as it is", "Nothing in it holds resin."});
    if (!plan->hollowing())
        out.push_back({true, "Hollowing a solid model", "Saves resin on big parts. Wall thickness and holes are set here too."});
    for (std::size_t i = 0; i < out.size(); ++i) {
        const HollowOutcome& o = out[i];
        beginCard(strf("out%zu", i).c_str(), ImVec2(0, 0), 12, o.ok ? col::Card : col::WarnBg,
                  o.ok ? col::Border : col::WarnBorder);
        text(o.title.c_str(), fonts().semibold, size::Body, o.ok ? col::Text : col::Warn);
        text(o.detail.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        endCard();
        ImGui::Dummy(ImVec2(0, 2));
    }
    (void)app;
}

void hollowCutControls(App& app) {
    ImGui::Dummy(ImVec2(0, 2));
    static const char* const kAxes[] = {"Across X", "Across Y", "Height"};
    const int axis = app.hollowCut().axis;
    for (int a = 0; a < 3; ++a) {
        if (a) ImGui::SameLine(0, 4);
        if (segmentButton(kAxes[a], axis == a) && axis != a) app.setHollowCutAxis(a);
    }
    ImGui::SameLine(0, 12);
    if (segmentButton("Flip", false)) {
        App::HollowCut c = app.hollowCut();
        c.keep_above = !c.keep_above;
        app.setHollowCut(c);
    }
    tooltip("Shows the other side of the cut.");
    // Where along that axis: a slider and the number, in mm.
    App::HollowCut c = app.hollowCut();
    const auto [lo, hi] = app.hollowCutRange(c.axis);
    bool changed = false;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Input);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::Active);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col::Active);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 7);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 8));
    ImGui::SetNextItemWidth(230);
    float f = static_cast<float>(c.at);
    if (ImGui::SliderFloat("##cut_at", &f, static_cast<float>(lo), static_cast<float>(hi), "", ImGuiSliderFlags_AlwaysClamp)) {
        c.at = std::round(static_cast<double>(f) * 20) / 20;  // to the half tenth
        changed = true;
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    tooltip("Where the cut is. It stays there as you turn the view; choosing a hollow moves it to that one.");
    ImGui::SameLine(0, 8);
    if (numberField("cut_at", &c.at, 80, "%.2f", lo, hi)) changed = true;
    ImGui::SameLine(0, 6);
    text("mm", fonts().regular, size::Body, col::TextSecondary);
    if (changed) app.setHollowCut(c);
}

void hollowOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* vp) {
    auto project = [&](const fruit::Vec3& w, ImVec2* out) {
        const float x = vp[0] * w.x + vp[4] * w.y + vp[8] * w.z + vp[12];
        const float y = vp[1] * w.x + vp[5] * w.y + vp[9] * w.z + vp[13];
        const float c = vp[3] * w.x + vp[7] * w.y + vp[11] * w.z + vp[15];
        if (c <= 1e-4f) return false;
        *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
        return true;
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto tag = [&](ImVec2 at, const std::string& label, ImU32 colour) {
        const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
        const ImVec2 lp(at.x - ls.x / 2, at.y - ls.y / 2);
        dl->AddRectFilled(ImVec2(lp.x - 8, lp.y - 4), ImVec2(lp.x + ls.x + 8, lp.y + ls.y + 4),
                          IM_COL32(0x0E, 0x10, 0x14, 215), 7);
        dl->AddText(lp, colour, label.c_str());
    };
    const HollowState& hs = doc.hollow;
    const bool fresh = hs.planFresh(doc.revision());
    if (hs.plan && hs.plan_revision == doc.revision()) {
        const HollowPlan& p = *hs.plan;
        // The hollow chosen, labelled in its middle, brought onto the cut
        // when the cut takes that away (or at its top, with the view whole).
        // The others are painted only.
        const int sel = app.hollowSelected();
        if (sel >= 0 && static_cast<std::size_t>(sel) < p.closed.cavities.size()) {
            const fruit::Cavity& c = p.closed.cavities[static_cast<std::size_t>(sel)];
            fruit::Vec3 at = cavityLabelAt(p.closed, static_cast<std::size_t>(sel), p.layer_height);
            if (!app.cutAway() || !hs.choices.auto_holes) at.z = (c.last_layer + 1) * p.layer_height + 1.5;
            else at = app.ontoCut(at);
            ImVec2 a;
            if (project(at, &a)) {
                const std::size_t of = p.hollowing() ? 0 : static_cast<std::size_t>(sel);
                const HollowOutcome l = p.hollowing() ? HollowOutcome{true, volumeText(c.volume) + " " +
                                                                                (c.opens_up ? "suction cup" : "of trapped resin"),
                                                                      ""}
                                                      : cavityLabel(p, of, hs.applied);
                ImGui::PushFont(fonts().semibold, size::Section);
                tag(a, l.title, l.ok ? col::TextSecondary : col::Warn);
                ImGui::PopFont();
            }
        }
        // A hole that reaches nothing, and the vent shown before it is added.
        ImGui::PushFont(fonts().regular, size::Help);
        for (std::size_t i = 0; i < p.placed.size(); ++i) {
            if (p.placed[i].cavity >= 0) continue;  // drawn through the model and the cut, as its tube is
            ImVec2 a;
            if (project(p.placed[i].from, &a)) tag(ImVec2(a.x, a.y - 16), "reaches no hollow", col::Warn);
        }
        if (fresh && sel >= 0 && static_cast<std::size_t>(sel) < p.vent_preview.size() &&
            p.vent_preview[static_cast<std::size_t>(sel)]) {
            const fruit::PlacedHole& v = *p.vent_preview[static_cast<std::size_t>(sel)];
            ImVec2 a;
            if (project(v.from, &a))
                tag(ImVec2(a.x, a.y + 22), strf("vent · %.1f mm · not added", 2 * v.radius), col::Text);
        }
        ImGui::PopFont();
    }
    // A click: by hand, a hole goes in there; else the hollow there is chosen.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool inside = m.x >= origin.x && m.y >= origin.y + 44 && m.x < origin.x + size.x && m.y < origin.y + size.y &&
                        ImGui::IsWindowHovered();  // not over a panel floating on the view
    if (!inside || !ImGui::IsMouseReleased(ImGuiMouseButton_Left) || io.MouseDragMaxDistanceSqr[0] >= 9 ||
        ImGui::IsAnyItemHovered())
        return;
    const float nx = 2 * (m.x - origin.x) / size.x - 1, ny = 1 - 2 * (m.y - origin.y) / size.y;
    V3 o, d;
    app.camera().ray(nx, ny, size.x / std::max(1.0f, size.y), &o, &d);
    const bool by_hand = !hs.choices.auto_holes && (hs.choices.hollow || hs.choices.fix == HollowChoices::Fix::Holes);
    if (by_hand) {
        fruit::Vec3 pos, n;
        if (app.pickModel({o.x, o.y, o.z}, {d.x, d.y, d.z}, &pos, &n)) app.addHoleAt(pos, n);
        return;
    }
    const int k = app.pickHollow({o.x, o.y, o.z}, {d.x, d.y, d.z});
    if (k >= 0) app.selectHollow(k, false);
}

}  // namespace gui::screens::parts
