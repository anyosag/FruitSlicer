// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "App.h"

#include <imgui.h>

#include <filesystem>

#include "Icons.h"
#include "Log.h"
#include "Theme.h"
#include "Widgets.h"
#include "platform/Platform.h"
#include "screens/Screens.h"

namespace gui {

App::App(SDL_Window* window, AppOptions opts, std::function<void()> wake)
    : window_(window), opts_(opts), jobs_(std::move(wake)) {
    camera_.preset(ViewPreset::Angled);
}

bool App::initGraphics(std::string* error) {
    gl_info_ = Renderer::glInfo();
    log::info("OpenGL: " + gl_info_);
    return renderer_.init(printer_, error);
}

void App::showError(std::string title, std::string message) {
    error_title_ = std::move(title);
    error_message_ = std::move(message);
    error_open_ = true;
    log::error(error_title_ + ": " + error_message_);
}

void App::frameModel() {
    if (!doc_ || doc_->placed.empty()) {
        camera_.frame({0, 0, 15}, 110);
        return;
    }
    const fruit::BBox3 b = doc_->placed.bbox();
    const fruit::Vec3 c = b.center();
    const double r = 0.5 * std::sqrt((b.max.x - b.min.x) * (b.max.x - b.min.x) +
                                     (b.max.y - b.min.y) * (b.max.y - b.min.y) +
                                     (b.max.z - b.min.z) * (b.max.z - b.min.z));
    camera_.frame({static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z)},
                  static_cast<float>(std::max(r, 20.0)));
}

void App::setView(ViewPreset v) {
    camera_.preset(v);
    frameModel();
}

void App::openPath(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    screen_ = Screen::Job;
    auto result = std::make_shared<std::shared_ptr<Document>>();
    jobs_.start(
        "Opening " + name, [path, result](const fruit::JobControl& ctl) { *result = loadDocument(path, ctl); },
        [this, result, name](std::exception_ptr err) {
            if (!err) {
                doc_ = *result;
                frameModel();
                return;
            }
            try {
                std::rethrow_exception(err);
            } catch (const fruit::Cancelled&) {
            } catch (const std::exception& e) {
                showError("Couldn't open " + name, e.what());
            }
        });
}

void App::openDialog() {
    platform::openFileDialog(window_, {{"3D models", "stl;obj"}}, [this](const std::string& path) {
        if (!path.empty()) openPath(path);
    });
}

void App::rail() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Rail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::BeginChild("rail", ImVec2(76, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + ws.x - 1, wp.y), ImVec2(wp.x + ws.x - 1, wp.y + ws.y), col::Border);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("logo", ImVec2(64, 44))) screen_ = Screen::Home;
        icon::logo(dl, ImVec2(p.x + 18, p.y + 8), 28);
    }
    ImGui::Dummy(ImVec2(0, 4));
    if (railItem("rail_home", "Home", icon::home, screen_ == Screen::Home)) screen_ = Screen::Home;
    if (railItem("rail_job", "Job", icon::cube, screen_ == Screen::Job)) screen_ = Screen::Job;
    // About sits at the bottom, where Settings will be.
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 56 - 12);
    if (railItem("rail_about", "About", icon::gear, screen_ == Screen::About)) screen_ = Screen::About;
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void App::errorDialog() {
    if (error_open_) {
        ImGui::OpenPopup("error");
        error_open_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(ImVec2(center.x, center.y * 0.6f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col::Panel);
    ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(5, 6, 8, 184));
    if (ImGui::BeginPopupModal("error", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 18, p.y + 18), 18, col::ErrorBg);
        icon::cross(dl, ImVec2(p.x + 8, p.y + 8), 20, col::Error);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 50);
        ImGui::BeginGroup();
        text(error_title_.c_str(), fonts().bold, size::DialogTitle, col::Text);
        ImGui::Dummy(ImVec2(0, 2));
        text(error_message_.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, 10));
        const float bw = 90;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw - 24);
        if (primaryButton("Close", ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

void App::frame() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    rail();
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    switch (screen_) {
        case Screen::Home: screens::home(*this); break;
        case Screen::Job: screens::model(*this); break;
        case Screen::About: screens::about(*this); break;
    }
    ImGui::EndChild();
    errorDialog();
    ImGui::End();
}

}  // namespace gui
