// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "Capture.h"

#include <glad/gl.h>
#include <miniz.h>

#include <stdexcept>

#include "core/OutFile.h"

namespace gui {

OffscreenTarget::~OffscreenTarget() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (tex_) glDeleteTextures(1, &tex_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
}

void OffscreenTarget::resize(int w, int h) {
    if (w == w_ && h == h_ && fbo_) return;
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (tex_) glDeleteTextures(1, &tex_);
    if (depth_) glDeleteRenderbuffers(1, &depth_);
    w_ = w, h_ = h;
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenRenderbuffers(1, &depth_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("offscreen framebuffer incomplete");
}

void OffscreenTarget::bind() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w_, h_);
}

void OffscreenTarget::unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

std::vector<uint8_t> OffscreenTarget::readRgba() const {
    std::vector<uint8_t> px(static_cast<size_t>(w_) * h_ * 4);
    GLint prev = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev));
    // GL rows run bottom-up; images are stored top row first.
    const size_t row = static_cast<size_t>(w_) * 4;
    std::vector<uint8_t> tmp(row);
    for (int y = 0; y < h_ / 2; ++y) {
        uint8_t* a = px.data() + static_cast<size_t>(y) * row;
        uint8_t* b = px.data() + static_cast<size_t>(h_ - 1 - y) * row;
        std::copy(a, a + row, tmp.data());
        std::copy(b, b + row, a);
        std::copy(tmp.data(), tmp.data() + row, b);
    }
    return px;
}

void writePngRgba(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), w, h, 4, &len, 6, MZ_FALSE);
    if (!png) throw std::runtime_error("PNG encoding failed for " + path);
    try {
        fruit::OutFile f(path);
        f.write(png, len);
        f.commit();
    } catch (...) {
        mz_free(png);
        throw;
    }
    mz_free(png);
}

}  // namespace gui
