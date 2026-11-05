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

/// Grey pixel runs for one row: sorted, non-overlapping `(x0, x1, value)`
/// with value > 0.
pub type GreyRow = Vec<(u32, u32, u8)>;

/// Anti-aliased version of [`slice_layer`]: each pixel's grey level is the
/// fraction of it covered by the cross-section, estimated exactly along X
/// and with `samples` sub-rows along Y.
pub fn slice_layer_grey<'a>(
    tris: impl IntoIterator<Item = &'a Triangle>,
    z: f64,
    grid: &PixelGrid,
    samples: u32,
) -> Vec<GreyRow> {
    let s = samples.max(1) as usize;
    let h = grid.height as usize;
    // Crossings per sub-row: sub-row j of row r is at y = r + (j + 0.5) / s.
    let mut crossings: Vec<Vec<(f64, i32)>> = vec![Vec::new(); h * s];
    for t in tris {
        let Some(seg) = cut(t, z, grid) else { continue };
        let (lo, hi, dir) = if seg.y0 < seg.y1 {
            (seg.y0, seg.y1, 1)
        } else {
            (seg.y1, seg.y0, -1)
        };
        if lo == hi {
            continue;
        }
        let sf = s as f64;
        let first = ((lo * sf - 0.5).ceil().max(0.0)) as i64;
        let last = (((hi * sf - 0.5).ceil()) as i64 - 1).min((h * s) as i64 - 1);
        for k in first..=last {
            let yc = (k as f64 + 0.5) / sf;
            let x = seg.x0 + (seg.x1 - seg.x0) * (yc - seg.y0) / (seg.y1 - seg.y0);
            crossings[k as usize].push((x, dir));
        }
    }
    let width = grid.width as f64;
    let mut rows = Vec::with_capacity(h);
    let mut events: Vec<(f64, i32)> = Vec::new();
    for r in 0..h {
        events.clear();
        for sub in &mut crossings[r * s..(r + 1) * s] {
            if sub.is_empty() {
                continue;
            }
            sub.sort_by(|a, b| a.0.total_cmp(&b.0));
            let mut winding = 0;
            let mut start = 0.0;
            for &(x, dir) in sub.iter() {
                let was = winding != 0;
                winding += dir;
                let now = winding != 0;
                if !was && now {
                    start = x;
                } else if was && !now {
                    let (a, b) = (start.clamp(0.0, width), x.clamp(0.0, width));
                    if b > a {
                        events.push((a, 1));
                        events.push((b, -1));
                    }
                }
            }
        }
        rows.push(coverage_runs(&mut events, s as u32, grid.width));
    }
    rows
}

/// Turns interval endpoints (+1 at a start, -1 at an end, from `s`
/// sub-rows) into grey runs.
fn coverage_runs(events: &mut [(f64, i32)], s: u32, width: u32) -> GreyRow {
    let mut out: GreyRow = Vec::new();
    if events.is_empty() {
        return out;
    }
    events.sort_by(|a, b| a.0.total_cmp(&b.0));
    let full = s as f64;
    let push = |a: u32, b: u32, cov: f64, out: &mut GreyRow| {
        let v = ((cov / full) * 255.0).round().clamp(0.0, 255.0) as u8;
        if v == 0 || a >= b {
            return;
        }
        match out.last_mut() {
            Some(l) if l.1 == a && l.2 == v => l.1 = b,
            _ => out.push((a, b, v)),
        }
    };
    let mut level = 0i32;
    let mut x = events[0].0;
    // The pixel being accumulated, and its covered area (in sub-row units).
    let mut pix = x.floor() as u32;
    let mut acc = 0.0f64;
    for &(ex, d) in events.iter() {
        // Constant `level` from x to ex.
        while x < ex {
            let c = x.floor() as u32;
            if c != pix {
                push(pix, pix + 1, acc, &mut out);
                pix = c;
                acc = 0.0;
            }
            let next = (c as f64 + 1.0).min(ex);
            if x == c as f64 && ex >= c as f64 + 1.0 {
                // Whole pixels at this level.
                let end = (ex.floor() as u32).min(width);
                push(c, end, level as f64, &mut out);
                x = end as f64;
                pix = end;
                acc = 0.0;
                continue;
            }
            acc += (next - x) * level as f64;
            x = next;
        }
        level += d;
    }
    if pix < width {
        push(pix, pix + 1, acc, &mut out);
    }
    out
}

/// Grey runs minus binary spans (for hollowing an anti-aliased layer).
pub fn grey_subtract(row: &GreyRow, cut: &[(u32, u32)]) -> GreyRow {
    if cut.is_empty() {
        return row.clone();
    }
    let mut out = Vec::with_capacity(row.len());
    for &(a, b, v) in row {
        let plain: Vec<(u32, u32)> = crate::hollow::subtract(&[(a, b)], cut);
        out.extend(plain.into_iter().map(|(x, y)| (x, y, v)));
    }
    out
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
    fn grey_square_edges_have_partial_coverage() {
        // x from -7.75 to -5.75 mm is 22.5 .. 42.5 px; y from -9.0 to -6.9 mm
        // is rows 5.0 .. 15.5 (0.2 mm rows, plate centred).
        let m = Mesh::cuboid(
            [2.25 - 10.0, 1.0 - 10.0, 0.0],
            [4.25 - 10.0, 3.1 - 10.0, 1.0],
        );
        let g = slice_layer_grey(&m.triangles, 0.5, &grid(), 4);
        // Rows fully inside: 22.5 -> half pixel 22, full 23..42, half 42.
        let row = &g[10];
        assert_eq!(row, &vec![(22, 23, 128), (23, 42, 255), (42, 43, 128)]);
        // Top row 15 is covered from 15.0 to 15.5: 2 of 4 sub-rows.
        let top = &g[15];
        assert_eq!(top, &vec![(22, 23, 64), (23, 42, 128), (42, 43, 64)]);
        assert!(g[16].is_empty() && g[4].is_empty());
        // Total coverage matches the area: 20 px x 10.5 rows.
        let lit: f64 = g
            .iter()
            .flatten()
            .map(|&(a, b, v)| (b - a) as f64 * v as f64 / 255.0)
            .sum();
        assert!((lit - 20.0 * 10.5).abs() < 0.5, "{lit}");
    }

    #[test]
    fn clips_to_lcd() {
        let m = Mesh::cuboid([-50.0, -50.0, 0.0], [50.0, 50.0, 1.0]);
        let s = slice_layer(&m.triangles, 0.5, &grid());
        assert_eq!(s.lit_pixels(), 200 * 100);
    }
}
