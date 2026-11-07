// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "view3d/Renderer.h"

#include <glad/gl.h>

#include <vector>

namespace gui {

namespace {

// GLSL 330 core, valid on macOS (4.1 core) and Mesa alike. No extensions.
const char* kMeshVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
out vec3 vWorld;
void main() {
    vWorld = aPos;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

// Flat shading from screen-space derivatives: meshes stay indexed (no
// per-face vertex copies). Faces seen from inside (open meshes, cut-away)
// are darkened.
const char* kMeshFs = R"(#version 330 core
in vec3 vWorld;
uniform vec3 uColor;
uniform vec3 uEye;
uniform float uOverhangSin;   // > 0: tint faces leaning more than this
uniform vec3 uOverhangColor;
out vec4 fragColor;
void main() {
    vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    vec3 toEye = normalize(uEye - vWorld);
    if (dot(n, toEye) < 0.0) n = -n;            // faces the viewer
    vec3 outward = gl_FrontFacing ? n : -n;     // true outward normal
    vec3 base = uColor;
    if (uOverhangSin > 0.0 && outward.z < -uOverhangSin) base = mix(base, uOverhangColor, 0.55);
    vec3 key = normalize(vec3(-0.35, -0.55, 0.75));
    vec3 fill = normalize(vec3(0.6, 0.45, -0.25));
    float d = 0.38 + 0.55 * max(dot(n, key), 0.0) + 0.22 * max(dot(n, fill), 0.0);
    vec3 c = base * d;
    if (!gl_FrontFacing) c *= 0.45;
    fragColor = vec4(c, 1.0);
}
)";

const char* kLineVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * vec4(aPos, 1.0); }
)";

const char* kLineFs = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";

unsigned compile(GLenum type, const char* src, std::string* error) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        if (error) *error = std::string("shader: ") + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

unsigned link(const char* vs, const char* fs, std::string* error) {
    const GLuint v = compile(GL_VERTEX_SHADER, vs, error);
    if (!v) return 0;
    const GLuint f = compile(GL_FRAGMENT_SHADER, fs, error);
    if (!f) {
        glDeleteShader(v);
        return 0;
    }
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        if (error) *error = std::string("program: ") + log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void rgb(unsigned hex, float out[3]) {
    out[0] = ((hex >> 16) & 255) / 255.0f;
    out[1] = ((hex >> 8) & 255) / 255.0f;
    out[2] = (hex & 255) / 255.0f;
}

}  // namespace

Renderer::~Renderer() {
    if (!mesh_prog_) return;  // never initialised (no GL context)
    destroyMesh(plate_fill_);
    destroyMesh(plate_lines_);
    destroyMesh(model_);
    glDeleteProgram(mesh_prog_);
    glDeleteProgram(line_prog_);
    if (fbo_ms_) glDeleteFramebuffers(1, &fbo_ms_);
    if (rb_color_) glDeleteRenderbuffers(1, &rb_color_);
    if (rb_depth_) glDeleteRenderbuffers(1, &rb_depth_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (tex_) glDeleteTextures(1, &tex_);
    if (rb_depth_plain_) glDeleteRenderbuffers(1, &rb_depth_plain_);
}

void Renderer::destroyMesh(Mesh& m) {
    if (m.vao) glDeleteVertexArrays(1, &m.vao);
    if (m.vbo) glDeleteBuffers(1, &m.vbo);
    if (m.ibo) glDeleteBuffers(1, &m.ibo);
    m = Mesh{};
}

namespace {

void upload(unsigned& vao, unsigned& vbo, unsigned& ibo, const std::vector<float>& pos,
            const std::vector<unsigned>& idx) {
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pos.size() * sizeof(float)), pos.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(idx.size() * sizeof(unsigned)), idx.data(),
                 GL_STATIC_DRAW);
    glBindVertexArray(0);
}

}  // namespace

bool Renderer::init(const fruit::PrinterProfile& printer, std::string* error) {
    mesh_prog_ = link(kMeshVs, kMeshFs, error);
    if (!mesh_prog_) return false;
    line_prog_ = link(kLineVs, kLineFs, error);
    if (!line_prog_) return false;
    const float hx = static_cast<float>(printer.build_x / 2), hy = static_cast<float>(printer.build_y / 2);
    upload(plate_fill_.vao, plate_fill_.vbo, plate_fill_.ibo, {-hx, -hy, 0, hx, -hy, 0, hx, hy, 0, -hx, hy, 0},
           {0, 1, 2, 0, 2, 3});
    plate_fill_.count = 6;
    std::vector<float> lp;
    std::vector<unsigned> li;
    auto seg = [&](float x0, float y0, float x1, float y1) {
        const unsigned b = static_cast<unsigned>(lp.size() / 3);
        lp.insert(lp.end(), {x0, y0, 0.02f, x1, y1, 0.02f});
        li.insert(li.end(), {b, b + 1});
    };
    for (float x = 0; x <= hx; x += 10) {
        seg(x, -hy, x, hy);
        if (x > 0) seg(-x, -hy, -x, hy);
    }
    for (float y = 0; y <= hy; y += 10) {
        seg(-hx, y, hx, y);
        if (y > 0) seg(-hx, -y, hx, -y);
    }
    upload(plate_lines_.vao, plate_lines_.vbo, plate_lines_.ibo, lp, li);
    plate_lines_.count = static_cast<int>(li.size());
    return true;
}

void Renderer::setModel(const fruit::TriangleMesh& mesh, int revision) {
    if (revision == model_revision_ && model_.vao) return;
    destroyMesh(model_);
    std::vector<float> pos;
    pos.reserve(mesh.vertices.size() * 3);
    for (const auto& v : mesh.vertices)
        pos.insert(pos.end(), {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)});
    std::vector<unsigned> idx;
    idx.reserve(mesh.faces.size() * 3);
    for (const auto& f : mesh.faces)
        idx.insert(idx.end(), {static_cast<unsigned>(f[0]), static_cast<unsigned>(f[1]), static_cast<unsigned>(f[2])});
    upload(model_.vao, model_.vbo, model_.ibo, pos, idx);
    model_.count = static_cast<int>(idx.size());
    model_revision_ = revision;
}

void Renderer::clearModel() {
    destroyMesh(model_);
    model_revision_ = -1;
}

void Renderer::ensureTarget(int w, int h, int samples) {
    if (w == w_ && h == h_ && samples == samples_) return;
    if (fbo_ms_) glDeleteFramebuffers(1, &fbo_ms_), fbo_ms_ = 0;
    if (rb_color_) glDeleteRenderbuffers(1, &rb_color_), rb_color_ = 0;
    if (rb_depth_) glDeleteRenderbuffers(1, &rb_depth_), rb_depth_ = 0;
    if (fbo_) glDeleteFramebuffers(1, &fbo_), fbo_ = 0;
    if (tex_) glDeleteTextures(1, &tex_), tex_ = 0;
    if (rb_depth_plain_) glDeleteRenderbuffers(1, &rb_depth_plain_), rb_depth_plain_ = 0;
    w_ = w, h_ = h, samples_ = samples;
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    if (samples <= 1) {
        glGenRenderbuffers(1, &rb_depth_plain_);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_depth_plain_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb_depth_plain_);
    } else {
        glGenFramebuffers(1, &fbo_ms_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_ms_);
        glGenRenderbuffers(1, &rb_color_);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_color_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_color_);
        glGenRenderbuffers(1, &rb_depth_);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_depth_);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb_depth_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

unsigned Renderer::render(int w, int h, const Camera& cam, int msaa_samples) {
    if (w <= 0 || h <= 0 || !mesh_prog_) return 0;
    GLint prev_fbo = 0, prev_vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    ensureTarget(w, h, msaa_samples);
    glBindFramebuffer(GL_FRAMEBUFFER, msaa_samples > 1 ? fbo_ms_ : fbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    float bg[3];
    rgb(0x0E1014, bg);
    glClearColor(bg[0], bg[1], bg[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    const M4 vp = cam.proj(static_cast<float>(w) / static_cast<float>(h)) * cam.view();
    const V3 eye = cam.eye();
    // Plate: filled, pushed back so a model standing on it wins the depth test.
    glUseProgram(mesh_prog_);
    glUniformMatrix4fv(glGetUniformLocation(mesh_prog_, "uViewProj"), 1, GL_FALSE, vp.m);
    glUniform3f(glGetUniformLocation(mesh_prog_, "uEye"), eye.x, eye.y, eye.z);
    glUniform1f(glGetUniformLocation(mesh_prog_, "uOverhangSin"), 0.0f);
    float c[3];
    rgb(0x1C2028, c);
    glUniform3f(glGetUniformLocation(mesh_prog_, "uColor"), c[0], c[1], c[2]);
    // Seen from below, the plate is left out so the model's underside (where
    // the supports go) stays visible; only the grid remains, fainter.
    const bool from_below = eye.z < 0;
    if (!from_below) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 4.0f);
        glBindVertexArray(plate_fill_.vao);
        glDrawElements(GL_TRIANGLES, plate_fill_.count, GL_UNSIGNED_INT, nullptr);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    // Grid.
    glUseProgram(line_prog_);
    glUniformMatrix4fv(glGetUniformLocation(line_prog_, "uViewProj"), 1, GL_FALSE, vp.m);
    const float g = from_below ? 0.45f : 1.0f;
    glUniform4f(glGetUniformLocation(line_prog_, "uColor"), g * 0x2E / 255.0f, g * 0x34 / 255.0f, g * 0x3F / 255.0f, 1);
    glBindVertexArray(plate_lines_.vao);
    glDrawElements(GL_LINES, plate_lines_.count, GL_UNSIGNED_INT, nullptr);
    // Model.
    if (model_.vao) {
        glUseProgram(mesh_prog_);
        rgb(0xAEB6C4, c);
        glUniform3f(glGetUniformLocation(mesh_prog_, "uColor"), c[0], c[1], c[2]);
        glBindVertexArray(model_.vao);
        glDrawElements(GL_TRIANGLES, model_.count, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    glUseProgram(0);
    if (msaa_samples > 1) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_ms_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glDisable(GL_DEPTH_TEST);
    return tex_;
}

std::string Renderer::glInfo() {
    auto str = [](GLenum e) {
        const auto* s = reinterpret_cast<const char*>(glGetString(e));
        return std::string(s ? s : "?");
    };
    return str(GL_VENDOR) + " · " + str(GL_RENDERER) + " · GL " + str(GL_VERSION) + " · GLSL " +
           str(GL_SHADING_LANGUAGE_VERSION);
}

}  // namespace gui
