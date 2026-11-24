// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "calibration/CalibrationFit.h"
#include "stability/Stability.h"
#include "support/SupportGeometry.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fruit {

using json = nlohmann::json;

namespace {

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n\"");
    const auto e = s.find_last_not_of(" \t\r\n\"");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') quoted = !quoted;
        else if (c == ',' && !quoted) { out.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

double num(const json& j, const char* key) { return j.at(key).get<double>(); }

std::string fmt(double v) {
    std::ostringstream s;
    s.precision(3);
    s << v;
    return s.str();
}

}  // namespace

std::string manifestForResults(const std::string& csv) {
    const std::string suffix = ".results.csv";
    if (csv.size() > suffix.size() && csv.compare(csv.size() - suffix.size(), suffix.size(), suffix) == 0)
        return csv.substr(0, csv.size() - suffix.size()) + ".json";
    throw std::runtime_error("cannot infer the manifest for " + csv + " (expected <plate>.stl.results.csv)");
}

std::string blindResultsCsv(const json& manifest, const std::vector<CouponResult>& scores) {
    std::map<int, const CouponResult*> by_id;
    for (const auto& s : scores) by_id[s.id] = &s;
    std::ostringstream out;
    out << "id,row,col,result,scar,tips_let_go,notes\n";
    for (const auto& c : manifest.at("coupons")) {
        const int id = c.at("id").get<int>();
        out << id << "," << c.value("row", 0) << "," << c.value("col", 0) << ",";
        const auto it = by_id.find(id);
        const CouponResult* s = it == by_id.end() ? nullptr : it->second;
        if (s && s->recorded) out << (s->ok ? "ok" : s->partial ? "partial" : "failed");
        out << ",";
        if (s && s->scar > 0) out << s->scar;
        out << ",";
        if (s && s->recorded && s->partial && s->tips_let_go > 0) out << s->tips_let_go;
        out << ",";
        if (s && !s->notes.empty()) {
            // Quoted, so commas and spaces stay; line breaks and double quotes don't.
            std::string n = s->notes;
            for (char& ch : n) {
                if (ch == '\n' || ch == '\r') ch = ' ';
                else if (ch == '"') ch = '\'';
            }
            out << '"' << n << '"';
        }
        out << "\n";
    }
    return out.str();
}

PlateResults loadPlateResults(const std::string& manifest_path, const std::string& csv_path) {
    std::ifstream mf(manifest_path);
    if (!mf) throw std::runtime_error("cannot open " + manifest_path);
    const json manifest = json::parse(mf);
    PlateResults r;
    r.kind = manifest.at("kind").get<std::string>();
    r.settings = manifest.value("settings", json::object());
    std::map<int, json> params;
    for (const auto& c : manifest.at("coupons")) params[c.at("id").get<int>()] = c.at("params");

    std::ifstream in(csv_path);
    if (!in) throw std::runtime_error("cannot open " + csv_path);
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error(csv_path + " is empty");
    const auto header = splitCsv(line);
    auto col = [&](const std::string& name) {
        for (size_t i = 0; i < header.size(); ++i)
            if (lower(header[i]) == name) return static_cast<int>(i);
        throw std::runtime_error(csv_path + ": missing column '" + name + "'");
    };
    const int c_id = col("id"), c_res = col("result"), c_scar = col("scar");
    // Optional: how many tips let go (sheets before 2026-11-18 have none), and the notes, the rest of the line.
    auto optional = [&](const std::string& name) {
        for (size_t i = 0; i < header.size(); ++i)
            if (lower(header[i]) == name) return static_cast<int>(i);
        return -1;
    };
    const int c_tips = optional("tips_let_go"), c_notes = optional("notes");
    size_t line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        if (trim(line).empty()) continue;
        const auto f = splitCsv(line);
        if (static_cast<int>(f.size()) <= c_id) continue;
        CouponResult c;
        c.id = std::stoi(f[c_id]);
        auto it = params.find(c.id);
        if (it == params.end())
            throw std::runtime_error(csv_path + ":" + std::to_string(line_no) + ": coupon id not in the manifest");
        c.params = it->second;
        const std::string res = static_cast<int>(f.size()) > c_res ? lower(f[c_res]) : "";
        if (!res.empty()) {
            if (res != "ok" && res != "failed" && res != "partial")
                throw std::runtime_error(csv_path + ":" + std::to_string(line_no) +
                                         ": result must be ok, failed or partial, got '" + res + "'");
            c.recorded = true;
            c.ok = res == "ok";
            c.partial = res == "partial";
        }
        if (static_cast<int>(f.size()) > c_scar && !f[c_scar].empty()) {
            c.scar = std::stoi(f[c_scar]);  // 0 or empty = not scored
            if (c.scar < 0 || c.scar > 5)
                throw std::runtime_error(csv_path + ":" + std::to_string(line_no) + ": scar must be 1..5 (or empty)");
        }
        if (c_tips >= 0 && static_cast<int>(f.size()) > c_tips && !trim(f[c_tips]).empty()) {
            c.tips_let_go = std::stoi(f[c_tips]);
            // Up to the coupon's own tips: three on a disc, as many as the planner gave a tile.
            const int most = c.params.value("tips", 3);
            if (c.tips_let_go < 0 || c.tips_let_go > most)
                throw std::runtime_error(csv_path + ":" + std::to_string(line_no) + ": tips_let_go must be 0.." +
                                         std::to_string(most) + " (or empty)");
        }
        const size_t from = c_notes >= 0 ? static_cast<size_t>(c_notes) : static_cast<size_t>(c_scar) + 1;
        for (size_t k = from; k < f.size(); ++k)
            if (static_cast<int>(k) != c_tips) c.notes += (c.notes.empty() && k == from ? "" : ",") + f[k];
        r.coupons.push_back(c);
    }
    return r;
}

LogisticFit fitLogistic(const std::vector<double>& x, const std::vector<int>& ok) {
    LogisticFit f;
    if (x.empty()) return f;
    double max_ok = -1e300, min_fail = 1e300, min_x = 1e300, max_x = -1e300;
    size_t n_ok = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        min_x = std::min(min_x, x[i]);
        max_x = std::max(max_x, x[i]);
        if (ok[i]) { max_ok = std::max(max_ok, x[i]); ++n_ok; }
        else min_fail = std::min(min_fail, x[i]);
    }
    if (n_ok == x.size()) {  // never failed: only a lower bound
        f.separable = true;
        f.l50 = std::numeric_limits<double>::infinity();
        return f;
    }
    if (n_ok == 0) {  // never held: only an upper bound
        f.separable = true;
        f.l50 = -std::numeric_limits<double>::infinity();
        return f;
    }
    if (max_ok < min_fail) {  // cleanly separated: midpoint of the gap
        f.separable = true;
        f.l50 = 0.5 * (max_ok + min_fail);
        return f;
    }
    // Newton / IRLS with a small ridge, x centred and scaled for stability.
    double mean = 0;
    for (double v : x) mean += v;
    mean /= static_cast<double>(x.size());
    const double scale = std::max(1e-9, max_x - min_x);
    double a = 0, b = 0;
    const double ridge = 1e-3;
    for (int it = 0; it < 100; ++it) {
        double g0 = 0, g1 = 0, h00 = ridge, h01 = 0, h11 = ridge;
        for (size_t i = 0; i < x.size(); ++i) {
            const double u = (x[i] - mean) / scale;
            const double p = 1.0 / (1.0 + std::exp(-(a + b * u)));
            const double w = p * (1 - p);
            g0 += ok[i] - p;
            g1 += (ok[i] - p) * u;
            h00 += w;
            h01 += w * u;
            h11 += w * u * u;
        }
        g0 -= ridge * a;
        g1 -= ridge * b;
        const double det = h00 * h11 - h01 * h01;
        if (std::abs(det) < 1e-15) break;
        const double da = (h11 * g0 - h01 * g1) / det, db = (h00 * g1 - h01 * g0) / det;
        a += da;
        b += db;
        if (std::abs(da) + std::abs(db) < 1e-10) break;
    }
    // Back to the original x scale.
    f.b = b / scale;
    f.a = a - f.b * mean;
    f.l50 = std::abs(f.b) > 1e-12 ? -f.a / f.b : std::numeric_limits<double>::quiet_NaN();
    return f;
}

json fitProfile(const std::vector<PlateResults>& plates, const FitOptions& o) {
    json prof;
    prof["version"] = 1;
    prof["created_by"] = "fruitslicer fit";
    prof["resin"] = o.resin;
    if (o.layer_height > 0) prof["layer_height"] = o.layer_height;
    if (o.exposure_s > 0) prof["exposure_s"] = o.exposure_s;
    if (o.vat_temp_c > 0) prof["vat_temp_c"] = o.vat_temp_c;
    json notes = json::array(), fits = json::object();

    for (const PlateResults& plate : plates) {
        std::vector<const CouponResult*> rec;
        for (const auto& c : plate.coupons)
            if (c.recorded) rec.push_back(&c);
        if (rec.empty()) {
            notes.push_back(plate.kind + " plate: no results recorded; skipped");
            continue;
        }

        if (plate.kind == "tips") {
            // The neck coupons are compared with plain ones (below); the holds and depths come from the plain.
            std::vector<const CouponResult*> necked;
            {
                std::vector<const CouponResult*> plain;
                for (const auto* c : rec) (c->params.value("neck_back", 0.0) > 0 ? necked : plain).push_back(c);
                rec = plain;
            }
            std::map<double, std::vector<const CouponResult*>> by_d;
            for (const auto* c : rec) by_d[num(c->params, "tip_diameter")].push_back(c);
            // What each tip holds the part by: its contact, as wide as the plate drew the head where it meets
            // the disc (contactDiameter). A tip's hold grows with it: on the first plate printed, 0.6 mm tips
            // 0.1 mm deep held like 0.4 mm tips 0.2 mm deep, whose contacts are alike. So the holds are fitted
            // as one strength, mm² of layer per mm² of contact, over every coupon, and each size's and
            // depth's hold is that strength times its contact; sizes and depths whose own coupons say
            // otherwise keep their own.
            const double head_len = plate.settings.value("head_length", 1.0);
            const bool job_shafts = plate.settings.value("shafts_as_jobs", false);
            const double plate_shaft = plate.settings.value("shaft_diameter", 1.0);
            auto contactArea = [&](double d, double pen) {
                PinheadSpec sp;
                sp.tip_diameter = d;
                sp.penetration = pen;
                sp.head_length = head_len;
                sp.shaft_diameter = job_shafts ? shaftDiameterFor(d) : std::max(plate_shaft, 1.5 * d);
                const double cd = contactDiameter(sp);
                return kPi * cd * cd / 4;
            };
            auto areaOf = [&](const CouponResult* c) {
                return contactArea(num(c->params, "tip_diameter"), num(c->params, "penetration"));
            };
            // A coupon held when every one of its tips did. Its load is the one a tip alone lets go at as often
            // as one of them does, on the replay's weakest-link curve (StabilityConfig::steepness): then the
            // strength found is a single tip's, the one the replay and the planner's table stand for. A tile
            // of the tips plate has it from the replay (load_mm2). A disc on equal tips: its load per tip
            // times tips^(1/steepness), 1.17 for three (until 2026-11-24 the load per tip alone was taken, and
            // a tip's strength came out about 15 % low).
            const double steep = plate.settings.value("steepness", StabilityConfig{}.steepness);
            auto loadOf = [&](const CouponResult* c) {
                if (c->params.contains("load_mm2")) return num(c->params, "load_mm2");
                return num(c->params, "load_per_tip_mm2") * std::pow(c->params.value("tips", 1.0), 1 / steep);
            };
            auto peelAt = [](const CouponResult* c, double b) {
                return c->params.contains("peel_t") ? 1 + b * 0.5 * (num(c->params, "peel_t") + 1) : 1.0;
            };
            // A plate in two sets, by the hinge and far from it: how much harder the peel pulls far from the
            // hinge. The peel at a point is 1 + bias × (t + 1) / 2, t from -1 at the hinge's edge to +1 at the
            // far one, as the planner has it. Every coupon's load is taken as at the hinge's edge (load ×
            // peel) over its contact; for each bias tried, the one strength that leaves the fewest discs on
            // the wrong side of it (held above, let go below). The bias the fewest disagree with is kept only
            // when those that tie lie within kPeelBiasPinned of each other: with steps of 1.4 times between
            // loads and one coupon per step and side, they spread over 0 to about 0.6 whatever the truth
            // (simulated, 2026-11-18), which says no more than the assumed 25 % does.
            double bias = kAssumedPeelBias;
            {
                bool sided = false;
                double th = 0, tf = 0;
                int nh = 0, nf = 0;
                for (const auto* c : rec)
                    if (c->params.contains("side")) {
                        sided = true;
                        const double t = num(c->params, "peel_t");
                        if (c->params["side"] == "hinge") th += t, ++nh;
                        else if (c->params["side"] == "far") tf += t, ++nf;
                    }
                if (sided && nh > 0 && nf > 0) {
                    th /= nh, tf /= nf;
                    json per = json::array();
                    for (const auto& [d, cs] : by_d) {
                        std::vector<double> xh, xf;
                        std::vector<int> yh, yf;
                        for (const auto* c : cs) {
                            const std::string side = c->params.value("side", std::string());
                            if (side != "hinge" && side != "far") continue;
                            (side == "hinge" ? xh : xf).push_back(loadOf(c) / areaOf(c));
                            (side == "hinge" ? yh : yf).push_back(c->ok ? 1 : 0);
                        }
                        if (xh.empty() || xf.empty()) continue;
                        const LogisticFit h = fitLogistic(xh, yh), f = fitLogistic(xf, yf);
                        per.push_back({{"tip_diameter", d},
                                       {"strength_50pct_hinge", std::isfinite(h.l50) ? json(h.l50) : json(nullptr)},
                                       {"strength_50pct_far", std::isfinite(f.l50) ? json(f.l50) : json(nullptr)}});
                    }
                    json peel = {{"t_hinge", th}, {"t_far", tf}, {"tips", per}};
                    auto misfits = [&](double b) {
                        std::vector<std::pair<double, int>> v;
                        for (const auto* c : rec)
                            v.push_back({loadOf(c) * peelAt(c, b) / areaOf(c), c->ok ? 1 : 0});
                        std::sort(v.begin(), v.end());
                        int fails_below = 0, held_above = 0;
                        for (const auto& e : v) held_above += e.second;
                        int best = held_above;  // a strength below every load: every held disc is wrong
                        for (const auto& e : v) {
                            (e.second ? held_above : fails_below) += e.second ? -1 : 1;
                            best = std::min(best, fails_below + held_above);
                        }
                        return best;
                    };
                    int ok_all = 0;
                    for (const auto* c : rec) ok_all += c->ok;
                    if (ok_all > 0 && ok_all < static_cast<int>(rec.size())) {
                        int least = 1 << 30;
                        std::vector<double> at;
                        for (int k = 0; k <= 150; ++k) {
                            const double b = k / 100.0;
                            const int m = misfits(b);
                            if (m < least) least = m, at = {b};
                            else if (m == least) at.push_back(b);
                        }
                        peel["bias_range"] = {at.front(), at.back()};
                        peel["misfits"] = least;
                        if (at.back() - at.front() >= kPeelBiasPinned - 1e-9) {
                            peel["unpinned"] = true;
                            notes.push_back("tips plate in two sets: peel biases from " + fmt(at.front()) + " to " +
                                            fmt(at.back()) + " fit the scores as well, so the extra pull far from "
                                            "the hinge stays as it was");
                        } else {
                            bias = at[at.size() / 2];
                            prof["peel_bias"] = bias;
                            peel["bias"] = bias;
                        }
                    } else {
                        notes.push_back("tips plate in two sets: every coupon held, or every one let go, so the extra "
                                        "pull far from the hinge stays as it was");
                    }
                    fits["peel"] = peel;
                }
            }
            auto strengthOf = [&](const CouponResult* c) {
                return loadOf(c) * peelAt(c, bias) / areaOf(c);
            };

            // Each size and depth's own coupons: the strongest that held, the weakest that let go.
            struct Own {
                double lo = 0, hi = std::numeric_limits<double>::infinity();
            };
            std::map<std::pair<double, double>, Own> own;
            for (const auto* c : rec) {
                Own& w = own[{num(c->params, "tip_diameter"), num(c->params, "penetration")}];
                if (c->ok) w.lo = std::max(w.lo, strengthOf(c));
                else w.hi = std::min(w.hi, strengthOf(c));
            }
            // Those that held well past the one strength, or let go well short of it (more than a step of
            // loads, kOneLoadStep), don't follow it: they keep their own, and the strength is found again
            // without them, so one odd size doesn't move every other's.
            std::set<std::pair<double, double>> odd;
            auto oddAt = [&](double s50) {
                std::set<std::pair<double, double>> o2;
                for (const auto& [key, w] : own)
                    if (w.lo < w.hi && (w.lo > s50 * kOneLoadStep || w.hi < s50 / kOneLoadStep)) o2.insert(key);
                return o2;
            };
            auto fitWithout = [&](const std::set<std::pair<double, double>>& out, std::vector<double>& x, std::vector<int>& y) {
                x.clear(), y.clear();
                for (const auto* c : rec)
                    if (!out.count({num(c->params, "tip_diameter"), num(c->params, "penetration")}))
                        x.push_back(strengthOf(c)), y.push_back(c->ok ? 1 : 0);
                return fitLogistic(x, y);
            };
            auto bracketed = [](const LogisticFit& m) { return std::isfinite(m.l50) && m.l50 > 0 && !(m.b > 0); };
            std::vector<double> sx;
            std::vector<int> sy;
            LogisticFit model = fitWithout(odd, sx, sy);
            for (int pass = 0; pass < 3 && bracketed(model); ++pass) {
                const auto next = oddAt(model.l50);
                if (next == odd || next.size() == own.size()) break;
                std::vector<double> tx;
                std::vector<int> ty;
                const LogisticFit trial = fitWithout(next, tx, ty);
                if (!bracketed(trial)) break;  // without them it brackets nothing: keep the fit with them
                odd = next, model = trial, sx = std::move(tx), sy = std::move(ty);
            }
            const double s_min = *std::min_element(sx.begin(), sx.end()), s_max = *std::max_element(sx.begin(), sx.end());
            double s50 = -1;  // the strength at which half let go, where the plate brackets it
            double s_keep;    // mm² of layer per mm² of contact that FruitSlicer keeps
            std::string model_basis;
            if (std::isinf(model.l50) && model.l50 > 0) {
                s_keep = o.capacity_fraction * s_max;
                model_basis = "lower bound";
                notes.push_back("tips: every coupon held; holds are a floor (test higher loads)");
            } else if (std::isinf(model.l50)) {
                s_keep = 0.5 * s_min;
                model_basis = "upper bound";
                notes.push_back("tips: every coupon let go; holds set to half the lightest (test lower loads)");
            } else if (!(model.l50 > 0) || model.b > 0) {
                s_keep = o.capacity_fraction * s_min;
                model_basis = "no load dependence";
                notes.push_back("tips: success did not fall with load; holds set conservatively");
            } else {
                s50 = std::min(model.l50, s_max * 2);
                s_keep = o.capacity_fraction * s50;
                model_basis = model.separable ? "gap midpoint" : "logistic";
            }
            if (s50 <= 0) odd.clear();
            int held_all = 0;
            for (int y : sy) held_all += y;
            fits["tips_model"] = {{"basis", model_basis},
                                  {"coupons", sx.size()},
                                  {"held", held_all},
                                  {"strength_50pct", s50 > 0 ? json(s50) : json(nullptr)},
                                  {"strength_kept", s_keep},
                                  {"lightest", s_min},
                                  {"heaviest", s_max}};
            // The planner trusts a measured table nearer to where tips let go (kMeasuredTipLoadLimit); a
            // floor or a guess is not one.
            prof["capacity_measured"] = s50 > 0;

            json cap_table = json::array(), pen_table = json::array(), detail = json::array();
            struct Cand { double d, cap, scar; };
            std::vector<Cand> cands;
            for (const auto& [d, cs] : by_d) {
                int part_one = 0, part_two = 0, part_unknown = 0;  // partly held: one tip let go, two, not said
                for (const auto* c : cs)
                    if (c->partial) (c->tips_let_go == 1 ? part_one : c->tips_let_go == 2 ? part_two : part_unknown)++;
                struct Depth {
                    int n = 0, held = 0, scar_n = 0;
                    double scar = 0, lo = 0, hi = std::numeric_limits<double>::infinity();  // held up to / let go from
                };
                std::map<double, Depth> depths;
                int held = 0;
                double max_held = -1, min_failed = -1;
                for (const auto* c : cs) {
                    Depth& k = depths[num(c->params, "penetration")];
                    const double load = loadOf(c) * peelAt(c, bias), st = strengthOf(c);
                    ++k.n;
                    if (c->ok) {
                        ++k.held, ++held;
                        k.lo = std::max(k.lo, st);
                        max_held = std::max(max_held, load);
                        if (c->scar > 0) k.scar += c->scar, ++k.scar_n;
                    } else {
                        k.hi = std::min(k.hi, st);
                        if (min_failed < 0 || load < min_failed) min_failed = load;
                    }
                }
                // Depth: the one that leaves the least mark, marks within half a point counting as alike (as
                // for necks); alike or not scored, the deeper, which touches more of the part and so holds more.
                auto mark = [&](const Depth& k) { return k.scar_n ? k.scar / k.scar_n : -1.0; };
                double pen = depths.rbegin()->first;
                for (auto it = std::next(depths.rbegin()); it != depths.rend(); ++it) {
                    const double m = mark(it->second), mb = mark(depths.at(pen));
                    if (m > 0 && mb > 0 && m <= mb - 0.5 + 1e-9) pen = it->first;
                }
                const Depth& k = depths.at(pen);
                const double area = contactArea(d, pen);
                // At that depth, its own coupons where they don't follow the one strength: halfway between
                // what held and what let go; all held, only a floor; all let go, half the lightest.
                double cap = s_keep * area;
                std::string basis = model_basis == "gap midpoint" || model_basis == "logistic" ? "contact model" : model_basis;
                if (odd.count({d, pen})) {
                    const bool held_all = !std::isfinite(k.hi), failed_all = k.lo <= 0;
                    const double mine = held_all ? k.lo : failed_all ? k.hi : std::sqrt(k.lo * k.hi);
                    cap = (failed_all ? 0.5 : o.capacity_fraction) * mine * area;
                    basis = held_all ? "own floor" : failed_all ? "own guess" : "own";
                    notes.push_back("tips " + fmt(d) + " mm at " + fmt(pen) + " mm deep: their own coupons don't follow "
                                    "the other sizes' strength; their own are used");
                }
                json dj = json::array();
                for (const auto& [p, kk] : depths)
                    dj.push_back({{"penetration", p},
                                  {"held", kk.held},
                                  {"coupons", kk.n},
                                  {"contact_mm2", contactArea(d, p)},
                                  {"mean_scar", kk.scar_n ? json(kk.scar / kk.scar_n) : json(nullptr)}});
                cap_table.push_back({d, cap});
                // A depth is chosen only where the plate tried more than one (the tips plate since 2026-11-24
                // puts each size as deep as a job does: nothing to choose, and the planner's own rule stays).
                if (depths.size() > 1) pen_table.push_back({d, pen});
                const double m = mark(k);
                cands.push_back({d, cap, m > 0 ? m : 0});
                detail.push_back({{"tip_diameter", d},
                                  {"coupons", cs.size()},
                                  {"held", held},
                                  {"max_held_mm2", max_held >= 0 ? json(max_held) : json(nullptr)},
                                  {"min_failed_mm2", min_failed >= 0 ? json(min_failed) : json(nullptr)},
                                  {"penetrations", dj},
                                  {"contact_mm2", area},
                                  {"load_50pct", s50 > 0 && basis == "contact model" ? json(s50 * area) : json(nullptr)},
                                  {"capacity_mm2", cap},
                                  {"basis", basis},
                                  {"preferred_penetration", pen},
                                  // Kept for a fit by single tips: a disc where one of three let go is one tip's hold.
                                  {"partly", {{"one_let_go", part_one}, {"two_let_go", part_two}, {"not_said", part_unknown}}},
                                  {"at_depth", {{"coupons", k.n},
                                                {"held", k.held},
                                                {"max_held_mm2", k.lo > 0 ? json(k.lo * area) : json(nullptr)},
                                                {"min_failed_mm2", std::isfinite(k.hi) ? json(k.hi * area) : json(nullptr)}}},
                                  {"mean_scar", m > 0 ? json(m) : json(nullptr)}});
            }
            prof["capacity_table"] = cap_table;
            if (!pen_table.empty()) prof["penetration_table"] = pen_table;
            // Default tip: lowest mean scar among tips that hold enough.
            const Cand* def = nullptr;
            for (const auto& c : cands) {
                if (c.cap < o.min_default_capacity) continue;
                const double s = c.scar > 0 ? c.scar : 99;
                const double ds = def ? (def->scar > 0 ? def->scar : 99) : 1e9;
                if (!def || s < ds - 1e-9 || (std::abs(s - ds) <= 1e-9 && c.d < def->d)) def = &c;
            }
            if (def) prof["default_tip_diameter"] = def->d;
            else notes.push_back("no tip held " + fmt(o.min_default_capacity) + " mm²; default tip left unchanged");
            if (!cands.empty()) prof["minima_tip_diameter"] = cands.back().d;
            fits["tips"] = detail;
            // Necks: for each tip size, each neck against plain tips at the same loads and the same depth. A
            // neck is kept where its discs held at least as often and its mark was at least half a point less;
            // of those, the one with the least mark. Held as often at those loads, the tip's hold stands.
            if (!necked.empty()) {
                json neck_detail = json::array(), neck_table = json::array();
                std::map<double, std::map<double, std::vector<const CouponResult*>>> by_neck;  // d -> back -> coupons
                for (const auto* c : necked) by_neck[num(c->params, "tip_diameter")][num(c->params, "neck_back")].push_back(c);
                for (const auto& [d, backs] : by_neck) {
                    const auto first = backs.begin()->second.front();
                    const double pen = num(first->params, "penetration");
                    std::set<double> loads;
                    for (const auto& [b, cs] : backs) for (const auto* c : cs) loads.insert(num(c->params, "load_per_tip_mm2"));
                    int base_n = 0, base_held = 0, base_scar_n = 0;
                    double base_scar = 0;
                    if (by_d.count(d))
                        for (const auto* c : by_d.at(d)) {
                            if (std::abs(num(c->params, "penetration") - pen) > 1e-9) continue;
                            if (!loads.count(num(c->params, "load_per_tip_mm2"))) continue;
                            ++base_n, base_held += c->ok;
                            if (c->scar > 0) base_scar += c->scar, ++base_scar_n;
                        }
                    const double base_mark = base_scar_n ? base_scar / base_scar_n : 0;
                    json per = {{"tip_diameter", d}, {"plain_held", base_held}, {"plain_coupons", base_n},
                                {"plain_mark", base_scar_n ? json(base_mark) : json(nullptr)}};
                    json opts = json::array();
                    double best_back = 0, best_mark = 1e9, ratio = 0.6;
                    for (const auto& [b, cs] : backs) {
                        int held = 0, scar_n = 0;
                        double scar = 0;
                        for (const auto* c : cs) {
                            held += c->ok;
                            if (c->scar > 0) scar += c->scar, ++scar_n;
                            ratio = c->params.value("neck_ratio", 0.6);
                        }
                        const double mark = scar_n ? scar / scar_n : 0;
                        opts.push_back({{"neck_back", b}, {"held", held}, {"coupons", cs.size()},
                                        {"mark", scar_n ? json(mark) : json(nullptr)}});
                        // Held as often (as a share, so a missing plain coupon doesn't count against it)...
                        const bool holds = base_n > 0 && static_cast<double>(held) / cs.size() + 1e-9 >=
                                                             static_cast<double>(base_held) / base_n;
                        // ...and a mark at least half a point less.
                        const bool better = scar_n > 0 && base_scar_n > 0 && mark <= base_mark - 0.5 + 1e-9;
                        if (holds && better && mark < best_mark) best_mark = mark, best_back = b;
                    }
                    per["necks"] = opts;
                    if (best_back > 0) {
                        per["kept"] = best_back;
                        neck_table.push_back({d, best_back, ratio});
                    }
                    neck_detail.push_back(per);
                }
                if (!neck_table.empty()) prof["neck_table"] = neck_table;
                fits["necks"] = neck_detail;
            }
        } else if (plate.kind == "spacing") {
            std::map<double, std::map<double, bool>> plates_ok;  // angle -> spacing -> all ok
            std::map<double, bool> wedges_ok;                    // angle -> all ok
            for (const auto* c : rec) {
                const std::string k = c->params.value("kind", std::string("plate"));
                const double ang = num(c->params, "angle_deg");
                if (k == "wedge") {
                    auto [it, ins] = wedges_ok.try_emplace(ang, true);
                    it->second = it->second && c->ok;
                } else {
                    auto [it, ins] = plates_ok[ang].try_emplace(num(c->params, "spacing_mm"), true);
                    it->second = it->second && c->ok;
                }
            }
            json detail = json::object();
            if (!plates_ok.empty()) {
                double r0 = 1e300;
                json per_angle = json::array();
                for (const auto& [ang, sp] : plates_ok) {
                    double s_ok = -1;
                    for (const auto& [s, good] : sp) {  // ascending spacing
                        if (!good) break;
                        s_ok = s;
                    }
                    double r;
                    if (s_ok < 0) {
                        r = 0.7 * sp.begin()->first / std::sqrt(2.0);
                        notes.push_back("spacing plate " + fmt(ang) + "°: even " + fmt(sp.begin()->first) +
                                        " mm pitch failed; radius set below it (test tighter pitches)");
                    } else {
                        r = s_ok / std::sqrt(2.0);
                        if (s_ok == sp.rbegin()->first)
                            notes.push_back("spacing plate " + fmt(ang) + "°: every pitch up to " + fmt(s_ok) +
                                            " mm held; radius is a lower bound (test wider pitches)");
                    }
                    per_angle.push_back({{"angle_deg", ang}, {"max_ok_pitch", s_ok > 0 ? json(s_ok) : json(nullptr)},
                                         {"radius", r}});
                    r0 = std::min(r0, r);
                }
                // Keep the seed curve's shape, scaled to the measured Δz≈0 radius.
                const CoverageModel seed;
                const double k = r0 / seed.curve.front().second;
                json curve = json::array();
                for (const auto& [dz, r] : seed.curve) curve.push_back({dz, r * k});
                prof["coverage_curve"] = curve;
                detail["plates"] = per_angle;
                detail["radius0"] = r0;
            }
            if (!wedges_ok.empty()) {
                json per_wedge = json::array();
                for (const auto& [ang, good] : wedges_ok) per_wedge.push_back({{"angle_deg", ang}, {"ok", good}});
                detail["wedges"] = per_wedge;
                double crit = -1;
                for (const auto& [ang, good] : wedges_ok) {
                    if (!good) break;
                    crit = ang;
                }
                if (crit < 0) {
                    crit = wedges_ok.begin()->first - 5;
                    notes.push_back("every wedge failed; critical angle set 5° below the shallowest tested");
                } else if (crit == wedges_ok.rbegin()->first) {
                    notes.push_back("every wedge up to " + fmt(crit) +
                                    "° printed; critical angle is a lower bound (test steeper wedges)");
                }
                prof["critical_angle_deg"] = crit;
                detail["critical_angle_deg"] = crit;
            }
            fits["spacing"] = detail;
        } else if (plate.kind == "islands") {
            std::map<double, std::map<int, bool>> by_d;  // diameter -> tips -> all ok
            for (const auto* c : rec) {
                auto [it, ins] = by_d[num(c->params, "island_diameter")].try_emplace(c->params.at("tips").get<int>(), true);
                it->second = it->second && c->ok;
            }
            double one = 0, two = 0;
            bool one_run = true, two_run = true;
            json detail = json::array();
            for (const auto& [D, tips] : by_d) {  // ascending diameter
                int min_k = -1;
                for (const auto& [k, good] : tips)
                    if (good) { min_k = k; break; }
                detail.push_back({{"island_diameter", D}, {"min_tips", min_k > 0 ? json(min_k) : json(nullptr)}});
                if (one_run && min_k == 1) one = D; else one_run = false;
                if (two_run && min_k > 0 && min_k <= 2) two = D; else two_run = false;
            }
            prof["island_one_tip_length"] = one;
            prof["island_two_tip_length"] = std::max(one, two);
            if (one == 0) notes.push_back("no island size held reliably on one tip; every island gets at least two");
            notes.push_back("islands plate does not test the next-layer absorption rule; it stays an assumption");
            fits["islands"] = detail;
        } else if (plate.kind == "gaps") {
            // By gap: how many shafts, and how many came off cleanly.
            std::map<double, std::pair<int, int>> by_gap;
            for (const auto* c : rec) {
                auto& e = by_gap[num(c->params, "gap")];
                ++e.first;
                e.second += c->ok ? 1 : 0;
            }
            // The narrowest gap from which every gap as wide or wider came off
            // cleanly; and the widest at which any stuck.
            double clean = 0, stuck = 0;
            for (auto it = by_gap.rbegin(); it != by_gap.rend(); ++it)
                if (it->second.second == it->second.first) clean = it->first;
                else break;
            json detail = json::array();
            for (const auto& [g, e] : by_gap) {
                if (e.second < e.first) stuck = g;
                detail.push_back({{"gap", g}, {"shafts", e.first}, {"clean", e.second}});
            }
            const double pixel = plate.settings.value("pixel", 0.0);
            if (clean > 0) {
                prof["shaft_gap"] = clean + pixel;
            } else {
                notes.push_back("on the gap plate a shaft stuck even at the widest gap; the gap stays as it was");
            }
            fits["gaps"] = {{"gaps", detail}, {"clean", clean}, {"stuck", stuck}, {"pixel", pixel}};
        } else {
            notes.push_back("unknown plate kind '" + plate.kind + "' skipped");
        }
    }
    prof["notes"] = notes;
    prof["fits"] = fits;
    return prof;
}

void applyProfile(const json& p, SupportConfig& cfg, TreeConfig* tree, std::vector<std::string>* notes) {
    auto pairs = [](const json& arr) {
        std::vector<std::pair<double, double>> v;
        for (const auto& e : arr) v.push_back({e.at(0).get<double>(), e.at(1).get<double>()});
        std::sort(v.begin(), v.end());
        return v;
    };
    if (p.contains("capacity_table")) {
        cfg.capacity.table = pairs(p.at("capacity_table"));
        // Tip classes become the measured diameters.
        if (!cfg.capacity.table.empty()) {
            // A measured table is trusted nearer to where its tips let go: each tip's own load is held
            // further under it (SupportConfig::tip_load_limit). Not a floor or a guess (a plate where every
            // disc held, or every one let go): that limit would make tips stricter for nothing learned.
            if (p.value("capacity_measured", true)) cfg.tip_load_limit = kMeasuredTipLoadLimit;
            cfg.tip_classes.clear();
            for (const auto& [d, c] : cfg.capacity.table) {
                std::ostringstream name;
                name.precision(2);
                name << std::fixed << "d" << d;
                cfg.tip_classes.push_back({name.str(), d});
            }
            const int n = static_cast<int>(cfg.tip_classes.size());
            auto nearest = [&](double d) {
                int best = 0;
                for (int i = 1; i < n; ++i)
                    if (std::abs(cfg.tip_classes[i].diameter - d) < std::abs(cfg.tip_classes[best].diameter - d)) best = i;
                return best;
            };
            cfg.default_tip = nearest(p.value("default_tip_diameter", cfg.tip_classes[n / 2].diameter));
            cfg.minima_tip = nearest(p.value("minima_tip_diameter", cfg.tip_classes[n - 1].diameter));
            // Near edges and visible faces, the size nearest FruitSlicer's own (0.3 mm), not the smallest
            // measured: a plate that tests 0.2 mm tips doesn't make them the edge tip.
            {
                const SupportConfig base;
                cfg.edge_tip = nearest(base.tip_classes[static_cast<size_t>(base.edge_tip)].diameter);
            }
        }
    }
    if (p.contains("coverage_curve")) cfg.coverage.curve = pairs(p.at("coverage_curve"));
    if (p.contains("critical_angle_deg")) cfg.critical_angle_deg = p.at("critical_angle_deg").get<double>();
    if (p.contains("island_one_tip_length")) cfg.island_one_tip_length = p.at("island_one_tip_length").get<double>();
    if (p.contains("island_two_tip_length")) cfg.island_two_tip_length = p.at("island_two_tip_length").get<double>();
    if (tree && p.contains("penetration_table")) tree->penetration_table = pairs(p.at("penetration_table"));
    if (p.contains("peel_bias")) cfg.peel_bias = p.at("peel_bias").get<double>();
    if (tree && p.contains("neck_table"))
        for (const auto& e : p.at("neck_table"))
            tree->neck_table.push_back({e.at(0).get<double>(), e.at(1).get<double>(), e.at(2).get<double>()});
    if (tree && p.contains("shaft_gap")) tree->clearance = p.at("shaft_gap").get<double>();
    if (notes && p.contains("layer_height") && std::abs(p.at("layer_height").get<double>() - cfg.layer_height) > 1e-9)
        notes->push_back("profile was measured at " + fmt(p.at("layer_height").get<double>()) +
                         " mm layers but this run uses " + fmt(cfg.layer_height) + " mm");
}

}  // namespace fruit
