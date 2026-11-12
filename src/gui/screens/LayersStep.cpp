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

// The colour of layers `count` thin layers thick, on the slider and in its legend.
ImU32 layerHeightColor(int count) {
    switch (count) {
        case 1: return IM_COL32(0x52, 0x5E, 0x70, 255);   // thin: slate
        case 2: return IM_COL32(0x5B, 0x9B, 0xE0, 255);   // blue
        case 4: return IM_COL32(0xA7, 0x8B, 0xFA, 255);   // violet
        default: return IM_COL32(0x3F, 0xC7, 0xB8, 255);  // teal
    }
}

void layerColumn(App& app, Document& doc, float h) {
    beginPanel("layer_column", ImVec2(kColumnW, h), col::Panel, ImVec2(16, 18));
    edge(true, false);
    const LayerResult& r = *doc.layers.result;
    const int thin_n = static_cast<int>(r.layers->size());
    // With adaptive layers worked out and merged, these are the print file's own layers; else the job's.
    const PrintEstimate* fv = app.fileLayersView();
    const int n = fv ? static_cast<int>(fv->file_layers.size()) : thin_n;
    const int at = fv ? app.shownFile(*fv).file : shownLayer(doc);
    groupLabel("Layer");
    double v = at + 1;
    if (numberField("layer", &v, kColumnW - 32, "%g", 1, n)) {
        if (fv) app.setFileLayer(static_cast<int>(v) - 1);
        else app.setLayer(static_cast<int>(v) - 1);
    }
    text(("of " + thousands(static_cast<std::size_t>(n))).c_str(), fonts().regular, size::Body, col::TextSecondary);
    if (fv) {
        double thickest = 0;
        for (const FileLayer& l : fv->file_layers) thickest = std::max(thickest, l.thickness);
        text(strf("These are the saved file's layers: thin ones of %.2f mm merged up to %.2f mm. The checks ran on the "
                  "%s layers at %.2f mm.",
                  fv->thin_height, thickest, thousands(static_cast<std::size_t>(thin_n)).c_str(), r.layers->layer_height)
                 .c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
    } else if (const auto plan = app.printPlan(); plan && plan->ready && plan->reslice) {
        // The print file's layers are another height: say which these are.
        text(strf("At %.2f mm. Saving cuts %s layers at %.2f mm.", r.layers->layer_height,
                  thousands(plan->layers).c_str(), plan->layer_height)
                 .c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
    }
    // Adaptive layers: where each layer height is used, once worked out.
    const App::AdaptiveView adaptive = app.adaptiveView();
    const PrintEstimate* est = adaptive.numbers;
    const bool banded = est && est->bands.size() > 1;
    std::vector<int> heights;  // the thin-layer counts in use, thinnest first
    if (banded)
        for (const LayerBand& b : est->bands)
            if (std::find(heights.begin(), heights.end(), b.count) == heights.end()) heights.push_back(b.count);
    std::sort(heights.begin(), heights.end());
    if (banded && !fv)
        text(strf("Adaptive layers: %s when saved.", thousands(est->layers).c_str()).c_str(), fonts().regular, size::Help,
             col::TextMuted, true);
    else if (adaptive.working)
        text("Working out the adaptive layers.", fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 4));
    // Tall slider, layer 1 at the bottom, with marks for layers worth a look.
    const float legend_lines = 1.0f + (r.hollows.components.empty() ? 0.0f : 1.0f) + static_cast<float>(heights.size());
    const float sh = std::max(80.0f, ImGui::GetContentRegionAvail().y - std::max(46.0f, 14 + 16 * legend_lines));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = p.x + (kColumnW - 32) / 2 - 12;
    if (banded) {
        // The track, coloured by layer height (drawn under the slider, whose own track is then clear).
        ImDrawList* bl = ImGui::GetWindowDrawList();
        const double top = std::max(1e-9, est->bands.back().z1);
        auto yAt = [&](double z) {
            return p.y + sh - 7 - (sh - 14) * static_cast<float>(std::clamp(z / top, 0.0, 1.0));
        };
        bl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + 24, p.y + sh), col::Input, 6);
        for (std::size_t k = 0; k < est->bands.size(); ++k) {
            const LayerBand& b = est->bands[k];
            // The first band reaches the track's bottom end and the last its top end.
            const float y0 = k == 0 ? p.y + sh - 3 : yAt(b.z0);
            const float y1 = k + 1 == est->bands.size() ? p.y + 3 : yAt(b.z1);
            bl->AddRectFilled(ImVec2(x + 3, std::min(y1, y0 - 1)), ImVec2(x + 21, y0), layerHeightColor(b.count));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(x, p.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, banded ? IM_COL32(0, 0, 0, 0) : col::Input);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, banded ? IM_COL32(255, 255, 255, 18) : col::Active);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, banded ? IM_COL32(255, 255, 255, 18) : col::Active);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, col::Text);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, col::Accent);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14);
    if (fv) {
        // The track is height, the handle sits at the shown layer's mid-height and picks the layer at its height:
        // in a thick stretch there are fewer places to stop.
        const double top = std::max(1e-9, fv->file_layers.back().z1);
        float zf = static_cast<float>(fv->file_layers[static_cast<std::size_t>(at)].mid());
        if (ImGui::VSliderFloat("##layers", ImVec2(24, sh), &zf, 0.0f, static_cast<float>(top), "", ImGuiSliderFlags_AlwaysClamp))
            app.setFileLayerAtHeight(static_cast<double>(zf));
    } else {
        int s = at;
        if (ImGui::VSliderInt("##layers", ImVec2(24, sh), &s, 0, std::max(0, n - 1), "")) app.setLayer(s);
    }
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
            if (step.layer != at) {
                if (fv) app.setFileLayer(step.layer);
                else app.setLayer(step.layer);
            }
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (std::size_t m : r.marks) {
        // (a mark stays at its own height, in either kind of layers)
        const float frac = fv ? static_cast<float>(thinLayerMid(static_cast<int>(m), r.layers->layer_height) /
                                                    std::max(1e-9, fv->file_layers.back().z1))
                              : static_cast<float>(m) / static_cast<float>(std::max(1, n - 1));
        const float y = p.y + sh - 7 - (sh - 14) * std::clamp(frac, 0.0f, 1.0f);
        const bool big = m == r.biggest;
        dl->AddLine(ImVec2(x + 28, y), ImVec2(x + 40, y), big ? col::Accent : IM_COL32(0xF0, 0x6B, 0x9A, 255), 2);
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
    // The layer heights' colours, thinnest first.
    for (const int count : heights) {
        dl->AddRectFilled(ImVec2(p.x, ly + 3), ImVec2(p.x + 10, ly + 13), layerHeightColor(count), 2);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 14, ly + 1));
        text(strf("%g mm layers", count * est->thin_height).c_str(), fonts().regular, size::Label, col::TextMuted);
        ly += 16;
    }
    endPanel();
}

// `view`: the print file's layers when they are shown (then `at` is the 0.05 mm layer at the shown
// file layer's mid-height, and scrolling and clicks go by file layers).
void chart(App& app, const LayerResult& r, int at, ImVec2 size, double& scroll_carry, const PrintEstimate* view,
           int file_at) {
    beginPanel("area_chart", size, col::Panel, ImVec2(16, 12));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        dl->AddLine(wp, ImVec2(wp.x + size.x, wp.y), col::Border);
    }
    text("Layer area by height", fonts().semibold, size::Body, col::Text);
    ImGui::SameLine(0, 10);
    text("Bigger layers pull harder on the film as they peel.", fonts().regular, size::Body, col::TextMuted);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y - 4;
    const float left = 44, bottom = 16;
    const ImVec2 a(p.x + left, p.y), b(p.x + w, p.y + h - bottom);  // plot area
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
            if (view) {
                const LayerScrollStep step =
                    scrollLayer(file_at, static_cast<int>(view->file_layers.size()), units, scroll_carry);
                scroll_carry = step.carry;
                if (step.layer != file_at) {
                    app.setFileLayer(step.layer);
                    file_at = step.layer;
                    at = app.shownFile(*view).thin;
                }
            } else {
                const LayerScrollStep step = scrollLayer(at, static_cast<int>(n), units, scroll_carry);
                scroll_carry = step.carry;
                if (step.layer != at) app.setLayer(step.layer);
                at = step.layer;
            }
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
        ImGui::EndTooltip();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (view) app.setFileLayerAtHeight(thinLayerMid(static_cast<int>(i), r.layers->layer_height));
            else app.setLayer(static_cast<int>(i));
        }
    }
    endPanel();
}

void layerPicture(App& app, Document& doc, ImVec2 size) {
    const LayerResult& r = *doc.layers.result;
    const PrintEstimate* fv = app.fileLayersView();
    const int at = fv ? app.shownFile(*fv).file : shownLayer(doc);
    beginPanel("layer_picture", size, col::Viewport, ImVec2(16, 10));
    const float y = ImGui::GetCursorPosY();
    bool at_biggest = static_cast<std::size_t>(at) == r.biggest;
    if (fv) {
        // The print file's layer: where it sits, how thick it is, how much of it is lit.
        const FileLayer& l = fv->file_layers[static_cast<std::size_t>(at)];
        text(strf("Layer %d · %.2f to %.2f mm · %.2f mm thick", at + 1, l.z0, l.z1, l.thickness).c_str(),
             fonts().semibold, size::Body, col::Text);
        ImGui::SameLine(0, 10);
        text(strf("%.0f mm² lit", l.area).c_str(), fonts().regular, size::Body, col::TextSecondary);
        // The biggest layer is one of the job's: this layer holds it when it spans its mid-height.
        const double bz = thinLayerMid(static_cast<int>(r.biggest), r.layers->layer_height);
        at_biggest = bz >= l.z0 && bz < l.z1;
    } else {
        text(strf("Layer %d · %.2f mm", at + 1, r.layers->layerTop(static_cast<std::size_t>(at))).c_str(),
             fonts().semibold, size::Body, col::Text);
        ImGui::SameLine(0, 10);
        text(strf("%.0f mm² lit", r.area[static_cast<std::size_t>(at)]).c_str(), fonts().regular, size::Body,
             col::TextSecondary);
    }
    if (!at_biggest && r.biggest < r.area.size()) {
        ImGui::SameLine(0, 16);
        if (fv) {
            const std::size_t fb = static_cast<std::size_t>(fileLayerAt(
                fv->file_layers, thinLayerMid(static_cast<int>(r.biggest), r.layers->layer_height)));
            text(strf("Biggest: %.0f mm² at layer %zu", fv->file_layers[fb].area, fb + 1).c_str(), fonts().regular,
                 size::Body, col::TextMuted);
        } else {
            text(strf("Biggest: %.0f mm² at layer %zu", r.area[r.biggest], r.biggest + 1).c_str(), fonts().regular,
                 size::Body, col::TextMuted);
        }
        ImGui::SameLine(0, 8);
        if (linkButton("Go there")) {
            if (fv) app.setFileLayerAtHeight(thinLayerMid(static_cast<int>(r.biggest), r.layers->layer_height));
            else app.setLayer(static_cast<int>(r.biggest));
        }
    }
    // "Show the layer below", right-aligned.
    ImGui::PushFont(fonts().regular, size::Body);
    const float cw = ImGui::CalcTextSize("Show the layer below").x + 30;
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 16 - cw, y - 3));
    bool below = doc.layers.show_below;
    ImGui::PushFont(fonts().regular, size::Body);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Input);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
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
        ImGui::SetCursorScreenPos(ImVec2(o.x + (avail.x - sz.x) / 2, o.y + (avail.y - sz.y) / 2));
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex)), sz);
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
    // The chart's marker is at the 0.05 mm layer at the shown file layer's mid-height.
    const PrintEstimate* fv = app.fileLayersView();
    const LayerPick shown = fv ? app.shownFile(*fv) : LayerPick{};
    chart(app, *doc.layers.result, fv ? shown.thin : shownLayer(doc), ImVec2(w, kChartH), doc.layers.scroll_carry, fv,
          shown.file);
    ImGui::EndGroup();
    ImGui::SameLine(0, 0);
    printSide(app, doc, size.y);
}

}  // namespace gui::screens::parts
