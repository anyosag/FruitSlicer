// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Supports · advanced: every value the tip planner and the tree
// builder use, where it comes from, and the ones you can change (canvas
// board "Supports · advanced").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

using Kind = AdvancedSetting::Kind;
using Source = AdvancedSetting::Source;

struct BadgeStyle {
    const char* label;
    ImU32 fg, bg, border;
};

BadgeStyle measuredBadge() { return {"Measured", col::Ok, col::OkBg, IM_COL32(0x1E, 0x5A, 0x3E, 255)}; }

BadgeStyle badgeFor(Source s, bool yours) {
    if (yours) return {"Yours", col::Accent, IM_COL32(0x2A, 0x1D, 0x12, 255), IM_COL32(0x5A, 0x3A, 0x1E, 255)};
    switch (s) {
        case Source::Borrowed:
            return {"Borrowed", IM_COL32(0x9D, 0xB7, 0xF5, 255), IM_COL32(0x1B, 0x22, 0x33, 255),
                    IM_COL32(0x2E, 0x3A, 0x57, 255)};
        case Source::Assumption: return {"Assumption", col::Warn, col::WarnBg, col::WarnBorder};
        default: return {"Default", col::TextMuted, col::Panel, col::BorderStrong};
    }
}

constexpr float kBadgeW = 84, kBadgeH = 20;

// A small pill; drawn at the cursor, `w` wide.
void badge(const BadgeStyle& b, float w = kBadgeW) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + kBadgeH), b.bg, kBadgeH / 2);
    dl->AddRect(p, ImVec2(p.x + w, p.y + kBadgeH), b.border, kBadgeH / 2);
    ImGui::PushFont(fonts().semibold, size::Label);
    const ImVec2 ts = ImGui::CalcTextSize(b.label);
    dl->AddText(ImVec2(p.x + std::round((w - ts.x) / 2), p.y + std::round((kBadgeH - ts.y) / 2)), b.fg, b.label);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, kBadgeH));
}

float textWidth(const char* s, ImFont* f, float sz) {
    ImGui::PushFont(f, sz);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    return w;
}

// Words on the current line, centred on the 30 px control row. Drawn
// directly: ImGui would lower text that follows a framed field.
void inlineText(const char* s, ImFont* f, float sz, ImU32 c, float row_y) {
    ImGui::PushFont(f, sz);
    const ImVec2 ts = ImGui::CalcTextSize(s);
    ImGui::SetCursorPosY(row_y);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + std::round((29 - ts.y) / 2)), c, s);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ts.x, 29));
}

const char* tipLabel(int cls) {
    static const char* names[] = {"0.2", "0.3", "0.4", "0.6"};
    return names[std::clamp(cls, 0, 3)];
}

// The value part of a row: how wide it is, and drawing it.
float valueWidth(const AdvancedSetting& s, const AdvancedContext& x) {
    const float field = 62, gap = 6;
    switch (s.kind) {
        case Kind::Fixed: return textWidth(s.text(x).c_str(), fonts().semibold, size::Body);
        case Kind::Toggle:
            return textWidth(s.text(x).c_str(), fonts().semibold, size::Body) +
                   textWidth(s.off_text, fonts().semibold, size::Body) + 2 * 22 + gap;
        case Kind::TipClass: {
            const AdvancedField& f = s.fields[0];
            float w = 0;
            for (int c = static_cast<int>(f.lo); c <= static_cast<int>(f.hi); ++c)
                w += textWidth(tipLabel(c), fonts().semibold, size::Body) + 22 + 4;
            return w + textWidth("mm", fonts().regular, size::Body) + gap;
        }
        case Kind::Number: {
            float w = 0;
            for (const AdvancedField& f : s.fields) {
                if (*f.prefix) w += textWidth(f.prefix, fonts().regular, size::Body) + gap;
                w += field;
                if (*f.suffix) w += gap + textWidth(f.suffix, fonts().regular, size::Body);
                w += gap;
            }
            return w - gap;
        }
    }
    return 0;
}

void drawValue(App& app, const AdvancedSetting& s, const AdvancedContext& x, float row_y) {
    const float gap = 6;
    switch (s.kind) {
        case Kind::Fixed:
            inlineText(s.text(x).c_str(), fonts().semibold, size::Body, col::Text, row_y);
            break;
        case Kind::Toggle: {
            const AdvancedField& f = s.fields[0];
            const bool on = f.get(x.cfg, x.tc) > 0.5;
            ImGui::SetCursorPosY(row_y);
            ImGui::PushID(f.key);
            if (segmentButton(s.text(x).c_str(), on)) app.setAdvanced(f.key, 1);
            ImGui::SameLine(0, gap);
            if (segmentButton(s.off_text, !on)) app.setAdvanced(f.key, 0);
            ImGui::PopID();
            break;
        }
        case Kind::TipClass: {
            const AdvancedField& f = s.fields[0];
            const int now = standardTipClass(x.cfg, static_cast<int>(std::lround(f.get(x.cfg, x.tc))));
            ImGui::PushID(f.key);
            for (int c = static_cast<int>(f.lo); c <= static_cast<int>(f.hi); ++c) {
                ImGui::SetCursorPosY(row_y);
                if (segmentButton(tipLabel(c), c == now)) app.setAdvanced(f.key, c);
                ImGui::SameLine(0, 4);
            }
            ImGui::PopID();
            ImGui::SameLine(0, gap - 4);
            inlineText("mm", fonts().regular, size::Body, col::TextSecondary, row_y);
            break;
        }
        case Kind::Number: {
            for (std::size_t i = 0; i < s.fields.size(); ++i) {
                const AdvancedField& f = s.fields[i];
                if (i > 0) ImGui::SameLine(0, gap);
                if (*f.prefix) {
                    inlineText(f.prefix, fonts().regular, size::Body, col::TextSecondary, row_y);
                    ImGui::SameLine(0, gap);
                }
                ImGui::SetCursorPosY(row_y);
                double v = f.get(x.cfg, x.tc);
                const std::string fmtv = strf("%%.%df", f.decimals);
                if (numberField(f.key, &v, 62, fmtv.c_str(), f.lo, f.hi)) app.setAdvanced(f.key, v);
                if (ImGui::IsItemHovered())
                    tooltip(strf("From %g to %g%s%s", f.lo, f.hi, *f.suffix ? " " : "", f.suffix).c_str());
                if (*f.suffix) {
                    ImGui::SameLine(0, gap);
                    inlineText(f.suffix, fonts().regular, size::Body, col::TextSecondary, row_y);
                }
            }
            break;
        }
    }
}

void row(App& app, const AdvancedSetting& s, const AdvancedContext& x, float w, bool last) {
    ImGui::PushID(s.label);
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    const float vw = valueWidth(s, x);
    const float left_w = w - vw - kBadgeW - 32;
    // Label and help, wrapped to the left part.
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(x0 + left_w);
    text(s.label, fonts().semibold, size::Body, col::Text);
    ImGui::PushFont(fonts().regular, size::Help);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
    ImGui::TextWrapped("%s", s.help(x).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const float text_bottom = ImGui::GetCursorPosY();
    // Value and badge on the right, on the label's line.
    const float row_y = y0 - 4;
    ImGui::SetCursorPos(ImVec2(x0 + w - kBadgeW - 12 - vw, row_y));
    ImGui::BeginGroup();
    drawValue(app, s, x, row_y);
    ImGui::EndGroup();
    ImGui::SetCursorPos(ImVec2(x0 + w - kBadgeW, row_y + 5));
    badge(measuredByResin(s, x.choices) ? measuredBadge() : badgeFor(s.source, advancedChanged(s, x.choices)));
    // Next row below whichever part is taller.
    const float bottom = std::max(text_bottom, row_y + 34);
    ImGui::SetCursorPos(ImVec2(x0, bottom + 6));
    if (!last) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), col::Border);
        ImGui::Dummy(ImVec2(w, 10));
    } else {
        ImGui::Dummy(ImVec2(w, 0));
    }
    ImGui::PopID();
}

void groupCard(App& app, const char* group, const AdvancedContext& x, float w) {
    beginCard(group, ImVec2(w, 0), 18, col::Panel, col::Border);
    text(group, fonts().semibold, size::Section + 1, col::Text);
    ImGui::Dummy(ImVec2(0, 6));
    const auto& all = advancedSettings();
    std::vector<const AdvancedSetting*> rows;
    for (const AdvancedSetting& s : all)
        if (std::string(s.group) == group) rows.push_back(&s);
    const float iw = ImGui::GetContentRegionAvail().x;
    for (std::size_t i = 0; i < rows.size(); ++i) row(app, *rows[i], x, iw, i + 1 == rows.size());
    endCard();
}

}  // namespace

void supportsAdvanced(App& app, Document& doc, ImVec2 size) {
    beginPanel("advanced", ImVec2(0, size.y), col::App, ImVec2(28, 18));
    const SupportChoices& c = doc.supports.choices;
    const fruit::SupportConfig cfg = supportConfig(doc.placement(), c);
    const fruit::TreeConfig tc = treeConfig(c);
    AdvancedContext x{cfg, tc, c, doc.placement()};
    // The raft row's help: the print file's bottom and transition layers.
    x.raft_layers = doc.printer.bottom_layers + doc.printer.transition_layers;
    x.raft_layer_height = doc.printer.layer_height;
    const PrinterSetup& ps = app.printerSetup();
    if (const auto side = hingeChoice(ps.hinge))
        x.hinge = std::string(hingeLabel(*side)) + (ps.orientation_confirmed ? "" : ", waiting");
    if (doc.layers.result) {
        const auto plan = app.printPlan();
        if (plan && plan->ready) {
            x.raft_layers = static_cast<int>(plan->setup.header.print.bottom_layers +
                                             plan->setup.header.print.transition_layers);
            x.raft_layer_height = plan->layer_height;
        }
    }

    // ---- Header ----
    if (linkButton("‹ Back to supports")) app.showAdvanced(false);
    ImGui::Dummy(ImVec2(0, 2));
    const float ty = ImGui::GetCursorPosY();
    text("Advanced support settings", fonts().bold, size::DialogTitle + 2, col::Text);
    text(("For: " + resinLabel(c.resin, c.profile)).c_str(), fonts().regular, size::Body, col::TextMuted);
    {
        const float right = ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(fonts().bold, size::Body);
        const float aw = ImGui::CalcTextSize("Apply and regenerate").x + 32;
        ImGui::PopFont();
        ImGui::PushFont(fonts().semibold, size::Body);
        const float rw = ImGui::CalcTextSize("Reset all").x + 28;
        ImGui::PopFont();
        const float by = ty + 2;
        ImGui::SetCursorPos(ImVec2(right - aw - 10 - rw, by));
        if (secondaryButton("Reset all", ImVec2(rw, 38))) app.resetAdvanced();
        tooltip("Every value on this page back to FruitSlicer's own.");
        ImGui::SameLine(0, 10);
        if (primaryButton("Apply and regenerate", ImVec2(aw, 38))) app.applyAdvanced();
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ty + 52));
    }
    ImGui::Dummy(ImVec2(0, 4));
    // What the badges mean.
    {
        const float y = ImGui::GetCursorPosY();
        text("Every value below is what FruitSlicer uses today.", fonts().regular, size::Body, col::TextSecondary);
        ImGui::SameLine(0, 18);
        struct Key {
            Source s;
            bool yours;
            const char* what;
        };
        const Key keys[] = {{Source::Default, false, "built in"},
                            {Source::Borrowed, false, "from PrusaSlicer until you calibrate"},
                            {Source::Assumption, false, "not measured yet"},
                            {Source::Default, true, "set by you"}};
        for (const Key& k : keys) {
            ImGui::SetCursorPosY(y - 2);
            badge(badgeFor(k.s, k.yours), 76);
            ImGui::SameLine(0, 6);
            ImGui::SetCursorPosY(y);
            text(k.what, fonts().regular, size::Help, col::TextMuted);
            ImGui::SameLine(0, 16);
        }
        if (c.profile.is_object()) {  // the job's resin was measured
            ImGui::SetCursorPosY(y - 2);
            badge(measuredBadge(), 76);
            ImGui::SameLine(0, 6);
            ImGui::SetCursorPosY(y);
            text("on your resin's test plates", fonts().regular, size::Help, col::TextMuted);
        }
        ImGui::NewLine();
    }
    ImGui::Dummy(ImVec2(0, 6));

    // ---- The groups, in two columns ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, col::App);
    ImGui::BeginChild("advanced_rows", ImVec2(0, 0), ImGuiChildFlags_None);
    const float gap = 16;
    const float cw = std::floor((ImGui::GetContentRegionAvail().x - gap) / 2);
    const char* left[] = {"Tips", "Keeping marks off", "Pillars and trunks", "Raft"};
    const char* right[] = {"Where tips go", "Heads", "Braces", "Peel side"};
    ImGui::BeginGroup();
    for (const char* g : left) {
        groupCard(app, g, x, cw);
        ImGui::Dummy(ImVec2(0, gap - 8));
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    for (const char* g : right) {
        groupCard(app, g, x, cw);
        ImGui::Dummy(ImVec2(0, gap - 8));
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    endPanel();
}

}  // namespace gui::screens::parts
