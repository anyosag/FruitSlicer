// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// "Send to the printer": the checked print file goes to a printer found on
// the network, and starts printing only when you ask (canvas board
// "SendWifi").
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include <SDL3/SDL.h>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

// A card that toggles, with a check box.
bool checkCard(const char* id, const char* title, const char* body, bool on) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float pad = 12, box = 16;
    ImGui::PushFont(fonts().regular, size::Body);
    const float text_w = w - pad * 2 - box - 10;
    const float body_h = ImGui::CalcTextSize(body, nullptr, false, text_w).y;
    ImGui::PopFont();
    const float h = pad * 2 + 18 + body_h + 2;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hovered ? col::Card : col::Panel, 10);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), on ? col::Warn : (hovered ? col::BorderStrong : col::Border), 10,
                on ? 1.5f : 1.0f);
    const ImVec2 b(p.x + pad, p.y + pad + 1);
    if (on) {
        dl->AddRectFilled(b, ImVec2(b.x + box, b.y + box), col::Warn, 4);
        icon::check(dl, ImVec2(b.x + 2, b.y + 2), 12, col::OnAccent);
    } else {
        dl->AddRect(b, ImVec2(b.x + box, b.y + box), col::TextMuted, 4, 1.5f);
    }
    const float tx = p.x + pad + box + 10;
    ImGui::PushFont(fonts().semibold, size::Body);
    dl->AddText(ImVec2(tx, p.y + pad + 1), col::Text, title);
    ImGui::PopFont();
    ImGui::PushFont(fonts().regular, size::Body);
    dl->AddText(nullptr, 0, ImVec2(tx, p.y + pad + 21), col::TextSecondary, body, nullptr, text_w);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

void progressBar(float f) {
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    const float bw = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + 6), col::Input, 3);
    dl->AddRectFilled(bp, ImVec2(bp.x + bw * std::clamp(f, 0.0f, 1.0f), bp.y + 6), col::Accent, 3);
    ImGui::Dummy(ImVec2(bw, 12));
}

// Buttons at the right end of the dialog's last line.
float rightAligned(float total_w) {
    const float x = ImGui::GetWindowWidth() - 24 - total_w;
    ImGui::SetCursorPosX(x);
    return x;
}

float buttonWidth(const char* label, bool bold) {
    ImGui::PushFont(bold ? fonts().bold : fonts().semibold, size::Body);
    const float w = ImGui::CalcTextSize(label).x + 32;
    ImGui::PopFont();
    return w;
}

void sendingBody(App& app, SendDialog& s, const JobProgress& jp) {
    const auto& printer = s.printers[static_cast<std::size_t>(std::max(0, s.chosen))];
    text(("Sending to " + printerName(printer)).c_str(), fonts().semibold, size::Body, col::Text);
    const double now = static_cast<double>(SDL_GetTicks()) / 1000.0;
    const double f = jp.fraction, took = s.send_started >= 0 ? now - s.send_started : 0;
    std::string line = jp.stage == "start" ? std::string("Waiting for the printer to check the file, then starting")
                                           : strf("%.0f %%", 100 * f);
    if (jp.stage == "send" && f > 0.03 && took > 1 && !app.options().deterministic)
        line += strf(" · about %.0f s left", std::max(1.0, took * (1 - f) / f));
    text(line.c_str(), fonts().regular, size::Body, col::TextSecondary);
    ImGui::Dummy(ImVec2(0, 2));
    progressBar(static_cast<float>(f));
    text("Cancelling tells the printer to drop what has arrived so far.", fonts().regular, size::Help,
         col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 8));
    rightAligned(buttonWidth("Cancel", false));
    if (secondaryButton("Cancel", ImVec2(0, 38))) app.jobs().cancel();
}

void sentBody(App& app, SendDialog& s) {
    const SendResult& r = *s.sent;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 16, p.y + 16), 16, col::OkBg, 28);
    icon::check(dl, ImVec2(p.x + 8, p.y + 8), 16, col::Ok);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 44, p.y));
    ImGui::BeginGroup();
    text((r.upload.remoteName + " is on " + r.printer).c_str(), fonts().semibold, size::Section + 1, col::Text);
    text(r.started ? "Printing has started. You can follow it on the printer's screen."
                   : "Choose it on the printer's screen to print it.",
         fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 12));
    rightAligned(buttonWidth("Done", true));
    if (primaryButton("Done", ImVec2(0, 38))) app.closeSend();
}

void chooseBody(App& app, SendDialog& s, const JobProgress& jp) {
    const bool searching = jp.running && jp.name == "Looking for printers";
    const bool asking = jp.running && jp.name.rfind("Asking ", 0) == 0;
    {
        const float y = ImGui::GetCursorPosY();
        groupLabel("Printers on this network");
        if (!searching && !asking) {
            ImGui::PushFont(fonts().semibold, size::Body);
            const float lw = ImGui::CalcTextSize("Search again").x;
            ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 24 - lw, y - 2));
            if (linkButton("Search again")) app.findPrinters();
        }
    }
    for (std::size_t i = 0; i < s.printers.size(); ++i) {
        const auto& pr = s.printers[i];
        std::string detail = pr.machineName.empty() ? pr.ip : pr.machineName + " · " + pr.ip;
        if (!pr.firmwareVersion.empty()) detail += " · firmware " + pr.firmwareVersion;
        ImGui::PushID(static_cast<int>(i));
        if (radioCard("printer", printerName(pr).c_str(), detail.c_str(), s.chosen == static_cast<int>(i)))
            s.chosen = static_cast<int>(i);
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 2));
    }
    if (searching) {
        text("Looking for printers…", fonts().regular, size::Body, col::TextSecondary);
    } else if (asking) {
        text((jp.name + "…").c_str(), fonts().regular, size::Body, col::TextSecondary);
    } else if (s.searched && s.printers.empty()) {
        text("No printer answered. Check that it is on and on the same Wi-Fi as this Mac, or add it by its "
             "address below.",
             fonts().regular, size::Body, col::TextSecondary, true);
        text("macOS asks once whether FruitSlicer may find devices on your local network. If that was turned off, "
             "turn it on in System Settings, Privacy & Security, Local Network, then search again.",
             fonts().regular, size::Help, col::TextMuted, true);
    }
    ImGui::Dummy(ImVec2(0, 4));
    // Add by address.
    text("Add a printer by its address", fonts().semibold, size::Body, col::Text);
    {
        const float aw = buttonWidth("Add", false);
        ImGui::PushFont(fonts().regular, size::Body);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Viewport);
        ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - aw - 8);
        const bool enter = ImGui::InputTextWithHint("##address", "e.g. 192.168.1.40", s.address, sizeof s.address,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::PopFont();
        ImGui::SameLine(0, 8);
        const bool can = s.address[0] != 0 && !searching && !asking;
        if ((secondaryButton("Add", ImVec2(aw, 29), can) || (enter && can))) app.addPrinter(s.address);
    }
    ImGui::Dummy(ImVec2(0, 8));
    if (checkCard("start_print", "Start printing when it arrives",
                  s.start_print ? "The printer starts as soon as the file arrives. Check now that the vat has enough "
                                  "resin, the plate is clean and tight, and nothing is in the way."
                                : "Off by default: check the vat and the plate at the printer first.",
                  s.start_print))
        s.start_print = !s.start_print;
    if (!s.problem.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        text(s.problem.c_str(), fonts().regular, size::Body, col::Error, true);
    }
    ImGui::Dummy(ImVec2(0, 10));
    const char* go = s.start_print ? "Send and print" : "Send";
    const float cw = buttonWidth("Cancel", false), gw = buttonWidth(go, true);
    rightAligned(cw + 10 + gw);
    if (secondaryButton("Cancel", ImVec2(cw, 38))) app.closeSend();
    ImGui::SameLine(0, 10);
    const bool ready = s.chosen >= 0 && s.file && s.file->good && !jp.running;
    if (primaryButton(go, ImVec2(gw, 38), ready)) app.sendToPrinter();
    if (!ready && s.chosen < 0) tooltip("Choose a printer first.");
}

}  // namespace

void sendDialog(App& app) {
    SendDialog& s = app.send();
    if (s.open && !ImGui::IsPopupOpen("send_wifi")) ImGui::OpenPopup("send_wifi");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + 110), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(600, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(5, 6, 8, 184));
    if (ImGui::BeginPopupModal("send_wifi", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        if (!s.open) ImGui::CloseCurrentPopup();
        text("Send to the printer", fonts().bold, size::DialogTitle, col::Text);
        if (s.file) {
            const std::string file = std::filesystem::path(s.file->path).filename().string();
            text(strf("%s · %.1f MB · %s", file.c_str(), static_cast<double>(s.file->written.bytes) / 1e6,
                      s.file->good ? "read back and checked" : "failed its read-back check")
                     .c_str(),
                 fonts().regular, size::Body, s.file->good ? col::TextSecondary : col::Error);
        }
        ImGui::Dummy(ImVec2(0, 6));
        const JobProgress jp = app.jobs().progress();
        const bool sending = jp.running && jp.name.rfind("Sending ", 0) == 0;
        if (sending) sendingBody(app, s, jp);
        else if (s.sent) sentBody(app, s);
        else chooseBody(app, s, jp);
        if (!sending && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) app.closeSend();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

}  // namespace gui::screens::parts
