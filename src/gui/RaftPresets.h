// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Raft presets of your own (the Raft page), kept on this computer, and which
// raft new jobs start with. Built-in presets never change: changing a value of
// one, or Make a copy, saves a preset of your own, named "Copy of …".

#include <string>
#include <vector>

#include "support/Raft.h"

namespace gui {

struct OwnRaftPreset {
    std::string name;
    std::string from;  // the built-in preset it was first copied from: what "as in the preset" compares with
    fruit::RaftSpec spec;
    bool operator==(const OwnRaftPreset&) const = default;
};

struct RaftPresets {
    std::vector<OwnRaftPreset> own;
    std::string new_jobs = fruit::kDefaultRaftPreset;  // the preset new jobs start with
    bool operator==(const RaftPresets&) const = default;
    const OwnRaftPreset* find(const std::string& name) const;
    OwnRaftPreset* find(const std::string& name);
};

// A missing or unreadable file reads as none of your own; writing creates the folder.
RaftPresets loadRaftPresets(const std::string& file);
void saveRaftPresets(const std::string& file, const RaftPresets& p);

// A name not taken yet: "Copy of <base>", then "Copy of <base> 2", 3, …
std::string copyName(const std::string& base, const RaftPresets& p);
// Why `name` can't be used ("" when it can): empty, a built-in preset's, or another of yours (`except`: the one
// being renamed, which may keep its name).
std::string nameProblem(const std::string& name, const RaftPresets& p, const std::string& except = "");
// A preset's settings by name, built in or yours (null: no such preset).
const fruit::RaftSpec* presetSpec(const std::string& name, const RaftPresets& p);
// The built-in preset a preset is, or was first copied from ("" for none).
std::string presetOrigin(const std::string& name, const RaftPresets& p);
bool isBuiltInPreset(const std::string& name);

// What the raft is, in a sentence, for the Supports panel. `thickness`: the real one (raised to the printer's
// long-exposure layers if the setting is thinner).
std::string raftSummary(const fruit::RaftSpec& s, double thickness);
// The change from `was` to `now`, in words, for the copy prompt ("Reach past the trunks 1.8 → 2.5 mm"), or
// "" when more than one value differs.
std::string raftChange(const fruit::RaftSpec& was, const fruit::RaftSpec& now);

}  // namespace gui
