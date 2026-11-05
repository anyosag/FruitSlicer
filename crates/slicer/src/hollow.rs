//! Hollowing and drain holes, done on the sliced layers.
//!
//! The interior of layer `k` is every pixel that stays solid when layers
//! `k-n ..= k+n` are each eroded by the wall thickness in X and Y (a box, so
//! walls are at least `wall_mm` thick in every direction and up to about
//! 1.4x that on diagonals and slopes). Layers outside the model count as
//! empty, so the top and bottom also get a wall.
//!
//! Drain holes: wherever a new hollow begins (interior in layer `k` with no
//! interior directly below it in layer `k-1`), a round hole is cut down
//! through the floor beneath it, so the hollow opens towards the plate side.
//! That vents it while printing (no suction cup) and lets resin drain out
//! afterwards (no trapped resin).

use std::collections::VecDeque;

/// Hollowing settings.
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(default))]
pub struct HollowParams {
    pub wall_mm: f32,
    pub drain_holes: bool,
    pub hole_diameter_mm: f32,
    /// Minimum distance between two drain holes in the same hollow.
    pub hole_spacing_mm: f32,
}

impl Default for HollowParams {
    fn default() -> Self {
        Self {
            wall_mm: 2.0,
            drain_holes: true,
            hole_diameter_mm: 2.5,
            hole_spacing_mm: 15.0,
        }
    }
}

/// Lit spans per row, as in [`crate::raster::LayerSpans`].
pub type Rows = Vec<Vec<(u32, u32)>>;

pub fn intersect(a: &[(u32, u32)], b: &[(u32, u32)], out: &mut Vec<(u32, u32)>) {
    out.clear();
    let (mut i, mut j) = (0, 0);
    while i < a.len() && j < b.len() {
        let lo = a[i].0.max(b[j].0);
        let hi = a[i].1.min(b[j].1);
        if lo < hi {
            out.push((lo, hi));
        }
        if a[i].1 < b[j].1 {
            i += 1;
        } else {
            j += 1;
        }
    }
}

pub fn subtract(a: &[(u32, u32)], b: &[(u32, u32)]) -> Vec<(u32, u32)> {
    let mut out = Vec::with_capacity(a.len());
    let mut j = 0;
    for &(mut lo, hi) in a {
        while j < b.len() && b[j].1 <= lo {
            j += 1;
        }
        let mut k = j;
        while k < b.len() && b[k].0 < hi {
            if b[k].0 > lo {
                out.push((lo, b[k].0));
            }
            lo = lo.max(b[k].1);
            k += 1;
        }
        if lo < hi {
            out.push((lo, hi));
        }
    }
    out
}

fn rows_intersect(a: &Rows, b: &Rows) -> Rows {
    let mut tmp = Vec::new();
    a.iter()
        .zip(b)
        .map(|(x, y)| {
            intersect(x, y, &mut tmp);
            tmp.clone()
        })
        .collect()
}

/// Erodes a layer by a box: `kx` pixels in X and `ky` rows in Y.
pub fn erode(rows: &Rows, kx: u32, ky: usize) -> Rows {
    let n = rows.len();
    // Shrink in X.
    let s: Rows = rows
        .iter()
        .map(|r| {
            r.iter()
                .filter_map(|&(a, b)| {
                    let (a, b) = (a + kx, b.saturating_sub(kx));
                    (a < b).then_some((a, b))
                })
                .collect()
        })
        .collect();
    if ky == 0 {
        return s;
    }
    // Sliding intersection over 2ky+1 rows (van Herk / Gil-Werman).
    // Rows outside the layer are empty, so the first and last ky rows erode
    // to nothing.
    let w = 2 * ky + 1;
    let mut out: Rows = vec![Vec::new(); n];
    if n < w {
        return out;
    }
    let mut g: Rows = vec![Vec::new(); n]; // prefix within block
    let mut h: Rows = vec![Vec::new(); n]; // suffix within block
    let mut tmp = Vec::new();
    for start in (0..n).step_by(w) {
        let end = (start + w).min(n);
        g[start] = s[start].clone();
        for i in start + 1..end {
            intersect(&g[i - 1], &s[i], &mut tmp);
            g[i] = tmp.clone();
        }
        h[end - 1] = s[end - 1].clone();
        for i in (start..end - 1).rev() {
            intersect(&h[i + 1], &s[i], &mut tmp);
            h[i] = tmp.clone();
        }
    }
    for (c, o) in out.iter_mut().enumerate().take(n - ky).skip(ky) {
        intersect(&h[c - ky], &g[c + ky], &mut tmp);
        *o = tmp.clone();
    }
    out
}

/// A queue whose intersection is available in amortised O(1) per step.
struct MinQueue {
    /// (value, intersection of this and everything after it in the stack).
    front: Vec<(Rows, Rows)>,
    back: Vec<Rows>,
    back_agg: Option<Rows>,
}

impl MinQueue {
    fn new() -> Self {
        Self {
            front: Vec::new(),
            back: Vec::new(),
            back_agg: None,
        }
    }

    fn push(&mut self, v: Rows) {
        self.back_agg = Some(match self.back_agg.take() {
            None => v.clone(),
            Some(a) => rows_intersect(&a, &v),
        });
        self.back.push(v);
    }

    fn pop(&mut self) {
        if self.front.is_empty() {
            let mut agg: Option<Rows> = None;
            while let Some(v) = self.back.pop() {
                let a = match agg {
                    None => v.clone(),
                    Some(a) => rows_intersect(&a, &v),
                };
                agg = Some(a.clone());
                self.front.push((v, a));
            }
            self.back_agg = None;
        }
        self.front.pop();
    }

    fn all(&self) -> Option<Rows> {
        match (self.front.last(), &self.back_agg) {
            (Some((_, f)), Some(b)) => Some(rows_intersect(f, b)),
            (Some((_, f)), None) => Some(f.clone()),
            (None, Some(b)) => Some(b.clone()),
            (None, None) => None,
        }
    }
}

/// Streams layers through hollowing. Push raw layers in order with
/// [`Hollower::push`]; finished layers come out `delay()` layers later.
pub struct Hollower {
    params: HollowParams,
    /// Rows covered (all layers are cropped to this range of LCD rows).
    row0: usize,
    nrows: usize,
    kx: u32,
    ky: usize,
    nz: usize,
    hole_rx: f64,
    hole_ry: f64,
    /// Raw layers waiting for their interior.
    raw: VecDeque<Rows>,
    eroded: MinQueue,
    pushed: usize,
    /// Interiors computed so far, newest last (kept for hole detection).
    prev_interior: Option<Rows>,
    /// Hollowed layers not yet released (holes may still be cut into them).
    pending: VecDeque<Rows>,
    hole_depth: usize,
    pub holes: Vec<DrainHole>,
    emitted: usize,
}

/// Where a drain hole was cut, for the report.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DrainHole {
    /// LCD pixel column and row (not mirrored) of the centre.
    pub px: (u32, u32),
    /// The centre on the plate in mm (filled in by the slicer).
    pub center_mm: [f64; 2],
    /// Layers the hole goes through.
    pub first_layer: u32,
    pub last_layer: u32,
}

impl Hollower {
    /// `rows`: the LCD rows the model covers. `mm_per_px`: pixel pitch.
    pub fn new(
        params: HollowParams,
        rows: std::ops::Range<usize>,
        mm_per_px: (f64, f64),
        layer_mm: f64,
    ) -> Self {
        let kx = (params.wall_mm as f64 / mm_per_px.0).ceil() as u32;
        let ky = (params.wall_mm as f64 / mm_per_px.1).ceil() as usize;
        let nz = (params.wall_mm as f64 / layer_mm).ceil() as usize;
        let r = params.hole_diameter_mm as f64 / 2.0;
        Self {
            params,
            row0: rows.start,
            nrows: rows.len(),
            kx,
            ky,
            nz,
            hole_rx: r / mm_per_px.0,
            hole_ry: r / mm_per_px.1,
            raw: VecDeque::new(),
            eroded: MinQueue::new(),
            pushed: 0,
            prev_interior: None,
            pending: VecDeque::new(),
            hole_depth: nz + 2,
            holes: Vec::new(),
            emitted: 0,
        }
    }

    /// Crops full-height rows to the hollower's range.
    pub fn crop(&self, full: &[Vec<(u32, u32)>]) -> Rows {
        full[self.row0..self.row0 + self.nrows].to_vec()
    }

    /// The eroded version of a cropped layer. Pure, so callers can compute
    /// it in parallel before pushing.
    pub fn erode_layer(&self, cropped: &Rows) -> Rows {
        erode(cropped, self.kx, self.ky)
    }

    /// Adds the next layer (cropped) and its erosion. Returns any layers
    /// that are now final, in order.
    pub fn push(&mut self, cropped: Rows, eroded: Rows) -> Vec<Rows> {
        if self.pushed == 0 {
            // Layers below the model are empty: prime the window with n of them.
            for _ in 0..self.nz {
                self.eroded.push(vec![Vec::new(); self.nrows]);
            }
        }
        self.raw.push_back(cropped);
        self.eroded.push(eroded);
        self.pushed += 1;
        let mut out = Vec::new();
        // Window for layer k is k-n ..= k+n: ready once k+n has been pushed.
        if self.eroded_len() == 2 * self.nz + 1 {
            out.extend(self.finish_one());
        }
        out
    }

    fn eroded_len(&self) -> usize {
        self.eroded.front.len() + self.eroded.back.len()
    }

    fn finish_one(&mut self) -> Vec<Rows> {
        let interior = self
            .eroded
            .all()
            .unwrap_or_else(|| vec![Vec::new(); self.nrows]);
        self.eroded.pop();
        let raw = self.raw.pop_front().expect("raw layer");
        let k = self.pending.len() + self.emitted;
        if self.params.drain_holes {
            self.cut_holes(&interior, k);
        }
        let hollowed: Rows = raw
            .iter()
            .zip(&interior)
            .map(|(r, i)| {
                if i.is_empty() {
                    r.clone()
                } else {
                    subtract(r, i)
                }
            })
            .collect();
        self.prev_interior = Some(interior);
        self.pending.push_back(hollowed);
        let mut out = Vec::new();
        while self.pending.len() > self.hole_depth {
            out.push(self.pending.pop_front().unwrap());
            self.emitted += 1;
        }
        out
    }

    /// Finds where new hollows start in layer `k` and cuts holes below.
    fn cut_holes(&mut self, interior: &Rows, k: usize) {
        let empty = Vec::new();
        let mut tmp = Vec::new();
        // Candidate spans: interior with nothing under it in the layer below.
        let mut cands: Vec<(usize, u32, u32)> = Vec::new();
        for (r, row) in interior.iter().enumerate() {
            if row.is_empty() {
                continue;
            }
            let below = self.prev_interior.as_ref().map(|p| &p[r]).unwrap_or(&empty);
            let fresh = if below.is_empty() {
                row.clone()
            } else {
                intersect(row, below, &mut tmp);
                subtract(row, &tmp)
            };
            for (a, b) in fresh {
                if (b - a) as f64 > 2.0 * self.hole_rx {
                    cands.push((r, a, b));
                }
            }
        }
        if cands.is_empty() {
            return;
        }
        // A new hollow needs at least a hole's height of rows to fit one.
        let min_rows = (2.0 * self.hole_ry) as usize;
        let sp_x = self.params.hole_spacing_mm as f64 / self.params.hole_diameter_mm as f64
            * 2.0
            * self.hole_rx;
        let sp_y = self.params.hole_spacing_mm as f64 / self.params.hole_diameter_mm as f64
            * 2.0
            * self.hole_ry;
        let mut placed: Vec<(f64, f64)> = Vec::new();
        for &(r, a, b) in &cands {
            // Check the hole fits: the same x range is fresh interior for
            // rows r .. r+min_rows (roughly), using the candidate list.
            let cx = (a + b) as f64 / 2.0;
            let cy = r as f64 + self.hole_ry;
            if r + min_rows >= interior.len() {
                continue;
            }
            let fits = (r..=r + min_rows).all(|rr| {
                interior[rr].iter().any(|&(x0, x1)| {
                    (x0 as f64) <= cx - self.hole_rx && (x1 as f64) >= cx + self.hole_rx
                })
            });
            if !fits {
                continue;
            }
            let row0 = self.row0 as f64;
            let recent = self
                .holes
                .iter()
                .filter(|d| k <= d.last_layer as usize + 2 * self.nz + 2);
            let near = placed
                .iter()
                .copied()
                .chain(recent.map(|d| (d.px.0 as f64, d.px.1 as f64 - row0)))
                .any(|(px, py)| {
                    let (dx, dy) = ((px - cx) / sp_x, (py - cy) / sp_y);
                    dx * dx + dy * dy < 1.0
                });
            if near {
                continue;
            }
            placed.push((cx, cy));
        }
        for &(cx, cy) in &placed {
            let depth = self.pending.len().min(self.hole_depth);
            if depth == 0 {
                continue; // hollow starts on layer 0: nothing below to drill
            }
            let start = self.pending.len() - depth;
            for layer in self.pending.iter_mut().skip(start) {
                cut_disc(layer, cx, cy, self.hole_rx, self.hole_ry);
            }
            self.holes.push(DrainHole {
                px: (cx as u32, (cy as usize + self.row0) as u32),
                center_mm: [0.0, 0.0],
                first_layer: (k - depth) as u32,
                last_layer: k as u32 - 1,
            });
        }
    }

    /// Releases the remaining layers after the last push.
    pub fn finish(mut self) -> (Vec<Rows>, Vec<DrainHole>) {
        let mut out = Vec::new();
        // Pad with empty layers above the model to close the window.
        while !self.raw.is_empty() {
            self.eroded.push(vec![Vec::new(); self.nrows]);
            out.extend(self.finish_one());
        }
        out.extend(self.pending.drain(..));
        (out, self.holes)
    }

    pub fn row0(&self) -> usize {
        self.row0
    }
}

/// Removes an ellipse (a circle in mm on non-square pixels) from a layer.
fn cut_disc(rows: &mut Rows, cx: f64, cy: f64, rx: f64, ry: f64) {
    let r0 = ((cy - ry).floor().max(0.0)) as usize;
    let r1 = ((cy + ry).ceil() as usize).min(rows.len());
    for (r, row) in rows.iter_mut().enumerate().take(r1).skip(r0) {
        let dy = (r as f64 + 0.5 - cy) / ry;
        if dy.abs() >= 1.0 {
            continue;
        }
        let half = rx * (1.0 - dy * dy).sqrt();
        let a = (cx - half).round().max(0.0) as u32;
        let b = (cx + half).round().max(0.0) as u32;
        if a < b {
            *row = subtract(row, &[(a, b)]);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn span_ops() {
        let mut out = Vec::new();
        intersect(&[(0, 10), (20, 30)], &[(5, 25)], &mut out);
        assert_eq!(out, vec![(5, 10), (20, 25)]);
        assert_eq!(
            subtract(&[(0, 30)], &[(5, 10), (20, 25)]),
            vec![(0, 5), (10, 20), (25, 30)]
        );
        assert_eq!(subtract(&[(0, 10)], &[(0, 10)]), vec![]);
        assert_eq!(
            subtract(&[(0, 10), (12, 14)], &[(5, 13)]),
            vec![(0, 5), (13, 14)]
        );
    }

    #[test]
    fn erosion_shrinks_a_square_by_the_box() {
        let rows: Rows = (0..20)
            .map(|r| {
                if (5..15).contains(&r) {
                    vec![(10, 40)]
                } else {
                    vec![]
                }
            })
            .collect();
        let e = erode(&rows, 3, 2);
        for (r, row) in e.iter().enumerate() {
            if (7..13).contains(&r) {
                assert_eq!(row, &vec![(13, 37)], "row {r}");
            } else {
                assert!(row.is_empty(), "row {r}: {row:?}");
            }
        }
    }

    #[test]
    fn min_queue_matches_brute_force() {
        let layers: Vec<Rows> = (0..12)
            .map(|i| vec![vec![(i as u32, 50 - i as u32 % 5)]])
            .collect();
        let mut q = MinQueue::new();
        let w = 3;
        for (i, l) in layers.iter().enumerate() {
            q.push(l.clone());
            if i + 1 > w {
                q.pop();
            }
            if i + 1 >= w {
                let lo = i + 1 - w;
                let mut expect = layers[lo].clone();
                for l in &layers[lo + 1..=i] {
                    expect = rows_intersect(&expect, l);
                }
                assert_eq!(q.all().unwrap(), expect, "at {i}");
            }
        }
    }

    #[test]
    fn solid_block_is_hollowed_with_walls_and_a_drain_hole() {
        // 1 px = 0.1 mm in X and Y; layers 0.1 mm; a 6 x 6 x 3 mm block.
        let block: Rows = (0..80)
            .map(|r| {
                if (10..70).contains(&r) {
                    vec![(10, 70)]
                } else {
                    vec![]
                }
            })
            .collect();
        let p = HollowParams {
            wall_mm: 1.0,
            drain_holes: true,
            hole_diameter_mm: 1.0,
            hole_spacing_mm: 100.0,
        };
        let mut h = Hollower::new(p, 0..80, (0.1, 0.1), 0.1);
        let mut out = Vec::new();
        for _ in 0..30 {
            let e = h.erode_layer(&block);
            out.extend(h.push(block.clone(), e));
        }
        let (rest, holes) = h.finish();
        out.extend(rest);
        assert_eq!(out.len(), 30);
        let lit = |l: &Rows| {
            l.iter()
                .flatten()
                .map(|&(a, b)| (b - a) as u64)
                .sum::<u64>()
        };
        // Top and bottom 10 layers stay solid except where the hole goes.
        assert_eq!(lit(&out[29]), 60 * 60);
        // Middle layers: a 1 mm wall around a 4 x 4 mm hollow.
        assert_eq!(lit(&out[15]), 60 * 60 - 40 * 40);
        assert_eq!(out[15][40], vec![(10, 20), (60, 70)]);
        // One hole, through the floor (layers below the hollow's first layer).
        assert_eq!(holes.len(), 1, "{holes:?}");
        let hole = holes[0];
        assert_eq!(hole.last_layer, 9);
        assert!(hole.first_layer == 0, "{hole:?}");
        assert!(lit(&out[0]) < 60 * 60, "the hole goes through layer 0");
    }
}
