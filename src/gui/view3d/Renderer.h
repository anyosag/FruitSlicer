// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// OpenGL 3.3 core renderer for the 3D view: the build plate and meshes,
// drawn into an offscreen (optionally multisampled) target that the UI shows
// as an image. Only the UI thread may call it.

#include <cstddef>
#include <string>
#include <vector>

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

    // The models on the plate (each part of the job), drawn in order. Each
    // is in its own coordinates and uploaded once per `geometry`: moving it
    // only changes its transform, and colours can change every frame.
    struct ModelDraw {
        const fruit::TriangleMesh* mesh = nullptr;
        int geometry = -1;
        fruit::Transform transform;  // model -> printer
        unsigned rgb = 0xAEB6C4;     // 0xRRGGBB
        // `tint_count` faces from face `tint_first` on in `tint_rgb` instead:
        // the patches a repair added (its last faces), or all of it in red
        // while it doesn't fit. 0 faces: none.
        std::size_t tint_first = 0, tint_count = 0;
        unsigned tint_rgb = 0;
    };
    void setModels(const std::vector<ModelDraw>& models);
    void clearModels();

    // Extra meshes in printer coordinates, each with its own colour
    // (0xRRGGBB): the support tree and the tips. Uploaded when `key`
    // changes; colours and visibility can change every frame.
    void setExtras(const std::vector<const fruit::TriangleMesh*>& meshes, int key);
    void clearExtras();
    void setExtraStyle(std::size_t i, unsigned rgb, bool visible);
    // Tints faces leaning further than `critical_deg` from vertical (0 = off).
    void setOverhangTint(double critical_deg) { overhang_deg_ = critical_deg; }

    // The cut-away: the model and the extras are drawn only where
    // a·x + b·y + c·z + d >= 0 for `plane` (a, b, c, d), and the cap is
    // painted over the cut, in the plane: groups of triangles (x, y, z per
    // corner, printer coordinates) each in one colour (0xRRGGBB), later
    // groups over earlier ones. Uploaded when `key` changes.
    struct CapGroup {
        unsigned rgb = 0;
        std::vector<float> triangles;
    };
    void setCut(const float plane[4], const std::vector<CapGroup>& cap, int key);
    void clearCut();

    // A `w` × `h` picture in colour `rgb` (0xRRGGBB) with `alpha` as its
    // coverage, for the UI to show; free it with freeTexture.
    unsigned uploadMask(int w, int h, const std::vector<unsigned char>& alpha, unsigned rgb);
    static void freeTexture(unsigned tex);
    // A `w` × `h` RGBA picture (top row first), nearest-neighbour filtered so
    // printer pixels stay sharp when zoomed in.
    unsigned uploadRgba(int w, int h, const std::vector<unsigned char>& rgba);

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
    Mesh plate_fill_, plate_lines_;
    struct Model {
        Mesh mesh;
        int geometry = -1;
        float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        unsigned rgb = 0xAEB6C4;
        std::size_t tint_first = 0, tint_count = 0;
        unsigned tint_rgb = 0;
        bool mirrored = false;  // its transform is a mirror: triangles turn inside out
    };
    std::vector<Model> models_;
    struct Extra {
        Mesh mesh;
        unsigned rgb = 0xC8CDD6;
        bool visible = true;
    };
    std::vector<Extra> extras_;
    int extras_key_ = -1;
    bool cut_ = false;
    float cut_plane_[4] = {0, 0, 0, 1};
    struct Cap {
        unsigned vao = 0, vbo = 0;
        int count = 0;  // corners
        unsigned rgb = 0;
    };
    std::vector<Cap> cap_;
    int cap_key_ = -1;
    void destroyCap();
    double overhang_deg_ = 0;
    unsigned fbo_ms_ = 0, rb_color_ = 0, rb_depth_ = 0;  // multisampled target
    unsigned fbo_ = 0, tex_ = 0, rb_depth_plain_ = 0;    // resolved target
    int w_ = 0, h_ = 0, samples_ = -1;
};

}  // namespace gui
