// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "formats/GooFile.h"

#include "core/JobControl.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace fruit {

namespace {

constexpr uint8_t kMagic[8] = {0x07, 0x00, 0x00, 0x00, 0x44, 0x4C, 0x50, 0x00};
constexpr uint8_t kDelim[2] = {0x0D, 0x0A};
constexpr uint8_t kLayerMagic = 0x55;

// ---- big-endian serialisation

struct Out {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
    void u32(uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
    }
    void f32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        u32(v);
    }
    // Zero-padded; `terminated` keeps the last byte 0 (text fields), the
    // 4-byte version field is written in full ("V3.0", as UVtools does).
    void str(const std::string& s, size_t n, bool terminated = true) {
        const size_t limit = terminated ? n - 1 : n;
        for (size_t i = 0; i < n; ++i) b.push_back(i < limit && i < s.size() ? uint8_t(s[i]) : 0);
    }
    void raw(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    void raw(const std::vector<uint8_t>& v, size_t n) {
        for (size_t i = 0; i < n; ++i) b.push_back(i < v.size() ? v[i] : 0);
    }
};

struct In {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    void need(size_t n) const {
        if (pos + n > b.size()) throw std::runtime_error("GOO: unexpected end of file at byte " + std::to_string(pos));
    }
    uint8_t u8() { need(1); return b[pos++]; }
    uint16_t u16() { need(2); const uint16_t v = uint16_t(b[pos] << 8 | b[pos + 1]); pos += 2; return v; }
    uint32_t u32() {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = v << 8 | b[pos + i];
        pos += 4;
        return v;
    }
    float f32() {
        const uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    std::string str(size_t n) {
        need(n);
        std::string s;
        for (size_t i = 0; i < n && b[pos + i] != 0; ++i) s += char(b[pos + i]);
        pos += n;
        return s;
    }
    std::vector<uint8_t> raw(size_t n) {
        need(n);
        std::vector<uint8_t> v(b.begin() + static_cast<long>(pos), b.begin() + static_cast<long>(pos + n));
        pos += n;
        return v;
    }
    void expect(const uint8_t* p, size_t n, const char* what) {
        need(n);
        if (std::memcmp(b.data() + pos, p, n) != 0) throw std::runtime_error(std::string("GOO: bad ") + what);
        pos += n;
    }
};

std::vector<uint8_t> serializeHeader(const GooHeader& h) {
    Out o;
    o.str(h.version, 4, false);
    o.raw(kMagic, 8);
    o.str(h.software_name, 32);
    o.str(h.software_version, 24);
    o.str(h.file_time, 24);
    o.str(h.printer_name, 32);
    o.str(h.printer_type, 32);
    o.str(h.profile_name, 32);
    o.u16(h.aa_level);
    o.u16(h.grey_level);
    o.u16(h.blur_level);
    o.raw(h.small_preview, size_t(kGooSmallPreview) * kGooSmallPreview * 2);
    o.raw(kDelim, 2);
    o.raw(h.big_preview, size_t(kGooBigPreview) * kGooBigPreview * 2);
    o.raw(kDelim, 2);
    o.u32(h.layer_count);
    o.u16(h.res_x);
    o.u16(h.res_y);
    o.u8(h.mirror_x);
    o.u8(h.mirror_y);
    o.f32(h.size_x);
    o.f32(h.size_y);
    o.f32(h.size_z);
    o.f32(h.layer_height);
    const GooPrintSettings& p = h.print;
    o.f32(p.exposure);
    o.u8(p.delay_mode_static);
    o.f32(p.turn_off_time);
    o.f32(p.bottom_wait_before_lift);
    o.f32(p.bottom_wait_after_lift);
    o.f32(p.bottom_wait_after_retract);
    o.f32(p.wait_before_lift);
    o.f32(p.wait_after_lift);
    o.f32(p.wait_after_retract);
    o.f32(p.bottom_exposure);
    o.u32(p.bottom_layers);
    o.f32(p.bottom_lift_distance);
    o.f32(p.bottom_lift_speed);
    o.f32(p.lift_distance);
    o.f32(p.lift_speed);
    o.f32(p.bottom_retract_distance);
    o.f32(p.bottom_retract_speed);
    o.f32(p.retract_distance);
    o.f32(p.retract_speed);
    o.f32(p.bottom_lift2_distance);
    o.f32(p.bottom_lift2_speed);
    o.f32(p.lift2_distance);
    o.f32(p.lift2_speed);
    o.f32(p.bottom_retract2_distance);
    o.f32(p.bottom_retract2_speed);
    o.f32(p.retract2_distance);
    o.f32(p.retract2_speed);
    o.u16(p.bottom_pwm);
    o.u16(p.pwm);
    o.u8(p.advance_mode);
    o.u32(h.print_time);
    o.f32(h.volume);
    o.f32(h.weight);
    o.f32(h.price);
    o.str(h.price_unit, 8);
    o.u32(h.layer_offset);
    o.u8(h.grayscale_8bit);
    o.u16(p.transition_layers);
    if (o.b.size() != kGooHeaderSize) throw std::logic_error("GOO header size mismatch");
    return std::move(o.b);
}

GooHeader parseHeader(In& in) {
    GooHeader h;
    h.version = in.str(4);
    in.expect(kMagic, 8, "magic");
    h.software_name = in.str(32);
    h.software_version = in.str(24);
    h.file_time = in.str(24);
    h.printer_name = in.str(32);
    h.printer_type = in.str(32);
    h.profile_name = in.str(32);
    h.aa_level = in.u16();
    h.grey_level = in.u16();
    h.blur_level = in.u16();
    h.small_preview = in.raw(size_t(kGooSmallPreview) * kGooSmallPreview * 2);
    in.expect(kDelim, 2, "delimiter after small preview");
    h.big_preview = in.raw(size_t(kGooBigPreview) * kGooBigPreview * 2);
    in.expect(kDelim, 2, "delimiter after big preview");
    h.layer_count = in.u32();
    h.res_x = in.u16();
    h.res_y = in.u16();
    h.mirror_x = in.u8();
    h.mirror_y = in.u8();
    h.size_x = in.f32();
    h.size_y = in.f32();
    h.size_z = in.f32();
    h.layer_height = in.f32();
    GooPrintSettings& p = h.print;
    p.exposure = in.f32();
    p.delay_mode_static = in.u8();
    p.turn_off_time = in.f32();
    p.bottom_wait_before_lift = in.f32();
    p.bottom_wait_after_lift = in.f32();
    p.bottom_wait_after_retract = in.f32();
    p.wait_before_lift = in.f32();
    p.wait_after_lift = in.f32();
    p.wait_after_retract = in.f32();
    p.bottom_exposure = in.f32();
    p.bottom_layers = in.u32();
    p.bottom_lift_distance = in.f32();
    p.bottom_lift_speed = in.f32();
    p.lift_distance = in.f32();
    p.lift_speed = in.f32();
    p.bottom_retract_distance = in.f32();
    p.bottom_retract_speed = in.f32();
    p.retract_distance = in.f32();
    p.retract_speed = in.f32();
    p.bottom_lift2_distance = in.f32();
    p.bottom_lift2_speed = in.f32();
    p.lift2_distance = in.f32();
    p.lift2_speed = in.f32();
    p.bottom_retract2_distance = in.f32();
    p.bottom_retract2_speed = in.f32();
    p.retract2_distance = in.f32();
    p.retract2_speed = in.f32();
    p.bottom_pwm = in.u16();
    p.pwm = in.u16();
    p.advance_mode = in.u8();
    h.print_time = in.u32();
    h.volume = in.f32();
    h.weight = in.f32();
    h.price = in.f32();
    h.price_unit = in.str(8);
    h.layer_offset = in.u32();
    h.grayscale_8bit = in.u8();
    p.transition_layers = in.u16();
    return h;
}

void serializeLayerDef(Out& o, const GooLayerDef& d) {
    o.u16(d.pause);
    o.f32(d.pause_z);
    o.f32(d.z);
    o.f32(d.exposure);
    o.f32(d.off_time);
    o.f32(d.wait_before_lift);
    o.f32(d.wait_after_lift);
    o.f32(d.wait_after_retract);
    o.f32(d.lift_distance);
    o.f32(d.lift_speed);
    o.f32(d.lift2_distance);
    o.f32(d.lift2_speed);
    o.f32(d.retract_distance);
    o.f32(d.retract_speed);
    o.f32(d.retract2_distance);
    o.f32(d.retract2_speed);
    o.u16(d.pwm);
    o.raw(kDelim, 2);
}

GooLayerDef parseLayerDef(In& in) {
    GooLayerDef d;
    d.pause = in.u16();
    d.pause_z = in.f32();
    d.z = in.f32();
    d.exposure = in.f32();
    d.off_time = in.f32();
    d.wait_before_lift = in.f32();
    d.wait_after_lift = in.f32();
    d.wait_after_retract = in.f32();
    d.lift_distance = in.f32();
    d.lift_speed = in.f32();
    d.lift2_distance = in.f32();
    d.lift2_speed = in.f32();
    d.retract_distance = in.f32();
    d.retract_speed = in.f32();
    d.retract2_distance = in.f32();
    d.retract2_speed = in.f32();
    d.pwm = in.u16();
    in.expect(kDelim, 2, "delimiter after layer definition");
    return d;
}

}  // namespace

GooLayerDef layerDefFor(const GooHeader& h, uint32_t i) {
    const GooPrintSettings& p = h.print;
    GooLayerDef d;
    d.z = h.layer_height * static_cast<float>(i + 1);
    d.pause_z = h.size_z;
    const bool bottom = i < p.bottom_layers;
    if (bottom) {
        d.exposure = p.bottom_exposure;
    } else if (i < p.bottom_layers + p.transition_layers) {
        // SatelLite's ramp: reaches the normal exposure on the last
        // transition layer, rounded to 0.01 s (27.5 -> 1.5 over 6 layers:
        // 23.17, 18.83, 14.50, 10.17, 5.83, 1.50).
        const float k = static_cast<float>(i - p.bottom_layers + 1);
        const float e = p.bottom_exposure - (p.bottom_exposure - p.exposure) * k / static_cast<float>(p.transition_layers);
        d.exposure = std::round(e * 100.0f) / 100.0f;
    } else {
        d.exposure = p.exposure;
    }
    d.off_time = p.turn_off_time;
    d.wait_before_lift = bottom ? p.bottom_wait_before_lift : p.wait_before_lift;
    d.wait_after_lift = bottom ? p.bottom_wait_after_lift : p.wait_after_lift;
    d.wait_after_retract = bottom ? p.bottom_wait_after_retract : p.wait_after_retract;
    d.lift_distance = bottom ? p.bottom_lift_distance : p.lift_distance;
    d.lift_speed = bottom ? p.bottom_lift_speed : p.lift_speed;
    d.lift2_distance = bottom ? p.bottom_lift2_distance : p.lift2_distance;
    d.lift2_speed = bottom ? p.bottom_lift2_speed : p.lift2_speed;
    d.retract_distance = bottom ? p.bottom_retract_distance : p.retract_distance;
    d.retract_speed = bottom ? p.bottom_retract_speed : p.retract_speed;
    d.retract2_distance = bottom ? p.bottom_retract2_distance : p.retract2_distance;
    d.retract2_speed = bottom ? p.bottom_retract2_speed : p.retract2_speed;
    d.pwm = bottom ? p.bottom_pwm : p.pwm;
    return d;
}

std::vector<double> gooLayerThicknesses(const GooHeader& h, const std::vector<GooLayerDef>& layers) {
    std::vector<double> t(layers.size(), h.layer_height);
    if (!h.print.advance_mode) return t;
    double below = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        const double z = layers[i].z;
        const double d = std::round((z - below) * 1e4) / 1e4;
        if (d > 0) t[i] = d;
        below = z;
    }
    return t;
}

std::vector<std::pair<double, uint32_t>> gooThicknessCounts(const GooHeader& h, const std::vector<GooLayerDef>& layers) {
    std::vector<std::pair<double, uint32_t>> out;
    for (const double t : gooLayerThicknesses(h, layers)) {
        auto it = std::find_if(out.begin(), out.end(), [&](const auto& e) { return std::abs(e.first - t) < 5e-5; });
        if (it == out.end()) out.push_back({t, 1});
        else ++it->second;
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- RLE

GooRleEncoder::GooRleEncoder() { out_.push_back(kLayerMagic); }

void GooRleEncoder::add(uint8_t v, uint32_t n) {
    if (n == 0) return;
    if (stride_ > 0 && v == cur_) {
        stride_ += n;
        return;
    }
    flush();
    prev_ = cur_;
    cur_ = v;
    stride_ = n;
}

void GooRleEncoder::flush() {
    if (stride_ == 0) return;
    if (stride_ > 0xFFFFFFF) throw std::runtime_error("GOO: run too long for one chunk");
    const uint32_t run = static_cast<uint32_t>(stride_);
    // Difference chunk: small change from the previous run's grey, short run.
    const int diff = std::abs(int(cur_) - int(prev_));
    if (diff <= 0xF && run <= 0xFF && cur_ > 0 && cur_ < 0xFF) {
        uint8_t b = uint8_t(0b10 << 6 | diff);
        if (run > 1) b |= 0x1 << 4;
        if (cur_ < prev_) b |= 0x1 << 5;
        out_.push_back(b);
        if (run > 1) out_.push_back(uint8_t(run));
        return;
    }
    uint8_t b = uint8_t(run & 0xF);
    bool grey = false;
    if (cur_ == 0xFF) b |= 0b11 << 6;
    else if (cur_ > 0) {
        b |= 0b01 << 6;
        grey = true;
    }
    const int extra = run <= 0xF ? 0 : run <= 0xFFF ? 1 : run <= 0xFFFFF ? 2 : 3;
    b |= uint8_t(extra << 4);
    out_.push_back(b);
    if (grey) out_.push_back(cur_);
    if (extra >= 3) out_.push_back(uint8_t(run >> 20));
    if (extra >= 2) out_.push_back(uint8_t(run >> 12));
    if (extra >= 1) out_.push_back(uint8_t(run >> 4));
}

std::vector<uint8_t> GooRleEncoder::finish() {
    flush();
    stride_ = 0;
    uint8_t sum = 0;
    for (size_t i = 1; i < out_.size(); ++i) sum = uint8_t(sum + out_[i]);
    out_.push_back(uint8_t(~sum));
    return std::move(out_);
}

namespace {

// Walks a layer's runs, checking the magic byte, the checksum and that the
// runs cover exactly `pixels`; calls f(colour, run) for each run.
template <class F>
void walkGooRuns(const std::vector<uint8_t>& d, size_t pixels, F&& f) {
    if (d.size() < 2 || d[0] != kLayerMagic) throw std::runtime_error("GOO: layer data does not start with 0x55");
    const size_t last = d.size() - 1;
    uint8_t sum = 0;
    for (size_t i = 1; i < last; ++i) sum = uint8_t(sum + d[i]);
    if (uint8_t(~sum) != d[last]) throw std::runtime_error("GOO: layer checksum mismatch");
    size_t pos = 0;
    uint8_t color = 0;
    auto byteAt = [&](size_t i) {
        if (i >= last) throw std::runtime_error("GOO: truncated chunk");
        return d[i];
    };
    for (size_t i = 1; i < last; ++i) {
        const uint8_t b0 = d[i];
        const int type = b0 >> 6;
        uint32_t run = 0;
        if (type == 0b10) {
            const int mode = (b0 >> 4) & 0x3;
            const uint8_t delta = b0 & 0xF;
            color = (mode & 0x2) ? uint8_t(color - delta) : uint8_t(color + delta);
            run = (mode & 0x1) ? byteAt(++i) : 1;
        } else {
            if (type == 0b00) color = 0;
            else if (type == 0b11) color = 0xFF;
            else color = byteAt(++i);
            const int extra = (b0 >> 4) & 0x3;
            run = b0 & 0xF;
            uint32_t high = 0;
            for (int k = 0; k < extra; ++k) high = high << 8 | byteAt(++i);
            run |= high << 4;  // extra bytes hold bits 4 and up, most significant first
        }
        if (pos + run > pixels) throw std::runtime_error("GOO: layer data overruns the image");
        f(color, pos, run);
        pos += run;
    }
    if (pos != pixels)
        throw std::runtime_error("GOO: layer data covers " + std::to_string(pos) + " of " + std::to_string(pixels) +
                                 " pixels");
}

}  // namespace

void decodeGooRle(const std::vector<uint8_t>& d, std::vector<uint8_t>& px) {
    walkGooRuns(d, px.size(), [&](uint8_t color, size_t pos, uint32_t run) { std::memset(px.data() + pos, color, run); });
}

GooVerifyResult verifyGoo(const std::string& path, const JobControl& ctl) {
    GooVerifyResult v;
    const GooFileInfo f = readGoo(path);
    const GooHeader& h = f.header;
    v.layers = static_cast<uint32_t>(f.rle.size());
    v.footer_ok = f.footer_ok;
    const size_t pixels = static_cast<size_t>(h.res_x) * h.res_y;
    bool seen[256] = {};
    const std::vector<double> thick = gooLayerThicknesses(h, f.layers);
    double volume_px = 0;  // Σ lit coverage × layer thickness
    for (size_t i = 0; i < f.rle.size(); ++i) {
        double lit = 0;
        try {
            walkGooRuns(f.rle[i], pixels, [&](uint8_t color, size_t, uint32_t run) {
                if (!color) return;
                seen[color] = true;
                lit += color / 255.0 * run;
            });
        } catch (const std::exception& e) {
            if (v.checksums_ok) {
                v.checksums_ok = false;
                v.first_bad_layer = static_cast<int>(i);
                v.error = e.what();
            }
        }
        volume_px += lit * (i < thick.size() ? thick[i] : h.layer_height);
        if ((i & 63) == 0) ctl.step("verify", static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, f.rle.size())));
    }
    for (int c = 1; c < 256; ++c) {
        v.lit_values += seen[c];
        v.grey_values += seen[c] && c != 255;
    }
    const double pixel_area = static_cast<double>(h.size_x) / h.res_x * h.size_y / h.res_y;
    v.volume_mm3 = volume_px * pixel_area;
    ctl.report("verify", 1.0);
    return v;
}

// ---------------------------------------------------------------- writer / reader

GooWriter::GooWriter(const std::string& path, const GooHeader& header) : out_(path, /*atomic=*/true) {
    const auto hb = serializeHeader(header);  // placeholder, rewritten by finish()
    out_.write(hb.data(), hb.size());
}

void GooWriter::addLayer(const GooLayerDef& def, const std::vector<uint8_t>& rle) {
    Out o;
    serializeLayerDef(o, def);
    o.u32(static_cast<uint32_t>(rle.size()));
    out_.write(o.b.data(), o.b.size());
    out_.write(rle.data(), rle.size());
    out_.write(kDelim, 2);
    ++layers_;
}

void GooWriter::finish(const GooHeader& header, bool sync_to_device) {
    if (header.layer_count != layers_)
        throw std::runtime_error("GOO: header says " + std::to_string(header.layer_count) + " layers but " +
                                 std::to_string(layers_) + " were written");
    const uint8_t pad[3] = {0, 0, 0};
    out_.write(pad, 3);
    out_.write(kMagic, 8);
    const auto hb = serializeHeader(header);
    out_.seek(0);
    out_.write(hb.data(), hb.size());
    out_.commit(sync_to_device);
}

GooFileInfo readGoo(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> bytes;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    In in{bytes};
    GooFileInfo info;
    info.header = parseHeader(in);
    in.pos = info.header.layer_offset;
    for (uint32_t i = 0; i < info.header.layer_count; ++i) {
        info.layers.push_back(parseLayerDef(in));
        const uint32_t len = in.u32();
        info.rle.push_back(in.raw(len));
        in.expect(kDelim, 2, "delimiter after layer data");
    }
    if (in.pos + 11 <= bytes.size()) {
        const uint8_t pad[3] = {0, 0, 0};
        info.footer_ok = std::memcmp(bytes.data() + in.pos, pad, 3) == 0 &&
                         std::memcmp(bytes.data() + in.pos + 3, kMagic, 8) == 0;
    }
    return info;
}

GooHeader readGooHeader(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> bytes(kGooHeaderSize);
    const size_t n = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    bytes.resize(n);
    return parseGooHeader(bytes);
}

std::vector<uint8_t> gooHeaderBytes(const GooHeader& h) { return serializeHeader(h); }

GooHeader parseGooHeader(const std::vector<uint8_t>& bytes) {
    In in{bytes};
    return parseHeader(in);
}

void copyPrintSettings(const GooHeader& from, GooHeader& to) {
    to.version = from.version;
    to.printer_name = from.printer_name;
    to.printer_type = from.printer_type;
    to.profile_name = from.profile_name;
    to.aa_level = from.aa_level;
    to.grey_level = from.grey_level;
    to.blur_level = from.blur_level;
    to.res_x = from.res_x;
    to.res_y = from.res_y;
    to.mirror_x = from.mirror_x;
    to.mirror_y = from.mirror_y;
    to.size_x = from.size_x;
    to.size_y = from.size_y;
    to.size_z = from.size_z;
    to.layer_height = from.layer_height;
    to.print = from.print;
    to.price_unit = from.price_unit;
    to.grayscale_8bit = from.grayscale_8bit;
}

std::vector<uint8_t> toRgb565BE(const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> out;
    out.reserve(rgb.size() / 3 * 2);
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const uint16_t v = uint16_t((rgb[i] >> 3) << 11 | (rgb[i + 1] >> 2) << 5 | (rgb[i + 2] >> 3));
        out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v));
    }
    return out;
}

}  // namespace fruit
