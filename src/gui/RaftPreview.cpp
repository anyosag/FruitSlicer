// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "RaftPreview.h"

#include <algorithm>

namespace gui {

RaftPreviewPicture drawRaftPreview(const fruit::SupportTree& made, const fruit::TreeConfig& made_cfg,
                                   const fruit::RaftSpec& s, double min_thick, int w, int h) {
    RaftPreviewPicture out;
    fruit::TreeConfig tc = made_cfg;
    tc.raft = fruit::RaftStyle::Pads;
    tc.raft_spec = s;
    fruit::SupportTree tree = made;
    tree.raft_top = std::max(s.thickness, min_thick);  // as the tree builder makes it
    out.shape = fruit::specRaftShape(tree, tc);
    out.height = tree.raft_top;
    std::vector<PlanFoot> feet;
    for (const fruit::Pillar& P : tree.pillars) {
        if (P.on_model || P.merged) continue;
        const double base = P.base_diameter > 0 ? std::max(P.base_diameter, P.diameter) : tc.base_diameter;
        feet.push_back({P.xy, base / 2, P.diameter / 2, tree.raft_top - 0.1, tc.base_height + 0.1});
    }
    out.picture = drawRaftPlan(out.shape.layers, out.shape.layer_dz, feet,
                               fruit::raftWindowNotches(out.shape, s, tree.raft_top), w, h);
    return out;
}

}  // namespace gui
