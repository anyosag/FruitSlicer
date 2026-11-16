// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Supports: the settings, for all parts or the selected one, in three
// sections that fold to a line (Tips, Raft and lift, Rafts on the plate); the
// result and its checks, the problems drawer, and the out-of-date state
// (canvas "FruitSlicer Supports panel redesign": All parts, one part, a part
// on the plate, rafts on the plate).
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "RaftPresets.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

bool running(App& app) { return app.jobs().busy() && app.jobs().progress().name == "Generating supports"; }

// Text on the right of the current line, at `y` (cursor y).
void rightText(const char* s, ImFont* f, float size, ImU32 c, float y) {
    ImGui::PushFont(f, size);
    const float w = ImGui::CalcTextSize(s).x;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - w, y));
    text(s, f, size, c);
}

// A button that opens a menu: `label` and a small caret, as wide as they are.
bool presetButton(const char* id, const char* label) {
    ImGui::PushFont(fonts().medium, size::Body);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 sz(ts.x + 40, 30);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, sz);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), hovered ? col::Active : col::Panel, 15);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), col::BorderStrong, 15);
    dl->AddText(ImVec2(p.x + 14, p.y + std::round((sz.y - ts.y) / 2)), col::Text, label);
    const float cx = p.x + sz.x - 17, cy = p.y + sz.y / 2;
    dl->AddTriangleFilled(ImVec2(cx - 4, cy - 2), ImVec2(cx + 4, cy - 2), ImVec2(cx, cy + 3), col::TextSecondary);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// A slider with a stop at each whole number from `lo` to `hi`, `w` wide: a
// thin track, a round grip and (with `dots`) a dot for each stop. True when
// it moved. `activated`, `held` and `released` tell when a drag starts, that
// it goes on (every frame of it) and when it ends. Left and Right move it a
// stop while it has the focus. `track`: where its stops are drawn.
bool stopSlider(const char* id, int* v, int lo, int hi, float w, bool dots, bool* activated = nullptr,
                bool* released = nullptr, bool* held = nullptr, App::SliderTrack* track = nullptr) {
    const float h = 22, r = 9;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (activated) *activated = ImGui::IsItemActivated();
    if (released) *released = ImGui::IsItemDeactivated();
    if (held) *held = active;
    const int n = hi - lo + 1;
    const float x0 = p.x + r, x1 = p.x + w - r;
    if (track) *track = App::SliderTrack{x0, x1, p.y + h / 2, lo, hi};
    int next = *v;
    if (active) {
        const float t = (ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0);
        next = std::clamp(lo + static_cast<int>(std::lround(t * (n - 1))), lo, hi);
    }
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) next = std::max(lo, *v - 1);
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) next = std::min(hi, *v + 1);
    const bool moved = next != *v;
    *v = next;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + h / 2;
    dl->AddRectFilled(ImVec2(p.x, cy - 2), ImVec2(p.x + w, cy + 2), IM_COL32(0x2E, 0x34, 0x3E, 255), 2);
    const float gx = x0 + (x1 - x0) * (n > 1 ? static_cast<float>(*v - lo) / (n - 1) : 0.0f);
    dl->AddCircleFilled(ImVec2(gx, cy), r, hovered || active ? col::AccentHover : col::Accent, 24);
    dl->AddCircle(ImVec2(gx, cy), r - 1.5f, col::Panel, 24, 3);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (dots) {
        for (int i = 0; i < n; ++i) {
            const float x = x0 + (x1 - x0) * (n > 1 ? static_cast<float>(i) / (n - 1) : 0.0f);
            dl->AddCircleFilled(ImVec2(x, p.y + h + 3), 2, IM_COL32(0x4A, 0x51, 0x60, 255));
        }
        ImGui::Dummy(ImVec2(0, 4));
    }
    return moved;
}

// An on/off switch, right-aligned on the line at `y`. True when clicked.
bool switchAt(const char* id, bool on, float y) {
    const ImVec2 sz(36, 20);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - sz.x, y));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), on ? IM_COL32(0xB8, 0x62, 0x24, 255) : col::BorderStrong, 10);
    dl->AddCircleFilled(ImVec2(on ? p.x + sz.x - 10 : p.x + 10, p.y + sz.y / 2), 7, col::Text, 16);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// The orange dot that marks a value of a part's own, at the cursor (`r` radius), on a line of text.
void ownDot(float r = 3.5f) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h / 2), r, col::Accent, 12);
    ImGui::Dummy(ImVec2(2 * r, h));
}

std::string nameOf(const Document& doc, int id) {
    const Part* p = doc.findPart(id);
    return p ? p->name() : std::string("a part");
}

std::string namesOf(const Document& doc, const std::vector<int>& ids) {
    std::string out;
    for (std::size_t i = 0; i < ids.size(); ++i)
        out += (i == 0 ? "" : i + 1 == ids.size() ? " and " : ", ") + nameOf(doc, ids[i]);
    return out;
}

const char* const kTipNames[] = {"0.2", "0.3", "0.4", "0.6"};

// The values a part can have of its own, as the panel shows them.
enum class Field { HowMany, Tip, LookFurther, Base, Lift, RaftType };

// Part `id`'s value of `f`, in words, when it is its own (not the job's).
std::optional<std::string> ownValue(const Document& doc, int id, Field f) {
    const Part* part = doc.findPart(id);
    if (!part) return std::nullopt;
    const fruit::Placement& p = part->placement();
    const JobLift& job = doc.jobLift();
    if (f == Field::Base)
        return p.on_plate != job.on_plate ? std::optional<std::string>(p.on_plate ? "on the plate" : "a raft")
                                          : std::nullopt;
    if (f == Field::Lift)
        return !p.on_plate && p.elevation != job.elevation ? std::optional<std::string>(strf("%g mm", p.elevation))
                                                           : std::nullopt;
    const auto it = doc.supports.own.find(id);
    if (it == doc.supports.own.end()) return std::nullopt;
    const PartChoices& o = it->second;
    switch (f) {
        case Field::HowMany:
            if (o.how_many) return std::string(howManyWord(*o.how_many, doc.supports.setup().forPart(id)));
            break;
        case Field::Tip:
            if (o.usual_tip) return strf("%s mm", kTipNames[std::clamp(*o.usual_tip, 0, 3)]);
            break;
        case Field::LookFurther:
            if (o.look_further) return std::string(*o.look_further ? "on" : "off");
            break;
        case Field::RaftType:
            if (o.raft) return o.raft_preset.name.empty() ? std::string("a raft of its own") : o.raft_preset.name;
            break;
        default: break;
    }
    return std::nullopt;
}

// The job's value of `f`, in words.
std::string jobValue(const Document& doc, Field f) {
    const SupportChoices& c = doc.supports.choices;
    switch (f) {
        case Field::HowMany: return howManyWord(c.how_many, c);
        case Field::Tip: return strf("%s mm", kTipNames[std::clamp(c.usual_tip, 0, 3)]);
        case Field::LookFurther: return c.look_further ? "on" : "off";
        case Field::Base: return doc.jobLift().on_plate ? "on the plate" : "a raft";
        case Field::Lift: return strf("%g mm", doc.jobLift().elevation);
        case Field::RaftType: return c.raft_preset.name.empty() ? std::string("the job's raft") : c.raft_preset.name;
    }
    return "";
}

// Whether the part has any value of its own (its dot on the switch).
bool hasOwn(const Document& doc, int id) {
    for (const Field f : {Field::HowMany, Field::Tip, Field::LookFurther, Field::Base, Field::Lift, Field::RaftType})
        if (ownValue(doc, id, f)) return true;
    const auto it = doc.supports.own.find(id);
    return it != doc.supports.own.end() && !it->second.advanced.empty();
}

// Puts the selected part's `f` back to the job's.
void backToJob(App& app, Document& doc, Field f) {
    SupportChoices c = app.supportChoicesShown();
    const SupportChoices& job = doc.supports.choices;
    switch (f) {
        case Field::HowMany: c.how_many = job.how_many; break;
        case Field::Tip: c.usual_tip = job.usual_tip; break;
        case Field::LookFurther: c.look_further = job.look_further; break;
        case Field::RaftType:
            c.raft = job.raft;
            c.raft_preset = job.raft_preset;
            break;
        case Field::Base: app.setOnPlate(doc.jobLift().on_plate); return;
        case Field::Lift: app.setLift(doc.jobLift().elevation); return;
    }
    app.setSupportChoices(c);
}

// Under a control: for All parts, the parts that keep a value of their own
// (a click on a name picks that part); for one part, that the value is its
// own and the way back.
void ownLines(App& app, Document& doc, Field f) {
    ImGui::PushID(static_cast<int>(f) + 100);
    if (app.supportsForPart()) {
        if (ownValue(doc, doc.selectedId(), f)) {
            ownDot();
            ImGui::SameLine(0, 7);
            text("Its own value.", fonts().regular, size::Help, col::TextSecondary);
            ImGui::SameLine(0, 5);
            if (linkButton(strf("Back to all parts' %s", jobValue(doc, f).c_str()).c_str())) backToJob(app, doc, f);
        }
        ImGui::PopID();
        return;
    }
    std::vector<std::pair<int, std::string>> own;
    for (const Part& p : doc.parts())
        if (auto v = ownValue(doc, p.id(), f)) own.emplace_back(p.id(), *v);
    if (own.size() > 2) {
        ownDot();
        ImGui::SameLine(0, 7);
        std::vector<int> ids;
        for (const auto& [id, v] : own) ids.push_back(id);
        text(strf("%zu parts have their own: %s", own.size(), namesOf(doc, ids).c_str()).c_str(), fonts().regular,
             size::Help, col::TextSecondary, true);
    } else {
        for (const auto& [id, v] : own) {
            ImGui::PushID(id);
            ownDot();
            ImGui::SameLine(0, 7);
            if (linkButton(nameOf(doc, id).c_str())) {
                app.selectPart(id);
                app.setSupportsForPart(true);
            }
            ImGui::SameLine(0, 5);
            text(strf("has its own: %s", v.c_str()).c_str(), fonts().regular, size::Help, col::TextSecondary);
            ImGui::PopID();
        }
    }
    ImGui::PopID();
}

// A section's header: a chevron and its title, and folded, what it has, on
// the right. A click folds or opens it. True while it is open. `right`: a
// link drawn at its right end while it is open (true when clicked).
bool sectionHeader(App& app, int section, const char* title, const std::string& summary, bool first,
                   const char* right = nullptr, bool* right_clicked = nullptr) {
    ImGui::PushID(section);
    const bool open = app.supportsSectionOpen(section);
    const float w = ImGui::GetContentRegionAvail().x, h = 38;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!first) dl->AddLine(p, ImVec2(p.x + w, p.y), col::Border, 1);
    float link_w = 0;
    if (open && right) {
        ImGui::PushFont(fonts().semibold, size::Help);
        link_w = ImGui::CalcTextSize(right).x + 8;
        ImGui::PopFont();
    }
    if (ImGui::InvisibleButton("##section", ImVec2(w - link_w, h))) app.setSupportsSectionOpen(section, !open);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float cy = p.y + h / 2;
    if (open)
        dl->AddTriangleFilled(ImVec2(p.x + 1, cy - 3), ImVec2(p.x + 9, cy - 3), ImVec2(p.x + 5, cy + 2), col::TextMuted);
    else
        dl->AddTriangleFilled(ImVec2(p.x + 3, cy - 4), ImVec2(p.x + 3, cy + 4), ImVec2(p.x + 8, cy), col::TextMuted);
    ImGui::PushFont(fonts().semibold, size::Body);
    const ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(p.x + 16, cy - ts.y / 2), col::Text, title);
    ImGui::PopFont();
    if (!open && !summary.empty()) {
        ImGui::PushFont(fonts().regular, size::Help);
        std::string s = summary;
        const float room = w - 16 - ts.x - 16;
        while (s.size() > 1 && ImGui::CalcTextSize((s + "…").c_str()).x > room) s.pop_back();
        if (s.size() < summary.size()) s += "…";
        const ImVec2 ss = ImGui::CalcTextSize(s.c_str());
        dl->AddText(ImVec2(p.x + w - ss.x, cy - ss.y / 2), col::TextMuted, s.c_str());
        ImGui::PopFont();
    }
    if (open && right) {
        ImGui::SameLine(0, 8);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - link_w + 8, cy - ImGui::GetTextLineHeight() / 2));
        const bool c = linkButton(right);
        if (right_clicked) *right_clicked = c;
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    }
    ImGui::PopID();
    return open;
}

// Raft | On the plate: two tiles side by side, each with a small picture.
// True when one is clicked (*on_plate is then the one clicked).
bool baseTiles(bool* on_plate) {
    const float gap = 8, w = (ImGui::GetContentRegionAvail().x - gap) / 2, h = 44;
    bool clicked = false;
    for (int i = 0; i < 2; ++i) {
        const bool plate = i == 1, on = plate == *on_plate;
        if (i) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tile", ImVec2(w, h)) && !on) {
            *on_plate = plate;
            clicked = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          on ? IM_COL32(0x26, 0x1D, 0x16, 255) : (hovered ? col::Active : col::Card), 9);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), on ? col::Accent : col::BorderStrong, 9, on ? 1.5f : 1.0f);
        // The picture: a part on its supports and raft, or a part flat on the plate.
        const float ix = p.x + 12, iy = p.y + h / 2 - 9;
        const ImU32 part = col::Model, base = on ? col::Accent : IM_COL32(0x6B, 0x72, 0x80, 255);
        if (!plate) {
            dl->AddRectFilled(ImVec2(ix + 5, iy + 2), ImVec2(ix + 21, iy + 7), part, 1);
            for (const float x : {8.0f, 13.0f, 18.0f}) dl->AddLine(ImVec2(ix + x, iy + 7), ImVec2(ix + x, iy + 12), part, 1.2f);
            dl->AddRectFilled(ImVec2(ix + 2, iy + 12), ImVec2(ix + 24, iy + 15), base, 1.5f);
        } else {
            dl->AddRectFilled(ImVec2(ix + 6, iy + 5), ImVec2(ix + 20, iy + 14), part, 1);
            dl->AddLine(ImVec2(ix + 1, iy + 15), ImVec2(ix + 25, iy + 15), base, 1.6f);
        }
        const char* label = plate ? "On the plate" : "Raft";
        ImGui::PushFont(on ? fonts().semibold : fonts().medium, size::Body);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(ix + 36, p.y + (h - ts.y) / 2), on ? col::Text : col::TextSecondary, label);
        ImGui::PopFont();
        ImGui::PopID();
    }
    return clicked;
}

// How many: the slider with Fewer and More at its ends, the tips each stop
// would give under it, and what the stop sets.
void howMany(App& app, Document& doc) {
    const SupportChoices choices = app.supportChoicesShown();
    ImGui::PushID("how_many");
    const int stop = std::clamp(choices.how_many, 1, kQuickStops);
    const float y = ImGui::GetCursorPosY();
    text("How many", fonts().semibold, size::Body, col::Text);
    rightText(howManyWord(stop, choices), fonts().semibold, size::Help,
              stop == kHowManyCentre ? col::TextSecondary : col::Accent, y);
    ImGui::Dummy(ImVec2(0, 2));
    // Fewer [slider] More, on one line.
    ImGui::PushFont(fonts().regular, size::Label);
    const float few_w = ImGui::CalcTextSize("Fewer").x, more_w = ImGui::CalcTextSize("More").x;
    ImGui::PopFont();
    const float w = ImGui::GetContentRegionAvail().x, gap = 8, r = 9;
    const float row_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(row_y + 4);
    text("Fewer", fonts().regular, size::Label, col::TextMuted);
    ImGui::SameLine(0, gap);
    ImGui::SetCursorPosY(row_y);
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    const float sw = w - few_w - more_w - 2 * gap;
    int v = stop;
    if (stopSlider("##stop", &v, 1, kQuickStops, sw, false)) {
        SupportChoices c = choices;
        c.how_many = v;
        app.setSupportChoices(c);
    }
    ImGui::SameLine(0, gap);
    ImGui::SetCursorPosY(row_y + 4);
    text("More", fonts().regular, size::Label, col::TextMuted);
    // Under each stop, the tips it would give (counted in the background; "…"
    // until then). A job that doesn't fit the plate gets no supports, so nothing to count.
    const auto counts = app.tipCountsByStop();
    if (doc.fits()) {
        ImGui::PushFont(fonts().regular, size::Label);
        const float cy = ImGui::GetCursorScreenPos().y + 2;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < kQuickStops; ++i) {
            const std::string n = counts ? thousands((*counts)[static_cast<std::size_t>(i)]) : std::string("…");
            const float tw = ImGui::CalcTextSize(n.c_str()).x;
            const float cx = sp.x + r + (sw - 2 * r) * static_cast<float>(i) / (kQuickStops - 1);
            dl->AddText(ImVec2(std::round(cx - tw / 2), cy), i + 1 == stop && counts ? col::Text : col::TextMuted, n.c_str());
        }
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + 4));
        ImGui::PopFont();
        if (ImGui::IsItemHovered())
            tooltip(app.supportsForPart() ? "The tips this part would get at each stop, made again now."
                                          : "The tips the job would get at each stop, made again now. A part with "
                                            "a How many of its own keeps its stop.");
    }
    std::string caption = howManyCaption(stop, choices);
    if (counts) {
        // What the stop comes to here, where that is not what its words lead one to expect.
        const std::size_t here = (*counts)[static_cast<std::size_t>(stop - 1)], middle = (*counts)[kHowManyCentre - 1];
        if (stop != kHowManyCentre && here == middle)
            caption += strf(" Here that changes nothing: %s tips, as in the middle.", thousands(here).c_str());
        else if (stop > kHowManyCentre && here < middle)
            caption += strf(" Here that is fewer tips than in the middle (%s against %s)%s", thousands(here).c_str(),
                            thousands(middle).c_str(),
                            supportConfig(doc.placement(), choices).tip_load_limit > 0
                                ? ": with tips closer together, fewer extra ones are needed to share the load."
                                : ".");
    }
    text(caption.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    ownLines(app, doc, Field::HowMany);
    ImGui::PopID();
}

// Tip size: the usual tip, as buttons on its label's line; the other two sizes move with it.
void tipSize(App& app, Document& doc) {
    const SupportChoices choices = app.supportChoicesShown();
    ImGui::PushID("tip_size");
    const int now = std::clamp(choices.usual_tip, 0, 3);
    const int first = now == 0 ? 0 : 1;  // 0.2 mm is offered only to a job that has it (an older one)
    const float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y + 6);
    text("Tip size", fonts().semibold, size::Body, col::Text);
    // The buttons' width: as segmentButton draws them.
    ImGui::PushFont(fonts().semibold, size::Body);
    float bw = 0;
    for (int cls = first; cls <= 3; ++cls) bw += ImGui::CalcTextSize(kTipNames[cls]).x + 22 + 4;
    const float mm_w = ImGui::CalcTextSize("mm").x;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x - bw - mm_w - 4, y));
    for (int cls = first; cls <= 3; ++cls) {
        if (segmentButton(kTipNames[cls], cls == now) && cls != now) {
            SupportChoices c = choices;
            c.usual_tip = cls;
            app.setSupportChoices(c);
        }
        ImGui::SameLine(0, 4);
    }
    ImGui::SetCursorPosY(y + 6);
    text("mm", fonts().regular, size::Body, col::TextSecondary);
    ImGui::Dummy(ImVec2(0, 2));
    text(tipSizeCaption(choices).c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    ownLines(app, doc, Field::Tip);
    ImGui::PopID();
}

// Look further: a support walks further for a way down before it stands on the model.
void lookFurther(App& app, Document& doc) {
    const SupportChoices choices = app.supportChoicesShown();
    const float y = ImGui::GetCursorPosY();
    text("Look further for a way down", fonts().semibold, size::Body, col::Text);
    if (switchAt("##look_further", choices.look_further, y - 1)) {
        SupportChoices c = choices;
        c.look_further = !c.look_further;
        app.setSupportChoices(c);
    }
    ImGui::SetCursorPosY(y + ImGui::GetTextLineHeightWithSpacing() + 2);
    text(lookFurtherCaption(choices).c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    ownLines(app, doc, Field::LookFurther);
}

void tipsSection(App& app, Document& doc) {
    const SupportChoices c = app.supportChoicesShown();
    const std::string summary =
        strf("%s · %s mm", howManyWord(c.how_many, c), kTipNames[std::clamp(c.usual_tip, 0, 3)]);
    // Back to the resin's values: All parts only (a part goes back to All parts' at the top).
    const fruit::Placement base{};
    const bool differs = !app.supportsForPart() &&
                         (quickChanged(doc.supports.choices) || doc.jobLift().on_plate ||
                          std::abs(doc.jobLift().elevation - base.elevation) > 1e-9);
    bool reset = false;
    if (!sectionHeader(app, kSectionTips, "Tips", summary, doc.partCount() <= 1,
                       differs ? "Reset to the resin's" : nullptr, &reset)) {
        return;
    }
    if (reset) app.resetQuickSupport();
    // Each control reads the choices as they are now, so a change earlier in the frame shows.
    howMany(app, doc);
    ImGui::Dummy(ImVec2(0, 10));
    tipSize(app, doc);
    ImGui::Dummy(ImVec2(0, 10));
    lookFurther(app, doc);
    ImGui::Dummy(ImVec2(0, 10));
}

// The raft's thickness: the built one when the supports are up to date, else what the settings will give.
double raftThickness(const Document& doc, const SupportChoices& c) {
    const SupportState& made = doc.supports;
    if (made.result && made.result->setup == made.setup())
        for (const SupportResult::RaftMade& g : made.result->rafts)
            if (g.spec == c.raft && g.top > 0) return g.top;
    return std::max(treeConfig(c).raft_thickness,
                    fruit::minRaftThickness(doc.printer.bottom_layers, doc.printer.transition_layers, kLayerHeight));
}

// With no raft: the height added under the model. It is a print setting of
// the resin (next to its bottom layers), so here it is only shown.
void heightCorrection(App& app, const Document& doc) {
    const PrintEdits& e = doc.print.settings;
    const double mm = heightCorrection(e);
    const int layers = baseLayers(doc, kLayerHeight);
    const float y = ImGui::GetCursorPosY();
    text("Height correction", fonts().semibold, size::Body, col::Text);
    rightText(strf("%g mm", mm).c_str(), fonts().semibold, size::Help, col::TextSecondary, y);
    const bool rafts = !doc.allOnPlate();
    std::string what =
        layers > 0 ? strf("Adds %g mm (%d %s) under %s, because the printer squashes the first layers.",
                          layers * kLayerHeight, layers, layers == 1 ? "layer" : "layers",
                          rafts ? "the parts on the plate, and under the rafts with them" : "the model")
                   : std::string("Adds nothing, so a part on the plate may print a little short: the printer "
                                 "squashes the first layers.");
    what += e.height_correction >= 0 ? " Your value, from the resin's print settings."
                                     : " 0.10 mm is a typical value, not measured on your printer.";
    text(what.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    if (linkButton("Change it on the Resins page")) {
        std::string id = app.newJobResin().id;
        for (const Resin& r : app.resins())
            if (r.name == doc.supports.choices.resin) id = r.id;
        app.resinSelected() = id;
        app.setScreen(Screen::Resins);
    }
}

// The rafts as the panel shows them: as made when the supports are up to date
// (rafts that would meet are joined then), else as chosen.
struct RaftRow {
    RaftGroup g;
    bool made = false;
};
std::vector<RaftRow> raftRows(const Document& doc) {
    std::vector<RaftRow> out;
    const SupportState& s = doc.supports;
    if (s.result && s.fresh(doc.revision(), doc.supportSettings())) {
        for (const SupportResult::RaftMade& g : s.result->rafts) out.push_back({g, true});
        return out;
    }
    for (const RaftGroup& g : chosenRafts(doc, s.setup())) out.push_back({g, false});
    return out;
}

int raftNumber(const std::vector<RaftRow>& rows, int part) {
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (std::find(rows[i].g.parts.begin(), rows[i].g.parts.end(), part) != rows[i].g.parts.end())
            return static_cast<int>(i) + 1;
    return 0;
}

// A raft's number in a small round badge, at the cursor; dashed and blank for a part on the plate.
void raftBadge(int number, float r = 10) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + r, p.y + r);
    if (number > 0) {
        dl->AddCircleFilled(c, r, col::Active, 20);
        ImGui::PushFont(fonts().bold, size::Label);
        const std::string n = std::to_string(number);
        const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col::Text, n.c_str());
        ImGui::PopFont();
    } else {
        for (int i = 0; i < 12; i += 2) {  // dashed
            dl->PathArcTo(c, r - 0.5f, static_cast<float>(i) * 3.14159f / 6, static_cast<float>(i + 1) * 3.14159f / 6, 4);
            dl->PathStroke(IM_COL32(0x6B, 0x72, 0x80, 255), 1.0f);
        }
        dl->AddLine(ImVec2(c.x - 3, c.y), ImVec2(c.x + 3, c.y), col::TextMuted, 1);
    }
    ImGui::Dummy(ImVec2(2 * r, 2 * r));
}

// Raft and lift: the raft or the plate, the raft's type, the lift.
void raftSection(App& app, Document& doc) {
    const bool for_part = app.supportsForPart();
    bool on_plate = for_part ? doc.placement().on_plate : doc.jobLift().on_plate;
    const SupportChoices c = app.supportChoicesShown();
    const int lift = static_cast<int>(std::lround(for_part ? doc.placement().elevation : doc.jobLift().elevation));
    const std::string summary =
        on_plate ? std::string("On the plate")
                 : strf("Raft · %s · %d mm", c.raft_preset.name.empty() ? "its own" : c.raft_preset.name.c_str(), lift);
    if (!sectionHeader(app, kSectionRaft, "Raft and lift", summary, false)) return;
    if (baseTiles(&on_plate)) app.setOnPlate(on_plate);
    ownLines(app, doc, Field::Base);
    if (on_plate) {
        ImGui::Dummy(ImVec2(0, 6));
        const double held = std::round(modelInBottomLayers(doc) * 100) / 100;
        text(held > 0 ? strf("Only overhangs get tips, and none in %s first %g mm: that prints in the bottom layers, "
                             "whose long exposures should cure it onto the plate. Not measured.",
                             for_part || doc.partCount() == 1 ? "its" : "each part's", held)
                            .c_str()
                      : "Only overhangs get tips. For flat-backed parts.",
             fonts().regular, size::Help, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 10));
        heightCorrection(app, doc);
        ImGui::Dummy(ImVec2(0, 10));
        return;
    }
    // One part on a raft it shares: with which parts, and the way to a raft of its own.
    const std::vector<RaftRow> rows = raftRows(doc);
    const RaftRow* shared = nullptr;
    if (for_part)
        for (const RaftRow& r : rows)
            if (r.g.parts.size() > 1 &&
                std::find(r.g.parts.begin(), r.g.parts.end(), doc.selectedId()) != r.g.parts.end())
                shared = &r;
    if (shared) {
        ImGui::Dummy(ImVec2(0, 8));
        beginCard("shared", ImVec2(0, 0), 8, col::Card, col::Border);
        const float cx0 = ImGui::GetCursorPosX(), cw = ImGui::GetContentRegionAvail().x;
        raftBadge(raftNumber(rows, doc.selectedId()), 9);
        ImGui::SameLine(0, 8);
        std::vector<int> others;
        for (const int id : shared->g.parts)
            if (id != doc.selectedId()) others.push_back(id);
        const float bw = 76;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw - 8);
        text(strf("Shares raft %d with %s%s", raftNumber(rows, doc.selectedId()), namesOf(doc, others).c_str(),
                  shared->g.touching ? ": their rafts would meet" : "")
                 .c_str(),
             fonts().regular, size::Help, col::TextSecondary, true);
        ImGui::PopTextWrapPos();
        if (!shared->g.touching) {
            ImGui::SameLine(cx0 + cw - bw);
            if (secondaryButton("Own raft", ImVec2(bw, 26))) app.leaveRaft(doc.selectedId());
        }
        endCard();
    }
    // The raft's type: its preset (a menu of them), the way to the Raft page, and what it is.
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushID("raft_preset");
    const float ry = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(ry + 7);
    text("Raft type", fonts().semibold, size::Body, col::Text);
    ImGui::SameLine(0, 12);
    ImGui::SetCursorPosY(ry);
    const std::string& name = c.raft_preset.name;
    const std::string label = name.empty() ? std::string("This raft's own") : name;
    if (presetButton("##preset", label.c_str())) ImGui::OpenPopup("raft_presets");
    ImGui::SameLine(0, 10);
    ImGui::SetCursorPosY(ry + 7);
    if (linkButton("Edit raft…")) app.showRaft(true);
    if (ImGui::BeginPopup("raft_presets")) {
        auto item = [&](const std::string& n) {
            if (ImGui::Selectable(n.c_str(), n == name, 0, ImVec2(220, 0))) app.useRaftPreset(n);
        };
        groupLabel("Built in");
        for (const fruit::RaftPreset& p : fruit::builtInRaftPresets()) item(p.name);
        if (!app.raftPresets().own.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            groupLabel("Yours");
            for (const OwnRaftPreset& p : app.raftPresets().own) item(p.name);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 2));
    text(raftSummary(c.raft, raftThickness(doc, c)).c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    if (shared)
        text(strf("Raft %d's type: changing it changes %s too.", raftNumber(rows, doc.selectedId()),
                  shared->g.parts.size() == 2 ? "the other part's raft" : "the other parts' raft")
                 .c_str(),
             fonts().regular, size::Help, col::TextSecondary, true);
    ownLines(app, doc, Field::RaftType);
    // The lift.
    ImGui::Dummy(ImVec2(0, 10));
    const float y = ImGui::GetCursorPosY();
    text("Lift", fonts().semibold, size::Body, col::Text);
    const bool differs = std::abs(lift - fruit::Placement{}.elevation) > 1e-9;
    rightText(strf("%d mm", lift).c_str(), fonts().semibold, size::Help, differs ? col::Accent : col::TextSecondary, y);
    int v = lift;
    bool begin = false, end = false, holding = false;
    const bool moved = stopSlider("##lift", &v, 1, 10, ImGui::GetContentRegionAvail().x, true, &begin, &end, &holding,
                                  &app.liftTrack());
    if (begin) app.beginLift();
    if (holding) app.previewLift(v);  // every frame it is held, so the drag lasts until it is let go
    else if (moved) app.setLift(v);   // Left and Right: a stop each, each its own undo step
    if (end) app.endLift();
    text("From the raft to the part's lowest point: higher snaps off more easily, lower uses less resin.",
         fonts().regular, size::Help, col::TextSecondary, true);
    ownLines(app, doc, Field::Lift);
    ImGui::Dummy(ImVec2(0, 10));
}

// The gap between two rafts' parts seen from above, mm (their outlines' boxes).
double gapBetween(const Document& doc, const RaftGroup& a, const RaftGroup& b) {
    double best = 1e9;
    for (const int i : a.parts)
        for (const int j : b.parts) {
            const Part* p = doc.findPart(i);
            const Part* q = doc.findPart(j);
            if (!p || !q) continue;
            const fruit::BBox2 x = p->footprintBox(), y = q->footprintBox();
            const double dx = std::max({0.0, y.min.x - x.max.x, x.min.x - y.max.x});
            const double dy = std::max({0.0, y.min.y - x.max.y, x.min.y - y.max.y});
            best = std::min(best, std::hypot(dx, dy));
        }
    return best;
}

// Rafts this close (their parts' outlines, mm) are offered as one base.
constexpr double kOfferShare = 10.0;

// Rafts on the plate: each raft with its parts, and the parts on the plate.
// Separate gives a shared raft's parts each their own; a raft's ··· menu puts
// it with another.
void raftsSection(App& app, Document& doc) {
    const std::vector<RaftRow> rows = raftRows(doc);
    std::vector<int> on_plate;
    for (const Part& p : doc.parts())
        if (p.placement().on_plate) on_plate.push_back(p.id());
    std::string summary = rows.empty() ? std::string("No raft")
                                       : strf("%zu %s", rows.size(), rows.size() == 1 ? "raft" : "rafts");
    if (!on_plate.empty())
        summary += on_plate.size() == 1 ? " · " + nameOf(doc, on_plate.front()) + " on the plate"
                                        : strf(" · %zu on the plate", on_plate.size());
    if (!sectionHeader(app, kSectionRafts, "Rafts on the plate", summary, false)) return;
    ImGui::PushID("rafts");
    const float w = ImGui::GetContentRegionAvail().x;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const RaftGroup& g = rows[i].g;
        ImGui::PushID(static_cast<int>(i));
        const float y = ImGui::GetCursorPosY();
        raftBadge(static_cast<int>(i) + 1);
        ImGui::SameLine(0, 10);
        const bool by_choice = g.parts.size() > 1 && !g.touching;
        const float bw = by_choice || g.touching ? 76 : 28;
        ImGui::SetCursorPosY(y + 2);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - 30 - bw - 8);
        text(namesOf(doc, g.parts).c_str(), fonts().regular, size::Body, col::Text, true);
        ImGui::PopTextWrapPos();
        const float after = ImGui::GetCursorPosY();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorStartPos().x + w - bw, y - 2));
        if (by_choice || g.touching) {
            if (secondaryButton("Separate", ImVec2(bw, 24), by_choice)) app.separateRaft(g.parts.front());
            if (g.touching) tooltip("Their rafts would meet, so they print as one piece. Move the parts apart to separate them.");
        } else {
            if (secondaryButton("···", ImVec2(bw, 24))) ImGui::OpenPopup("raft_menu");
            tooltip("Put this raft with another");
            if (ImGui::BeginPopup("raft_menu")) {
                for (std::size_t j = 0; j < rows.size(); ++j) {
                    if (j == i) continue;
                    if (ImGui::Selectable(strf("Share raft %zu: %s", j + 1, namesOf(doc, rows[j].g.parts).c_str()).c_str()))
                        app.shareRaft(g.parts.front(), rows[j].g.parts.front());
                }
                if (rows.size() > 1) ImGui::Separator();
                if (ImGui::Selectable(strf("Pick %s", nameOf(doc, g.parts.front()).c_str()).c_str())) {
                    app.selectPart(g.parts.front());
                    app.setSupportsForPart(true);
                }
                ImGui::EndPopup();
            }
        }
        ImGui::SetCursorPosY(std::max(after, y + 26));
        if (g.touching) {
            ImGui::Indent(30);
            text("Shared: their rafts would meet, so they print as one piece. Further apart, each could have its own.",
                 fonts().regular, size::Help, col::TextSecondary, true);
            ImGui::Unindent(30);
        }
        ImGui::PopID();
    }
    for (const int id : on_plate) {
        ImGui::PushID(id);
        const float y = ImGui::GetCursorPosY();
        raftBadge(0);
        ImGui::SameLine(0, 10);
        ImGui::SetCursorPosY(y + 2);
        text(nameOf(doc, id).c_str(), fonts().regular, size::Body, col::Text);
        ImGui::SameLine(0, 4);
        text("· on the plate", fonts().regular, size::Body, col::TextMuted);
        ImGui::SetCursorPosY(y + 26);
        ImGui::PopID();
    }
    // Two rafts of their own, close enough to be one base: offered (the nearest pair).
    double best = kOfferShare;
    int a = -1, b = -1;
    for (std::size_t i = 0; i < rows.size(); ++i)
        for (std::size_t j = i + 1; j < rows.size(); ++j)
            if (const double d = gapBetween(doc, rows[i].g, rows[j].g); d <= best) {
                best = d;
                a = static_cast<int>(i);
                b = static_cast<int>(j);
            }
    if (a >= 0) {
        ImGui::Dummy(ImVec2(0, 4));
        beginCard("offer", ImVec2(0, 0), 10, col::Card, col::BorderStrong);
        const RaftGroup& ga = rows[static_cast<std::size_t>(a)].g;
        const RaftGroup& gb = rows[static_cast<std::size_t>(b)].g;
        text(strf("%s and %s sit %.0f mm apart: their rafts could be one base, which comes off in one piece.",
                  namesOf(doc, ga.parts).c_str(), namesOf(doc, gb.parts).c_str(), best)
                 .c_str(),
             fonts().regular, size::Help, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 2));
        if (secondaryButton("Share one raft", ImVec2(0, 28))) app.shareRaft(gb.parts.front(), ga.parts.front());
        endCard();
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PopID();
}

// The switch at the top: All parts, or the selected part (a dot when it has values of its own).
void scopeSwitch(App& app, Document& doc) {
    const float w = ImGui::GetContentRegionAvail().x, h = 34, pad = 3;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col::App, 9);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), col::Border, 9);
    const int id = doc.selectedId();
    const std::string all = strf("All %zu parts", doc.partCount());
    std::string part = nameOf(doc, id);
    const bool for_part = app.supportsForPart();
    const float sw = (w - 3 * pad) / 2;
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == 1) == for_part;
        const ImVec2 a(p.x + pad + i * (sw + pad), p.y + pad), b(a.x + sw, p.y + h - pad);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##scope", ImVec2(sw, b.y - a.y))) app.setSupportsForPart(i == 1);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (on || hovered) dl->AddRectFilled(a, b, on ? col::Active : col::Card, 7);
        ImGui::PushFont(on ? fonts().semibold : fonts().medium, size::Body);
        const bool dot = i == 1 && hasOwn(doc, id);
        const float room = sw - 16 - (dot ? 14 : 0);
        std::string s = i == 0 ? all : part;
        if (ImGui::CalcTextSize(s.c_str()).x > room) {
            while (s.size() > 1 && ImGui::CalcTextSize((s + "…").c_str()).x > room) s.pop_back();
            s += "…";
        }
        const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        const float x = a.x + (sw - ts.x - (dot ? 14 : 0)) / 2;
        if (dot) dl->AddCircleFilled(ImVec2(x + 3.5f, (a.y + b.y) / 2), 3.5f, col::Accent, 12);
        dl->AddText(ImVec2(x + (dot ? 14 : 0), (a.y + b.y - ts.y) / 2), on ? col::Text : col::TextSecondary, s.c_str());
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 8));
    if (!for_part) {
        text("A change here goes to every part, except where a part has a value of its own (an orange dot).",
             fonts().regular, size::Help, col::TextSecondary, true);
    } else {
        std::vector<std::string> own;
        const std::pair<Field, const char*> names[] = {{Field::HowMany, "How many"}, {Field::Tip, "Tip size"},
                                                       {Field::LookFurther, "Look further"}, {Field::Base, "Raft or plate"},
                                                       {Field::RaftType, "Raft type"}, {Field::Lift, "Lift"}};
        for (const auto& [f, n] : names)
            if (ownValue(doc, id, f)) own.push_back(n);
        if (const auto it = doc.supports.own.find(id); it != doc.supports.own.end() && !it->second.advanced.empty())
            own.push_back("Advanced values");
        std::string words;
        for (std::size_t i = 0; i < own.size(); ++i) words += (i == 0 ? "" : i + 1 == own.size() ? " and " : ", ") + own[i];
        text(own.empty() ? strf("Only %s changes here. It has All parts' values.", part.c_str()).c_str()
                         : strf("Only %s changes here. Its own: %s.", part.c_str(), words.c_str()).c_str(),
             fonts().regular, size::Help, col::TextSecondary, true);
        if (!own.empty() && linkButton("Use all parts' values")) app.useJobValues(id);
    }
    ImGui::Dummy(ImVec2(0, 4));
}

// Why supports that were made no longer count, and whether undo is the way back.
struct Stale {
    const char* why;
    bool undo;
};
Stale whyStale(const Document& doc) {
    // With no raft no tip goes where the bottom layers print the model: a print setting, not an edit.
    if (doc.supportsOutdatedByBottomLayers())
        return {"The resin's bottom layers or height correction changed after they were made.", false};
    return {doc.supports.result->setup == doc.supports.setup() ? "The model or the picked sides changed after they were made."
                                                               : "The settings changed after they were made.",
            true};
}

// One line of numbers: bold figures and their words, as `parts` say (figure, words after it).
void statLine(const std::vector<std::pair<std::string, std::string>>& parts, const std::string& before = "") {
    bool first = true;
    if (!before.empty()) {
        text(before.c_str(), fonts().regular, size::Body, col::TextSecondary);
        first = false;
    }
    for (const auto& [fig, words] : parts) {
        if (!first) ImGui::SameLine(0, 4);
        first = false;
        text(fig.c_str(), fonts().semibold, size::Body, col::Text);
        ImGui::SameLine(0, 4);
        text(words.c_str(), fonts().regular, size::Body, col::TextSecondary);
    }
}

// Above the footer's buttons: nothing made, what was made (for the part, when one is picked), or that it is out of date.
void statusArea(App& app, Document& doc) {
    const SupportState& s = doc.supports;
    if (!s.result) {
        text("Nothing made yet. Make supports uses these settings.", fonts().regular, size::Help, col::TextSecondary, true);
        return;
    }
    if (!s.fresh(doc.revision(), doc.supportSettings()) && !running(app)) {
        beginCard("stale", ImVec2(0, 0), 10, col::WarnBg, col::WarnBorder);
        text("Supports are out of date", fonts().semibold, size::Body, col::Text);
        text(whyStale(doc).why, fonts().regular, size::Help, col::TextSecondary, true);
        endCard();
        return;
    }
    const SupportResult& r = *s.result;
    const SupportResult::PartMade* pm = app.supportsForPart() ? r.partMade(doc.selectedId()) : nullptr;
    if (pm) {
        statLine({{thousands(pm->tips), "tips ·"}, {thousands(pm->trunks), "trunks ·"}, {strf("%.1f ml", pm->ml), ""}},
                 nameOf(doc, pm->part) + ":");
        text(strf("All %zu parts: %s tips · %.1f ml%s", doc.partCount(), thousands(r.tips).c_str(), r.support_ml,
                  pm->raft >= 0 && r.rafts[static_cast<std::size_t>(pm->raft)].parts.size() > 1
                      ? strf(", with raft %d (not counted in %s's)", pm->raft + 1, nameOf(doc, pm->part).c_str()).c_str()
                      : "")
                 .c_str(),
             fonts().regular, size::Label, col::TextMuted, true);
        return;
    }
    statLine({{thousands(r.tips), "tips ·"}, {thousands(r.trunks), "trunks ·"}, {strf("%.1f ml", r.support_ml), "of supports"}});
    // The tips by size, how many supports stand on the model, and what the run before this one gave.
    std::string more;
    for (const auto& [d, n] : r.tip_sizes)
        more += (more.empty() ? "" : " · ") + strf("%s × %g", thousands(n).c_str(), d) + (more.empty() ? " mm" : "");
    if (r.on_model > 0) more += (more.empty() ? "" : " · ") + strf("%s stand on the model", thousands(r.on_model).c_str());
    if (s.before_ml >= 0 && (s.before_tips != r.tips || std::abs(s.before_ml - r.support_ml) >= 0.05))
        more += (more.empty() ? "" : " · ") + strf("Before: %s tips · %.1f ml", thousands(s.before_tips).c_str(), s.before_ml);
    if (!more.empty()) text(more.c_str(), fonts().regular, size::Label, col::TextMuted, true);
}

}  // namespace

void supportsPanel(App& app, Document& doc) {
    if (doc.partCount() > 1) scopeSwitch(app, doc);
    tipsSection(app, doc);
    raftSection(app, doc);
    if (doc.partCount() > 1) raftsSection(app, doc);
    // The tip picked in the view, and why it is there.
    const SupportState& s = doc.supports;
    // (Not in the Risk view: its marks are the places that may not print, and no tip is ringed there.)
    if (s.result && !running(app) && s.selected_tip >= 0 && app.supportsView() != SupportsView::Risk &&
        s.selected_tip < static_cast<int>(s.result->points.size())) {
        ImGui::Dummy(ImVec2(0, 6));
        groupLabel("Selected tip");
        const SupportResult& r = *s.result;
        const auto i = static_cast<std::size_t>(s.selected_tip);
        const fruit::SupportPoint& p = r.points[i];
        const SupportResult::PartMade* pm = r.partOfTip(i);
        beginCard("tip", ImVec2(0, 0), 13, col::Card, col::Accent);
        text(tipKind(p).c_str(), fonts().semibold, size::Body, col::Text);
        text(tipWhy(p).c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::Dummy(ImVec2(0, 2));
        text(tipNumbers(p, pm ? pm->cfg : r.cfg, pm ? pm->tree_cfg : r.tree_cfg).c_str(), fonts().regular, size::Help,
             col::TextMuted, true);
        endCard();
    } else if (s.result && !running(app) && app.supportsView() == SupportsView::Risk) {
        ImGui::Dummy(ImVec2(0, 4));
        text("The view shows what may not print. Each numbered place is listed under Checks.", fonts().regular,
             size::Body, col::TextSecondary, true);
    } else if (s.result && !running(app) && app.supportsView() == SupportsView::Why) {
        ImGui::Dummy(ImVec2(0, 4));
        text("Click a tip in the view to see why it is there.", fonts().regular, size::Body, col::TextSecondary, true);
    }
}

// Pinned under the panel's body: always in sight, however far it scrolls.
void supportsFooter(App& app, Document& doc) {
    const bool have = doc.supports.result != nullptr;
    ImGui::Dummy(ImVec2(0, 2));
    statusArea(app, doc);
    ImGui::Dummy(ImVec2(0, 6));
    const float gap = 8, bw = (ImGui::GetContentRegionAvail().x - gap) / 2;
    const bool fresh = have && doc.supports.fresh(doc.revision(), doc.supportSettings());
    if (secondaryButton("Edit tips by hand", ImVec2(bw, 34), fresh && !running(app))) app.editTips(true);
    if (!fresh) tooltip(have ? "Remake the supports first." : "Make supports first.");
    ImGui::SameLine(0, gap);
    // Advanced, with how many of its rows hold values of the job's own (or the part's).
    std::size_t changed = 0;
    if (app.supportsForPart()) {
        if (const auto it = doc.supports.own.find(doc.selectedId()); it != doc.supports.own.end())
            changed = it->second.advanced.size() + (it->second.usual_tip ? 1 : 0);
    } else {
        for (const AdvancedSetting& a : advancedSettings()) changed += advancedChanged(a, doc.supports.choices);
    }
    const std::string label = changed ? strf("Advanced · %zu", changed) : std::string("Advanced");
    if (secondaryButton(label.c_str(), ImVec2(bw, 34))) app.showAdvanced(true);
    tooltip(app.supportsForPart() ? "Every value the planner and the tree use, for this part"
                                  : "Every value the planner and the tree use");
}


namespace {

void checkRow(const char* id, const SupportCheck& c) {
    beginCard(id, ImVec2(0, 0), 12, col::Panel, c.ok ? col::Border : IM_COL32(0x6B, 0x2A, 0x2A, 255));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 9, p.y + 9), 9, c.ok ? col::OkBg : col::ErrorBg, 20);
    if (c.ok) icon::check(dl, ImVec2(p.x + 3, p.y + 3), 12, col::Ok);
    else icon::cross(dl, ImVec2(p.x + 4, p.y + 4), 10, col::Error);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 28, p.y));
    ImGui::BeginGroup();
    text(c.title.c_str(), fonts().semibold, size::Body, col::Text);
    if (!c.detail.empty()) text(c.detail.c_str(), fonts().regular, size::Help, col::TextSecondary, true);
    ImGui::EndGroup();
    endCard();
}

void problemCard(App& app, const SupportProblem& p, int i) {
    const bool review = p.level == SupportProblem::Level::Review, blocks = p.level == SupportProblem::Level::Blocks;
    ImGui::PushID(i);
    beginCard("p", ImVec2(0, 0), 14, blocks ? col::ErrorBg : (review ? col::WarnBg : col::Panel),
              blocks ? IM_COL32(0x6B, 0x2A, 0x2A, 255) : (review ? col::WarnBorder : col::Border));
    const float w = ImGui::GetContentRegionAvail().x;
    const float y = ImGui::GetCursorPosY();
    text(p.title.c_str(), fonts().semibold, size::Body, col::Text);
    const std::string n = thousands(p.count);
    ImGui::PushFont(fonts().semibold, size::Body);
    const float nw = ImGui::CalcTextSize(n.c_str()).x;
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(ImGui::GetWindowPos().x + ImGui::GetCursorStartPos().x + w - nw, ImGui::GetWindowPos().y + y),
        blocks ? col::Error : (review ? col::Warn : col::TextMuted), n.c_str());
    ImGui::PopFont();
    text(p.body.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    if (p.has_where) {
        if (secondaryButton("Show", ImVec2(0, 0))) app.focusOn(p.where);
    }
    if (p.code == "suction_cup" || p.code == "resin_trap" || p.code == "cavity_ceiling") {
        if (p.has_where) ImGui::SameLine(0, 8);
        if (secondaryButton("Open Hollows", ImVec2(0, 0))) app.showHollow();
    }
    if (!p.engine.empty()) {
        ImGui::PushFont(fonts().regular, size::Help);
        ImGui::PushStyleColor(ImGuiCol_Text, col::TextMuted);
        if (ImGui::TreeNodeEx("Engine message", ImGuiTreeNodeFlags_SpanAvailWidth)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(p.engine.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TreePop();
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    endCard();
    ImGui::PopID();
}

}  // namespace

void supportChecks(App& app, Document& doc) {
    const SupportState& s = doc.supports;
    if (!s.result) return;
    const SupportResult& r = *s.result;
    for (std::size_t i = 0; i < r.checks.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        checkRow("check", r.checks[i]);
        ImGui::PopID();
        // Right after "No part starts in mid-air": what else may not print, from replaying the print.
        if (i == 0) riskChecks(app, doc);
    }
    if (r.checks.empty()) riskChecks(app, doc);
    if (r.problems.empty()) return;
    ImGui::Dummy(ImVec2(0, 6));
    int i = 0;
    auto section = [&](SupportProblem::Level level, const char* label) {
        const std::size_t n = r.count(level);
        if (n == 0) return;
        groupLabel(strf("%s · %zu", label, n).c_str());
        for (const auto& p : r.problems)
            if (p.level == level) problemCard(app, p, i++);
        ImGui::Dummy(ImVec2(0, 4));
    };
    section(SupportProblem::Level::Blocks, "Blocks saving");
    section(SupportProblem::Level::Review, "Review");
    section(SupportProblem::Level::Note, "Notes");
}

// ---- Over the 3D view ----

namespace {

// The legend, left to right from the cursor, on one line.
void legend(App& app, const SupportResult& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    float x = at.x;
    const float y = at.y + 14;
    if (app.supportsView() == SupportsView::Risk) {
        // What the paint and the tips' colours mean, or that the replay is not done yet.
        ImGui::Dummy(ImVec2(riskLegend(app, at), 28));
        if (const RiskReport* replay = app.risk(); replay && app.riskHeatmap()) {
            text(app.riskHeatOf() == HeatOf::Lost
                     ? "How likely each spot is not to print: lost with what breaks under it, or its own layer "
                       "not forming."
                     : "Where a break would start: how likely each layer's joint is to let go first.",
                 fonts().regular, size::Help, col::TextSecondary, true);
            text(riskStrengthsNote(*replay).c_str(), fonts().regular, size::Help, col::TextMuted, true);
        }
        return;
    }
    ImGui::PushFont(fonts().regular, size::Help);
    auto item = [&](const char* label, ImU32 c, bool line) {
        if (line) dl->AddLine(ImVec2(x, y), ImVec2(x + 12, y), c, 2);
        else dl->AddCircleFilled(ImVec2(x + 5, y), 4, c);
        x += line ? 18 : 14;
        dl->AddText(ImVec2(x, y - 7), col::TextSecondary, label);
        x += ImGui::CalcTextSize(label).x + 14;
    };
    if (app.supportsView() == SupportsView::Why) {
        item("Island", col::Accent, false);
        item("Overhang", IM_COL32(0x5B, 0x9B, 0xD5, 255), false);
        item("Extra hold", IM_COL32(0xB4, 0x8E, 0xF0, 255), false);
        item("Steadying", IM_COL32(0x5B, 0xD6, 0x9A, 255), false);
        item("By hand", IM_COL32(0xF2, 0xF4, 0xF7, 255), false);
    } else {
        for (const auto& t : r.tip_sizes) item(strf("%.1f mm", t.first).c_str(), IM_COL32(0xF2, 0xF4, 0xF7, 255), false);
        if (app.supportsView() == SupportsView::Supports) item("Support", IM_COL32(0xE2, 0xE6, 0xED, 255), true);
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(x - at.x, 28));
}

void runningCard(App& app, ImVec2 origin, ImVec2 size) {
    const JobProgress p = app.jobs().progress();
    const ImVec2 cs(340, 286);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - cs.x - 16, origin.y + size.y - cs.y - 16));
    beginCard("running", cs, 16, col::Panel, col::BorderStrong);
    text("Generating supports", fonts().semibold, size::Body + 1, col::Text);
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 5), col::Input, 3);
    dl->AddRectFilled(bp, ImVec2(bp.x + bw * static_cast<float>(p.fraction), bp.y + 5), col::Accent, 3);
    ImGui::Dummy(ImVec2(bw, 10));
    const int at = supportStage(p.stage, p.fraction);
    const auto& stages = supportStages();
    for (int i = 0; i < static_cast<int>(stages.size()); ++i) {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const ImVec2 c(q.x + 8, q.y + 9);
        if (i < at) {
            dl->AddCircleFilled(c, 8, col::OkBg, 20);
            icon::check(dl, ImVec2(c.x - 5, c.y - 5), 10, col::Ok);
        } else if (i == at) {
            dl->AddCircleFilled(c, 8, col::Accent, 20);
        } else {
            dl->AddCircle(c, 7.5f, col::BorderStrong, 20);
        }
        ImGui::SetCursorScreenPos(ImVec2(q.x + 24, q.y));
        text(stages[static_cast<std::size_t>(i)].c_str(), fonts().regular, size::Body,
             i <= at ? col::Text : col::TextMuted);
        ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + 24));
    }
    ImGui::SetCursorPosY(cs.y - 16 - 34);
    if (secondaryButton("Cancel", ImVec2(0, 34))) app.jobs().cancel();
    endCard();
}

// Banners sit in the free space right of the tool panel, above the model tools.
ImVec2 bannerSize(ImVec2 size) { return ImVec2(std::min(560.0f, size.x - kToolPanelRight - 32), 64); }
ImVec2 bannerAt(ImVec2 origin, ImVec2 size, ImVec2 bs) {
    const float left = origin.x + kToolPanelRight + 16;
    return ImVec2(std::max(left, origin.x + size.x - 16 - bs.x), origin.y + size.y - kToolsBarH - 8 - bs.y);
}

void staleBanner(App& app, Document& doc, ImVec2 origin, ImVec2 size) {
    const ImVec2 bs = bannerSize(size);
    ImGui::SetCursorScreenPos(bannerAt(origin, size, bs));
    beginCard("stale", bs, 14, col::WarnBg, col::WarnBorder);
    const float y = ImGui::GetCursorPosY();
    text("Supports are out of date", fonts().semibold, size::Body + 1, col::Text);
    const Stale stale = whyStale(doc);
    text(stale.why, fonts().regular, size::Body, col::TextSecondary);
    // Remaking them is the Job panel's main button; here, the way back.
    if (stale.undo && doc.canUndo()) {
        ImGui::PushFont(fonts().semibold, size::Body);
        const float uw = ImGui::CalcTextSize("Undo the change").x + 32;
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(bs.x - 14 - uw, y + 1));
        if (secondaryButton("Undo the change", ImVec2(uw, 34))) app.undo();
    }
    endCard();
}

// Supports are made for the model where it stands, and only when it fits there.
void fitBanner(App& app, Document& doc, ImVec2 origin, ImVec2 size) {
    const Part* off = doc.partOff();
    if (!off) return;
    const int id = off->id();
    const fruit::PlateFit f = off->fit();
    const ImVec2 bs = bannerSize(size);
    ImGui::SetCursorScreenPos(bannerAt(origin, size, bs));
    beginCard("fit", bs, 14, col::ErrorBg, col::Error);
    const float y = ImGui::GetCursorPosY();
    const std::string title = doc.partCount() > 1
                                  ? off->name() + (f.overEdge() ? " is over the plate's edge" : " doesn't fit the plate")
                                  : (f.overEdge() ? "It's over the plate's edge" : "It doesn't fit the plate");
    text(title.c_str(), fonts().semibold, size::Body + 1, col::Text);
    text(f.overEdge() ? "Supports are made for it where it stands, inside the dashed line Move shows."
         : f.fits_turned ? "Too big as it lies; turned 90° it fits at this size."
                         : "Too big at this size, however it is turned.",
         fonts().regular, size::Body, col::TextSecondary);
    const char* fix = f.overEdge() ? "Move it back on" : f.fits_turned ? "Turn 90°" : "Scale it";
    ImGui::PushFont(fonts().semibold, size::Body);
    const float rw = ImGui::CalcTextSize(fix).x + 32;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(bs.x - 14 - rw, y + 1));
    if (secondaryButton(fix, ImVec2(rw, 34))) {
        app.selectPart(id);  // the fixes act on the selected part
        if (f.overEdge()) app.slideOntoPlate();
        else if (f.fits_turned) app.turnQuarter();
        else app.openTool(Tool::Scale);
    }
    endCard();
}

}  // namespace

bool supportsBanner(App& app, const Document& doc, ImVec2 origin, ImVec2 size, ImVec2* min, ImVec2* max) {
    if (app.step() != Step::Supports || app.editingTips() || running(app)) return false;
    const SupportState& s = doc.supports;
    const bool fits = doc.fits();
    if (fits && !(s.result && !s.fresh(doc.revision(), doc.supportSettings()))) return false;
    const ImVec2 bs = bannerSize(size);
    *min = bannerAt(origin, size, bs);
    *max = ImVec2(min->x + bs.x, min->y + bs.y);
    return true;
}

void supportsOverlay(App& app, Document& doc, ImVec2 origin, ImVec2 size, const float* vp) {
    const SupportState& s = doc.supports;
    if (running(app)) {
        runningCard(app, origin, size);
        return;
    }
    if (!doc.fits()) fitBanner(app, doc, origin, size);
    if (!s.result) return;
    const SupportResult& r = *s.result;
    const bool fresh = s.fresh(doc.revision(), doc.supportSettings());
    if (!fresh && doc.fits()) staleBanner(app, doc, origin, size);
    // The Risk view has its own marks: the places that may not print, by number.
    if (app.supportsView() == SupportsView::Risk) {
        if (fresh) riskOverlay(app, doc, origin, size, vp);
        return;
    }
    // Tips: the selected one is ringed; a click (not a drag) selects the nearest.
    auto project = [&](const fruit::Vec3& w, ImVec2* out) {
        const float x = vp[0] * w.x + vp[4] * w.y + vp[8] * w.z + vp[12];
        const float y = vp[1] * w.x + vp[5] * w.y + vp[9] * w.z + vp[13];
        const float c = vp[3] * w.x + vp[7] * w.y + vp[11] * w.z + vp[15];
        if (c <= 1e-4f) return false;
        *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
        return true;
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (fresh && s.selected_tip >= 0) {
        ImVec2 q;
        if (project(r.points[static_cast<std::size_t>(s.selected_tip)].pos, &q)) {
            dl->AddCircle(q, 9, col::Accent, 24, 2.5f);
            dl->AddCircle(q, 12, IM_COL32(0xFF, 0x8F, 0x3F, 90), 24, 2.0f);
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool inside = m.x >= origin.x && m.y >= origin.y + 50 && m.x < origin.x + size.x && m.y < origin.y + size.y &&
                        ImGui::IsWindowHovered();
    if (fresh && inside && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9 &&
        !ImGui::IsAnyItemHovered()) {
        int best = -1;
        float best_d = 14 * 14;
        for (std::size_t i = 0; i < r.points.size(); ++i) {
            ImVec2 q;
            if (!project(r.points[i].pos, &q)) continue;
            const float d = (q.x - m.x) * (q.x - m.x) + (q.y - m.y) * (q.y - m.y);
            if (d < best_d) {
                best_d = d;
                best = static_cast<int>(i);
            }
        }
        app.selectTip(best);
    }
}

// Over the view's top: which supports to show, and what the marks mean.
void supportsViewPill(App& app, Document& doc) {
    if (!doc.supports.result || running(app)) return;
    const std::pair<const char*, SupportsView> views[] = {{"Supports", SupportsView::Supports},
                                                          {"Tips only", SupportsView::TipsOnly},
                                                          {"Why", SupportsView::Why},
                                                          {"Risk", SupportsView::Risk}};
    bool first = true;
    for (const auto& [label, v] : views) {
        if (!first) ImGui::SameLine(0, 4);
        first = false;
        if (segmentButton(label, app.supportsView() == v)) app.setSupportsView(v);
    }
    // The Risk view: the places in their three bands, or the heatmap; and what the heatmap is coloured by.
    if (app.supportsView() == SupportsView::Risk && app.risk()) {
        if (segmentButton("Places", !app.riskHeatmap())) app.setRiskHeatmap(false);
        ImGui::SameLine(0, 4);
        if (segmentButton("Heatmap", app.riskHeatmap())) app.setRiskHeatmap(true);
        if (app.riskHeatmap()) {
            ImGui::SameLine(0, 12);
            if (segmentButton("What would be lost", app.riskHeatOf() == HeatOf::Lost)) app.setRiskHeatOf(HeatOf::Lost);
            ImGui::SameLine(0, 4);
            if (segmentButton("Where a break starts", app.riskHeatOf() == HeatOf::Breaks))
                app.setRiskHeatOf(HeatOf::Breaks);
        }
    }
    legend(app, *doc.supports.result);
}

}  // namespace gui::screens::parts
