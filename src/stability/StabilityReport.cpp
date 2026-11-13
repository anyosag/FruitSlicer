// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "stability/StabilityReport.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fruit {

using nlohmann::json;

json stabilityJson(const StabilityResult& r, const StabilityConfig& cfg, std::size_t max_spots) {
    const StabilityStats& s = r.stats;
    json j;
    j["layers"] = s.layers;
    j["layer_height"] = r.layer_height;
    j["parts"] = s.parts;
    j["model_parts"] = s.model_parts;
    j["tips"] = s.tips;
    j["model_volume_mm3"] = s.model_volume;
    j["unheld_parts"] = s.unheld;
    j["unheld_specks"] = s.unheld_specks;
    j["slivers_not_listed"] = s.slivers;
    j["whole_print_chance"] = s.whole_chance;
    j["expected_loss_mm3"] = s.expected_loss;
    j["volume_at_risk_mm3"] = {{"1%", s.at_risk[0]}, {"5%", s.at_risk[1]}, {"20%", s.at_risk[2]}, {"50%", s.at_risk[3]}};
    j["closest_to_breaking"] = s.worst_closeness;
    j["replays"] = s.replays;
    j["seconds"] = s.seconds;
    const double d = cfg.capacity.reference_diameter > 0 ? cfg.capacity.reference_diameter : 0.4;
    j["rests_on"] = {
        {"tip_diameter_mm", d},
        {"tip_trusted_with_mm2", cfg.capacity.capacity(d)},
        {"tip_half_let_go_at_mm2", cfg.capacity.capacity(d) / cfg.usable_fraction},
        {"tip_table_measured", !cfg.capacity.table.empty()},
        {"resin_per_mm2_as_multiple_of_tip_contact", cfg.bulk_factor},
        {"steepness", cfg.steepness},
        {"skin_half_reach_mm", cfg.skin_half_reach},
        {"cup_pull", cfg.cup_pull},
        {"thin_slab_mm", cfg.slab},
        {"tip_share_reach_mm", cfg.share_reach},
        {"shape_effect", cfg.shape_effect},
        {"peel_bias", cfg.peel_bias},
        {"note", "Loads are mm2 of layer. Strengths are the planner's own (borrowed unless a resin's tip plate was "
                 "fitted), so the chances rank places against each other; they are not measured chances. Bending "
                 "assumes nothing holds a piece sideways: an upper limit. The tilt's sideways pull, sway and curl "
                 "are left out."}};
    json spots = json::array();
    for (const WeakSpot& w : r.spots) {
        if (spots.size() >= max_spots) break;
        json e = {{"cause", causeName(w.cause)},
                  {"at", {w.at.x, w.at.y, w.at.z}},
                  {"layer", w.layer},
                  {"chance", w.chance},
                  {"chance_lost_above", w.lost_chance},
                  {"area_mm2", w.area}};
        // What goes with it: averaged over the replays in which it let go.
        if (w.cause != RiskCause::Skin) e["model_lost_with_it_mm3"] = w.lost_volume;
        if (w.cause != RiskCause::Unheld && w.cause != RiskCause::Skin) {
            e["closeness"] = w.closeness;
            // Bending assumes nothing holds the piece sideways. If the film
            // does, this is all that is left of it.
            if (w.cause == RiskCause::Bend && w.holds > 0) e["closeness_if_pulled_straight"] = w.load / w.holds;
            e["load_mm2"] = w.load;
            // A tip is pulled by the thin, fresh model near it, and by its share of the whole piece.
            if (w.cause == RiskCause::Tip) e["of_which_thin_model_near_it_mm2"] = w.near;
            e["half_break_at_mm2"] = w.holds;
            e["while_printing_layer"] = w.at_layer;
            e["joints_like_it_nearby"] = w.count;
        }
        spots.push_back(std::move(e));
    }
    {
        // Tips with a chance of 1 in 20 or more of letting go, and the worst of them.
        std::size_t at_risk = 0;
        double worst = 0;
        for (const TipRisk& t : r.tips) {
            at_risk += t.chance >= 0.05;
            worst = std::max(worst, t.chance);
        }
        j["tips_that_may_let_go"] = at_risk;
        j["worst_tip_chance"] = worst;
        // How often part of the model went with tips that let go, and how much then.
        j["chance_model_goes_with_tips"] = r.stats.tips_cost_chance;
        j["model_gone_with_tips_then_mm3"] = r.stats.tips_cost_volume;
        // Each of them (the first forty), the likeliest first.
        std::vector<const TipRisk*> order;
        for (const TipRisk& t : r.tips)
            if (t.chance >= 0.05) order.push_back(&t);
        std::stable_sort(order.begin(), order.end(), [](const TipRisk* a, const TipRisk* b) { return a->chance > b->chance; });
        json tips = json::array();
        for (const TipRisk* t : order) {
            if (tips.size() >= 40) break;
            tips.push_back({{"at", {t->at.x, t->at.y, t->at.z}},
                            {"upside_down", t->foot},
                            {"chance", t->chance},
                            {"closeness", t->closeness},
                            {"touches_mm2", t->area},
                            {"load_mm2", t->load},
                            {"of_which_thin_model_near_it_mm2", t->near},
                            {"half_let_go_at_mm2", t->holds},
                            {"while_printing_layer", t->at_layer},
                            {"model_lost_with_it_mm3", t->lost_volume},
                            {"model_lost_with_it_per_print_mm3", t->lost_per_print},
                            {"chance_lost_above", t->lost_chance}});
        }
        j["tips_at_risk"] = std::move(tips);
    }
    j["spots_listed"] = spots.size();
    j["spots_found"] = r.spots.size();
    j["spots"] = std::move(spots);
    j["closeness_by_layer"] = r.closeness_by_layer;
    return j;
}

namespace {

// 0 background, 1 support, 2 model, 3..6 the four bands.
int band(double chance) {
    if (chance >= 0.5) return 6;
    if (chance >= 0.2) return 5;
    if (chance >= 0.05) return 4;
    if (chance >= 0.01) return 3;
    return 0;
}

const std::uint8_t kColours[7][3] = {{255, 255, 255}, {221, 225, 231}, {174, 182, 196}, {242, 211, 75},
                                     {242, 154, 58},  {229, 83, 58},   {142, 27, 45}};

struct Canvas {
    int w = 0, h = 0;
    std::vector<std::uint8_t> cls;
    void put(int x, int y, int c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        std::uint8_t& p = cls[static_cast<size_t>(y) * w + x];
        if (c > p) p = static_cast<std::uint8_t>(c);
    }
    void span(int x0, int x1, int y, int c) {
        for (int x = std::max(x0, 0); x <= std::min(x1, w - 1); ++x) put(x, y, c);
    }
};

}  // namespace

std::vector<std::uint8_t> stabilityPicture(const StabilityResult& r, int& width, int& height, int max_size) {
    width = height = 0;
    if (r.where.empty()) return {};
    BBox2 box;
    for (const PrintPart& p : r.where) box.extend(p.box);
    const double zs = static_cast<double>(r.in_layer.size()) * r.layer_height;
    const Vec2 size = box.size();
    const int m = 16;
    const double scale = std::clamp(
        std::min((max_size - 3.0 * m) / std::max(size.x + size.y, 1.0), (max_size - 3.0 * m) / std::max(2 * zs, 1.0)), 2.0,
        14.0);
    const int pw_front = static_cast<int>(std::ceil(size.x * scale)) + 1, pw_side = static_cast<int>(std::ceil(size.y * scale)) + 1;
    const int ph = static_cast<int>(std::ceil(zs * scale)) + 1, legend = 14;
    Canvas c;
    c.w = 3 * m + pw_front + pw_side;
    c.h = 3 * m + 2 * ph + legend;
    c.cls.assign(static_cast<size_t>(c.w) * c.h, 0);
    const int x_front = m, x_side = 2 * m + pw_front;
    const int y_risk = m + ph - 1, y_weak = 2 * m + 2 * ph - 1;  // the plate's row in each panel row
    auto col = [&](double v, double lo) { return static_cast<int>(std::floor((v - lo) * scale)); };
    auto paint = [&](int base_y, int row, const BBox2& b, int cl) {
        c.span(x_front + col(b.min.x, box.min.x), x_front + col(b.max.x, box.min.x), base_y - row, cl);
        c.span(x_side + col(b.min.y, box.min.y), x_side + col(b.max.y, box.min.y), base_y - row, cl);
    };
    for (std::size_t L = 0; L < r.in_layer.size(); ++L) {
        const int row = static_cast<int>(std::floor((static_cast<double>(L) + 0.5) * r.layer_height * scale));
        for (int id : r.in_layer[L]) {
            const PrintPart& p = r.where[static_cast<size_t>(id)];
            const PartRisk& pr = r.parts[static_cast<size_t>(id)];
            const int plain = pr.model ? 2 : 1;
            paint(y_risk, row, p.box, std::max(plain, band(r.partChance(id))));
            paint(y_weak, row, p.box, plain);
            // A weak joint where it is, not across the whole part that rests on it.
            if (const int cl = band(pr.break_chance)) {
                const double half = std::max(0.25, std::sqrt(static_cast<double>(pr.joint_area)) / 2);
                BBox2 at;
                at.extend({pr.joint.x - half, pr.joint.y - half});
                at.extend({pr.joint.x + half, pr.joint.y + half});
                paint(y_weak, row, at, cl);
            }
        }
        if (L < r.skins.size())
            for (const SkinBand& sb : r.skins[L]) {
                const int cl = band(1 - (1 - r.partChance(sb.part)) * (1 - sb.chance));
                if (cl == 0) continue;
                for (const Path& path : sb.region)
                    if (area(path) > 0) paint(y_risk, row, bbox(path), cl);
            }
    }
    for (const WeakSpot& w : r.spots) {
        const int cl = band(w.chance);
        if (cl == 0) continue;
        const int row = static_cast<int>(std::floor(w.at.z * scale));
        for (int dy = -3; dy <= 3; ++dy) {
            const int fx = x_front + col(w.at.x, box.min.x), sx = x_side + col(w.at.y, box.min.y);
            c.span(fx - 3, fx + 3, y_weak - row + dy, cl);
            c.span(sx - 3, sx + 3, y_weak - row + dy, cl);
        }
    }
    for (int k = 1; k <= 6; ++k)
        for (int y = 0; y < 10; ++y) c.span(m + (k - 1) * 24, m + (k - 1) * 24 + 17, c.h - m + y - 6, k);

    width = c.w;
    height = c.h;
    std::vector<std::uint8_t> rgb(static_cast<size_t>(c.w) * c.h * 3);
    for (size_t i = 0; i < c.cls.size(); ++i)
        for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = kColours[c.cls[i]][k];
    // The plate under each row of pictures.
    for (int base : {y_risk + 1, y_weak + 1})
        for (int x = m / 2; x < c.w - m / 2; ++x)
            if (base < c.h)
                for (int k = 0; k < 3; ++k) rgb[(static_cast<size_t>(base) * c.w + x) * 3 + k] = 70;
    return rgb;
}

}  // namespace fruit
