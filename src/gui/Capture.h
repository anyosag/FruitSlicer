// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Offscreen rendering of whole frames, for scripted screenshots and the GL
// self-test: a fixed-size framebuffer, read back as RGBA and saved as PNG.

#include <cstdint>
#include <string>
#include <vector>

namespace gui {

class OffscreenTarget {
public:
    ~OffscreenTarget();
    void resize(int w, int h);
    void bind();    // binds as the draw framebuffer, sets the viewport
    void unbind();  // back to the window's framebuffer
    std::vector<uint8_t> readRgba() const;  // top row first
    int width() const { return w_; }
    int height() const { return h_; }

private:
    unsigned fbo_ = 0, tex_ = 0, depth_ = 0;
    int w_ = 0, h_ = 0;
};

// Compressed PNG (RGBA, 8-bit). Throws on write errors.
void writePngRgba(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba);

}  // namespace gui
