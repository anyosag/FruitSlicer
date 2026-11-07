// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// FruitSlicer desktop app: window, OpenGL context, Dear ImGui and the loop.
//
//   FruitSlicer [model.stl]               interactive
//   FruitSlicer --self-test               engine + fonts check, no window
//   FruitSlicer --self-test-gl            also opens a hidden window and draws a frame
//   FruitSlicer --script s.json --shots d scripted run writing screenshots

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>

#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "App.h"
#include "Capture.h"
#include "Document.h"
#include "Jobs.h"
#include "Log.h"
#include "Script.h"
#include "Theme.h"
#include "layers/LayerStack.h"
#include "mesh/Placement.h"
#include "platform/Platform.h"
#include "support/PointGenerator.h"

extern const std::size_t kFontInter_regular_size;
extern const std::uint8_t kFontInter_regular[];

using namespace gui;

namespace {

struct Args {
    std::string script, shots, open;
    bool self_test = false, self_test_gl = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (s == "--script") a.script = next();
        else if (s == "--shots") a.shots = next();
        else if (s == "--self-test") a.self_test = true;
        else if (s == "--self-test-gl") a.self_test = a.self_test_gl = true;
        else if (s.rfind("-psn_", 0) == 0) continue;  // old macOS launch argument
        else if (!s.empty() && s[0] != '-') a.open = s;
    }
    return a;
}

// Checks that need no window: embedded fonts and an engine job on a worker.
int selfTest() {
    std::printf("fonts: Inter Regular %zu bytes\n", kFontInter_regular_size);
    if (kFontInter_regular_size < 1000 || kFontInter_regular[0] != 0x00 || kFontInter_regular[1] != 0x01) {
        std::printf("FAIL: embedded font data looks wrong\n");
        return 1;
    }
    JobRunner jobs([] {});
    size_t points = 0, layers = 0;
    bool ok = false;
    jobs.start(
        "self-test",
        [&](const fruit::JobControl& ctl) {
            fruit::Placement p;
            const fruit::TriangleMesh m = fruit::placeModel(demoModel(), p);
            fruit::SupportConfig cfg;
            cfg.layer_height = 0.1;
            const auto res = fruit::generateSupportPoints(m, cfg, {}, ctl);
            points = res.points.size();
            layers = res.analysis.layers->size();
        },
        [&](std::exception_ptr e) { ok = !e; });
    jobs.wait();
    std::printf("engine: %zu support points on %zu layers\n", points, layers);
    if (!ok || points == 0) {
        std::printf("FAIL: engine job\n");
        return 1;
    }
    std::printf("self-test passed\n");
    return 0;
}

void wakeUi() {
    SDL_Event e{};
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    const bool scripted = !args.script.empty();
    log::open(scripted || args.self_test ? (std::filesystem::temp_directory_path() / "fruitslicer-test").string()
                                         : platform::dataDir());
    log::info("FruitSlicer " FRUIT_VERSION " starting");
    if (args.self_test && !args.self_test_gl) return selfTest();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log::error(std::string("SDL_Init: ") + SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    const bool hidden = scripted || args.self_test_gl;
    SDL_Window* window =
        SDL_CreateWindow("FruitSlicer", 1440, 900,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                             (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!window) {
        log::error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 960, 600);
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        log::error(std::string("OpenGL 3.3 core context: ") + SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        log::error("cannot load OpenGL functions");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the layout is fixed; nothing to remember
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (scripted) io.ConfigInputTextCursorBlink = false;
    ImGui_ImplSDL3_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init(nullptr);
    applyTheme();

    AppOptions opts;
    opts.deterministic = scripted;
    int code = 0;
    {  // the app's GL objects must go before the GL context
    App app(window, opts, wakeUi);
    std::string err;
    if (!app.initGraphics(&err)) {
        log::error("graphics: " + err);
        return 1;
    }

    if (scripted || args.self_test_gl) {
        // Fixed-size offscreen frames: the same pixels on any machine with
        // the same GL implementation.
        Script* script = nullptr;
        int lw = 1440, lh = 900;
        float scale = 1;
        std::unique_ptr<Script> sp;
        if (scripted) {
            sp = std::make_unique<Script>(args.script);
            script = sp.get();
            lw = script->width(), lh = script->height(), scale = script->scale();
        }
        OffscreenTarget target;
        target.resize(static_cast<int>(lw * scale), static_cast<int>(lh * scale));
        auto render = [&]() {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
            }  // input is ignored in scripted runs
            platform::pumpCallbacks();
            app.jobs().poll();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            io.DisplaySize = ImVec2(static_cast<float>(lw), static_cast<float>(lh));
            io.DisplayFramebufferScale = ImVec2(scale, scale);
            io.DeltaTime = 1.0f / 60.0f;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            target.bind();
            glClearColor(0x11 / 255.0f, 0x13 / 255.0f, 0x17 / 255.0f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            target.unbind();
            return target.readRgba();
        };
        if (scripted) {
            try {
                code = script->run(app, args.shots, render);
            } catch (const std::exception& e) {
                log::error(std::string("script: ") + e.what());
                code = 2;
            }
        } else {
            const auto px = render();
            std::printf("graphics: %s\n", app.glInfo().c_str());
            std::printf("frame: %dx%d rendered, %zu bytes read back\n", target.width(), target.height(), px.size());
            code = px.empty() ? 1 : selfTest();
        }
    } else {
        if (!args.open.empty()) app.openPath(args.open);
        bool quit = false;
        int frames = 3;  // frames still to draw after the last event
        while (!quit) {
            SDL_Event e;
            const bool hovering = ImGui::GetCurrentContext() && ImGui::IsAnyItemHovered();
            bool got;
            if (frames > 0) got = SDL_PollEvent(&e);
            else got = SDL_WaitEventTimeout(&e, app.jobs().busy() || hovering ? 100 : 1000);
            while (got) {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) quit = true;
                if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window))
                    quit = true;
                if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) app.openPath(e.drop.data);
                frames = 3;
                got = SDL_PollEvent(&e);
            }
            platform::pumpCallbacks();
            app.jobs().poll();
            if (frames <= 0 && !app.jobs().busy() && !hovering) continue;
            if (frames > 0) --frames;
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) continue;
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            int fw = 0, fh = 0;
            SDL_GetWindowSizeInPixels(window, &fw, &fh);
            glViewport(0, 0, fw, fh);
            glClearColor(0x11 / 255.0f, 0x13 / 255.0f, 0x17 / 255.0f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            SDL_GL_SwapWindow(window);
        }
    }
    app.jobs().cancel();
    app.jobs().wait();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    log::info("exit " + std::to_string(code));
    return code;
}
