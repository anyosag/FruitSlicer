//! Top-down preview thumbnails, rendered straight from the mesh.

use goo::{rgb565, Preview};

use crate::mesh::Mesh;
use crate::printer::Printer;

/// Renders an orthographic top view of the plate: the model shaded by
/// height and surface slope on a dark background, with the plate outline.
pub fn render(mesh: &Mesh, printer: &Printer, size: (u16, u16)) -> Preview {
    let (w, h) = (size.0 as usize, size.1 as usize);
    // Fit the plate inside the square image, keeping its aspect ratio.
    let scale = (w as f64 / printer.size_x as f64).min(h as f64 / printer.size_y as f64);
    let (ox, oy) = (w as f64 / 2.0, h as f64 / 2.0);
    let to_img = |x: f32, y: f32| (ox + x as f64 * scale, oy - y as f64 * scale);

    let mut depth = vec![f64::NEG_INFINITY; w * h];
    let mut shade = vec![0f64; w * h];
    let zmax = mesh.bounds().map(|b| b.max[2].max(1e-3)).unwrap_or(1.0) as f64;

    for t in &mesh.triangles {
        let p: Vec<(f64, f64, f64)> = t
            .iter()
            .map(|v| {
                let (x, y) = to_img(v[0], v[1]);
                (x, y, v[2] as f64)
            })
            .collect();
        // Light from the normal's Z component (flat tops brightest).
        let u = [t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]];
        let v = [t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]];
        let n = [
            (u[1] * v[2] - u[2] * v[1]) as f64,
            (u[2] * v[0] - u[0] * v[2]) as f64,
            (u[0] * v[1] - u[1] * v[0]) as f64,
        ];
        let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
        if len == 0.0 {
            continue;
        }
        let light = (n[2] / len).abs();

        let area = (p[1].0 - p[0].0) * (p[2].1 - p[0].1) - (p[2].0 - p[0].0) * (p[1].1 - p[0].1);
        if area.abs() < 1e-12 {
            continue;
        }
        let xmin = p
            .iter()
            .map(|q| q.0)
            .fold(f64::INFINITY, f64::min)
            .floor()
            .max(0.0) as usize;
        let xmax = p
            .iter()
            .map(|q| q.0)
            .fold(f64::NEG_INFINITY, f64::max)
            .ceil()
            .min(w as f64) as usize;
        let ymin = p
            .iter()
            .map(|q| q.1)
            .fold(f64::INFINITY, f64::min)
            .floor()
            .max(0.0) as usize;
        let ymax = p
            .iter()
            .map(|q| q.1)
            .fold(f64::NEG_INFINITY, f64::max)
            .ceil()
            .min(h as f64) as usize;
        for py in ymin..ymax {
            for px in xmin..xmax {
                let (cx, cy) = (px as f64 + 0.5, py as f64 + 0.5);
                let w0 = ((p[1].0 - cx) * (p[2].1 - cy) - (p[2].0 - cx) * (p[1].1 - cy)) / area;
                let w1 = ((p[2].0 - cx) * (p[0].1 - cy) - (p[0].0 - cx) * (p[2].1 - cy)) / area;
                let w2 = 1.0 - w0 - w1;
                if w0 < 0.0 || w1 < 0.0 || w2 < 0.0 {
                    continue;
                }
                let z = w0 * p[0].2 + w1 * p[1].2 + w2 * p[2].2;
                let i = py * w + px;
                if z > depth[i] {
                    depth[i] = z;
                    shade[i] = 0.35 + 0.35 * light + 0.3 * (z / zmax);
                }
            }
        }
    }

    let (px0, px1) = (
        ox - printer.size_x as f64 / 2.0 * scale,
        ox + printer.size_x as f64 / 2.0 * scale,
    );
    let (py0, py1) = (
        oy - printer.size_y as f64 / 2.0 * scale,
        oy + printer.size_y as f64 / 2.0 * scale,
    );
    Preview::from_fn(size, |x, y| {
        let i = y as usize * w + x as usize;
        if depth[i].is_finite() {
            let s = shade[i].clamp(0.0, 1.0);
            rgb565((240.0 * s) as u8, (130.0 * s) as u8, (50.0 * s) as u8)
        } else {
            let (fx, fy) = (x as f64 + 0.5, y as f64 + 0.5);
            let on_plate = fx >= px0 && fx <= px1 && fy >= py0 && fy <= py1;
            let edge =
                on_plate && (fx - px0 < 1.0 || px1 - fx < 1.0 || fy - py0 < 1.0 || py1 - fy < 1.0);
            match (on_plate, edge) {
                (_, true) => rgb565(90, 90, 110),
                (true, false) => rgb565(36, 36, 48),
                _ => rgb565(18, 18, 24),
            }
        }
    })
}
