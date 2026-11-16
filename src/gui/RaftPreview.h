// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// The Raft page's preview: the raft a spec would make round the trunks of the
// supports made last, seen from straight above. No UI here.

#include "JobPicture.h"
#include "support/Raft.h"
#include "support/SupportTree.h"

namespace gui {

struct RaftPreviewPicture {
    fruit::RaftShape shape;
    double height = 0;  // the raft's thickness as it will be built
    JobPicture picture;
};

// `made` and `made_cfg`: the supports made last, whose trunks the raft goes round. `min_thick`: the printer's
// long-exposure layers, which a thinner raft is raised to. The old Pads and Full rafts are drawn as the Raft
// page's builder makes them, which is how they look.
RaftPreviewPicture drawRaftPreview(const fruit::SupportTree& made, const fruit::TreeConfig& made_cfg,
                                   const fruit::RaftSpec& s, double min_thick, int w, int h);

}  // namespace gui
