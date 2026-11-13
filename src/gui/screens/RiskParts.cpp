// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// What may not print, on the screens (Risk.h; canvas "Unstable areas in
// FruitSlicer"): the row in the Job panel's Checks with its list of places,
// the Risk view's legend, the numbered marks and the picked place's card
// over the 3D view, the status bar's chip, and the row under Layers'
// "Before you save". Amber at most: nothing here stops a print being saved.
#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Risk.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

ImU32 bandColor(RiskBand b) {
    const unsigned c = riskColour(b);
    return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
}

// A dot in a band's colour. The darkest gets a light rim, so it shows on a dark card.
void bandDot(ImDrawList* dl, ImVec2 c, float r, RiskBand b) {
    dl->AddCircleFilled(c, r, bandColor(b), 20);
    if (b == RiskBand::VeryLikely) dl->AddCircle(c, r + 0.5f, col::Text, 20, 1.0f);
}

// A place's number in a ring: the same in the list, on the model and on its card.
void numberRing(ImDrawList* dl, ImVec2 c, float r, int n, ImU32 ring, ImU32 fill) {
    if (fill) dl->AddCircleFilled(c, r, fill, 24);
    dl->AddCircle(c, r - 0.75f, ring, 24, 1.5f);
    const std::string s = std::to_string(n);
    ImGui::PushFont(fonts().bold, size::Label);
    const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col::Text, s.c_str());
    ImGui::PopFont();
}

// One place in the list: its number, what it is, how likely and what goes with it. True when clicked.
bool placeRow(int i, const RiskPlace& p, bool selected) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float pad = 10, ring = 22, dot = 10;
    const float text_w = w - pad * 2 - ring - 10 - dot - 10;
    ImGui::PushFont(fonts().semibold, size::Body);
    const float th = ImGui::CalcTextSize(p.title.c_str(), nullptr, false, text_w).y;
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Help);
    const float bh = ImGui::CalcTextSize(p.brief.c_str(), nullptr, false, text_w).y;
    ImGui::PopFont();
    const float h = pad * 2 + th + 3 + bh;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(i);
    const bool clicked = ImGui::InvisibleButton("place", ImVec2(w, h));
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(at.x + w, at.y + h);
    dl->AddRectFilled(at, q, hovered ? col::Input : col::Card, 8);
    dl->AddRect(at, q, selected ? col::Accent : col::Border, 8, selected ? 1.5f : 1.0f);
    numberRing(dl, ImVec2(at.x + pad + ring / 2, at.y + h / 2), ring / 2, i + 1, col::Text, 0);
    const float tx = at.x + pad + ring + 10;
    ImGui::PushFont(fonts().semibold, size::Body);
    dl->AddText(nullptr, 0, ImVec2(tx, at.y + pad), col::Text, p.title.c_str(), nullptr, text_w);
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Help);
    dl->AddText(nullptr, 0, ImVec2(tx, at.y + pad + th + 3), col::TextSecondary, p.brief.c_str(), nullptr, text_w);
    ImGui::PopFont();
    bandDot(dl, ImVec2(q.x - pad - dot / 2, at.y + h / 2), dot / 2, p.band);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// A row of the picked place's card: what it is about, and what is so.
void cardRow(const char* label, const std::string& value) {
    if (value.empty()) return;
    const float x = ImGui::GetCursorPosX(), y = ImGui::GetCursorPosY();
    text(label, fonts().regular, size::Help, col::TextMuted);
    ImGui::SetCursorPos(ImVec2(x + 78, y));
    ImGui::PushTextWrapPos(0);
    text(value.c_str(), fonts().regular, size::Help, col::Text, true);
    ImGui::PopTextWrapPos();
}

// The picked place, bottom right of the view: what holds what, when, why, and what goes with it.
void placeCard(App& app, const RiskReport& r, int i, ImVec2 origin, ImVec2 size) {
    const RiskPlace& p = r.places[static_cast<std::size_t>(i)];
    static float height = 230;  // as it came out last frame: the card stands on the model tools' bar
    const float w = std::min(340.0f, size.x - kToolPanelRight - 32);
    if (w < 220) return;  // no room beside the tool panel
    ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - w - 16, origin.y + size.y - kToolsBarH - 8 - height));
    beginCard("risk_place", ImVec2(w, 0), 14, col::Panel, col::Accent);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 5));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        numberRing(dl, ImVec2(at.x + 12, at.y + 12), 12, i + 1, col::Text, 0);
        ImGui::SetCursorScreenPos(ImVec2(at.x + 34, at.y + 3));
        ImGui::PushTextWrapPos(0);
        text(p.title.c_str(), fonts().semibold, size::Body + 1, col::Text, true);
        ImGui::PopTextWrapPos();
        // How likely, in its colour.
        const ImVec2 q = ImGui::GetCursorScreenPos();
        bandDot(dl, ImVec2(at.x + 39, q.y + 8), 5, p.band);
        ImGui::SetCursorScreenPos(ImVec2(at.x + 50, q.y));
        ImGui::PushTextWrapPos(0);
        text(p.brief.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, 2));
    cardRow("Holds", p.holds);
    cardRow("When", p.when);
    cardRow("Why", p.why);
    cardRow("If it goes", p.if_goes);
    if (!p.note.empty() && p.kind != RiskPlace::Kind::Tips) {
        ImGui::Dummy(ImVec2(0, 1));
        text(p.note.c_str(), fonts().regular, size::Help, col::TextMuted, true);
    }
    if (p.layer >= 0) {
        ImGui::Dummy(ImVec2(0, 2));
        const std::string go = strf("Go to layer %d", app.layerOfRisk(p.layer) + 1);
        if (secondaryButton(go.c_str(), ImVec2(-1, 34))) app.showRiskLayer(i);
        tooltip("Shows that layer as the printer lights it, with what may not print coloured.");
    }
    ImGui::PopStyleVar();
    height = ImGui::GetWindowHeight();
    endCard();
}

}  // namespace

void riskChecks(App& app, Document& doc) {
    (void)doc;
    const RiskReport* r = app.risk();
    if (!r) {
        const std::string* why = app.riskProblem();
        checkLine("risk", Tick::Todo, "Nothing likely to be lost in printing",
                  why                  ? "Not checked: the replay of the print stopped (" + *why + ")."
                  : app.checkingRisk() ? std::string("Not checked yet: replaying the print now.")
                                       : std::string("Not checked yet."));
        return;
    }
    const RiskHeadline h = riskHeadline(*r);
    if (app.takeRiskScroll()) ImGui::SetScrollHereY(0.0f);  // asked for from the status bar or the Layers place
    beginCard("risk", ImVec2(0, 0), 12, h.ok ? col::Panel : col::WarnBg, h.ok ? col::Border : col::WarnBorder);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 c(p.x + 9, p.y + 9);
        if (h.ok) {
            dl->AddCircleFilled(c, 9, col::OkBg, 20);
            icon::check(dl, ImVec2(p.x + 3, p.y + 3), 12, col::Ok);
        } else {
            dl->AddCircleFilled(c, 9, IM_COL32(0x4A, 0x3A, 0x12, 255), 20);
            ImGui::PushFont(fonts().bold, size::Help);
            dl->AddText(ImVec2(c.x - 2, c.y - 7), col::Warn, "!");
            ImGui::PopFont();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + 28, p.y));
        ImGui::BeginGroup();
        text(h.title.c_str(), fonts().semibold, size::Body, col::Text);
        text(h.detail.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
        ImGui::EndGroup();
    }
    if (!h.ok) {
        ImGui::Dummy(ImVec2(0, 2));
        if (h.tips_only) {
            if (secondaryButton("Show them on the model", ImVec2(-1, 34))) app.selectRiskPlace(0);
        } else {
            const int picked = app.riskPlace();
            for (int i = 0; i < static_cast<int>(r->places.size()); ++i)
                if (placeRow(i, r->places[static_cast<std::size_t>(i)], picked == i))
                    app.selectRiskPlace(picked == i ? -1 : i);
        }
        ImGui::Dummy(ImVec2(0, 2));
    }
    // What the numbers behind it are worth.
    text(riskStrengthsNote(*r).c_str(), fonts().regular, size::Help, col::TextMuted, true);
    if (r->borrowed && linkButton("Print the tips plate to measure them")) app.openPlate("tips");
    endCard();
}

float riskLegend(App& app, ImVec2 at) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = at.x;
    const float y = at.y + 14;
    ImGui::PushFont(fonts().regular, size::Help);
    auto words = [&](const char* s, ImU32 c) {
        dl->AddText(ImVec2(x, y - 7), c, s);
        x += ImGui::CalcTextSize(s).x + 14;
    };
    if (!app.risk()) {
        const std::string* why = app.riskProblem();
        words(why ? "Not checked: the replay of the print stopped."
                  : app.checkingRisk() ? "Replaying the print layer by layer…" : "Not checked yet.",
              col::TextSecondary);
    } else {
        auto item = [&](const char* label, RiskBand b) {
            bandDot(dl, ImVec2(x + 5, y), 4, b);
            x += 14;
            words(label, col::TextSecondary);
        };
        item("may let go", RiskBand::May);
        item("likely", RiskBand::Likely);
        item("very likely", RiskBand::VeryLikely);
        words("painted on the model: what would be lost", col::TextMuted);
    }
    ImGui::PopFont();
    return x - at.x;
}

void riskOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* vp) {
    (void)doc;
    const RiskReport* r = app.risk();
    if (!r) return;
    auto project = [&](const fruit::Vec3& w, ImVec2* out) {
        const float x = vp[0] * w.x + vp[4] * w.y + vp[8] * w.z + vp[12];
        const float y = vp[1] * w.x + vp[5] * w.y + vp[9] * w.z + vp[13];
        const float c = vp[3] * w.x + vp[7] * w.y + vp[11] * w.z + vp[15];
        if (c <= 1e-4f) return false;
        *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
        return true;
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Every tip that may let go: a dot in its colour, over the view, so it shows from any side and however
    // far out (the tip itself is a third of a millimetre across, and the raft hides it from below).
    for (const RiskTip& t : r->tips) {
        ImVec2 q;
        if (!project(t.at, &q)) continue;
        dl->AddCircleFilled(q, 5.5f, IM_COL32(0x0E, 0x10, 0x14, 255), 20);
        bandDot(dl, q, 4, t.band);
    }
    const int picked = app.riskPlace();
    const int n = static_cast<int>(r->places.size());
    std::vector<ImVec2> at(static_cast<std::size_t>(n));
    std::vector<char> seen(static_cast<std::size_t>(n), 0);
    // Each place's number, where it is; the picked one ringed.
    for (int i = 0; i < n; ++i) {
        ImVec2 q;
        if (!project(r->places[static_cast<std::size_t>(i)].at, &q)) continue;
        at[static_cast<std::size_t>(i)] = q;
        seen[static_cast<std::size_t>(i)] = 1;
        if (i == picked) {
            dl->AddCircle(q, 15, col::Accent, 32, 2.5f);
            dl->AddCircle(q, 18.5f, IM_COL32(0xFF, 0x8F, 0x3F, 90), 32, 2.0f);
        }
        numberRing(dl, q, 11, i + 1, col::Text, IM_COL32(0x18, 0x1B, 0x21, 235));
    }
    // A click (not a drag) on a number picks that place; anywhere else, none.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool inside = m.x >= origin.x && m.y >= origin.y + 50 && m.x < origin.x + size.x && m.y < origin.y + size.y &&
                        ImGui::IsWindowHovered();
    if (inside && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9 &&
        !ImGui::IsAnyItemHovered()) {
        int best = -1;
        float best_d = 16 * 16;
        for (int i = 0; i < n; ++i) {
            if (!seen[static_cast<std::size_t>(i)]) continue;
            const ImVec2 q = at[static_cast<std::size_t>(i)];
            const float d = (q.x - m.x) * (q.x - m.x) + (q.y - m.y) * (q.y - m.y);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        app.pickRiskPlace(best);
    }
    if (picked >= 0) placeCard(app, *r, picked, origin, size);
}

void riskChip(App& app) {
    const RiskReport* r = app.document() ? app.risk() : nullptr;
    if (!r) return;
    const RiskHeadline h = riskHeadline(*r);
    if (h.ok) return;
    ImGui::SameLine(0, 20);
    ImGui::SetCursorPosY(kStatusH / 2 - 15);
    if (chip("risk", h.title.c_str(), col::Warn, col::Warn, col::WarnBg, col::WarnBorder)) app.showRisk();
    tooltip("Found by replaying the print layer by layer. Click to see where on the model. Saving is still allowed.");
}

void riskBeforeSave(App& app) {
    const RiskReport* r = app.risk();
    if (!r) return;
    const RiskHeadline h = riskHeadline(*r);
    if (h.ok) return;
    const std::string body = h.tips_only ? h.detail + " Saving is still allowed."
                                         : "The first to look at: " + r->places.front().title + ". Saving is still allowed.";
    verdictRow("risk", Verdict::Warn, h.title.c_str(), body.c_str());
    if (linkButton("Show them on the model")) app.showRisk();
}

}  // namespace gui::screens::parts
