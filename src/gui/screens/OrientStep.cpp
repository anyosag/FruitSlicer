// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Orient: the sides people will see, how it sits now, and the search's
// better ways to stand it (canvas boards "Orient" and "OrientResults").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

// A side button: picked ones carry the accent, like a chosen card.
bool sideButton(const char* label, bool picked, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q, picked ? IM_COL32(0x2A, 0x1E, 0x14, 255) : (hovered ? col::Active : col::Input), 8);
    dl->AddRect(p, q, picked ? col::Accent : col::BorderStrong, 8, picked ? 1.5f : 1.0f);
    ImGui::PushFont(fonts().semibold, size::Body);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) / 2, p.y + (size.y - ts.y) / 2), picked ? col::Text : col::TextSecondary,
                label);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

std::string sidesText(const App& app) {
    std::string picked;
    int n = 0;
    for (int i = 0; i < 6; ++i)
        if (app.sidePicked(static_cast<Side>(i))) {
            picked += std::string(n ? (n == 1 ? " and " : ", ") : "") + sideName(static_cast<Side>(i));
            ++n;
        }
    return n ? picked : "";
}

}  // namespace

void orientLeft(App& app, Document& doc, float h) {
    beginPanel("orient_left", ImVec2(kLeftW, h), col::Panel, ImVec2(18, 22));
    edge(true, false);
    const OrientState& o = doc.orient;
    text("Side you'll see", fonts().semibold, size::Section + 1, col::Text);
    text("Pick the sides people will look at. Tips stay off them, and the search counts them extra.",
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 2));
    const float w = ImGui::GetContentRegionAvail().x, bw = (w - 12) / 3;
    for (int i = 0; i < 6; ++i) {
        const Side s = static_cast<Side>(i);
        if (sideButton(sideName(s), app.sidePicked(s), ImVec2(bw, 40))) app.toggleSide(s);
        if (i % 3 != 2) ImGui::SameLine(0, 6);
    }
    ImGui::Dummy(ImVec2(0, 2));
    const std::string picked = sidesText(app);
    std::string help;
    if (picked.empty() && !o.visible.empty()) {
        help = strf("%zu %s picked earlier turned with the model and no longer faces straight out.", o.visible.size(),
                    o.visible.size() == 1 ? "side" : "sides");
        text(help.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        if (linkButton("Clear picked sides")) app.clearSides();
        help.clear();
    } else if (picked.empty()) {
        help = "None picked, so every side counts the same.";
    } else {
        help = "Tips stay off " + picked + " where they can.";
        if (o.nowFresh(doc.revision()))
            help += strf(" As it sits now, %zu %s on %s.", o.now.cosmetic_tips,
                         o.now.cosmetic_tips == 1 ? "tip lands" : "tips land", o.visible.size() == 1 ? "it" : "them");
    }
    if (!help.empty()) text(help.c_str(), fonts().regular, size::Body, col::TextSecondary, true);

    ImGui::Dummy(ImVec2(0, 10));
    text("Keep-out boxes", fonts().semibold, size::Section + 1, col::Text);
    text("Draw a box around a face or detail that must stay clean, like eyes or lettering.", fonts().regular,
         size::Body, col::TextSecondary, true);
    secondaryButton("Draw a box", ImVec2(0, 0), false);
    tooltip("Boxes arrive with tip editing, in a later update.");
    text("No boxes yet.", fonts().regular, size::Body, col::TextMuted);

    ImGui::Dummy(ImVec2(0, 10));
    text("Hollows", fonts().semibold, size::Section + 1, col::Text);
    beginCard("hollows", ImVec2(0, 0), 14, col::Panel, col::Border);
    const int cav = doc.check.cavities;
    text(cav ? strf("%d closed %s", cav, cav == 1 ? "hollow" : "hollows").c_str() : "No hollows found",
         fonts().semibold, size::Body, col::Text);
    text(cav ? "They trap resin. Drain holes arrive in a later update." : "Hollowing and drain holes arrive in a later update.",
         fonts().regular, size::Body, col::TextSecondary, true);
    endCard();
    endPanel();
}

namespace {

void metricCard(const char* id, const char* label, const std::string& value, const char* help) {
    beginCard(id, ImVec2(0, 0), 13, col::Panel, col::Border);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    text(label, fonts().regular, size::Body + 1, col::Text);
    ImGui::PushFont(fonts().bold, size::Body + 1);
    const float vw = ImGui::CalcTextSize(value.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + w - vw, p.y), col::Text, value.c_str());
    ImGui::PopFont();
    text(help, fonts().regular, size::Body, col::TextSecondary, true);
    endCard();
    ImGui::Dummy(ImVec2(0, 2));
}

}  // namespace

void orientRight(App& app, Document& doc, float h) {
    beginPanel("orient_right", ImVec2(kRightW, h), col::Panel, ImVec2(18, 22));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        dl->AddLine(p, ImVec2(p.x, p.y + h), col::Border);
    }
    groupLabel("As it sits now");
    ImGui::Dummy(ImVec2(0, 2));
    const OrientState& o = doc.orient;
    const bool fresh = o.nowFresh(doc.revision());
    auto v = [&](std::size_t n) { return fresh ? thousands(n) : std::string("…"); };
    metricCard("tips", "Tips", v(o.now.tips), "Fewer tips means fewer marks to sand off.");
    metricCard("islands", "Island tips", v(o.now.island_tips),
               "Spots that start in mid-air. Each needs a tip at exactly the right place.");
    metricCard("biggest", "Biggest layer", fresh ? strf("%s mm²", thousands(static_cast<std::size_t>(std::lround(o.now.biggest_layer))).c_str()) : "…",
               "The biggest layer pulls hardest on the film when it peels.");
    metricCard("height", "Height", fresh ? strf("%.1f mm", o.now.top) : "…",
               fresh ? strf("Taller prints take longer: %s layers at %.2f mm.", thousands(o.now.layers).c_str(), kLayerHeight).c_str()
                     : "Taller prints take longer.");
    metricCard("short", "Layers short of hold", v(o.now.short_of_hold),
               "Layers bigger than their tips can hold. Tilting usually fixes this.");
    if (!fresh) text("Scoring how it sits…", fonts().regular, size::Body, col::TextMuted);
    ImGui::Dummy(ImVec2(0, 4));
    text("Searching tries 64 directions plus this one, then runs the full tip planner on the best 6 and shows you "
         "the three that beat how it sits now.",
         fonts().regular, size::Body, col::TextSecondary, true);
    (void)app;
    endPanel();
}

namespace {

// "215 −83": the value, then the change against now, coloured by whether
// lower is better (it is for every metric here).
void metric(const char* label, const std::string& value, double delta, const std::string& delta_text, float x,
            float y) {
    ImGui::SetCursorPos(ImVec2(x, y));
    text(label, fonts().regular, size::Body, col::TextSecondary);
    ImGui::SetCursorPos(ImVec2(x, y + 18));
    text(value.c_str(), fonts().semibold, size::Body + 1, col::Text);
    if (!delta_text.empty()) {
        ImGui::SameLine(0, 5);
        text(delta_text.c_str(), fonts().medium, size::Body + 1, delta < 0 ? col::Ok : col::Warn);
    }
}

std::string signedCount(double d) { return d == 0 ? std::string() : strf(d < 0 ? "−%.0f" : "+%.0f", std::abs(d)); }

void optionCard(App& app, const char* id, const char* title, const OrientOption& op, const OrientMetrics* now,
                int index, bool recommended, ImVec2 size) {
    const ImU32 border = recommended ? col::Accent : col::Border;
    beginCard(id, size, 16, recommended ? col::Card : col::Panel, border);
    const float w = ImGui::GetContentRegionAvail().x;
    const float y0 = ImGui::GetCursorPosY();
    text(title, fonts().semibold, size::Section + 1, col::Text);
    if (recommended) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - 98, y0 - 3));
        chip("rec", "Recommended", 0, col::OnAccent, col::Accent, 0);
    }
    // The picture from below.
    ImGui::SetCursorPosY(y0 + 32);
    const float ph = 176;
    const ImVec2 pp = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pp, ImVec2(pp.x + w, pp.y + ph), col::Viewport, 10);
    if (const unsigned tex = app.orientPicture(index)) {
        const float iw = std::min(w, ph * 568.0f / 352.0f);
        ImGui::SetCursorScreenPos(ImVec2(pp.x + (w - iw) / 2, pp.y));
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(iw, ph));
    }
    ImGui::PushFont(fonts().regular, size::Body);
    dl->AddText(ImVec2(pp.x + 12, pp.y + ph - 24), col::TextSecondary, "seen from below");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(pp.x, pp.y + ph + 12));
    const OrientMetrics& m = op.m;
    const float x0 = ImGui::GetCursorPosX(), y = ImGui::GetCursorPosY(), x1 = x0 + w / 2;
    auto d = [&](std::size_t a, std::size_t b) { return static_cast<double>(a) - static_cast<double>(b); };
    metric("Tips", thousands(m.tips), now ? d(m.tips, now->tips) : 0, now ? signedCount(d(m.tips, now->tips)) : "", x0, y);
    metric("Island tips", thousands(m.island_tips), now ? d(m.island_tips, now->island_tips) : 0,
           now ? signedCount(d(m.island_tips, now->island_tips)) : "", x1, y);
    std::string area_delta;
    double area_d = 0;
    if (now && now->biggest_layer > 0) {
        area_d = m.biggest_layer / now->biggest_layer - 1;
        if (std::abs(area_d) >= 0.005) area_delta = strf(area_d < 0 ? "−%.0f %%" : "+%.0f %%", std::abs(area_d) * 100);
    }
    metric("Biggest layer", strf("%s mm²", thousands(static_cast<std::size_t>(std::lround(m.biggest_layer))).c_str()), area_d,
           area_delta, x0, y + 44);
    const double dh = now ? m.top - now->top : 0;
    metric("Height", strf("%.1f mm", m.top), dh, now && std::abs(dh) >= 0.05 ? strf(dh < 0 ? "−%.1f" : "+%.1f", std::abs(dh)) : "",
           x1, y + 44);
    metric("Short of hold", strf("%zu %s", m.short_of_hold, m.short_of_hold == 1 ? "layer" : "layers"),
           now ? d(m.short_of_hold, now->short_of_hold) : 0, now ? signedCount(d(m.short_of_hold, now->short_of_hold)) : "",
           x0, y + 88);
    ImGui::SetCursorPos(ImVec2(x0, y + 140));
    text(op.summary.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 4));
    auto angle = [](double a) { return strf(a < -0.005 ? "−%.2f°" : "%.2f°", std::abs(a)); };
    text(("Rotation " + angle(op.rotation_deg.x) + ", " + angle(op.rotation_deg.y) + ", " + angle(op.rotation_deg.z)).c_str(),
         fonts().regular, size::Help, col::TextMuted);
    if (!op.fits) text("Too big for the plate however it is turned.", fonts().regular, size::Help, col::Error, true);
    // The button sits at the bottom of the card.
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 16 - 40);
    const char* label = index < 0 ? "Keep this" : "Use this";
    const float bw = index < 0 ? 96.0f : 86.0f;
    const bool pressed = recommended ? primaryButton(label, ImVec2(bw, 40), op.fits)
                                     : secondaryButton(label, ImVec2(bw, 40), op.fits || index < 0);
    if (pressed) app.useOrientation(index);
    endCard();
}

}  // namespace

void orientResults(App& app, Document& doc, ImVec2 size) {
    beginPanel("orient_results", ImVec2(0, size.y), col::App, ImVec2(24, 26));
    const OrientSearch& r = *doc.orient.results;
    const std::size_t n = r.options.size();
    static const char* counts[] = {"No better way found", "One better way to stand it", "Two better ways to stand it",
                                   "Three better ways to stand it"};
    const float w = ImGui::GetContentRegionAvail().x;
    const float y0 = ImGui::GetCursorPosY();
    text(counts[std::min<std::size_t>(n, 3)], fonts().bold, size::DialogTitle + 2, col::Text);
    // Test captures leave out the time it took, which differs run to run.
    const std::string took = app.options().deterministic ? std::string() : strf(" in %.0f s", std::max(1.0, r.seconds));
    text(strf("Searched %d directions%s and planned tips for the best %d. Each option is compared with how it sits "
              "now. Nothing changes until you pick one, and you can undo.",
              r.directions, took.c_str(), r.finalists)
             .c_str(),
         fonts().regular, size::Body, col::TextSecondary);
    ImGui::PushFont(fonts().semibold, size::Body);
    const float cw = ImGui::CalcTextSize("Change what counts").x + 32;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - cw, y0 + 4));
    if (secondaryButton("Change what counts", ImVec2(cw, 0))) app.showOrientSettings();
    ImGui::SetCursorPosY(y0 + 72);
    const float gap = 16, card_w = (w - 3 * gap) / 4;
    const float card_h = std::min(560.0f, ImGui::GetContentRegionAvail().y - 4);
    static const char* titles[] = {"Option 1", "Option 2", "Option 3"};
    for (std::size_t i = 0; i < n && i < 3; ++i) {
        optionCard(app, titles[i], titles[i], r.options[i], &r.now.m, static_cast<int>(i), i == 0,
                   ImVec2(card_w, card_h));
        ImGui::SameLine(0, gap);
    }
    optionCard(app, "now", "As it sits now", r.now, nullptr, -1, false, ImVec2(card_w, card_h));
    endPanel();
}

}  // namespace gui::screens::parts
