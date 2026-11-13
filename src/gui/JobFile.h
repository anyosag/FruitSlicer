// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Saved jobs (.fruitjob): a zip holding job.json, the job's own copy of each
// part's mesh (model.bin for a job with one part, else part-1.bin,
// part-2.bin...), when a SatelLite file gave the print settings that
// file's header (sample-header.bin), and a picture of the job as it sat
// (picture.bin, for Home). The tips are kept, so supports come back as they
// were without planning them again. No UI here.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Document.h"
#include "JobPicture.h"
#include "core/JobControl.h"
#include "printer/PrinterProfile.h"

namespace gui {

inline constexpr const char* kJobExtension = "fruitjob";

// Writes the job (atomically: a failed save leaves any older file as it was).
// Throws std::runtime_error with a readable message.
void saveJob(const std::string& path, const Document& doc);

struct OpenedJob {
    std::shared_ptr<Document> doc;  // placed, with its supports rebuilt from the saved tips
    std::string model_path;         // where the model was when the job was saved
    bool model_missing = false;     // it is not there any more (the job's own copy is used)
    int part = 0;                   // the part (an id) whose model that is: the first whose file has gone
};

// Reads a job; supports are rebuilt (progress "tree", "check"). Throws with a
// readable message on a damaged or unknown file.
OpenedJob openJob(const std::string& path, const fruit::PrinterProfile& printer, const fruit::JobControl& ctl);

// A fingerprint of a mesh, to tell whether a model file still holds the
// same triangles as the job's copy.
std::uint64_t meshHash(const fruit::TriangleMesh& m);

// The model file `was` found again at `now`, holding `found` (as loaded).
// Every part from that file (copies too) takes the new place, as one edit;
// one whose mesh is not the file's now (the file changed since) takes
// `found`, and with it the job's supports go out of date. Parts made in the
// app have no file, so they keep their mesh. Returns how many took `found`.
int relinkParts(Document& doc, const std::string& was, const std::string& now, fruit::LoadedModel found);

// What changed since the last save, in words ("generated supports", ...).
std::vector<std::string> changesSince(const JobStamp& saved, const JobStamp& now);

// The picture kept in a saved job (empty for a job saved before pictures
// were kept, or a file that doesn't read).
JobPicture loadJobPicture(const std::string& path);

// The jobs opened or saved lately, newest first.
struct RecentJob {
    std::string path, name;
    std::string state;  // e.g. "supports ready"
    std::string when;   // local date and time, "2026-11-07 14:05"
    // As it was saved (jobs remembered before these were kept have none):
    int parts = 0;           // 0: not known
    bool on_plate = false;   // standing on the plate, no raft
    long tips = -1;          // with supports up to date; -1: none, or not known
    double support_ml = -1;  // the same
};
std::vector<RecentJob> loadRecentJobs(const std::string& file);
void rememberJob(const std::string& file, const RecentJob& job, std::size_t keep = 8);

}  // namespace gui
