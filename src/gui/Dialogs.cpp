// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Dialogs.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "App.h"
#include "Format.h"
#include "Repairs.h"
#include "Theme.h"
#include "Widgets.h"

namespace gui::dialogs {

namespace {

constexpr float kPad = 22;

// The canvas's modal: panel colour, strong border, dimmed backdrop, fixed
// width, height fitting the content (it scrolls inside when too tall).
bool beginModal(const char* id, float width, float top) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + top), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(5, 6, 8, 184));
    const bool open = ImGui::BeginPopupModal(
        id, nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (!open) {
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(4);
    }
    return open;
}

void endModal() {
    ImGui::EndPopup();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
}

// Full-width rule at the cursor, with `gap` above and below.
void rule(float gap) {
    ImGui::Dummy(ImVec2(0, gap));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x0 = ImGui::GetWindowPos().x, x1 = x0 + ImGui::GetWindowWidth();
    dl->AddLine(ImVec2(x0, p.y), ImVec2(x1, p.y), col::Border);
    ImGui::Dummy(ImVec2(0, gap));
}

// "W × D × H mm · V ml" for the model at `scale`.
std::string sizeLine(const Document& d, const fruit::Placement& p) {
    const fruit::PlateFit f = fruit::checkFit(d.part().mesh(), p, d.printer);
    return dims(f.size) + " · " + strf("%.1f ml", d.part().check().volume_ml * p.scale * p.scale * p.scale);
}

// Why it does not fit at full size, as the canvas words it.
std::string tooBig(const Document& d, const fruit::PrinterProfile& pr) {
    const fruit::PlateFit& f = d.fit();
    fruit::Placement p;
    const double m = fruit::plateMargin(p);
    const double rx = pr.build_x - 2 * m, ry = pr.build_y - 2 * m, rz = pr.build_z - p.lift();
    // Name the first limit it breaks, as the canvas does, then the turn.
    std::string s = dims(f.size) + ". ";
    if (f.size.y > ry)
        s += strf("The plate is %.2f mm deep, %.1f mm of it usable with room for the raft", pr.build_y, ry);
    else if (f.size.x > rx)
        s += strf("The plate is %.2f mm wide, %.1f mm of it usable with room for the raft", pr.build_x, rx);
    else
        s += strf("Only %.0f mm of height is left above the %.0f mm lift", rz, p.lift());
    fruit::Placement turned;
    turned.rotation_deg.z = 90;
    if (fruit::checkFit(d.part().mesh(), turned, pr).fits) s += "; turned a quarter, it fits at full size.";
    else if (f.size.z <= rz) s += strf(", and turning it doesn't help (%.1f mm).", std::max(f.size.x, f.size.y));
    else s += ".";
    (void)m;
    return s;
}

}  // namespace

void health(App& app) {
    HealthDialog& h = app.health();
    const std::shared_ptr<Document> doc = app.checking();
    if (h.open && doc && !ImGui::IsPopupOpen("health")) ImGui::OpenPopup("health");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Close to the top on short windows: the findings and choices are tall.
    if (!beginModal("health", 720, vp->Size.y < 980 ? 28.0f : 72.0f)) return;
    if (!doc || !h.open) {
        ImGui::CloseCurrentPopup();
        endModal();
        return;
    }
    const fruit::MeshCheck& c = doc->part().check();
    const fruit::PrinterProfile& pr = app.printer();
    const std::string file = std::filesystem::path(doc->part().path()).filename().string();

    ImGui::SetCursorPos(ImVec2(kPad, kPad));
    ImGui::BeginGroup();
    text(("Checked " + file).c_str(), fonts().bold, size::DialogTitle, col::Text);
    ImGui::Dummy(ImVec2(0, 1));
    text((thousands(c.triangles) + " triangles · " + thousands(c.points) + " points · " +
          strf("%.1f ml at full size", c.volume_ml))
             .c_str(),
         fonts().regular, size::Body, col::TextSecondary);
    ImGui::EndGroup();
    rule(10);

    // Findings and the size choice; scrolls on short windows.
    const float body_w = 720 - 2 * kPad;
    // What the window leaves below the header, less the footer (~92 px).
    const float top = ImGui::GetWindowPos().y - vp->Pos.y;
    const float max_h = std::max(160.0f, vp->Size.y - top - ImGui::GetCursorPosY() - 92 - 12);
    ImGui::SetCursorPosX(kPad);
    ImGui::SetNextWindowSizeConstraints(ImVec2(body_w, 0), ImVec2(body_w, max_h));
    ImGui::BeginChild("body", ImVec2(body_w, 0), ImGuiChildFlags_AutoResizeY);
    {
        // What loading repaired, if anything; else how it is closed.
        const RepairWords repair = repairWords(c);
        if (repair.level != RepairWords::Level::None)
            verdictRow("closed", repair.level == RepairWords::Level::Repaired ? Verdict::Ok : Verdict::Warn,
                       repair.title.c_str(), repair.detail.c_str());
        else if (c.shells == 1)
            verdictRow("closed", Verdict::Ok, "One closed shell",
                       "Watertight, so every layer has a clear inside and outside");
        else
            verdictRow("closed", Verdict::Ok, (std::to_string(c.shells) + " closed shells").c_str(),
                       "Watertight pieces; where they overlap they print as one");
        if (c.flipped == 0)
            verdictRow("flipped", Verdict::Ok, "No inside-out parts",
                       c.repair.faces_flipped ? "No whole part faced inward" : "Nothing needed flipping");
        else
            verdictRow("flipped", Verdict::Ok,
                       (std::to_string(c.flipped) + (c.flipped == 1 ? " inside-out part" : " inside-out parts") +
                        " turned around")
                           .c_str(),
                       "They were saved facing inward; FruitSlicer turned them the right way");
        if (c.cavities == 0)
            // A hollow is a closed shell facing inward: among open surfaces
            // there is no telling.
            verdictRow("hollows", Verdict::Ok, "No hollows inside",
                       c.closed() ? "Solid model: no suction cups or resin traps from cavities"
                                  : "None found, though with open edges that can't be told for sure");
        else
            verdictRow("hollows", Verdict::Warn,
                       (std::to_string(c.cavities) + (c.cavities == 1 ? " closed hollow" : " closed hollows") +
                        " inside")
                           .c_str(),
                       "A closed hollow traps resin and can burst. Give it drain holes, or print it solid, with "
                       "the Hollow tool.");
        verdictRow("low", Verdict::Ok, "Low points found",
                   strf("As it sits now, %zu %s in mid-air. Rotate › Find a good rotation looks for a way to have fewer.",
                        doc->low_points, doc->low_points == 1 ? "spot starts" : "spots start")
                       .c_str());
        if (doc->fit().fits)
            verdictRow("fit", Verdict::Ok, "Fits the plate",
                       (dims(doc->fit().size) + ", with room around it for the raft").c_str());
        else
            verdictRow("fit", Verdict::Error, "Too big for the plate", tooBig(*doc, pr).c_str());

        ImGui::Dummy(ImVec2(0, 6));
        text("Choose a size", fonts().semibold, size::Body, col::Text);
        ImGui::Dummy(ImVec2(0, 2));
        const fruit::PlateFit& f = doc->fit();
        fruit::Placement turned;
        turned.rotation_deg.z = 90;
        const bool full_fits = f.fits || fruit::checkFit(doc->part().mesh(), turned, pr).fits;
        if (full_fits) {
            fruit::Placement p;
            if (!f.fits) p.rotation_deg.z = 90;
            const std::string title = f.fits ? "Full size: 100 %" : "Full size, turned a quarter: 100 %";
            if (radioCard("full", title.c_str(), sizeLine(*doc, p).c_str(), h.choice == SizeChoice::Full))
                app.chooseSize(SizeChoice::Full);
        }
        {
            fruit::Placement p;
            p.scale = std::max(f.largest, f.largest_turned);
            const bool turn = f.largest_turned > f.largest;
            if (turn) p.rotation_deg.z = 90;
            const fruit::PlateFit at = fruit::checkFit(doc->part().mesh(), p, pr);
            const double m = fruit::plateMargin(p);
            const double sx = pr.build_x - 2 * m - at.size.x, sy = pr.build_y - 2 * m - at.size.y,
                         sz = pr.build_z - p.lift() - at.size.z;
            std::string spare;
            if (sy <= sx && sy <= sz) spare = strf("leaves %.1f mm of plate depth spare", sy);
            else if (sx <= sz) spare = strf("leaves %.1f mm of plate width spare", sx);
            else spare = strf("leaves %.1f mm of height spare", sz);
            const std::string title = "Largest that fits: " + percent(p.scale) + (turn ? ", turned a quarter" : "");
            if (radioCard("largest", title.c_str(), (dims(at.size) + " · " + spare).c_str(),
                          h.choice == SizeChoice::Largest))
                app.chooseSize(SizeChoice::Largest);
        }
        {
            fruit::Placement p;
            p.scale = h.custom_percent / 100.0;
            const ImVec2 card = ImGui::GetCursorScreenPos();
            if (radioCard("custom", "Custom", sizeLine(*doc, p).c_str(), h.choice == SizeChoice::Custom))
                app.chooseSize(SizeChoice::Custom);
            const ImVec2 after = ImGui::GetCursorScreenPos();
            const float card_h = ImGui::GetItemRectSize().y;
            ImGui::SetCursorScreenPos(ImVec2(card.x + body_w - 14 - 22 - 82, card.y + (card_h - 34) / 2));
            double v = h.custom_percent;
            if (numberField("custom_pct", &v, 82, "%g", 0.1, 100000)) app.chooseSize(SizeChoice::Custom, v);
            if (ImGui::IsItemActivated()) app.chooseSize(SizeChoice::Custom);
            ImGui::SameLine(0, 8);  // ImGui aligns text with the field's own text
            text("%", fonts().regular, size::Body, col::TextSecondary);
            ImGui::SetCursorScreenPos(after);
        }
        {
            const fruit::Vec3 s = c.bbox.size();
            const double longest = std::max({s.x, s.y, s.z});
            const std::string body =
                c.maybeInches()
                    ? strf("Possibly: at %.1f mm on its longest side it may have been drawn in inches. 2540 %% "
                           "makes it %.0f mm.",
                           longest, longest * 25.4)
                    : strf("Not likely here: at %.0f mm on its longest side it looks like millimetres already",
                           longest);
            if (radioCard("inches", "Made in inches?", body.c_str(), h.choice == SizeChoice::Inches))
                app.chooseSize(SizeChoice::Inches);
        }
        const fruit::Placement chosen = app.healthPlacement();
        if (!fruit::checkFit(doc->part().mesh(), chosen, pr).fits) {
            ImGui::Dummy(ImVec2(0, 2));
            text(("At " + percent(chosen.scale) + " it does not fit the plate; you can still place it and change "
                                                  "the size with the Scale tool.")
                     .c_str(),
                 fonts().regular, size::Help, col::Error, true);
        }
        ImGui::Dummy(ImVec2(0, 4));
    }
    ImGui::EndChild();
    rule(4);

    const fruit::Placement chosen = app.healthPlacement();
    const std::string place = (app.addingPart() ? "Add at " : "Place at ") + percent(chosen.scale);
    ImGui::PushFont(fonts().bold, size::Body);
    const float pw = ImGui::CalcTextSize(place.c_str()).x + 36;
    ImGui::PopFont();
    const float cw = 78;
    ImGui::SetCursorPos(ImVec2(720 - kPad - pw - 10 - cw, ImGui::GetCursorPosY() + 4));
    if (secondaryButton("Cancel", ImVec2(cw, 40)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        app.cancelChecked();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, 10);
    if (primaryButton(place.c_str(), ImVec2(pw, 40)) ||
        (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        app.placeChecked();
        ImGui::CloseCurrentPopup();
    }
    ImGui::Dummy(ImVec2(0, kPad - 8));
    endModal();
}

void shortcuts(App&, bool& open) {
    if (open && !ImGui::IsPopupOpen("shortcuts")) ImGui::OpenPopup("shortcuts");
    if (!beginModal("shortcuts", 640, 60)) return;
    if (!open) {
        ImGui::CloseCurrentPopup();
        endModal();
        return;
    }
    ImGui::SetCursorPos(ImVec2(kPad, kPad + 6));
    text("Keyboard shortcuts", fonts().bold, size::DialogTitle, col::Text);
    ImGui::SetCursorPos(ImVec2(640 - kPad - 36, kPad - 2));
    if (closeButton("close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetCursorPosY(kPad + 52);
    rule(4);
    struct Row {
        std::string what, keys;
    };
    const std::vector<Row> anywhere = {{"Open a model or a job", cmd("O")},
                                       {"Save the job", cmd("S")},
                                       {"Save the job as…", cmd("S", true)},
                                       {"Undo", cmd("Z")},
                                       {"Redo", cmd("Z", true)},
                                       {"Keyboard shortcuts", cmd("/")},
                                       {"The Job panel's main button", cmd("↩")}};
    const std::vector<Row> tools = {{"On plate · Move · Rotate · Scale", "P · M · R · S"},
                                    {"Supports · Hollow", "T · H"},
                                    {"Close the tool's panel", "Esc"}};
    const std::vector<Row> looking = {{"Turn the view", "drag"},
                                      {"Move the view", "Shift drag"},
                                      {"Zoom", "scroll"},
                                      {"Front · right · from below · start view", "1 · 2 · 3 · 4"},
                                      {"From the top", "5"},
                                      {"Fit the model", "F"}};
    const std::vector<Row> placing = {{"Move it on the plate", "drag an arrow"},
                                      {"Turn it", "drag a ring"},
                                      {"Turn it in 15° steps", "Shift drag a ring"},
                                      {"Move it 1 mm", "← → ↑ ↓"},
                                      {"Move it 10 mm", "Shift ← → ↑ ↓"},
                                      {"Let go and put it back", "Esc"}};
    const std::vector<Row> parts = {{"Add a part to the plate", cmd("O", true)},
                                    {"Select a part: the tools act on it", "click it"}};
    const std::vector<Row> layers = {{"Next or previous layer", "↑ · ↓"},
                                     {"Ten layers at a time", "Page Up · Page Down"},
                                     {"First or last layer", "Home · End"}};
    const float col_w = (640 - 2 * kPad - 24) / 2;
    const float y0 = ImGui::GetCursorPosY() + 4;
    // One group of rows from (x, y); returns where it ends.
    auto column = [&](float x, float y, const char* label, const std::vector<Row>& rows) {
        ImGui::SetCursorPos(ImVec2(x, y));
        ImGui::BeginGroup();
        groupLabel(label);
        ImGui::Dummy(ImVec2(0, 2));
        for (const Row& r : rows) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(p, ImVec2(p.x + col_w, p.y), col::Border);
            ImGui::Dummy(ImVec2(col_w, 6));
            const float ry = ImGui::GetCursorPosY();
            text(r.what.c_str(), fonts().regular, size::Body, col::TextSecondary);
            ImGui::SetCursorPos(ImVec2(x + col_w - keyBadgeWidth(r.keys.c_str()), ry - 3));
            keyBadge(r.keys.c_str());
            ImGui::SetCursorPos(ImVec2(x, ry + 22));
        }
        ImGui::Dummy(ImVec2(0, 0));  // the group ends at the last row
        ImGui::EndGroup();
        return ImGui::GetCursorPosY();
    };
    float left = column(kPad, y0, "Anywhere", anywhere);
    left = column(kPad, left + 14, "Tools", tools);
    left = column(kPad, left + 14, "Placing the model (Move and Rotate)", placing);
    const float x1 = kPad + col_w + 24;
    float right = column(x1, y0, "Looking around", looking);
    right = column(x1, right + 14, "In Layers", layers);
    right = column(x1, right + 14, "Several parts", parts);
    ImGui::SetCursorPos(ImVec2(kPad, std::max(left, right)));
    ImGui::Dummy(ImVec2(0, kPad - 4));
    endModal();
}

}  // namespace gui::dialogs
