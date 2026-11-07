// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "calibration/CalibrationFit.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
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
    out << "id,row,col,result,scar,notes\n";
    for (const auto& c : manifest.at("coupons")) {
        const int id = c.at("id").get<int>();
        out << id << "," << c.value("row", 0) << "," << c.value("col", 0) << ",";
        const auto it = by_id.find(id);
        const CouponResult* s = it == by_id.end() ? nullptr : it->second;
        if (s && s->recorded) out << (s->ok ? "ok" : s->partial ? "partial" : "failed");
        out << ",";
        if (s && s->scar > 0) out << s->scar;
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
        for (size_t k = c_scar + 1; k < f.size(); ++k) c.notes += (k > static_cast<size_t>(c_scar) + 1 ? "," : "") + f[k];
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
            std::map<double, std::vector<const CouponResult*>> by_d;
            for (const auto* c : rec) by_d[num(c->params, "tip_diameter")].push_back(c);
            json cap_table = json::array(), pen_table = json::array(), detail = json::array();
            struct Cand { double d, cap, scar; };
            std::vector<Cand> cands;
            for (const auto& [d, cs] : by_d) {
                std::vector<double> x;
                std::vector<int> y;
                double scar_sum = 0;
                int scar_n = 0;
                for (const auto* c : cs) {
                    x.push_back(num(c->params, "load_per_tip_mm2"));
                    y.push_back(c->ok ? 1 : 0);
                    if (c->scar > 0) { scar_sum += c->scar; ++scar_n; }
                }
                const LogisticFit lf = fitLogistic(x, y);
                const double min_x = *std::min_element(x.begin(), x.end());
                const double max_x = *std::max_element(x.begin(), x.end());
                double cap;
                std::string basis;
                if (std::isinf(lf.l50) && lf.l50 > 0) {
                    cap = max_x;
                    basis = "lower bound";
                    notes.push_back("tips " + fmt(d) + " mm: no failures up to " + fmt(max_x) +
                                    " mm² per tip; capacity set to that (a lower bound; test higher loads)");
                } else if (std::isinf(lf.l50)) {
                    cap = 0.5 * min_x;
                    basis = "upper bound";
                    notes.push_back("tips " + fmt(d) + " mm: failed even at " + fmt(min_x) +
                                    " mm² per tip; capacity set to half of that (test lower loads)");
                } else if (!(lf.l50 > 0) || lf.b > 0) {
                    cap = o.capacity_fraction * min_x;
                    basis = "no load dependence";
                    notes.push_back("tips " + fmt(d) + " mm: success did not fall with load; capacity set conservatively");
                } else {
                    cap = o.capacity_fraction * std::min(lf.l50, max_x * 2);
                    basis = lf.separable ? "gap midpoint" : "logistic";
                }
                // Preferred penetration: highest success rate, then lowest scar.
                std::map<double, std::pair<int, int>> pen_ok;  // pen -> (ok, n)
                std::map<double, std::pair<double, int>> pen_scar;
                for (const auto* c : cs) {
                    if (!c->params.contains("penetration")) continue;
                    const double p = num(c->params, "penetration");
                    pen_ok[p].first += c->ok;
                    pen_ok[p].second += 1;
                    if (c->scar > 0) { pen_scar[p].first += c->scar; pen_scar[p].second += 1; }
                }
                double best_pen = -1, best_rate = -1, best_scar = 1e9;
                for (const auto& [p, on] : pen_ok) {
                    const double rate = static_cast<double>(on.first) / on.second;
                    const double sc = pen_scar[p].second ? pen_scar[p].first / pen_scar[p].second : 3.0;
                    if (rate > best_rate + 1e-9 || (std::abs(rate - best_rate) <= 1e-9 && sc < best_scar)) {
                        best_rate = rate;
                        best_scar = sc;
                        best_pen = p;
                    }
                }
                cap_table.push_back({d, cap});
                if (best_pen > 0) pen_table.push_back({d, best_pen});
                const double mean_scar = scar_n ? scar_sum / scar_n : 0;
                cands.push_back({d, cap, mean_scar});
                detail.push_back({{"tip_diameter", d},
                                  {"coupons", cs.size()},
                                  {"load_50pct", std::isfinite(lf.l50) ? json(lf.l50) : json(nullptr)},
                                  {"capacity_mm2", cap},
                                  {"basis", basis},
                                  {"preferred_penetration", best_pen > 0 ? json(best_pen) : json(nullptr)},
                                  {"mean_scar", scar_n ? json(mean_scar) : json(nullptr)}});
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
            cfg.edge_tip = 0;
        }
    }
    if (p.contains("coverage_curve")) cfg.coverage.curve = pairs(p.at("coverage_curve"));
    if (p.contains("critical_angle_deg")) cfg.critical_angle_deg = p.at("critical_angle_deg").get<double>();
    if (p.contains("island_one_tip_length")) cfg.island_one_tip_length = p.at("island_one_tip_length").get<double>();
    if (p.contains("island_two_tip_length")) cfg.island_two_tip_length = p.at("island_two_tip_length").get<double>();
    if (tree && p.contains("penetration_table")) tree->penetration_table = pairs(p.at("penetration_table"));
    if (notes && p.contains("layer_height") && std::abs(p.at("layer_height").get<double>() - cfg.layer_height) > 1e-9)
        notes->push_back("profile was measured at " + fmt(p.at("layer_height").get<double>()) +
                         " mm layers but this run uses " + fmt(cfg.layer_height) + " mm");
}

}  // namespace fruit
