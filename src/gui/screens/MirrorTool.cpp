// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The Mirror tool's panel: three choices, each turns the model into its
// mirror image at once. Undo takes it back.
#include <imgui.h>

#include "App.h"
#include "Icons.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

void mirrorPanel(App& app, Document& doc) {
    groupLabel("Make a mirror image");
    struct Way {
        int axis;
        const char* label;
        const char* help;
    };
    const Way ways[] = {{0, "Left to right", "Swaps its left and right sides."},
                        {1, "Front to back", "Swaps its front and back."},
                        {2, "Top to bottom", "Swaps its top and bottom. This is a mirror image, not turned over."}};
    for (const Way& w : ways) {
        ImGui::PushID(w.axis);
        if (secondaryButton(w.label, ImVec2(0, 36))) app.mirrorModel(w.axis);
        text(w.help, fonts().regular, size::Help, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PopID();
    }
    text("Its middle stays where it is and it stays at the same height. Mirroring again gives the original back. "
         "Supports need making again.",
         fonts().regular, size::Help, col::TextMuted, true);
    if (doc.placement().mirrorImage()) {
        ImGui::Dummy(ImVec2(0, 6));
        beginCard("mirror_now", ImVec2(0, 0), 12);
        text("It is a mirror image of the file.", fonts().regular, size::Body, col::TextSecondary, true);
        endCard();
    }
}

}  // namespace gui::screens::parts
