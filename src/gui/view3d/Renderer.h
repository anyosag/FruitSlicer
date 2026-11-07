// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// OpenGL 3.3 core renderer for the 3D view: the build plate and meshes,
// drawn into an offscreen (optionally multisampled) target that the UI shows
// as an image. Only the UI thread may call it.

#include <string>

#include "geometry/Transform.h"
#include "mesh/TriangleMesh.h"
#include "printer/PrinterProfile.h"
#include "view3d/Camera.h"

namespace gui {

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Compiles shaders and builds the plate. False (with a message) when
    // the GL context cannot do it.
    bool init(const fruit::PrinterProfile& printer, std::string* error);

    // Uploads the model (its own coordinates) unless `geometry` is already
    // on the GPU; placement changes only change the transform.
    void setModel(const fruit::TriangleMesh& mesh, int geometry);
    void setModelTransform(const fruit::Transform& model_to_printer);
    void clearModel();

    // Draws into a `w` × `h` pixel target and returns its colour texture.
    unsigned render(int w, int h, const Camera& cam, int msaa_samples);

    // "Vendor · renderer · GL version · GLSL version", for diagnostics.
    static std::string glInfo();

private:
    struct Mesh {
        unsigned vao = 0, vbo = 0, ibo = 0;
        int count = 0;
    };
    void destroyMesh(Mesh& m);
    void ensureTarget(int w, int h, int samples);

    unsigned mesh_prog_ = 0, line_prog_ = 0;
    Mesh plate_fill_, plate_lines_, model_;
    int model_revision_ = -1;
    float model_matrix_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    unsigned fbo_ms_ = 0, rb_color_ = 0, rb_depth_ = 0;  // multisampled target
    unsigned fbo_ = 0, tex_ = 0, rb_depth_plain_ = 0;    // resolved target
    int w_ = 0, h_ = 0, samples_ = -1;
};

}  // namespace gui
