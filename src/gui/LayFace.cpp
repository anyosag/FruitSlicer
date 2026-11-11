// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "LayFace.h"

#include "Placing.h"

namespace gui {

std::vector<RestingFace> restingFaces(const Part& part) {
    const fruit::FlatFaces& flat = part.flatFaces();
    const fruit::Placement& p = part.placement();
    std::vector<RestingFace> out;
    std::vector<fruit::Vec3> dirs;
    for (std::size_t i = 0; i < flat.faces.size(); ++i) {
        RestingFace f;
        f.region = static_cast<int>(i);
        f.area = flat.faces[i].area * p.scale * p.scale;
        f.dir = printerDirection(p, flat.faces[i].normal);
        f.on_plate = facesDown(f.dir);
        dirs.push_back(f.dir);
        out.push_back(std::move(f));
    }
    const std::vector<std::string> names = faceNames(dirs);
    for (std::size_t i = 0; i < out.size(); ++i) out[i].name = names[i];
    return out;
}

}  // namespace gui
