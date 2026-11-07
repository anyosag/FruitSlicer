// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "mesh/ModelLoad.h"

#include "mesh/ObjIO.h"

namespace fruit {

LoadedModel prepareModel(TriangleMesh mesh) {
    LoadedModel m;
    mesh.shellIds(&m.shells);
    m.shells_flipped = mesh.orientShellsOutward(&m.cavities);
    m.mesh = std::move(mesh);
    return m;
}

LoadedModel loadModel(const std::string& path) { return prepareModel(readMesh(path)); }

}  // namespace fruit
