// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The check after supports are made: what of them comes nearer the model than
// it should, other than at its tips. As a warning each (codes below), with
// where, for the Supports screen's list:
//
//  - "shaft_near_model": a shaft, bridge, branch or brace nearer the model
//    than TreeConfig::clearance (the gap that fuses, plus a pixel);
//  - "tip_over_edge": a tip whose contact reaches over the edge of a slot or
//    hole, or onto a wall going down (SupportConfig::tip_edge_margin);
//  - "support_in_slot": any part of a support inside a slot or hole, or in
//    the space under where one opens (DetailMap).

#include <vector>

#include "mesh/TriangleMesh.h"
#include "support/SupportConfig.h"
#include "support/SupportPoint.h"
#include "support/SupportTree.h"

namespace fruit {

class AabbTree;
class DetailMap;
class SharpEdgeIndex;

struct GapCheck {
    std::vector<Warning> warnings;
    std::size_t near_model = 0, over_edge = 0, in_slot = 0;
    double nearest = 1e9;  // the least gap any of them keeps (mm)
};

GapCheck checkSupportGaps(const TriangleMesh& model, const SupportTree& tree, const TreeConfig& tc,
                          const SupportConfig& cfg, const DetailMap* details);
// The same with the model's tree and sharp edges built once (several trees on one model).
GapCheck checkSupportGaps(const TriangleMesh& model, const AabbTree& at, const SharpEdgeIndex& edges,
                          const SupportTree& tree, const TreeConfig& tc, const SupportConfig& cfg,
                          const DetailMap* details);

}  // namespace fruit
