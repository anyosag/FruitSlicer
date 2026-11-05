//! Cutting a mesh at a height and filling the cross-section into LCD pixels.
//!
//! Each triangle that crosses the cutting plane contributes one segment.
//! Segments are oriented from the triangle's winding so the outline runs
//! counter-clockwise around solid material (seen from above). Each pixel row
//! is then scanned at its centre with the non-zero winding rule, which
//! handles holes and overlapping shells and tolerates small mesh defects
//! better than even-odd filling.
//!
//! A pixel is lit when its centre lies inside the cross-section.

use crate::mesh::{Triangle, Vec3};

/// Maps plate millimetres (origin at the plate centre) to pixel coordinates
/// (origin at the LCD corner, one unit per pixel).
#[derive(Debug, Clone, Copy)]
pub struct PixelGrid {
    pub width: u32,
    pub height: u32,
    pub mm_per_px_x: f64,
    pub mm_per_px_y: f64,
}

impl PixelGrid {
    fn to_px(self, x: f64, y: f64) -> (f64, f64) {
        (
            x / self.mm_per_px_x + self.width as f64 / 2.0,
            y / self.mm_per_px_y + self.height as f64 / 2.0,
        )
    }
}

/// A 2D segment in pixel space, directed so solid material is on its left.
#[derive(Debug, Clone, Copy)]
struct Segment {
    x0: f64,
    y0: f64,
    x1: f64,
    y1: f64,
}

/// Lit spans of one layer: for each row, sorted, non-overlapping `[x0, x1)`
/// pixel ranges. Rows are in LCD order and not mirrored.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct LayerSpans {
    pub rows: Vec<Vec<(u32, u32)>>,
}

impl LayerSpans {
    pub fn lit_pixels(&self) -> u64 {
        self.rows
            .iter()
            .flatten()
            .map(|&(a, b)| (b - a) as u64)
            .sum()
    }

    pub fn is_empty(&self) -> bool {
        self.rows.iter().all(Vec::is_empty)
    }
}

/// Cuts `tris` at height `z` and fills the result onto `grid`.
pub fn slice_layer<'a>(
    tris: impl IntoIterator<Item = &'a Triangle>,
    z: f64,
    grid: &PixelGrid,
) -> LayerSpans {
    let mut crossings: Vec<Vec<(f64, i32)>> = vec![Vec::new(); grid.height as usize];
    for t in tris {
        if let Some(s) = cut(t, z, grid) {
            add_crossings(&s, grid.height, &mut crossings);
        }
    }
    let rows = crossings
        .into_iter()
        .map(|mut row| fill_row(&mut row, grid.width))
        .collect();
    LayerSpans { rows }
}

/// Intersects one triangle with the plane at `z`. Vertices exactly on the
/// plane count as below it, so a solid spanning `[z0, z1]` is cut for
/// `z0 <= z < z1` and shared edges are never counted twice.
fn cut(t: &Triangle, z: f64, grid: &PixelGrid) -> Option<Segment> {
    let d: [f64; 3] = [t[0][2] as f64 - z, t[1][2] as f64 - z, t[2][2] as f64 - z];
    let above = [d[0] > 0.0, d[1] > 0.0, d[2] > 0.0];
    let n_above = above.iter().filter(|&&a| a).count();
    if n_above == 0 || n_above == 3 {
        return None;
    }
    // The vertex alone on its side of the plane.
    let lone = (0..3).find(|&i| above[i] == (n_above == 1)).unwrap();
    let a = (lone + 1) % 3;
    let b = (lone + 2) % 3;
    let lerp = |i: usize, j: usize| -> (f64, f64) {
        let s = d[i] / (d[i] - d[j]);
        let p = |k: usize| t[i][k] as f64 + (t[j][k] as f64 - t[i][k] as f64) * s;
        (p(0), p(1))
    };
    let (pa, pb) = (lerp(lone, a), lerp(lone, b));
    let (x0, y0) = grid.to_px(pa.0, pa.1);
    let (x1, y1) = grid.to_px(pb.0, pb.1);
    let mut s = Segment { x0, y0, x1, y1 };

    // Orient by the triangle's normal: its XY part points out of the solid,
    // which must be on the segment's right for a counter-clockwise outline.
    let n = normal(t);
    let (dx, dy) = (s.x1 - s.x0, s.y1 - s.y0);
    // Right-hand perpendicular of (dx, dy) is (dy, -dx). Pixel axes are mm
    // axes scaled by positive factors, so the sign test is unaffected.
    if dy * (n[0] as f64) - dx * (n[1] as f64) < 0.0 {
        std::mem::swap(&mut s.x0, &mut s.x1);
        std::mem::swap(&mut s.y0, &mut s.y1);
    }
    Some(s)
}

fn normal(t: &Triangle) -> Vec3 {
    let u = [t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]];
    let v = [t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]];
    [
        u[1] * v[2] - u[2] * v[1],
        u[2] * v[0] - u[0] * v[2],
        u[0] * v[1] - u[1] * v[0],
    ]
}

/// Records where the segment crosses each row centre (`row + 0.5`), with +1
/// for upward and -1 for downward segments. Half-open in y, so a vertex on a
/// row centre is counted once.
fn add_crossings(s: &Segment, height: u32, rows: &mut [Vec<(f64, i32)>]) {
    let (lo, hi, dir) = if s.y0 < s.y1 {
        (s.y0, s.y1, 1)
    } else {
        (s.y1, s.y0, -1)
    };
    if lo == hi {
        return;
    }
    let first = (lo - 0.5).ceil().max(0.0) as i64;
    let last = ((hi - 0.5).ceil() as i64 - 1).min(height as i64 - 1);
    for r in first..=last {
        let yc = r as f64 + 0.5;
        let x = s.x0 + (s.x1 - s.x0) * (yc - s.y0) / (s.y1 - s.y0);
        rows[r as usize].push((x, dir));
    }
}

/// Turns one row's crossings into lit `[x0, x1)` pixel spans.
fn fill_row(row: &mut [(f64, i32)], width: u32) -> Vec<(u32, u32)> {
    if row.is_empty() {
        return Vec::new();
    }
    row.sort_by(|a, b| a.0.total_cmp(&b.0));
    let mut spans: Vec<(u32, u32)> = Vec::new();
    let mut winding = 0;
    let mut start = 0.0;
    for &(x, dir) in row.iter() {
        let was_inside = winding != 0;
        winding += dir;
        let inside = winding != 0;
        if !was_inside && inside {
            start = x;
        } else if was_inside && !inside {
            // Pixel c is lit when its centre c + 0.5 lies in [start, x).
            let c0 = (start - 0.5).ceil().clamp(0.0, width as f64) as u32;
            let c1 = (x - 0.5).ceil().clamp(0.0, width as f64) as u32;
            if c1 > c0 {
                match spans.last_mut() {
                    Some(last) if last.1 >= c0 => last.1 = last.1.max(c1),
                    _ => spans.push((c0, c1)),
                }
            }
        }
    }
    spans
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mesh::Mesh;

    fn grid() -> PixelGrid {
        // 1 mm = 10 px in x, 5 px in y, 200 x 100 px.
        PixelGrid {
            width: 200,
            height: 100,
            mm_per_px_x: 0.1,
            mm_per_px_y: 0.2,
        }
    }

    #[test]
    fn square_fills_expected_pixels() {
        let m = Mesh::cuboid([-2.0, -2.0, 0.0], [2.0, 2.0, 1.0]);
        let s = slice_layer(&m.triangles, 0.5, &grid());
        // 4 mm = 40 px wide, 20 rows tall, centred.
        assert_eq!(s.lit_pixels(), 40 * 20);
        assert_eq!(s.rows[50], vec![(80, 120)]);
        assert!(s.rows[39].is_empty() && s.rows[60].is_empty());
        assert_eq!(s.rows[40], vec![(80, 120)]);
    }

    #[test]
    fn inverted_winding_still_fills() {
        let mut m = Mesh::cuboid([-2.0, -2.0, 0.0], [2.0, 2.0, 1.0]);
        for t in &mut m.triangles {
            t.swap(1, 2);
        }
        assert_eq!(slice_layer(&m.triangles, 0.5, &grid()).lit_pixels(), 800);
    }

    #[test]
    fn hole_is_left_empty() {
        // An outer 8x8 mm square tube with a 4x4 mm hole, built from four bars.
        let mut m = Mesh::cuboid([-4.0, -4.0, 0.0], [4.0, -2.0, 1.0]);
        m.append(Mesh::cuboid([-4.0, 2.0, 0.0], [4.0, 4.0, 1.0]));
        m.append(Mesh::cuboid([-4.0, -2.0, 0.0], [-2.0, 2.0, 1.0]));
        m.append(Mesh::cuboid([2.0, -2.0, 0.0], [4.0, 2.0, 1.0]));
        let s = slice_layer(&m.triangles, 0.5, &grid());
        assert_eq!(s.rows[50], vec![(60, 80), (120, 140)]);
        assert_eq!(s.lit_pixels(), 80 * 40 - 40 * 20);
    }

    #[test]
    fn overlapping_shells_union() {
        let mut m = Mesh::cuboid([-2.0, -2.0, 0.0], [2.0, 2.0, 1.0]);
        m.append(Mesh::cuboid([0.0, -2.0, 0.0], [4.0, 2.0, 1.0]));
        let s = slice_layer(&m.triangles, 0.5, &grid());
        assert_eq!(s.rows[50], vec![(80, 140)]);
    }

    #[test]
    fn plane_through_vertices_is_not_double_counted() {
        let m = Mesh::cuboid([-2.0, -2.0, 0.0], [2.0, 2.0, 1.0]);
        // A solid spanning [0, 1] is cut at its bottom face but not its top.
        assert_eq!(slice_layer(&m.triangles, 0.0, &grid()).lit_pixels(), 800);
        assert!(slice_layer(&m.triangles, 1.0, &grid()).is_empty());
    }

    #[test]
    fn clips_to_lcd() {
        let m = Mesh::cuboid([-50.0, -50.0, 0.0], [50.0, 50.0, 1.0]);
        let s = slice_layer(&m.triangles, 0.5, &grid());
        assert_eq!(s.lit_pixels(), 200 * 100);
    }
}
