// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// Job · Layers: every layer as the printer will light it, the area of each
// by height, and checks on the whole print (canvas boards "Slicing" and
// "Layers").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "LayerScroll.h"
#include "Risk.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

constexpr float kColumnW = 128, kChartH = 150;

int shownLayer(const Document& doc) {
    const LayerResult& r = *doc.layers.result;
    return doc.layers.current >= 0 ? doc.layers.current : static_cast<int>(r.biggest);
}

const ImU32 kRiskLine = IM_COL32(0xFF, 0x6B, 0x6B, 255);

ImU32 bandColor(RiskBand b) {
    const unsigned c = riskColour(b);
    return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
}

// The replay of the print, when it found something to show (null otherwise).
const RiskReport* riskToShow(App& app) {
    const RiskReport* r = app.risk();
    return r && !riskHeadline(*r).ok ? r : nullptr;
}

// How close the weakest place is while layer `i` of the Layers place prints (1: half would let go).
float closenessAt(const RiskReport& r, const LayerResult& lr, std::size_t i) {
    if (r.closeness.empty()) return 0;
    const std::size_t at = i > lr.base_layers ? i - lr.base_layers : 0;
    return r.closeness[std::min(at, r.closeness.size() - 1)];
}

void layerColumn(App& app, Document& doc, float h) {
    beginPanel("layer_column", ImVec2(kColumnW, h), col::Panel, ImVec2(16, 18));
    edge(true, false);
    const LayerResult& r = *doc.layers.result;
    const int n = static_cast<int>(r.layers->size());
    const int at = shownLayer(doc);
    groupLabel("Layer");
    double v = at + 1;
    if (numberField("layer", &v, kColumnW - 32, "%g", 1, n)) app.setLayer(static_cast<int>(v) - 1);
    text(("of " + thousands(static_cast<std::size_t>(n))).c_str(), fonts().regular, size::Body, col::TextSecondary);
    if (const auto plan = app.printPlan(); plan && plan->ready && plan->reslice) {
        // The print file's layers are another height: say which these are.
        text(strf("At %.2f mm. Saving cuts %s layers at %.2f mm.", r.layers->layer_height,
                  thousands(plan->layers).c_str(), plan->layer_height)
                 .c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
    }
    ImGui::Dummy(ImVec2(0, 4));
    // Tall slider, layer 1 at the bottom, with marks for layers worth a look.
    const RiskReport* replay = riskToShow(app);
    const int riskiest = replay && replay->riskiest_layer >= 0 ? app.layerOfRisk(replay->riskiest_layer) : -1;
    const int legend_lines = 1 + (r.hollows.components.empty() ? 0 : 1) + (riskiest >= 0 ? 1 : 0);
    const float sh = std::max(80.0f, ImGui::GetContentRegionAvail().y - 14 - 16 * std::max(2, legend_lines));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = p.x + (kColumnW - 32) / 2 - 12;
    ImGui::SetCursorScreenPos(ImVec2(x, p.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Input);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::Active);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col::Active);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, col::Text);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, col::Accent);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14);
    int s = at;
    if (ImGui::VSliderInt("##layers", ImVec2(24, sh), &s, 0, std::max(0, n - 1), "")) app.setLayer(s);
    if (ImGui::IsItemHovered()) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the slider has the wheel
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    // Two-finger scrolling anywhere over this column moves the layer: swiping up goes up the print,
    // as the slider's handle does (a swipe up arrives as a negative MouseWheel with natural scrolling,
    // the Mac default).
    if (n > 1 && ImGui::IsWindowHovered()) {
        const double units = -static_cast<double>(ImGui::GetIO().MouseWheel);
        if (units != 0) {
            const LayerScrollStep step = scrollLayer(at, n, units, doc.layers.scroll_carry);
            doc.layers.scroll_carry = step.carry;
            if (step.layer != at) app.setLayer(step.layer);
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (std::size_t m : r.marks) {
        const float y = p.y + sh - 7 - (sh - 14) * static_cast<float>(m) / static_cast<float>(std::max(1, n - 1));
        const bool big = m == r.biggest;
        dl->AddLine(ImVec2(x + 28, y), ImVec2(x + 40, y), big ? col::Accent : IM_COL32(0xF0, 0x6B, 0x9A, 255), 2);
    }
    // The layer whose pull comes closest to breaking something: on the slider's other side.
    if (riskiest >= 0) {
        const float y = p.y + sh - 7 - (sh - 14) * static_cast<float>(riskiest) / static_cast<float>(std::max(1, n - 1));
        dl->AddLine(ImVec2(x - 16, y), ImVec2(x - 4, y), kRiskLine, 2);
    }
    // Legend: what each kind of mark is.
    float ly = p.y + sh + 8;
    auto legendLine = [&](ImU32 c, const char* label) {
        dl->AddLine(ImVec2(p.x, ly + 8), ImVec2(p.x + 10, ly + 8), c, 2);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 14, ly + 1));
        text(label, fonts().regular, size::Label, col::TextMuted);
        ly += 16;
    };
    legendLine(col::Accent, "biggest layer");
    if (!r.hollows.components.empty()) legendLine(IM_COL32(0xF0, 0x6B, 0x9A, 255), "cup or trap starts");
    if (riskiest >= 0) legendLine(kRiskLine, "riskiest layer");
    endPanel();
}

void chart(App& app, const LayerResult& r, int at, ImVec2 size, double& scroll_carry) {
    beginPanel("area_chart", size, col::Panel, ImVec2(16, 12));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        dl->AddLine(wp, ImVec2(wp.x + size.x, wp.y), col::Border);
    }
    // With the print replayed: a second line, how close the weakest place is to letting go.
    const RiskReport* replay = app.risk();
    if (replay && replay->closeness.empty()) replay = nullptr;
    if (replay) {
        text("Through the print", fonts().semibold, size::Body, col::Text);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto key = [&](ImU32 c, const char* label, bool dashed = false) {
            ImGui::SameLine(0, 16);
            const ImVec2 q = ImGui::GetCursorScreenPos();
            if (dashed) {
                for (float xx = 0; xx < 16; xx += 6) dl->AddLine(ImVec2(q.x + xx, q.y + 9), ImVec2(q.x + xx + 3, q.y + 9), c, 1);
            } else {
                dl->AddLine(ImVec2(q.x, q.y + 9), ImVec2(q.x + 12, q.y + 9), c, 2);
            }
            ImGui::SetCursorScreenPos(ImVec2(q.x + (dashed ? 21.0f : 18.0f), q.y));
            text(label, fonts().regular, size::Body, col::TextMuted);
        };
        key(IM_COL32(0x5B, 0x9B, 0xD5, 255), "layer area: bigger layers pull harder on the film");
        key(kRiskLine, "how close the weakest place is to letting go");
        key(kRiskLine, "half would let go", true);
    } else {
        text("Layer area by height", fonts().semibold, size::Body, col::Text);
        ImGui::SameLine(0, 10);
        text("Bigger layers pull harder on the film as they peel.", fonts().regular, size::Body, col::TextMuted);
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y - 4;
    const float left = 44, bottom = 16, right = replay ? 46.0f : 0.0f;
    const ImVec2 a(p.x + left, p.y), b(p.x + w - right, p.y + h - bottom);  // plot area
    ImGui::InvisibleButton("plot", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::size_t n = r.area.size();
    if (hovered && n > 1) {
        // Two-finger scrolling on a Mac trackpad (natural scrolling, the default): swiping right or up
        // goes to higher layers, as if pushing the marker along. Swiping up arrives as a negative
        // MouseWheel (the page moves up), swiping right as a positive MouseWheelH. The chart owns the
        // wheel while the pointer is over it, so no panel behind it scrolls too (from the next frame's input).
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
        const ImGuiIO& io = ImGui::GetIO();
        const double units = static_cast<double>(io.MouseWheelH) - static_cast<double>(io.MouseWheel);
        if (units != 0) {
            const LayerScrollStep step = scrollLayer(at, static_cast<int>(n), units, scroll_carry);
            scroll_carry = step.carry;
            if (step.layer != at) app.setLayer(step.layer);
            at = step.layer;
        }
    }
    double peak = 1;
    for (double v : r.area) peak = std::max(peak, v);
    const double nice = std::pow(10.0, std::floor(std::log10(peak)));
    const double ymax = std::ceil(peak / nice) * nice;
    ImGui::PushFont(fonts().regular, size::Label);
    for (int k = 0; k <= 4; ++k) {  // horizontal grid with labels
        const float y = b.y - (b.y - a.y) * k / 4.0f;
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), col::Border);
        const std::string l = strf("%.0f", ymax * k / 4);
        dl->AddText(ImVec2(p.x + left - 8 - ImGui::CalcTextSize(l.c_str()).x, y - 6), col::TextMuted, l.c_str());
    }
    const double top = r.top;
    const double step_mm = top > 100 ? 20 : (top > 40 ? 10 : (top > 15 ? 5 : 1));
    for (double z = 0; z <= top + 1e-9; z += step_mm) {
        const float x = a.x + (b.x - a.x) * static_cast<float>(z / std::max(top, 1e-9));
        const std::string l = strf("%.0f", z);
        dl->AddText(ImVec2(x - ImGui::CalcTextSize(l.c_str()).x / 2, b.y + 2), col::TextMuted, l.c_str());
    }
    ImGui::PopFont();
    auto px = [&](std::size_t i) {
        return ImVec2(a.x + (b.x - a.x) * static_cast<float>(i) / static_cast<float>(std::max<std::size_t>(1, n - 1)),
                      b.y - (b.y - a.y) * static_cast<float>(r.area[i] / ymax));
    };
    // The curve, thinned to about one point per pixel column.
    const std::size_t stride = std::max<std::size_t>(1, n / static_cast<std::size_t>(std::max(1.0f, b.x - a.x)));
    for (std::size_t i = stride; i < n; i += stride) dl->AddLine(px(i - stride), px(i), IM_COL32(0x5B, 0x9B, 0xD5, 255), 1.5f);
    if (n) {
        const ImVec2 q = px(static_cast<std::size_t>(at));
        dl->AddLine(ImVec2(q.x, a.y), ImVec2(q.x, b.y), col::TextMuted, 1);
        dl->AddCircleFilled(q, 4, IM_COL32(0x5B, 0x9B, 0xD5, 255));
    }
    // How close the weakest place is, against the right-hand scale: 100 % is where half of such places let go.
    double cmax = 1.5;
    auto py = [&](double closeness) { return b.y - (b.y - a.y) * static_cast<float>(std::min(closeness, cmax) / cmax); };
    if (replay && n) {
        float top = 0;
        for (float v : replay->closeness) top = std::max(top, v);
        cmax = std::max(1.5, std::ceil(static_cast<double>(top) * 2) / 2);
        ImGui::PushFont(fonts().regular, size::Label);
        const double step = cmax > 3 ? 1.0 : 0.5;
        for (double v = 0; v <= cmax + 1e-9; v += step) {
            const std::string l = strf("%.0f %%", v * 100);
            dl->AddText(ImVec2(b.x + 8, py(v) - 6), col::TextMuted, l.c_str());
        }
        // Half would let go: a dashed line (named in the key above, so nothing covers the curves).
        const float y1 = py(1.0);
        for (float xx = a.x; xx < b.x; xx += 8) dl->AddLine(ImVec2(xx, y1), ImVec2(std::min(xx + 4, b.x), y1), kRiskLine, 1);
        ImGui::PopFont();
        auto rx = [&](std::size_t i) { return ImVec2(px(i).x, py(closenessAt(*replay, r, i))); };
        for (std::size_t i = stride; i < n; i += stride) dl->AddLine(rx(i - stride), rx(i), kRiskLine, 1.5f);
        const float now = closenessAt(*replay, r, static_cast<std::size_t>(at));
        const ImVec2 q = rx(static_cast<std::size_t>(at));
        dl->AddCircleFilled(q, 4, now >= 1 ? bandColor(RiskBand::VeryLikely) : kRiskLine);
        dl->AddCircle(q, 4.5f, col::Text, 16, 1.5f);
        // What it is at this layer, beside the marker (on its left near the plot's right end).
        ImGui::PushFont(fonts().medium, size::Label);
        const std::string l = strf("%.0f %% at this layer", now * 100);
        const ImVec2 ls = ImGui::CalcTextSize(l.c_str());
        const float lx = q.x + 10 + ls.x > b.x ? q.x - 10 - ls.x : q.x + 10;
        const float ly = q.y - 20 >= a.y ? q.y - 20 : q.y + 8;  // over the marker, or under it at the plot's top
        dl->AddRectFilled(ImVec2(lx - 4, ly - 1), ImVec2(lx + ls.x + 4, ly + ls.y + 1), col::Panel, 3);
        dl->AddText(ImVec2(lx, ly), col::TextSecondary, l.c_str());
        ImGui::PopFont();
    }
    if (hovered && n) {
        const float mx = ImGui::GetIO().MousePos.x;
        const auto i = static_cast<std::size_t>(
            std::clamp((mx - a.x) / std::max(1.0f, b.x - a.x), 0.0f, 1.0f) * static_cast<float>(n - 1) + 0.5f);
        const ImVec2 q = px(i);
        dl->AddCircle(q, 5, col::Text, 16, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Card);
        ImGui::BeginTooltip();
        text(strf("%.0f mm²", r.area[i]).c_str(), fonts().semibold, size::Body, col::Text);
        text(strf("at %.2f mm · layer %zu", r.layers->layerTop(i), i + 1).c_str(), fonts().regular, size::Help,
             col::TextSecondary);
        if (replay)
            text(strf("weakest place at %.0f %% of where half let go", closenessAt(*replay, r, i) * 100).c_str(),
                 fonts().regular, size::Help, col::TextSecondary);
        ImGui::EndTooltip();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) app.setLayer(static_cast<int>(i));
    }
    endPanel();
}

void layerPicture(App& app, Document& doc, ImVec2 size) {
    const LayerResult& r = *doc.layers.result;
    const int at = shownLayer(doc);
    beginPanel("layer_picture", size, col::Viewport, ImVec2(16, 10));
    const float y = ImGui::GetCursorPosY();
    text(strf("Layer %d · %.2f mm", at + 1, r.layers->layerTop(static_cast<std::size_t>(at))).c_str(), fonts().semibold,
         size::Body, col::Text);
    ImGui::SameLine(0, 10);
    text(strf("%.0f mm² lit", r.area[static_cast<std::size_t>(at)]).c_str(), fonts().regular, size::Body,
         col::TextSecondary);
    if (static_cast<std::size_t>(at) != r.biggest && r.biggest < r.area.size()) {
        ImGui::SameLine(0, 16);
        text(strf("Biggest: %.0f mm² at layer %zu", r.area[r.biggest], r.biggest + 1).c_str(), fonts().regular,
             size::Body, col::TextMuted);
        ImGui::SameLine(0, 8);
        if (linkButton("Go there")) app.setLayer(static_cast<int>(r.biggest));
    }
    // "Show risk" (once the print has been replayed) and "Show the layer below", right-aligned.
    const RiskReport* replay = app.risk();
    ImGui::PushFont(fonts().regular, size::Body);
    const float cw = ImGui::CalcTextSize("Show the layer below").x + 30;
    const float rw = ImGui::CalcTextSize("Show risk").x + 30;
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Input);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
    if (replay) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 16 - cw - 16 - rw, y - 3));
        bool risk = app.layersRisk();
        if (ImGui::Checkbox("Show risk", &risk)) app.setLayersRisk(risk);
        tooltip("Colours what may not print, as the replay of the print found it.");
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 16 - cw, y - 3));
    bool below = doc.layers.show_below;
    if (ImGui::Checkbox("Show the layer below", &below)) app.setShowBelow(below);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // The picture, with plate millimetres kept square on screen.
    ImGui::SetCursorPosY(y + 28);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 avail(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 4);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + avail.x, o.y + avail.y), IM_COL32(0, 0, 0, 255), 6);
    const float fb = ImGui::GetIO().DisplayFramebufferScale.x;
    const LayerImage* im = nullptr;
    const unsigned tex = app.layerPicture(static_cast<int>(avail.x * fb), static_cast<int>(avail.y * fb), &im);
    if (tex && im) {
        const double mm_w = im->w * im->px, mm_h = im->h * im->py;
        const float k = static_cast<float>(std::min(avail.x / mm_w, avail.y / mm_h));
        const ImVec2 sz(static_cast<float>(mm_w) * k, static_cast<float>(mm_h) * k);
        const ImVec2 io(o.x + (avail.x - sz.x) / 2, o.y + (avail.y - sz.y) / 2);
        ImGui::SetCursorScreenPos(io);
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), sz);
        if (replay && app.layersRisk()) {
            // The places at their worst while this layer prints, by their numbers in the Checks.
            for (std::size_t i = 0; i < replay->places.size(); ++i) {
                const RiskPlace& pl = replay->places[i];
                if (pl.layer < 0 || app.layerOfRisk(pl.layer) != at) continue;
                const ImVec2 q(io.x + static_cast<float>((pl.at.x - im->x_left) / mm_w) * sz.x,
                               io.y + static_cast<float>((im->y_top - pl.at.y) / mm_h) * sz.y);
                dl->AddCircleFilled(q, 11, IM_COL32(0x18, 0x1B, 0x21, 235), 24);
                dl->AddCircle(q, 10.25f, col::Text, 24, 1.5f);
                const std::string s = std::to_string(i + 1);
                ImGui::PushFont(fonts().bold, size::Label);
                const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
                dl->AddText(ImVec2(q.x - ts.x / 2, q.y - ts.y / 2), col::Text, s.c_str());
                ImGui::PopFont();
            }
        }
    }
    if (replay && app.layersRisk()) {
        // What the colours mean, along the picture's foot.
        ImGui::PushFont(fonts().regular, size::Help);
        float x = o.x + 12;
        const float yl = o.y + avail.y - 18;
        auto item = [&](const char* label, ImU32 c) {
            dl->AddRectFilled(ImVec2(x, yl - 5), ImVec2(x + 10, yl + 5), c, 2);
            x += 16;
            dl->AddText(ImVec2(x, yl - 7), col::TextSecondary, label);
            x += ImGui::CalcTextSize(label).x + 16;
        };
        item("prints", IM_COL32(255, 255, 255, 255));
        item("may not", bandColor(RiskBand::May));
        item("likely not", bandColor(RiskBand::Likely));
        item("very likely not", bandColor(RiskBand::VeryLikely));
        ImGui::PopFont();
    }
    endPanel();
}

void slicingCard(App& app, ImVec2 size) {
    beginPanel("slicing", size, col::App, ImVec2(0, 0));
    const JobProgress p = app.jobs().progress();
    const ImVec2 cs(440, 220);
    ImGui::SetCursorPos(ImVec2((size.x - cs.x) / 2, (size.y - cs.y) / 2 - 30));
    beginCard("slicing_card", cs, 18, col::Panel, col::BorderStrong);
    text("Cutting the layers", fonts().semibold, size::Section + 2, col::Text);
    text("Each layer is drawn at the printer's own pixel size (14 × 19 µm) and checked as it goes.", fonts().regular,
         size::Body, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 4));
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 5), col::Input, 3);
    dl->AddRectFilled(bp, ImVec2(bp.x + bw * static_cast<float>(p.fraction), bp.y + 5), col::Accent, 3);
    ImGui::Dummy(ImVec2(bw, 10));
    const char* stages[] = {"Cut the model and supports into layers", "Look for suction cups and resin traps"};
    const int now = p.stage == "hollows" ? 1 : 0;
    for (int i = 0; i < 2; ++i) {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const ImVec2 c(q.x + 8, q.y + 9);
        if (i < now) {
            dl->AddCircleFilled(c, 8, col::OkBg, 20);
            icon::check(dl, ImVec2(c.x - 5, c.y - 5), 10, col::Ok);
        } else if (i == now) {
            dl->AddCircleFilled(c, 8, col::Accent, 20);
        } else {
            dl->AddCircle(c, 7.5f, col::BorderStrong, 20);
        }
        ImGui::SetCursorScreenPos(ImVec2(q.x + 24, q.y));
        text(stages[i], fonts().regular, size::Body, i <= now ? col::Text : col::TextMuted);
        ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + 24));
    }
    ImGui::SetCursorPosY(cs.y - 18 - 34);
    if (secondaryButton("Cancel", ImVec2(0, 34))) app.jobs().cancel();
    endCard();
    endPanel();
}

}  // namespace

void layersMode(App& app, Document& doc, ImVec2 size) {
    if (showingSaved(doc)) {
        savedView(app, doc, size);
        return;
    }
    if (const Need need = app.needFor(Step::Layers); need != Need::Nothing) {
        needCard(app, doc, need, ImVec2(ImGui::GetContentRegionAvail().x, size.y));
        return;
    }
    const bool cutting = app.jobs().busy() && app.jobs().progress().name == "Cutting layers";
    if (cutting || !doc.layersFresh()) {
        if (cutting) slicingCard(app, ImVec2(ImGui::GetContentRegionAvail().x, size.y));
        else {
            beginPanel("layers_wait", ImVec2(0, size.y), col::App, ImVec2(24, 24));
            text("Cutting the layers starts in a moment.", fonts().regular, size::Body, col::TextSecondary);
            endPanel();
        }
        return;
    }
    layerColumn(app, doc, size.y);
    ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
    const float w = ImGui::GetContentRegionAvail().x - kJobW;
    layerPicture(app, doc, ImVec2(w, size.y - kChartH));
    chart(app, *doc.layers.result, shownLayer(doc), ImVec2(w, kChartH), doc.layers.scroll_carry);
    ImGui::EndGroup();
    ImGui::SameLine(0, 0);
    printSide(app, doc, size.y);
}

}  // namespace gui::screens::parts
