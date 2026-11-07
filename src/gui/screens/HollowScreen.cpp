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

// A cavity's card: what it is, how big and where, and what it does as it is.
void cavityCard(const char* id, const fruit::Cavity& c, double layer_height) {
    beginCard(id, ImVec2(0, 0), 14, col::WarnBg, col::WarnBorder);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    text(c.opens_up ? "Open hollow" : "Closed hollow", fonts().semibold, size::Body, col::Text);
    const std::string kind = cavityKind(c, false);
    ImGui::PushFont(fonts().semibold, size::Body);
    const float kw = ImGui::CalcTextSize(kind.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + w - kw, p.y), col::Warn, kind.c_str());
    ImGui::PopFont();
    const ImU32 body = IM_COL32(0xE6, 0xD9, 0xA8, 255);
    text(cavityFacts(c, layer_height).c_str(), fonts().regular, size::Body, body);
    text(cavityMeaning(c, false).c_str(), fonts().regular, size::Body, body, true);
    endCard();
    ImGui::Dummy(ImVec2(0, 2));
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

void hollowLeft(App& app, Document& doc, float h) {
    beginPanel("hollow_left", ImVec2(kLeftW, h), col::Panel, ImVec2(18, 22), true);
    edge(true, false);
    if (linkButton("‹ Orient")) app.showOrientSettings();
    ImGui::Dummy(ImVec2(0, 2));
    const HollowState& hs = doc.hollow;
    HollowChoices c = hs.choices;
    bool changed = false;
    // The model's own hollows depend only on how it sits, not on the choices.
    const HollowPlan* plan = hs.plan && hs.plan_revision == doc.revision() ? hs.plan.get() : nullptr;
    const HollowPlan* fresh = hs.planFresh(doc.revision()) ? hs.plan.get() : nullptr;

    text("Hollows in this model", fonts().semibold, size::Section + 1, col::Text);
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
        for (std::size_t i = 0; i < plan->closed.cavities.size(); ++i)
            cavityCard(strf("own%zu", i).c_str(), plan->closed.cavities[i], plan->layer_height);
    }
    const bool own = plan ? !plan->closed.cavities.empty() : doc.check.cavities > 0;

    // One choice: how its own hollows print, or the whole model hollowed.
    ImGui::Dummy(ImVec2(0, 8));
    text(own ? "Fix it" : "Hollow the model", fonts().semibold, size::Body, col::Text);
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
    if (own) {
        option("fix_holes", "Drain holes", "One at the lowest point to drain, one higher up to let air in.",
               !c.hollow && c.fix == F::Holes, false, F::Holes);
        option("fix_fill", "Fill it", "Print it solid: more resin, no trap.", !c.hollow && c.fix == F::Fill, false,
               F::Fill);
        option("fix_leave", "Leave it", "Shown as a problem before saving.", !c.hollow && c.fix == F::Leave, false,
               F::Leave);
        option("fix_hollow", "Hollow the whole model",
               "Take the inside out, keeping a wall all round. This hollow goes with the rest.", c.hollow, true,
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

    if (c.hollow || (own && c.fix == F::Holes)) {
        ImGui::Dummy(ImVec2(0, 8));
        groupLabel("Holes");
        ImGui::Dummy(ImVec2(0, 2));
        if (fieldRow("Diameter", "hole_d", &c.hole_diameter, "%.1f", "mm", 1, 10)) changed = true;
        ImGui::Dummy(ImVec2(0, 2));
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
            for (std::size_t i = 0; i < hs.manual.size(); ++i) {
                const bool missed = fresh && i < fresh->placed.size() && fresh->placed[i].cavity < 0;
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
    endPanel();
}

void hollowRight(App& app, Document& doc, float h) {
    beginPanel("hollow_right", ImVec2(kRightW, h), col::Panel, ImVec2(18, 22));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        dl->AddLine(p, ImVec2(p.x, p.y + h), col::Border);
    }
    const HollowState& hs = doc.hollow;
    const HollowPlan* plan = hs.planFresh(doc.revision()) ? hs.plan.get() : nullptr;
    groupLabel(plan && !plan->holes.empty() ? "After the holes"
               : plan && plan->hollowing()  ? "After hollowing"
                                            : "What prints");
    ImGui::Dummy(ImVec2(0, 2));
    if (!plan) {
        text(app.planningHollow() ? "Working it out…" : "…", fonts().regular, size::Body, col::TextMuted);
        endPanel();
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
    if (!hs.applied && plan->changes()) {
        ImGui::Dummy(ImVec2(0, 4));
        text(("Nothing changes until you press " + applyLabel(*plan) + ".").c_str(), fonts().regular, size::Help,
             col::TextMuted, true);
    }
    endPanel();
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
    const HollowState& hs = doc.hollow;
    if (hs.planFresh(doc.revision())) {
        const HollowPlan& p = *hs.plan;
        // Each cavity, labelled inside: what it holds as things stand.
        const fruit::HollowResult& cav = p.cavities();
        ImGui::PushFont(fonts().semibold, size::Section);
        for (std::size_t i = 0; i < cav.cavities.size(); ++i) {
            ImVec2 a;
            if (!project(cavityLabelAt(cav, i, p.layer_height), &a)) continue;
            const HollowOutcome l = cavityLabel(p, i, hs.applied);
            const ImVec2 ts = ImGui::CalcTextSize(l.title.c_str());
            // On a dark backing: it may sit over the model or the cut.
            dl->AddRectFilled(ImVec2(a.x - ts.x / 2 - 10, a.y - ts.y / 2 - 5), ImVec2(a.x + ts.x / 2 + 10, a.y + ts.y / 2 + 5),
                              IM_COL32(0x0E, 0x10, 0x14, 210), 8);
            dl->AddText(ImVec2(a.x - ts.x / 2, a.y - ts.y / 2), l.ok ? col::TextSecondary : col::Warn, l.title.c_str());
        }
        ImGui::PopFont();
        // The holes, where they go in.
        const fruit::Transform& t = doc.transform();
        ImGui::PushFont(fonts().regular, size::Help);
        for (std::size_t i = 0; i < p.holes.size() && i < p.placed.size(); ++i) {
            const fruit::Vec3 at = t.apply(p.holes[i].at);
            ImVec2 a, b;
            if (!project(at, &a) || !project({at.x + p.placed[i].radius, at.y, at.z}, &b)) continue;
            const float r = std::max(5.0f, std::hypot(b.x - a.x, b.y - a.y));
            const bool reaches = p.placed[i].cavity >= 0;
            dl->AddCircleFilled(a, r, IM_COL32(0x0E, 0x10, 0x14, 220), 32);
            dl->AddCircle(a, r, reaches ? col::Accent : col::Warn, 32, 2.0f);
            // Placed automatically, each has a job; by hand, it is a hole.
            const char* what = !p.choices.auto_holes ? "hole" : p.holes[i].air ? "air" : "drain";
            const std::string label = reaches ? strf("%s · %g mm", what, p.holes[i].diameter)
                                              : std::string("reaches no hollow");
            const ImVec2 lp(a.x + r + 8, a.y - 8), ls = ImGui::CalcTextSize(label.c_str());
            dl->AddRectFilled(ImVec2(lp.x - 6, lp.y - 3), ImVec2(lp.x + ls.x + 6, lp.y + ls.y + 3),
                              IM_COL32(0x0E, 0x10, 0x14, 210), 6);
            dl->AddText(lp, reaches ? col::TextSecondary : col::Warn, label.c_str());
        }
        ImGui::PopFont();
    }
    // By hand: a click on the model puts a hole in there.
    if (hs.choices.auto_holes) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool inside = m.x >= origin.x && m.y >= origin.y + 44 && m.x < origin.x + size.x && m.y < origin.y + size.y;
    if (inside && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9 &&
        !ImGui::IsAnyItemHovered()) {
        const float nx = 2 * (m.x - origin.x) / size.x - 1, ny = 1 - 2 * (m.y - origin.y) / size.y;
        V3 o, d;
        app.camera().ray(nx, ny, size.x / std::max(1.0f, size.y), &o, &d);
        fruit::Vec3 pos, n;
        if (app.pickModel({o.x, o.y, o.z}, {d.x, d.y, d.z}, &pos, &n)) app.addHoleAt(pos, n);
    }
}

}  // namespace gui::screens::parts
