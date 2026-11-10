// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "mesh/ModelLoad.h"

#include "mesh/ObjIO.h"

namespace fruit {

LoadedModel prepareModel(TriangleMesh mesh, bool repair) {
    LoadedModel m;
    // Before shells are turned outward: that goes by a shell's volume, which
    // means nothing until its faces run one way round and it is closed.
    if (repair) m.repair = repairMesh(mesh);
    else m.repair.open_edges_before = m.repair.open_edges_after = countOpenEdges(mesh);
    mesh.shellIds(&m.shells);
    m.shells_flipped = mesh.orientShellsOutward(&m.cavities);
    m.mesh = std::move(mesh);
    return m;
}

LoadedModel loadModel(const std::string& path, bool repair) { return prepareModel(readMesh(path), repair); }

}  // namespace fruit
