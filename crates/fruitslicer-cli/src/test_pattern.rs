//! A small calibration print that exercises the .goo writer end to end.
//!
//! Five rectangular pads, symmetric about the plate centre so X/Y mirroring
//! cannot change the result:
//!
//! - a 20 x 20 mm square in the centre: measure both sides with calipers to
//!   check that the non-square 14 x 19 um pixels are scaled correctly;
//! - four 10 x 10 mm squares at (+-60 mm, +-35 mm) from the centre: check
//!   placement and that the whole LCD area is being addressed.

use std::io::Write;

use goo::{rgb565, DelayMode, GooWriter, Header, LayerDef, LayerEncoder, Preview, HEADER_SIZE};

use crate::printer::Printer;

/// Exposure and movement settings. Defaults are placeholders, not tuned for
/// any resin: check them against your resin's recommended settings.
#[derive(Debug, Clone)]
pub struct Settings {
    pub height_mm: f32,
    pub layer_mm: f32,
    pub exposure_s: f32,
    pub bottom_exposure_s: f32,
    pub bottom_layers: u32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            height_mm: 3.0,
            layer_mm: 0.05,
            exposure_s: 2.5,
            bottom_exposure_s: 30.0,
            bottom_layers: 4,
        }
    }
}

/// A rectangle in mm, relative to the plate centre.
#[derive(Debug, Clone, Copy)]
struct Rect {
    cx: f64,
    cy: f64,
    w: f64,
    h: f64,
}

const PADS: [Rect; 5] = [
    Rect {
        cx: 0.0,
        cy: 0.0,
        w: 20.0,
        h: 20.0,
    },
    Rect {
        cx: -60.0,
        cy: -35.0,
        w: 10.0,
        h: 10.0,
    },
    Rect {
        cx: 60.0,
        cy: -35.0,
        w: 10.0,
        h: 10.0,
    },
    Rect {
        cx: -60.0,
        cy: 35.0,
        w: 10.0,
        h: 10.0,
    },
    Rect {
        cx: 60.0,
        cy: 35.0,
        w: 10.0,
        h: 10.0,
    },
];

/// Pixel-space rectangle: [x0, x1) x [y0, y1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PixelRect {
    pub x0: u32,
    pub x1: u32,
    pub y0: u32,
    pub y1: u32,
}

pub fn pixel_rects(p: &Printer) -> Vec<PixelRect> {
    let (px, py) = p.pixel_mm();
    let (mid_x, mid_y) = (p.resolution_x as f64 / 2.0, p.resolution_y as f64 / 2.0);
    let mut rects: Vec<PixelRect> = PADS
        .iter()
        .map(|r| {
            // Rounding the half-width keeps each pad symmetric about its centre.
            let hw = (r.w / 2.0 / px).round();
            let hh = (r.h / 2.0 / py).round();
            let cx = mid_x + r.cx / px;
            let cy = mid_y + r.cy / py;
            PixelRect {
                x0: (cx - hw).round() as u32,
                x1: (cx + hw).round() as u32,
                y0: (cy - hh).round() as u32,
                y1: (cy + hh).round() as u32,
            }
        })
        .collect();
    rects.sort_by_key(|r| r.x0);
    rects
}

/// Encodes one layer of the pattern, streaming runs row by row.
fn encode_layer(p: &Printer, rects: &[PixelRect]) -> Vec<u8> {
    let width = p.resolution_x as u64;
    let mut enc = LayerEncoder::new();
    for y in 0..p.resolution_y as u32 {
        let mut x = 0u64;
        for r in rects.iter().filter(|r| y >= r.y0 && y < r.y1) {
            enc.push_run(0x00, r.x0 as u64 - x);
            enc.push_run(0xFF, (r.x1 - r.x0) as u64);
            x = r.x1 as u64;
        }
        enc.push_run(0x00, width - x);
    }
    enc.finish()
}

fn preview(size: (u16, u16), p: &Printer) -> Preview {
    let bg = rgb565(24, 24, 32);
    let fg = rgb565(230, 120, 40);
    let s = size.0 as f64;
    // Fit the plate into the square thumbnail, keeping its aspect ratio.
    let scale = s / p.size_x as f64;
    let off_y = (s - p.size_y as f64 * scale) / 2.0;
    Preview::from_fn(size, |x, y| {
        let mx = (x as f64 + 0.5) / scale - p.size_x as f64 / 2.0;
        let my = (y as f64 + 0.5 - off_y) / scale - p.size_y as f64 / 2.0;
        let hit = PADS
            .iter()
            .any(|r| (mx - r.cx).abs() <= r.w / 2.0 && (my - r.cy).abs() <= r.h / 2.0);
        if hit {
            fg
        } else {
            bg
        }
    })
}

pub fn build_header(p: &Printer, s: &Settings, layer_count: u32, timestamp: &str) -> Header {
    // Tilting-vat movement fields: UVtools' Saturn 4 Ultra 16K profile sets
    // lift height and speeds to 0.05, apparently as placeholders the firmware
    // ignores in favour of its tilt modes. Copied here until a real ELEGOO
    // file shows what SatelLite writes.
    let (lift, speed) = if p.tilting_vat {
        (0.05, 0.05)
    } else {
        (6.0, 60.0)
    };
    let normal_layers = layer_count.saturating_sub(s.bottom_layers);
    let volume: f64 = PADS.iter().map(|r| r.w * r.h).sum::<f64>() * s.height_mm as f64;
    // Rough: exposure plus about 5 s per tilt ("fast" tilt time in the profile).
    let print_time = s.bottom_layers as f32 * s.bottom_exposure_s
        + normal_layers as f32 * s.exposure_s
        + layer_count as f32 * 5.0;
    Header {
        version: "V3.0".into(),
        software_info: "FruitSlicer".into(),
        software_version: env!("CARGO_PKG_VERSION").into(),
        file_time: timestamp.into(),
        printer_name: p.name.into(),
        printer_type: "DLP".into(),
        profile_name: "FruitSlicer test pattern".into(),
        anti_aliasing_level: 1,
        grey_level: 0,
        blur_level: 0,
        small_preview: preview(Preview::SMALL, p),
        big_preview: preview(Preview::BIG, p),
        layer_count,
        resolution_x: p.resolution_x,
        resolution_y: p.resolution_y,
        mirror_x: p.mirror_x,
        mirror_y: false,
        size_x: p.size_x,
        size_y: p.size_y,
        size_z: p.size_z,
        layer_thickness: s.layer_mm,
        exposure_time: s.exposure_s,
        delay_mode: DelayMode::StaticTime,
        turn_off_time: 0.0,
        bottom_before_lift_time: 0.0,
        bottom_after_lift_time: 0.0,
        bottom_after_retract_time: 0.0,
        before_lift_time: 0.0,
        after_lift_time: 0.0,
        after_retract_time: 0.0,
        bottom_exposure_time: s.bottom_exposure_s,
        bottom_layers: s.bottom_layers,
        bottom_lift_distance: lift,
        bottom_lift_speed: speed,
        lift_distance: lift,
        lift_speed: speed,
        bottom_retract_distance: lift,
        bottom_retract_speed: speed,
        retract_distance: lift,
        retract_speed: speed,
        bottom_lift_distance_2: 0.0,
        bottom_lift_speed_2: 0.0,
        lift_distance_2: 0.0,
        lift_speed_2: 0.0,
        bottom_retract_distance_2: 0.0,
        bottom_retract_speed_2: 0.0,
        retract_distance_2: 0.0,
        retract_speed_2: 0.0,
        bottom_light_pwm: 255,
        light_pwm: 255,
        advance_mode: false,
        printing_time: print_time as u32,
        total_volume: volume as f32,
        total_weight: (volume / 1000.0 * 1.1) as f32, // ~1.1 g/ml, typical resin
        total_price: 0.0,
        price_unit: "$".into(),
        layer_content_offset: HEADER_SIZE,
        grey_scale_level: true,
        transition_layers: 0,
    }
}

/// Writes the test pattern. Every layer has the same image, so it is encoded
/// once and reused.
pub fn write(
    out: impl Write,
    p: &Printer,
    s: &Settings,
    timestamp: &str,
) -> Result<u32, goo::Error> {
    let layer_count = (s.height_mm / s.layer_mm).round() as u32;
    if layer_count == 0 || layer_count < s.bottom_layers {
        return Err(goo::Error::Invalid(
            "height must cover at least the bottom layers".into(),
        ));
    }
    let header = build_header(p, s, layer_count, timestamp);
    let data = encode_layer(p, &pixel_rects(p));
    let mut w = GooWriter::new(out, &header)?;
    for i in 0..layer_count {
        let bottom = i < s.bottom_layers;
        let def = LayerDef {
            pause: false,
            pause_position_z: p.size_z,
            position_z: (i + 1) as f32 * s.layer_mm,
            exposure_time: if bottom {
                s.bottom_exposure_s
            } else {
                s.exposure_s
            },
            off_time: 0.0,
            before_lift_time: 0.0,
            after_lift_time: 0.0,
            after_retract_time: 0.0,
            lift_distance: header.lift_distance,
            lift_speed: header.lift_speed,
            lift_distance_2: 0.0,
            lift_speed_2: 0.0,
            retract_distance: header.retract_distance,
            retract_speed: header.retract_speed,
            retract_distance_2: 0.0,
            retract_speed_2: 0.0,
            light_pwm: if bottom {
                header.bottom_light_pwm
            } else {
                header.light_pwm
            },
        };
        w.write_layer(&def, &data)?;
    }
    w.finish()?;
    Ok(layer_count)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::printer::SATURN_4_ULTRA_16K;

    #[test]
    fn centre_square_is_20mm_on_both_axes() {
        let p = SATURN_4_ULTRA_16K;
        let (px, py) = p.pixel_mm();
        let r = pixel_rects(&p)
            .into_iter()
            .find(|r| r.x1 - r.x0 > 1000)
            .unwrap();
        let w_mm = (r.x1 - r.x0) as f64 * px;
        let h_mm = (r.y1 - r.y0) as f64 * py;
        assert!((w_mm - 20.0).abs() < px, "width {w_mm}");
        assert!((h_mm - 20.0).abs() < py, "height {h_mm}");
    }

    #[test]
    fn pads_fit_on_the_lcd() {
        let p = SATURN_4_ULTRA_16K;
        for r in pixel_rects(&p) {
            assert!(r.x1 <= p.resolution_x as u32 && r.y1 <= p.resolution_y as u32);
        }
    }
}
