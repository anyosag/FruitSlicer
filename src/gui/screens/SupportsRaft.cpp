// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Supports · Raft: the job's raft, the presets (built in and yours) and
// every value they set, with a preview and what has been checked (canvas
// boards "Raft settings"). Built-in presets never change: changing a value of
// one asks for the name of a copy of your own.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Orient.h"
#include "RaftPresets.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

using fruit::PadJoin;
using fruit::RaftKind;
using fruit::RaftSpec;
using fruit::RaftWindows;

constexpr float kTagW = 70, kTagH = 20, kControlW = 190;
constexpr const char* kCloseMenus = "\x01";  // App::openRaftMenu(""): put any open preset menu away

// "Preset": as in the built-in preset; "Changed": different from it.
void tag(bool changed) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = changed ? IM_COL32(0x2A, 0x1D, 0x12, 255) : col::Panel;
    const ImU32 border = changed ? IM_COL32(0x5A, 0x3A, 0x1E, 255) : col::BorderStrong;
    const ImU32 fg = changed ? col::Accent : col::TextMuted;
    const char* s = changed ? "Changed" : "Preset";
    dl->AddRectFilled(p, ImVec2(p.x + kTagW, p.y + kTagH), bg, kTagH / 2);
    dl->AddRect(p, ImVec2(p.x + kTagW, p.y + kTagH), border, kTagH / 2);
    ImGui::PushFont(fonts().semibold, size::Label);
    const ImVec2 ts = ImGui::CalcTextSize(s);
    dl->AddText(ImVec2(p.x + std::round((kTagW - ts.x) / 2), p.y + std::round((kTagH - ts.y) / 2)), fg, s);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(kTagW, kTagH));
}

// A slider from lo to hi in steps of `step`, `w` wide; Left and Right move it a
// step. While it is dragged it shows where it is; true once, when it is let go
// (or a key moved it), with *v the new value.
bool valueSlider(const char* id, double* v, double lo, double hi, double step, float w, int stops = 0) {
    static std::map<ImGuiID, double> dragging;
    const float h = 20, r = 8;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const ImGuiID item = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const float x0 = p.x + r, x1 = p.x + w - r;
    auto snap = [&](double x) { return std::clamp(lo + std::round((x - lo) / step) * step, lo, hi); };
    bool done = false;
    double shown = std::clamp(*v, lo, hi);
    if (active) {
        const float t = (ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0);
        dragging[item] = snap(lo + std::clamp(t, 0.0f, 1.0f) * (hi - lo));
        shown = dragging[item];
    } else if (const auto it = dragging.find(item); it != dragging.end()) {
        shown = it->second;
        dragging.erase(it);
        if (std::abs(shown - *v) > 1e-9) {
            *v = shown;
            done = true;
        }
    }
    if (ImGui::IsItemFocused() && !active) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) *v = snap(*v - step), done = true;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) *v = snap(*v + step), done = true;
        shown = *v;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + h / 2;
    const float t = static_cast<float>((shown - lo) / std::max(1e-9, hi - lo));
    const float gx = x0 + (x1 - x0) * t;
    dl->AddRectFilled(ImVec2(p.x, cy - 2), ImVec2(p.x + w, cy + 2), IM_COL32(0x2E, 0x34, 0x3E, 255), 2);
    dl->AddRectFilled(ImVec2(p.x, cy - 2), ImVec2(gx, cy + 2), IM_COL32(0xB8, 0x62, 0x24, 255), 2);
    for (int i = 0; i < stops; ++i) {
        const float x = x0 + (x1 - x0) * (stops > 1 ? static_cast<float>(i) / (stops - 1) : 0.0f);
        dl->AddCircleFilled(ImVec2(x, cy + 9), 2, IM_COL32(0x4A, 0x51, 0x60, 255));
    }
    dl->AddCircleFilled(ImVec2(gx, cy), r, hovered || active ? col::AccentHover : col::Accent, 24);
    dl->AddCircle(ImVec2(gx, cy), r - 1.5f, col::Panel, 24, 3);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return done;
}

// An on/off switch with its word before it. True when clicked.
bool onOff(const char* id, bool on) {
    ImGui::PushFont(fonts().semibold, size::Body);
    const char* word = on ? "On" : "Off";
    const ImVec2 ts = ImGui::CalcTextSize(word);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(ts.x + 46, 22);
    const bool clicked = ImGui::InvisibleButton(id, sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(p.x, p.y + std::round((sz.y - ts.y) / 2)), col::Text, word);
    const ImVec2 a(p.x + ts.x + 8, p.y + 3), b(p.x + ts.x + 42, p.y + 19);
    dl->AddRectFilled(a, b, on ? IM_COL32(0xB8, 0x62, 0x24, 255) : col::BorderStrong, 8);
    const float kx = on ? b.x - 8 : a.x + 8;
    dl->AddCircleFilled(ImVec2(kx, (a.y + b.y) / 2), 6, col::Text, 16);
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// Segments side by side; true with *sel the one clicked.
bool segments(const char* id, const std::vector<const char*>& opts, int* sel) {
    ImGui::PushID(id);
    bool changed = false;
    for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
        if (i) ImGui::SameLine(0, 4);
        if (segmentButton(opts[i], i == *sel) && i != *sel) *sel = i, changed = true;
    }
    ImGui::PopID();
    return changed;
}

std::string mm(double v) { return strf("%.1f mm", v); }
std::string deg(double v) { return strf("%.0f°", v); }
std::string pct(double v) { return strf("%.0f%%", v); }

const char* windowsWord(RaftWindows w) {
    switch (w) {
        case RaftWindows::Few: return "Few";
        case RaftWindows::Some: return "Some";
        case RaftWindows::EveryTip: return "Every tip";
        default: return "None";
    }
}

// What the preview and the checks know about the raft on the job's trunks.
struct Preview {
    const App::RaftPreview* rp = nullptr;  // the raft round the trunks, and its picture
    bool have = false;
    bool made_last = false;  // the supports are out of date: their trunks still say where the raft goes
    bool drawing = false;    // a newer picture is being drawn: this one is the last drawn
};

constexpr float kPictureH = 230;

Preview previewFor(App& app, const Document& doc, const RaftSpec& s, double min_thick, ImVec2 box) {
    Preview p;
    const float fb = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    bool current = true;
    p.rp = &app.raftPreview(s, min_thick, static_cast<int>(std::lround(box.x * fb)), static_cast<int>(std::lround(box.y * fb)),
                            &current);
    p.have = p.rp->have;
    p.drawing = !current;
    p.made_last = doc.supports.result && !doc.supports.fresh(doc.revision(), doc.supportSettings());
    return p;
}

// A small cut through the edge: what each preset's raft looks like across it.
void edgeThumb(const RaftSpec& s, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::Panel, 6);
    const float k = 10;
    const float base = p.y + size.y - 4;
    std::vector<fruit::Vec2> prof = fruit::raftEdgeProfile(s, s.thickness);
    const double in = prof.empty() ? 0.0 : prof.front().x;
    const float x0 = p.x + 6 + static_cast<float>(std::max(0.0, -in)) * k;
    std::vector<ImVec2> pts;
    for (const fruit::Vec2& q : prof) {
        const double d = std::min(q.x, 6.5);
        pts.push_back(ImVec2(x0 + static_cast<float>(d) * k, base - static_cast<float>(q.y) * k));
    }
    if (!pts.empty() && prof.back().y > 0) pts.push_back(ImVec2(x0 + 6.5f * k, base));
    pts.push_back(ImVec2(pts.front().x, base));
    dl->PushClipRect(p, ImVec2(p.x + size.x, p.y + size.y), true);
    dl->AddRectFilled(ImVec2(p.x, base), ImVec2(p.x + size.x, base + 4), IM_COL32(0x3A, 0x40, 0x4B, 255));
    if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), IM_COL32(0xAE, 0xB6, 0xC4, 255));
    dl->PopClipRect();
    ImGui::Dummy(size);
}

// ---- The presets ----

std::string g_delete;  // the preset whose Delete… asks first

void presetMenu(App& app, const std::string& name, bool own) {
    const RaftPresets& rp = app.raftPresets();
    if (own && ImGui::MenuItem("Rename…")) app.askRaftRename(name);
    if (ImGui::MenuItem("Make a copy…")) app.askRaftCopy(name);
    if (ImGui::MenuItem("Start new jobs with this raft", nullptr, rp.new_jobs == name)) app.setNewJobsRaft(name);
    ImGui::Separator();
    if (own) {
        ImGui::PushStyleColor(ImGuiCol_Text, col::Warn);
        if (ImGui::MenuItem("Delete…")) g_delete = name;
        ImGui::PopStyleColor();
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 230);
        ImGui::PushFont(fonts().regular, size::Help);
        ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
        ImGui::TextWrapped("Built in: can't be changed, renamed or deleted. A copy is yours to change.");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
    }
}

void presetItem(App& app, const std::string& name, const std::string& about, const RaftSpec& s, bool own,
                const std::string& current, float w, bool open_menu, bool close_menus) {
    ImGui::PushID(name.c_str());
    const bool selected = name == current;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 56;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool clicked = ImGui::InvisibleButton("item", ImVec2(w - 34, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    if (selected || hovered)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), selected ? col::Active : col::Panel, 10);
    if (selected) dl->AddRect(p, ImVec2(p.x + w, p.y + h), col::BorderStrong, 10);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 7, p.y + 9));
    edgeThumb(s, ImVec2(64, 38));
    ImGui::SetCursorScreenPos(ImVec2(p.x + 82, p.y + 8));
    ImGui::PushFont(fonts().semibold, size::Body);
    dl->AddText(ImGui::GetCursorScreenPos(), col::Text, name.c_str());
    const float nw = ImGui::CalcTextSize(name.c_str()).x;
    ImGui::PopFont();
    if (app.raftPresets().new_jobs == name) {
        ImGui::PushFont(fonts().regular, size::Label);
        dl->AddText(ImVec2(p.x + 82 + nw + 8, p.y + 10), col::TextMuted, "new jobs");
        ImGui::PopFont();
    }
    ImGui::PushFont(fonts().regular, size::Help);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 82, p.y + 28));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 82 - 34);
    ImGui::TextWrapped("%s", about.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // The ··· button.
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - 30, p.y + 16));
    const bool dots = ImGui::InvisibleButton("menu", ImVec2(24, 24));
    const bool dots_hover = ImGui::IsItemHovered();
    const bool open = ImGui::IsPopupOpen("preset_menu");
    if (dots_hover || open) dl->AddRectFilled(ImVec2(p.x + w - 30, p.y + 16), ImVec2(p.x + w - 6, p.y + 40), col::BorderStrong, 6);
    for (int i = 0; i < 3; ++i)
        dl->AddCircleFilled(ImVec2(p.x + w - 24 + i * 6.0f, p.y + 28), 1.8f, dots_hover || open ? col::Text : col::TextMuted);
    if (dots || right || open_menu) ImGui::OpenPopup("preset_menu");
    // The menu opens under its ··· button, wherever it was asked for from.
    ImGui::SetNextWindowPos(ImVec2(p.x + w - 30, p.y + 42), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("preset_menu")) {
        if (close_menus) ImGui::CloseCurrentPopup();
        presetMenu(app, name, own);
        ImGui::EndPopup();
    }
    if (clicked && !selected) app.useRaftPreset(name);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 4));
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::PopID();
}

void presetsColumn(App& app, const std::string& current, const std::string& menu_for, float w) {
    text("Presets", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 4));
    groupLabel("Built in");
    for (const fruit::RaftPreset& p : fruit::builtInRaftPresets())
        presetItem(app, p.name, p.about, p.spec, false, current, w, p.name == menu_for, menu_for == kCloseMenus);
    ImGui::Dummy(ImVec2(0, 6));
    groupLabel("Yours");
    const auto& own = app.raftPresets().own;
    if (own.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        text("None yet. Change a value of a built-in preset, or Make a copy from its ··· menu.", fonts().regular,
             size::Help, col::TextMuted, true);
        ImGui::PopTextWrapPos();
    }
    for (const OwnRaftPreset& p : own) {
        const std::string about = p.from.empty() ? std::string("Yours") : "From " + p.from;
        presetItem(app, p.name, about, p.spec, true, current, w, p.name == menu_for, menu_for == kCloseMenus);
    }
}

// ---- The settings ----

struct Rows {
    App& app;
    RaftSpec now;            // what the page shows (with a change waiting for a name, that change)
    const RaftSpec* origin;  // the built-in preset "Preset" and "Changed" compare with (null: none)
    float w;
    bool changed = false;    // a value was changed this frame: `now` is what it should become
};

// One setting: label and help on the left; on the right its value, the tag and the control.
void row(Rows& r, const char* label, const std::string& help, const std::string& value, bool differs,
         const std::function<void(float)>& control, bool tall = true) {
    ImGui::PushID(label);
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + r.w, a.y), col::Border);
    }
    ImGui::SetCursorPos(ImVec2(x0, y0 + 9));
    const float left_w = r.w - kControlW - 18;
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(x0 + left_w);
    text(label, fonts().semibold, size::Body, col::Text);
    ImGui::PushFont(fonts().regular, size::Help);
    ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
    ImGui::TextWrapped("%s", help.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const float text_bottom = ImGui::GetCursorPosY();
    const float rx = x0 + r.w - kControlW;
    ImGui::SetCursorPos(ImVec2(rx, y0 + 8));
    text(value.c_str(), fonts().semibold, size::Body, differs ? col::Accent : col::Text);
    if (r.origin) {
        ImGui::SetCursorPos(ImVec2(x0 + r.w - kTagW, y0 + 7));
        tag(differs);
    }
    ImGui::SetCursorPos(ImVec2(rx, y0 + (tall ? 34 : 34)));
    control(kControlW);
    const float bottom = std::max(text_bottom, ImGui::GetCursorPosY()) + 8;
    ImGui::SetCursorPos(ImVec2(x0, bottom));
    ImGui::Dummy(ImVec2(r.w, 0));
    ImGui::PopID();
}

void sliderRow(Rows& r, const char* label, const std::string& help, double RaftSpec::*field, double lo, double hi,
               double step, const std::function<std::string(double)>& show) {
    double v = r.now.*field;
    const bool differs = r.origin && std::abs(r.origin->*field - v) > 1e-9;
    double shown = v;
    row(r, label, help, show(v), differs, [&](float w) {
        const float sx = ImGui::GetCursorScreenPos().x;
        if (valueSlider("##s", &shown, lo, hi, step, w)) {
            r.now.*field = shown;
            r.changed = true;
        }
        ImGui::PushFont(fonts().regular, size::Label);
        const ImVec2 a(sx, ImGui::GetCursorScreenPos().y);
        ImGui::GetWindowDrawList()->AddText(ImVec2(a.x, a.y + 2), col::TextMuted, show(lo).c_str());
        const std::string h = show(hi);
        ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + w - ImGui::CalcTextSize(h.c_str()).x, a.y + 2), col::TextMuted,
                                            h.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(w, 16));
    });
}

void settingsColumn(Rows& r, const Preview& pv, double min_thick, int long_layers) {
    RaftSpec& s = r.now;
    const bool full = s.kind == RaftKind::Full;
    const bool edge = full || s.frame;
    auto group = [&](const char* g) {
        ImGui::Dummy(ImVec2(0, 8));
        groupLabel(g);
        ImGui::Dummy(ImVec2(0, 2));
    };
    group("Shape");
    {
        int k = full ? 1 : 0;
        row(r, "Kind", full ? "One base under all the trunks." : "A pad under each trunk, joined by bars.",
            full ? "Full" : "Pads", r.origin && r.origin->kind != s.kind, [&](float) {
                if (segments("kind", {"Pads", "Full"}, &k)) {
                    s.kind = k ? RaftKind::Full : RaftKind::Pads;
                    r.changed = true;
                }
            });
    }
    const double in = fruit::sideInset(s.thickness, s.side_angle);
    const double plate_reach = s.reach - in;
    sliderRow(r, "Reach past the trunks",
              strf("How far the top edge goes past each trunk's foot. At the plate: %.1f mm.", plate_reach),
              &RaftSpec::reach, 0, 5, 0.1, mm);
    sliderRow(r, full ? "Thickness" : "Thickness",
              strf("%sNever thinner than the printer's %d long-exposure layers: %.2f mm.",
                   full ? "" : "The pads and bars. ", long_layers, min_thick),
              &RaftSpec::thickness, std::max(0.4, std::ceil(min_thick * 10 - 1e-9) / 10), 4, 0.1, mm);
    auto sideHelp = [](double inset, const char* what) {
        if (std::abs(inset) < 1e-6) return std::string("From the plate: 90° is a straight side.");
        if (inset > 0) return strf("From the plate. Under 90° %s base tucks in, %.1f mm here; over 90° it sticks out.", what, inset);
        return strf("From the plate. Over 90° %s base sticks out, %.1f mm here; under 90° it tucks in.", what, -inset);
    };
    sliderRow(r, "Side angle", sideHelp(in, full ? "the" : "each pad's"), &RaftSpec::side_angle, 45, 135, 1, deg);
    row(r, "Rounded edges",
        s.rounded ? "Every edge but the ones on the plate, so prying doesn't start a crack at a corner."
                  : "Off: sharp edges, as FruitSlicer's first rafts have.",
        "", r.origin && r.origin->rounded != s.rounded, [&](float) {
            if (onOff("##rounded", s.rounded)) {
                s.rounded = !s.rounded;
                r.changed = true;
            }
        });
    if (edge)
        sliderRow(r, "Bridge gaps up to",
                  "Narrower gaps between trunks are filled in, so the raft follows the supports.", &RaftSpec::gap_fill, 0,
                  30, 1, [](double v) { return strf("%.0f mm", v); });
    if (!full) {
        group("Bars");
        sliderRow(r, "Bar width", "Thin bars snap when a pad is pried, so the pads come off one by one; wide ones lift "
                  "the whole raft.", &RaftSpec::bar_width, 0.6, 3, 0.1, mm);
        sliderRow(r, "Bar height", strf("Part of the thickness: %.0f%% is %.1f mm.", s.bar_pct, s.bar_pct / 100 * s.thickness),
                  &RaftSpec::bar_pct, 30, 100, 5, pct);
        int j = s.join == PadJoin::Grid ? 1 : 0;
        row(r, "Join pads", "Grid joins every neighbour: stiffer, more resin.", j ? "Grid" : "Fewest bars",
            r.origin && r.origin->join != s.join, [&](float) {
                if (segments("join", {"Fewest bars", "Grid"}, &j)) {
                    s.join = j ? PadJoin::Grid : PadJoin::Fewest;
                    r.changed = true;
                }
            });
        group("Frame");
        row(r, "Frame round the outside", "Follows the supports' outline, and brings the lip and windows to pads.", "",
            r.origin && r.origin->frame != s.frame, [&](float) {
                if (onOff("##frame", s.frame)) {
                    s.frame = !s.frame;
                    r.changed = true;
                }
            });
        if (s.frame) {
            sliderRow(r, "Frame width", "Across the frame's top, lip included.", &RaftSpec::frame_width, 2, 6, 0.1, mm);
            sliderRow(r, "Gap to the pads", "Between the frame and the nearest pads; links bridge it.",
                      &RaftSpec::frame_gap, 0.5, 5, 0.1, mm);
            sliderRow(r, "Frame height", "Taller than the pads, so its windows have a roof.", &RaftSpec::frame_height,
                      1, 4, 0.1, mm);
            sliderRow(r, "Frame side angle", sideHelp(fruit::sideInset(s.frame_height, s.frame_angle), "the frame's"),
                      &RaftSpec::frame_angle, 45, 135, 1, deg);
        }
    }
    if (edge) {
        group("Lip");
        sliderRow(r, "Lip height",
                  s.lip_height > 0 ? strf("A rounded rim that stiffens the raft, %.1f mm wide at this height. 0: no lip.",
                                          fruit::lipWidth(s.lip_height))
                                   : std::string("A rounded rim round the top stiffens the raft. 0: no lip."),
                  &RaftSpec::lip_height, 0, 1.5, 0.1, mm);
        group("Windows");
        {
            double stop = static_cast<double>(static_cast<int>(s.windows));
            std::string value = windowsWord(s.windows);
            if (pv.have && s.windows != RaftWindows::None) value += strf(" · %d", static_cast<int>(pv.rp->shape.windows.size()));
            row(r, "How many", "Arched pockets at the plate for a screwdriver, only at lobe tips along the sides.", value,
                r.origin && r.origin->windows != s.windows, [&](float w) {
                    const float sx = ImGui::GetCursorScreenPos().x;
                    if (valueSlider("##windows", &stop, 0, 3, 1, w, 4)) {
                        s.windows = static_cast<RaftWindows>(static_cast<int>(std::lround(stop)));
                        r.changed = true;
                    }
                    ImGui::PushFont(fonts().regular, size::Label);
                    const ImVec2 a(sx, ImGui::GetCursorScreenPos().y);
                    ImGui::GetWindowDrawList()->AddText(ImVec2(a.x, a.y + 4), col::TextMuted, "None");
                    const float tw = ImGui::CalcTextSize("Every tip").x;
                    ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + w - tw, a.y + 4), col::TextMuted, "Every tip");
                    ImGui::PopFont();
                    ImGui::Dummy(ImVec2(w, 18));
                });
        }
        if (s.windows != RaftWindows::None) {
            const double h = full ? s.thickness : s.frame_height;
            sliderRow(r, "Window height",
                      strf("Part of the %s: %.0f%% is %.1f mm, leaving %.1f mm of roof.", full ? "thickness" : "frame's height",
                           s.window_pct, fruit::windowHeight(s), h - fruit::windowHeight(s)),
                      &RaftSpec::window_pct, 30, 75, 5, pct);
        }
    }
}

// ---- The preview ----

void checkLine(int state, const std::string& s, float w) {  // 1 checked, 0 not checked, -1 a problem
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + 7, p.y + 9);
    if (state > 0) {
        dl->AddLine(ImVec2(c.x - 4, c.y), ImVec2(c.x - 1, c.y + 3), col::Ok, 2);
        dl->AddLine(ImVec2(c.x - 1, c.y + 3), ImVec2(c.x + 5, c.y - 4), col::Ok, 2);
    } else if (state < 0) {
        dl->AddCircleFilled(c, 6, col::Warn, 16);
        dl->AddText(ImVec2(c.x - 2, c.y - 8), col::App, "!");
    } else {
        dl->AddCircle(c, 5, col::TextMuted, 16, 1.5f);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 22, p.y));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 22);
    text(s.c_str(), fonts().regular, size::Help, state < 0 ? col::Warn : col::TextSecondary, true);
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 3));
}

void pictureView(const Preview& pv, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (pv.rp && pv.rp->tex)
        dl->AddImageRounded(static_cast<ImTextureID>(static_cast<intptr_t>(pv.rp->tex)), p, ImVec2(p.x + size.x, p.y + size.y),
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 12);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), col::Border, 12);
    if (pv.drawing) {  // the last picture stays until the new one is ready
        ImGui::PushFont(fonts().regular, size::Label);
        const char* t = "Drawing the new raft…";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddRectFilled(ImVec2(p.x + 10, p.y + 10), ImVec2(p.x + 22 + ts.x, p.y + 16 + ts.y), IM_COL32(0x0E, 0x10, 0x14, 210), 6);
        dl->AddText(ImVec2(p.x + 16, p.y + 13), col::TextSecondary, t);
        ImGui::PopFont();
    }
    ImGui::Dummy(size);
}

// What the picture shows: a swatch of each, and its name.
void pictureKey(bool windows, float w) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(fonts().regular, size::Help);
    const float h = ImGui::GetTextLineHeight();
    float x = p.x;
    auto item = [&](const std::function<void(ImVec2)>& swatch, const char* label) {
        swatch(ImVec2(x, p.y + h / 2));
        dl->AddText(ImVec2(x + 20, p.y), col::TextMuted, label);
        x += 20 + ImGui::CalcTextSize(label).x + 16;
    };
    item([&](ImVec2 c) { dl->AddRectFilled(ImVec2(c.x, c.y - 6), ImVec2(c.x + 14, c.y + 6), IM_COL32(0xAE, 0xB6, 0xC4, 255), 2); },
         "raft");
    if (windows)
        item([&](ImVec2 c) {
            dl->AddRectFilled(ImVec2(c.x, c.y - 6), ImVec2(c.x + 14, c.y + 6), IM_COL32(0xAE, 0xB6, 0xC4, 255), 2);
            dl->AddRectFilled(ImVec2(c.x + 4, c.y - 6), ImVec2(c.x + 10, c.y + 3), IM_COL32(0x0E, 0x10, 0x14, 140), 1.5f);
        }, "windows, under its edge");
    item([&](ImVec2 c) { dl->AddCircleFilled(ImVec2(c.x + 7, c.y), 5, IM_COL32(0x7D, 0x86, 0x96, 255), 16); },
         "trunks' feet");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, h));
}

void cutView(const RaftSpec& s, double height, ImVec2 size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::Panel, 12);
    const std::vector<fruit::Vec2> prof = fruit::raftEdgeProfile(s, height);
    if (prof.empty()) {
        ImGui::Dummy(size);
        return;
    }
    double top = 0;
    for (const fruit::Vec2& q : prof) top = std::max(top, q.y);
    const double in = prof.front().x;
    const double span = 7.0 + std::max(0.0, -in);
    const float k = std::min((size.x - 60) / static_cast<float>(span), (size.y - 44) / static_cast<float>(top + 0.6));
    const float base = p.y + size.y - 26;
    const float x0 = p.x + 30 + static_cast<float>(std::max(0.0, -in)) * k;
    std::vector<ImVec2> pts;
    for (const fruit::Vec2& q : prof) pts.push_back(ImVec2(x0 + static_cast<float>(std::min(q.x, span)) * k, base - static_cast<float>(q.y) * k));
    if (prof.back().y > 0) pts.push_back(ImVec2(x0 + static_cast<float>(span) * k, base));
    pts.push_back(ImVec2(pts.front().x, base));
    dl->PushClipRect(p, ImVec2(p.x + size.x, p.y + size.y), true);
    dl->AddRectFilled(ImVec2(p.x, base), ImVec2(p.x + size.x, base + 10), IM_COL32(0x3A, 0x40, 0x4B, 255));
    dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), IM_COL32(0xAE, 0xB6, 0xC4, 255));
    dl->AddLine(ImVec2(pts.front().x, base + 1), ImVec2(p.x + size.x, base + 1), col::Accent, 2.5f);  // bonded
    dl->PopClipRect();
    ImGui::PushFont(fonts().regular, size::Label);
    dl->AddText(ImVec2(p.x + 12, p.y + size.y - 20), col::TextMuted,
                strf("Cut through the edge: %.1f mm thick, side %.0f°%s", s.kind == RaftKind::Pads && s.frame ? s.frame_height : height,
                     s.kind == RaftKind::Pads && s.frame ? s.frame_angle : s.side_angle,
                     s.lip_height > 0 && (s.kind == RaftKind::Full || s.frame) ? strf(", lip %.1f mm", s.lip_height).c_str() : "")
                    .c_str());
    ImGui::PopFont();
    ImGui::Dummy(size);
}

void previewColumn(const RaftSpec& s, const Preview& pv, double min_thick, int long_layers, float w) {
    text("Preview", fonts().semibold, size::Section, col::Text);
    ImGui::Dummy(ImVec2(0, 4));
    if (pv.have) {
        pictureView(pv, ImVec2(w, kPictureH));
        ImGui::Dummy(ImVec2(0, 6));
        pictureKey(!pv.rp->shape.windows.empty(), w);
        ImGui::Dummy(ImVec2(0, 2));
        const fruit::BBox2 b = fruit::bbox(pv.rp->shape.top.empty() ? pv.rp->shape.base : pv.rp->shape.top);
        text(strf("%s on the plate · raft resin %.1f ml%s", dims2(b.size().x, b.size().y).c_str(), pv.rp->shape.volume / 1000,
                  pv.made_last ? " · on the supports made last" : "")
                 .c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        text("Make supports to see this raft round the job's trunks.", fonts().regular, size::Help, col::TextMuted, true);
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, 8));
    cutView(s, s.thickness, ImVec2(w, 150));
    ImGui::Dummy(ImVec2(0, 10));
    // What has been checked, and what hasn't.
    beginCard("raft_checks", ImVec2(w, 0), 12, col::Panel, col::Border);
    const float iw = ImGui::GetContentRegionAvail().x;
    groupLabel("Checked");
    const double h = std::max(s.thickness, min_thick);
    if (s.thickness + 1e-9 >= min_thick)
        checkLine(1, strf("At least %.2f mm thick: the %d long-exposure layers.", min_thick, long_layers), iw);
    else
        checkLine(-1, strf("Thinner than the %d long-exposure layers: it is made %.2f mm.", long_layers, h), iw);
    const bool full = s.kind == RaftKind::Full;
    const double base_past = s.reach - fruit::sideInset(h, s.side_angle);
    if (base_past >= 0)
        checkLine(1, strf("The %s reaches %.1f mm past every trunk's foot.", full ? "base" : "base of each pad", base_past), iw);
    else
        checkLine(-1, strf("The %s stops %.1f mm short of the trunks' feet.", full ? "base" : "base of each pad", -base_past), iw);
    if ((full || s.frame) && s.windows != RaftWindows::None) {
        const double roof = (full ? h : s.frame_height) - fruit::windowHeight(s);
        if (roof + 1e-9 >= fruit::kMinWindowRoof)
            checkLine(1, strf("Every window keeps %.1f mm of roof (0.5 mm at least).", roof), iw);
        else
            checkLine(-1, strf("The windows' roof is only %.1f mm: under 0.5 mm it may crack.", roof), iw);
    }
    if (pv.have) {
        if (pv.rp->shape.pieces <= 1) checkLine(1, "One piece on the plate.", iw);
        else checkLine(-1, strf("%d separate pieces on the plate.", pv.rp->shape.pieces), iw);
    }
    ImGui::Dummy(ImVec2(0, 4));
    groupLabel("Not checked");
    checkLine(0, "How well it holds mid-print, or how easily it comes off. None of these sizes is measured yet.", iw);
    endCard();
}

// ---- The name prompt ----

void namePrompt(App& app) {
    auto& pr = app.raftPrompt();
    static char buf[128];
    static const void* for_prompt = nullptr;
    static std::string for_name;
    if (!pr) {
        for_prompt = nullptr;
        return;
    }
    using K = App::RaftNamePrompt::Kind;
    if (!ImGui::IsPopupOpen("raft_name")) ImGui::OpenPopup("raft_name");
    const ImVec2 vp = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(vp, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    if (ImGui::BeginPopupModal("raft_name", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        const bool fresh = for_prompt != &*pr || for_name != pr->of;
        if (fresh) {
            std::strncpy(buf, pr->name.c_str(), sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            for_prompt = &*pr;
            for_name = pr->of;
        }
        const std::string title = pr->kind == K::SaveChange ? pr->of + " is built in"
                                  : pr->kind == K::Copy     ? "Make a copy of " + (pr->of.empty() ? std::string("this job's raft") : pr->of)
                                                            : "Rename " + pr->of;
        text(title.c_str(), fonts().bold, size::Section + 1, col::Text);
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 390);
        if (pr->kind == K::SaveChange)
            text(pr->change.empty() ? "Built-in presets can't be changed. Save your changes as a preset of your own?"
                                    : ("Built-in presets can't be changed. Save your change, " + pr->change +
                                       ", as a preset of your own?")
                                          .c_str(),
                 fonts().regular, size::Body, col::TextSecondary, true);
        else if (pr->kind == K::Copy)
            text("The copy is yours: every value in it can be changed.", fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 8));
        text("Name", fonts().regular, size::Help, col::TextMuted);
        ImGui::SetNextItemWidth(390);
        if (fresh) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", buf, sizeof buf,
                                            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
        pr->name = buf;
        if (pr->kind != K::Rename) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Checkbox("Start new jobs with this raft", &pr->new_jobs);
        }
        const std::string problem = app.raftPromptProblem();
        if (!problem.empty()) text(problem.c_str(), fonts().regular, size::Help, col::Warn);
        ImGui::Dummy(ImVec2(0, 10));
        const char* cancel = pr->kind == K::SaveChange ? "Undo the change" : "Cancel";
        const char* save = pr->kind == K::Rename ? "Rename" : "Save copy";
        ImGui::PushFont(fonts().semibold, size::Body);
        const float cw = ImGui::CalcTextSize(cancel).x + 30, sw = ImGui::CalcTextSize(save).x + 34;
        ImGui::PopFont();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 390 - cw - sw - 10);
        bool close = false;
        if (secondaryButton(cancel, ImVec2(cw, 36))) {
            app.cancelRaftPrompt();
            close = true;
        }
        ImGui::SameLine(0, 10);
        if ((primaryButton(save, ImVec2(sw, 36), problem.empty()) || (enter && problem.empty())) && app.finishRaftPrompt())
            close = true;
        if (close || !app.raftPrompt()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void deletePrompt(App& app) {
    if (g_delete.empty()) return;
    if (!ImGui::IsPopupOpen("raft_delete")) ImGui::OpenPopup("raft_delete");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    if (ImGui::BeginPopupModal("raft_delete", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        text(("Delete " + g_delete + "?").c_str(), fonts().bold, size::Section + 1, col::Text);
        ImGui::Dummy(ImVec2(0, 6));
        text("Jobs made with it keep their raft.", fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 352 - 90 - 90 - 10);
        if (secondaryButton("Cancel", ImVec2(90, 36))) {
            g_delete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, 10);
        if (primaryButton("Delete", ImVec2(90, 36))) {
            app.deleteRaftPreset(g_delete);
            g_delete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

}  // namespace

void supportsRaft(App& app, Document& doc, ImVec2 size) {
    beginPanel("raft_page", ImVec2(0, size.y), col::App, ImVec2(28, 18));
    const SupportChoices c = app.supportChoicesShown();  // the job's raft, or the selected part's
    const auto& pr = app.raftPrompt();
    const bool waiting = pr && pr->kind == App::RaftNamePrompt::Kind::SaveChange;
    const std::string current = c.raft_preset.name;
    const std::string origin_name = presetOrigin(current, app.raftPresets());
    const fruit::RaftPreset* origin = fruit::builtInRaftPreset(origin_name);
    const int long_layers = doc.printer.bottom_layers + doc.printer.transition_layers;
    const double min_thick = fruit::minRaftThickness(doc.printer.bottom_layers, doc.printer.transition_layers, kLayerHeight);
    Rows rows{app, waiting ? pr->spec : c.raft, origin ? &origin->spec : nullptr, 0};
    const float preview_w = ImGui::GetContentRegionAvail().x - 270 - std::min(470.0f, std::floor((ImGui::GetContentRegionAvail().x - 270 - 48) * 0.58f)) - 48 - 12;
    const Preview pv = previewFor(app, doc, rows.now, min_thick, ImVec2(preview_w, kPictureH));

    // ---- Header ----
    if (linkButton("‹ Back to supports")) app.showRaft(false);
    ImGui::Dummy(ImVec2(0, 2));
    const float ty = ImGui::GetCursorPosY();
    text("Raft settings", fonts().bold, size::DialogTitle + 2, col::Text);
    // Which raft: with several parts, the selected part's raft, or every raft with the job's settings.
    std::string whose = doc.name;
    if (doc.partCount() > 1) {
        const auto& r = doc.supports.result;
        const std::size_t raft = app.raftShown();
        if (app.supportsForPart())
            whose = r && raft < r->rafts.size() && r->rafts[raft].parts.size() > 1
                        ? strf("raft %zu (%s and the parts it shares with)", raft + 1, doc.part().name().c_str())
                        : doc.part().name() + "'s raft";
        else
            whose = "every raft with All parts' settings";
    }
    const std::string for_what =
        pv.have ? strf("For: %s · %d trunks on %s", whose.c_str(), static_cast<int>(pv.rp->shape.trunks.size()),
                       doc.partCount() > 1 && !app.supportsForPart() ? "the first" : "the raft")
                : strf("For: %s", whose.c_str());
    text(for_what.c_str(), fonts().regular, size::Body, col::TextMuted);
    {
        const float right = ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(fonts().bold, size::Body);
        const float aw = ImGui::CalcTextSize("Apply and remake supports").x + 32;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(right - aw - 10, ty + 2));
        if (primaryButton("Apply and remake supports", ImVec2(aw, 38))) app.applyRaft();
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ty + 52));
    }
    ImGui::Dummy(ImVec2(0, 2));
    {
        const float y = ImGui::GetCursorPosY();
        text("None of these sizes is measured on your printer yet.", fonts().regular, size::Body, col::TextSecondary);
        ImGui::SameLine(0, 16);
        ImGui::SetCursorPosY(y - 1);
        tag(false);
        ImGui::SameLine(0, 6);
        ImGui::SetCursorPosY(y);
        text("as in the built-in preset", fonts().regular, size::Help, col::TextMuted);
        ImGui::SameLine(0, 14);
        ImGui::SetCursorPosY(y - 1);
        tag(true);
        ImGui::SameLine(0, 6);
        ImGui::SetCursorPosY(y);
        text("different", fonts().regular, size::Help, col::TextMuted);
    }
    ImGui::Dummy(ImVec2(0, 8));

    // ---- Three columns: the presets, the settings, the preview ----
    const float gap = 24;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float pw = 270, sw = std::min(470.0f, std::floor((avail - pw - 2 * gap) * 0.58f));
    const float vw = avail - pw - sw - 2 * gap;
    const float ch = ImGui::GetContentRegionAvail().y;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::App);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, col::App);
    ImGui::BeginChild("raft_presets", ImVec2(pw, ch));
    presetsColumn(app, current, app.takeRaftMenu(), pw - 12);
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("raft_rows", ImVec2(sw, ch));
    const std::string title = current.empty() ? std::string("This job's own raft") : current;
    text(title.c_str(), fonts().semibold, size::Section, col::Text);
    rows.w = sw - 14;
    ImGui::BeginDisabled(static_cast<bool>(pr));
    settingsColumn(rows, pv, min_thick, long_layers);
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("raft_preview", ImVec2(vw, ch));
    previewColumn(rows.now, pv, min_thick, long_layers, vw - 12);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    if (rows.changed) app.editRaft(rows.now);
    namePrompt(app);
    deletePrompt(app);
    endPanel();
}

}  // namespace gui::screens::parts
