// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#pragma once
// Elegoo GOO print files (the Saturn 4 Ultra 16K's native format).
//
// Layout from Elegoo's published spec ("Goo Format Spec V1.2", GPL-3.0,
// github.com/elegooofficial/GOO), with the details the PDF leaves out taken
// from UVtools' GooFile.cs (AGPL-3.0): version "V3.0", magic
// 07 00 00 00 44 4C 50 00, big-endian everything, RGB565 big-endian
// previews, a footer of three zero bytes plus the magic, and per-layer data
// lengths that include the leading 0x55 and the trailing checksum.
//
//   header (195 477 bytes, fields 0–61 in spec order)
//   per layer: definition (64 bytes) · 0D 0A · data size (4) · RLE data · 0D 0A
//   footer: 00 00 00 + magic
//
// Checked against a 16K file exported by ELEGOO SatelLite 1.0.2.43: same
// V3.0 layout, header size and footer; FruitSlicer's decoder reads all of
// its layers. Image orientation is still to be confirmed (goo-compare).

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/JobControl.h"
#include "core/OutFile.h"

namespace fruit {

constexpr int kGooSmallPreview = 116;
constexpr int kGooBigPreview = 290;
constexpr size_t kGooHeaderSize = 195477;

// Exposure and motion settings, in the header's field order (spec 24–53).
struct GooPrintSettings {
    float exposure = 0;               // s, normal layers
    bool delay_mode_static = true;    // 1: wait times below; 0: "turn off time"
    float turn_off_time = 0;
    float bottom_wait_before_lift = 0, bottom_wait_after_lift = 0, bottom_wait_after_retract = 5;
    // SatelLite 1.0.2.43 waits 5 s after each retract on the 16K (bottom and normal).
    float wait_before_lift = 0, wait_after_lift = 0, wait_after_retract = 5;
    float bottom_exposure = 0;        // s
    uint32_t bottom_layers = 0;
    // Distances in mm, speeds in mm/min. The 16K tilts its vat by itself;
    // UVtools writes 0.05 for Saturn 4 Ultra files and so do we by default.
    float bottom_lift_distance = 0.05f, bottom_lift_speed = 0.05f;
    float lift_distance = 0.05f, lift_speed = 0.05f;
    float bottom_retract_distance = 0.05f, bottom_retract_speed = 0.05f;
    float retract_distance = 0.05f, retract_speed = 0.05f;
    float bottom_lift2_distance = 0, bottom_lift2_speed = 0, lift2_distance = 0, lift2_speed = 0;
    float bottom_retract2_distance = 0, bottom_retract2_speed = 0, retract2_distance = 0, retract2_speed = 0;
    uint16_t bottom_pwm = 255, pwm = 255;
    bool advance_mode = false;        // printer uses per-layer values
    uint16_t transition_layers = 0;
};

struct GooHeader {
    std::string version = "V3.0";
    std::string software_name = "FruitSlicer";
    std::string software_version = "0.1.0";
    std::string file_time;            // "yyyy-MM-dd HH:mm:ss"
    std::string printer_name = "ELEGOO Saturn 4 Ultra 16K";  // matches SatelLite 1.0.2.43
    std::string printer_type = "ELEGOO Saturn 4 Ultra 16K";  // as SatelLite 1.0.2.43 writes it
    std::string profile_name = "FruitSlicer";
    uint16_t aa_level = 0, grey_level = 0, blur_level = 0;  // anti-aliasing off, as SatelLite writes
    std::vector<uint8_t> small_preview;  // 116×116 RGB565 big-endian (2 bytes per pixel)
    std::vector<uint8_t> big_preview;    // 290×290
    uint32_t layer_count = 0;
    uint16_t res_x = 15120, res_y = 6230;
    bool mirror_x = false, mirror_y = false;
    float size_x = 211.68f, size_y = 118.37f, size_z = 220.0f;
    float layer_height = 0.05f;
    GooPrintSettings print;
    uint32_t print_time = 0;          // s
    float volume = 0;                 // mm³
    float weight = 0;                 // g
    float price = 0;
    std::string price_unit = "$";
    uint32_t layer_offset = kGooHeaderSize;
    bool grayscale_8bit = false;      // 1: grey values 0x00–0xFF (SatelLite writes 0 with AA off)
};

struct GooLayerDef {
    uint16_t pause = 0;
    float pause_z = 0;
    float z = 0;                      // top of this layer, mm
    float exposure = 0, off_time = 0;
    float wait_before_lift = 0, wait_after_lift = 0, wait_after_retract = 0;
    float lift_distance = 0, lift_speed = 0, lift2_distance = 0, lift2_speed = 0;
    float retract_distance = 0, retract_speed = 0, retract2_distance = 0, retract2_speed = 0;
    uint16_t pwm = 255;
};

// Per-layer definition derived from the header (bottom / transition / normal).
GooLayerDef layerDefFor(const GooHeader& h, uint32_t index);

// Run-length encoder for one layer image (row-major pixel stream).
class GooRleEncoder {
public:
    GooRleEncoder();
    void add(uint8_t value, uint32_t count);
    std::vector<uint8_t> finish();  // appends the checksum; encoder is spent

private:
    void flush();
    std::vector<uint8_t> out_;
    uint8_t prev_ = 0, cur_ = 0;
    uint64_t stride_ = 0;
};

// Decodes one layer's RLE data into `pixels` (must be res_x × res_y).
// Throws on a bad magic byte, checksum or overrun.
void decodeGooRle(const std::vector<uint8_t>& data, std::vector<uint8_t>& pixels);

// Streaming writer: header placeholder, layers in order, footer, then the
// final header (with totals) is rewritten in place. Data goes to
// "<path>.part" and replaces `path` only when finish() succeeds; a writer
// destroyed before that (error, cancelled export) deletes the partial file.
class GooWriter {
public:
    GooWriter(const std::string& path, const GooHeader& header);
    GooWriter(const GooWriter&) = delete;
    GooWriter& operator=(const GooWriter&) = delete;

    void addLayer(const GooLayerDef& def, const std::vector<uint8_t>& rle);
    // Writes the footer and the final header. `header` totals may be updated
    // (layer count is checked against the layers added). `sync_to_device`
    // forces the data onto the drive before returning (USB sticks).
    void finish(const GooHeader& header, bool sync_to_device = false);

private:
    OutFile out_;
    uint32_t layers_ = 0;
};

struct GooFileInfo {
    GooHeader header;
    std::vector<GooLayerDef> layers;
    std::vector<std::vector<uint8_t>> rle;  // per layer, as stored
    bool footer_ok = false;
};

GooFileInfo readGoo(const std::string& path);
// Only the header (previews and settings): quick even for a large file.
GooHeader readGooHeader(const std::string& path);

// What reading a .goo back finds. Layers are checked from their runs (no
// image is built), so this stays quick for thousands of 16K layers.
struct GooVerifyResult {
    uint32_t layers = 0;
    bool footer_ok = false;
    bool checksums_ok = true;   // every layer decodes: magic, checksum, exact pixel count
    int first_bad_layer = -1;   // the first that did not, and why
    std::string error;
    size_t lit_values = 0;      // distinct non-zero pixel values
    size_t grey_values = 0;     // distinct values strictly between 0 and 255
    double volume_mm3 = 0;      // lit coverage × pixel area × layer height
};

// Reports stage "verify". Throws if the file cannot be read as a .goo at all.
GooVerifyResult verifyGoo(const std::string& path, const JobControl& ctl = JobControl::none());

// Copies print settings and identity strings (printer, profile, version,
// resolution, mirrors, AA) from `from` into `to`, leaving geometry-dependent
// totals and previews alone.
void copyPrintSettings(const GooHeader& from, GooHeader& to);

// RGB (3 bytes/pixel) → RGB565 big-endian.
std::vector<uint8_t> toRgb565BE(const std::vector<uint8_t>& rgb);

}  // namespace fruit
