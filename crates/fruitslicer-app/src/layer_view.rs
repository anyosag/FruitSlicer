//! Turning one .goo layer into an image for the layer viewer.

use goo::{rle, Header};

/// The part of the plate an image covers, in LCD pixels (not mirrored):
/// `[x0, x1) x [y0, y1)`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PixelRect {
    pub x0: u32,
    pub y0: u32,
    pub x1: u32,
    pub y1: u32,
}

impl PixelRect {
    pub fn full(h: &Header) -> Self {
        Self {
            x0: 0,
            y0: 0,
            x1: h.resolution_x as u32,
            y1: h.resolution_y as u32,
        }
    }

    /// The pixels covering plate X/Y `min..max` (mm, origin at the plate
    /// centre), grown by `margin` mm and clamped to the LCD.
    pub fn around_mm(h: &Header, min: [f32; 2], max: [f32; 2], margin: f32) -> Self {
        let (w, hh) = (h.resolution_x as f32, h.resolution_y as f32);
        let px = |x: f32| x / (h.size_x / w) + w / 2.0;
        let py = |y: f32| y / (h.size_y / hh) + hh / 2.0;
        let c = |v: f32, max: f32| v.clamp(0.0, max) as u32;
        Self {
            x0: c(px(min[0] - margin).floor(), w),
            y0: c(py(min[1] - margin).floor(), hh),
            x1: c(px(max[0] + margin).ceil(), w),
            y1: c(py(max[1] + margin).ceil(), hh),
        }
    }
}

/// A greyscale image of part of a layer, seen from above the plate: +X to
/// the right and +Y up, with the file's X mirroring undone.
pub struct LayerImage {
    pub width: usize,
    pub height: usize,
    /// Coverage 0..=255 per output pixel, row 0 at the top (max plate Y).
    pub pixels: Vec<u8>,
    pub rect: PixelRect,
}

/// Decodes `data` (one layer) into an image of `rect`, averaging blocks of
/// `step` LCD pixels so the result is at most about `max_px` wide or tall.
pub fn render(h: &Header, data: &[u8], rect: PixelRect, max_px: u32) -> Result<LayerImage, String> {
    let w = h.resolution_x as u64;
    let (rw, rh) = (rect.x1 - rect.x0, rect.y1 - rect.y0);
    // Keep output pixels roughly square in mm: the 16K's pixels are
    // 14 x 19 um, so step more in X than in Y.
    let (mx, my) = (
        h.size_x as f64 / w as f64,
        h.size_y as f64 / h.resolution_y as f64,
    );
    let base = ((rw.max(1) as f64 * mx).max(rh.max(1) as f64 * my) / max_px as f64).max(mx.min(my));
    let sx = ((base / mx).round() as u32).max(1);
    let sy = ((base / my).round() as u32).max(1);
    let (ow, oh) = (rw.div_ceil(sx) as usize, rh.div_ceil(sy) as usize);
    let mut acc = vec![0u32; ow.max(1) * oh.max(1)];
    let mut pos = 0u64;
    rle::decode_runs(data, |v, len| {
        if v != 0 {
            let (mut p, end) = (pos, pos + len);
            while p < end {
                let (y, x) = ((p / w) as u32, (p % w) as u32);
                let row_end = ((y as u64 + 1) * w).min(end);
                let x_end = x + (row_end - p) as u32;
                // LCD x as plate x: undo the mirror.
                let (a, b) = if h.mirror_x {
                    (w as u32 - x_end, w as u32 - x)
                } else {
                    (x, x_end)
                };
                if y >= rect.y0 && y < rect.y1 {
                    let (a, b) = (a.max(rect.x0), b.min(rect.x1));
                    if a < b {
                        let oy = (rect.y1 - 1 - y) / sy; // flip so +Y is up
                        let row = &mut acc[oy as usize * ow..(oy as usize + 1) * ow];
                        let mut c = a;
                        while c < b {
                            let ox = (c - rect.x0) / sx;
                            let cell_end = (rect.x0 + (ox + 1) * sx).min(b);
                            row[ox as usize] += (cell_end - c) * v as u32;
                            c = cell_end;
                        }
                    }
                }
                p = row_end;
            }
        }
        pos += len;
    })
    .map_err(|e| e.to_string())?;
    let full = (sx * sy * 255) as f32;
    let pixels = acc
        .iter()
        .map(|&a| ((a as f32 / full) * 255.0).round().min(255.0) as u8)
        .collect();
    Ok(LayerImage {
        width: ow,
        height: oh,
        pixels,
        rect,
    })
}

impl LayerImage {
    /// Plate mm of an image point given as a fraction (0..1) of its size.
    pub fn uv_to_mm(&self, h: &Header, u: f32, v: f32) -> [f32; 2] {
        let mx = h.size_x / h.resolution_x as f32;
        let my = h.size_y / h.resolution_y as f32;
        let px = self.rect.x0 as f32 + u * (self.rect.x1 - self.rect.x0) as f32;
        let py = self.rect.y1 as f32 - v * (self.rect.y1 - self.rect.y0) as f32;
        [px * mx - h.size_x / 2.0, py * my - h.size_y / 2.0]
    }

    /// The image fraction (0..1) of a plate point.
    pub fn mm_to_uv(&self, h: &Header, p: [f32; 2]) -> [f32; 2] {
        let mx = h.size_x / h.resolution_x as f32;
        let my = h.size_y / h.resolution_y as f32;
        let px = (p[0] + h.size_x / 2.0) / mx;
        let py = (p[1] + h.size_y / 2.0) / my;
        [
            (px - self.rect.x0 as f32) / (self.rect.x1 - self.rect.x0) as f32,
            (self.rect.y1 as f32 - py) / (self.rect.y1 - self.rect.y0) as f32,
        ]
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use slicer::{slice_to_goo, Mesh, PrintSettings, SATURN_4_ULTRA_16K};

    #[test]
    fn layer_image_shows_the_square_where_it_is_on_the_plate() {
        // A 10 x 10 mm block centred at (+30, +20) mm.
        let m = Mesh::cuboid([25.0, 15.0, 0.0], [35.0, 25.0, 0.2]);
        let mut out = Vec::new();
        slice_to_goo(
            &m,
            &SATURN_4_ULTRA_16K,
            &PrintSettings::default(),
            "t",
            "t",
            &mut out,
            2,
        )
        .unwrap();
        let goo = goo::GooFile::read(out.as_slice()).unwrap();
        assert!(goo.header.mirror_x);
        let h = &goo.header;
        let img = render(h, &goo.layers[0].1, PixelRect::full(h), 1000).unwrap();
        // Find the lit bounding box in image fractions.
        let (mut u0, mut u1, mut v0, mut v1) = (1.0f32, 0.0f32, 1.0f32, 0.0f32);
        for y in 0..img.height {
            for x in 0..img.width {
                if img.pixels[y * img.width + x] > 128 {
                    let (u, v) = (x as f32 / img.width as f32, y as f32 / img.height as f32);
                    u0 = u0.min(u);
                    u1 = u1.max(u);
                    v0 = v0.min(v);
                    v1 = v1.max(v);
                }
            }
        }
        let a = img.uv_to_mm(h, u0, v1);
        let b = img.uv_to_mm(h, u1, v0);
        let centre = [(a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0];
        assert!((centre[0] - 30.0).abs() < 0.5, "{centre:?}");
        assert!((centre[1] - 20.0).abs() < 0.5, "{centre:?}");
        let uv = img.mm_to_uv(h, [30.0, 20.0]);
        let back = img.uv_to_mm(h, uv[0], uv[1]);
        assert!((back[0] - 30.0).abs() < 1e-3 && (back[1] - 20.0).abs() < 1e-3);
    }
}
