//! Print checks that run on sliced layers: islands, unsupported overhangs,
//! suction cups and trapped resin.
//!
//! The checks work on a coarse occupancy grid (by default 0.25 mm cells): a
//! cell is solid when any LCD pixel inside it is lit. That keeps them fast
//! on a 16K LCD, but features smaller than a cell can be missed, and a small
//! gap between two parts can look closed.
//!
//! Layers are fed in print order (layer 0 is on the build plate), and only
//! the previous layer is kept, so memory does not grow with the print.
//!
//! Terms, as seen while printing (the model hangs from the plate and each
//! new layer forms against the vat floor):
//!
//! - **Island**: a connected solid area with nothing beneath it in the
//!   previous layer. It cures in mid-air and will drop into the vat.
//! - **Unsupported overhang**: part of an area that does rest on the
//!   previous layer but sticks out more than `overhang_mm` past it.
//! - **Suction cup**: a hollow that is open only towards the vat floor:
//!   sealed by the plate or by earlier layers on every other side. Each lift
//!   pulls a vacuum in it.
//! - **Trapped resin**: a hollow that the print closes off completely. The
//!   liquid resin inside cannot drain.

/// A coarse occupancy grid for one layer.
#[derive(Debug, Clone, PartialEq)]
pub struct CoarseLayer {
    pub width: usize,
    pub height: usize,
    /// Row-major, `true` = solid.
    pub cells: Vec<bool>,
}

impl CoarseLayer {
    pub fn empty(width: usize, height: usize) -> Self {
        Self {
            width,
            height,
            cells: vec![false; width * height],
        }
    }

    pub fn get(&self, x: usize, y: usize) -> bool {
        self.cells[y * self.width + x]
    }

    pub fn set(&mut self, x: usize, y: usize) {
        self.cells[y * self.width + x] = true;
    }
}

/// How coarse cells map onto the plate.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CoarseGrid {
    pub width: usize,
    pub height: usize,
    pub cell_mm: f64,
    /// Plate X/Y (mm, origin at the centre) of the grid's corner.
    pub origin_mm: [f64; 2],
}

impl CoarseGrid {
    /// A grid with `cell_mm` cells covering a plate of `size_x` x `size_y`.
    pub fn for_plate(size_x: f64, size_y: f64, cell_mm: f64) -> Self {
        Self {
            width: (size_x / cell_mm).ceil() as usize,
            height: (size_y / cell_mm).ceil() as usize,
            cell_mm,
            origin_mm: [-size_x / 2.0, -size_y / 2.0],
        }
    }

    fn cell_rect_mm(&self, b: &CellBox) -> RectMm {
        RectMm {
            min: [
                self.origin_mm[0] + b.x0 as f64 * self.cell_mm,
                self.origin_mm[1] + b.y0 as f64 * self.cell_mm,
            ],
            max: [
                self.origin_mm[0] + (b.x1 + 1) as f64 * self.cell_mm,
                self.origin_mm[1] + (b.y1 + 1) as f64 * self.cell_mm,
            ],
        }
    }
}

/// An axis-aligned rectangle on the plate, in mm.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RectMm {
    pub min: [f64; 2],
    pub max: [f64; 2],
}

impl RectMm {
    pub fn center(&self) -> [f64; 2] {
        [
            (self.min[0] + self.max[0]) / 2.0,
            (self.min[1] + self.max[1]) / 2.0,
        ]
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum IssueKind {
    Island,
    UnsupportedOverhang,
    SuctionCup,
    TrappedResin,
}

impl IssueKind {
    pub fn label(self) -> &'static str {
        match self {
            IssueKind::Island => "Island",
            IssueKind::UnsupportedOverhang => "Unsupported overhang",
            IssueKind::SuctionCup => "Suction cup",
            IssueKind::TrappedResin => "Trapped resin",
        }
    }
}

/// One problem found in the print.
#[derive(Debug, Clone, PartialEq)]
pub struct Issue {
    pub kind: IssueKind,
    /// First and last layer (0-based) the problem covers.
    pub first_layer: u32,
    pub last_layer: u32,
    /// Where on the plate, in mm.
    pub area: RectMm,
    /// Island / overhang / cup: area in mm^2 (the largest cross-section for a
    /// cup). Trapped resin: volume in mm^3.
    pub size: f64,
}

/// Thresholds for the checks.
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(default))]
pub struct CheckParams {
    /// Size of a coarse cell in mm.
    pub cell_mm: f64,
    /// An overhang is flagged when it reaches further than this past the
    /// layer below.
    pub overhang_mm: f64,
    /// Areas (mm^2) smaller than this are ignored for islands and overhangs.
    pub min_area_mm2: f64,
    /// Cups whose largest cross-section is smaller than this (mm^2) are
    /// ignored: a tiny hole barely pulls a vacuum.
    pub min_cup_area_mm2: f64,
}

impl Default for CheckParams {
    fn default() -> Self {
        Self {
            cell_mm: 0.25,
            overhang_mm: 2.5,
            min_area_mm2: 0.1,
            min_cup_area_mm2: 1.0,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct CellBox {
    x0: usize,
    y0: usize,
    x1: usize,
    y1: usize,
}

impl CellBox {
    fn point(x: usize, y: usize) -> Self {
        Self {
            x0: x,
            y0: y,
            x1: x,
            y1: y,
        }
    }

    fn add(&mut self, x: usize, y: usize) {
        self.x0 = self.x0.min(x);
        self.y0 = self.y0.min(y);
        self.x1 = self.x1.max(x);
        self.y1 = self.y1.max(y);
    }

    fn union(&mut self, o: &CellBox) {
        self.x0 = self.x0.min(o.x0);
        self.y0 = self.y0.min(o.y0);
        self.x1 = self.x1.max(o.x1);
        self.y1 = self.y1.max(o.y1);
    }
}

/// Labels 4-connected regions of cells where `cells[i] == want`. Returns
/// labels (`u32::MAX` for other cells) and the number of regions.
fn label(layer: &CoarseLayer, want: bool, labels: &mut Vec<u32>) -> usize {
    let (w, h) = (layer.width, layer.height);
    labels.clear();
    labels.resize(w * h, u32::MAX);
    let mut n = 0u32;
    let mut stack = Vec::new();
    for start in 0..w * h {
        if layer.cells[start] != want || labels[start] != u32::MAX {
            continue;
        }
        labels[start] = n;
        stack.push(start);
        while let Some(i) = stack.pop() {
            let (x, y) = (i % w, i / w);
            let mut visit = |j: usize| {
                if layer.cells[j] == want && labels[j] == u32::MAX {
                    labels[j] = n;
                    stack.push(j);
                }
            };
            if x > 0 {
                visit(i - 1);
            }
            if x + 1 < w {
                visit(i + 1);
            }
            if y > 0 {
                visit(i - w);
            }
            if y + 1 < h {
                visit(i + w);
            }
        }
        n += 1;
    }
    n as usize
}

/// Per-hollow bookkeeping across layers.
#[derive(Debug, Clone)]
struct Hollow {
    parent: usize,
    vented: bool,
    cells: u64,
    max_area_cells: u64,
    first_layer: u32,
    bbox: CellBox,
    /// Layers in which this hollow was a sealed-off cup, if any.
    cup_first: Option<u32>,
    cup_last: u32,
}

/// A rectangle of cells, half-open: `[x0, x1) x [y0, y1)`.
#[derive(Debug, Clone, Copy, PartialEq)]
struct Crop {
    x0: usize,
    y0: usize,
    x1: usize,
    y1: usize,
}

impl Crop {
    fn w(&self) -> usize {
        self.x1 - self.x0
    }
    fn h(&self) -> usize {
        self.y1 - self.y0
    }
    fn contains(&self, x: usize, y: usize) -> bool {
        x >= self.x0 && x < self.x1 && y >= self.y0 && y < self.y1
    }
}

/// Bounding box of solid cells, if any.
fn solid_bbox(l: &CoarseLayer) -> Option<Crop> {
    let mut b: Option<Crop> = None;
    for y in 0..l.height {
        let row = &l.cells[y * l.width..(y + 1) * l.width];
        let Some(first) = row.iter().position(|&c| c) else {
            continue;
        };
        let last = row.iter().rposition(|&c| c).unwrap();
        b = Some(match b {
            None => Crop {
                x0: first,
                y0: y,
                x1: last + 1,
                y1: y + 1,
            },
            Some(b) => Crop {
                x0: b.x0.min(first),
                y0: b.y0,
                x1: b.x1.max(last + 1),
                y1: y + 1,
            },
        });
    }
    b
}

fn crop_layer(l: &CoarseLayer, c: &Crop) -> CoarseLayer {
    let mut out = CoarseLayer::empty(c.w(), c.h());
    for y in c.y0..c.y1 {
        let src = &l.cells[y * l.width + c.x0..y * l.width + c.x1];
        out.cells[(y - c.y0) * c.w()..(y - c.y0 + 1) * c.w()].copy_from_slice(src);
    }
    out
}

/// The id of the open vat: all air outside the model, always vented.
const OPEN: usize = 0;

/// Runs the checks one layer at a time.
pub struct PrintChecker {
    grid: CoarseGrid,
    params: CheckParams,
    layer: u32,
    prev: Option<CoarseLayer>,
    prev_bbox: Option<Crop>,
    /// Air labels of the previous layer inside `prev_crop`, as hollow ids.
    prev_air: Vec<u32>,
    prev_crop: Option<Crop>,
    hollows: Vec<Hollow>,
    issues: Vec<Issue>,
    scratch: Vec<u32>,
    air_scratch: Vec<u32>,
    dist: Vec<u32>,
}

impl PrintChecker {
    pub fn new(grid: CoarseGrid, params: CheckParams) -> Self {
        Self {
            grid,
            params,
            layer: 0,
            prev: None,
            prev_bbox: None,
            prev_air: Vec::new(),
            prev_crop: None,
            hollows: vec![Hollow {
                parent: OPEN,
                vented: true,
                cells: 0,
                max_area_cells: 0,
                first_layer: 0,
                bbox: CellBox::point(0, 0),
                cup_first: None,
                cup_last: 0,
            }],
            issues: Vec::new(),
            scratch: Vec::new(),
            air_scratch: Vec::new(),
            dist: Vec::new(),
        }
    }

    pub fn grid(&self) -> &CoarseGrid {
        &self.grid
    }

    fn cell_area(&self) -> f64 {
        self.grid.cell_mm * self.grid.cell_mm
    }

    fn find(&mut self, mut i: usize) -> usize {
        while self.hollows[i].parent != i {
            let p = self.hollows[i].parent;
            self.hollows[i].parent = self.hollows[p].parent;
            i = p;
        }
        i
    }

    fn union(&mut self, a: usize, b: usize) -> usize {
        let (ra, rb) = (self.find(a), self.find(b));
        if ra == rb {
            return ra;
        }
        // A cup that is about to be vented by this join ends here.
        for (x, y) in [(ra, rb), (rb, ra)] {
            if !self.hollows[x].vented && self.hollows[y].vented {
                let hl = self.hollows[x].clone();
                self.report_cup(&hl);
                self.hollows[x].cup_first = None;
            }
        }
        let (keep, gone) = if ra < rb { (ra, rb) } else { (rb, ra) };
        let g = self.hollows[gone].clone();
        let k = &mut self.hollows[keep];
        k.vented |= g.vented;
        if keep != OPEN {
            k.cells += g.cells;
            k.max_area_cells = k.max_area_cells.max(g.max_area_cells);
            k.first_layer = k.first_layer.min(g.first_layer);
            k.bbox.union(&g.bbox);
        }
        k.cup_first = match (k.cup_first, g.cup_first) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (a, b) => a.or(b),
        };
        k.cup_last = k.cup_last.max(g.cup_last);
        if k.vented {
            k.cup_first = None;
        }
        self.hollows[gone].parent = keep;
        keep
    }

    /// Adds the next layer in print order.
    pub fn push_layer(&mut self, layer: CoarseLayer) {
        assert_eq!(
            (layer.width, layer.height),
            (self.grid.width, self.grid.height)
        );
        let bbox = solid_bbox(&layer);
        // Work only on the area around solid cells in this or the previous
        // layer, plus a one-cell margin of open air.
        let crop = match (bbox, self.prev_bbox) {
            (None, None) => None,
            (Some(a), None) | (None, Some(a)) => Some(a),
            (Some(a), Some(b)) => Some(Crop {
                x0: a.x0.min(b.x0),
                y0: a.y0.min(b.y0),
                x1: a.x1.max(b.x1),
                y1: a.y1.max(b.y1),
            }),
        }
        .map(|c| Crop {
            x0: c.x0.saturating_sub(1),
            y0: c.y0.saturating_sub(1),
            x1: (c.x1 + 1).min(self.grid.width),
            y1: (c.y1 + 1).min(self.grid.height),
        });
        match crop {
            Some(c) => {
                let cur = crop_layer(&layer, &c);
                if let Some(prev) = &self.prev {
                    let prev = crop_layer(prev, &c);
                    self.check_islands_and_overhangs(&cur, &prev, &c);
                }
                self.track_hollows(&cur, &c);
            }
            None => {
                self.prev_air.clear();
                self.prev_crop = None;
            }
        }
        self.prev = Some(layer);
        self.prev_bbox = bbox;
        self.layer += 1;
    }

    fn check_islands_and_overhangs(&mut self, layer: &CoarseLayer, prev: &CoarseLayer, c: &Crop) {
        let (w, h) = (layer.width, layer.height);
        let n = label(layer, true, &mut self.scratch);
        // Per region: does it touch the previous layer, its cell box, size.
        let mut supported = vec![false; n];
        let mut boxes: Vec<Option<CellBox>> = vec![None; n];
        let mut counts = vec![0u64; n];
        for i in 0..w * h {
            let l = self.scratch[i];
            if l == u32::MAX {
                continue;
            }
            let l = l as usize;
            let (x, y) = (i % w + c.x0, i / w + c.y0);
            counts[l] += 1;
            match &mut boxes[l] {
                Some(b) => b.add(x, y),
                b => *b = Some(CellBox::point(x, y)),
            }
            if prev.cells[i] {
                supported[l] = true;
            }
        }
        let min_cells = ((self.params.min_area_mm2 / self.cell_area()).ceil() as u64).max(1);
        for l in 0..n {
            if !supported[l] && counts[l] >= min_cells {
                self.issues.push(Issue {
                    kind: IssueKind::Island,
                    first_layer: self.layer,
                    last_layer: self.layer,
                    area: self.grid.cell_rect_mm(&boxes[l].unwrap()),
                    size: counts[l] as f64 * self.cell_area(),
                });
            }
        }

        // Overhangs: distance through the current layer's solid from the
        // previous layer's solid, 8-connected, in half cells (2 per straight
        // step, 3 per diagonal step).
        let limit = (2.0 * self.params.overhang_mm / self.grid.cell_mm).round() as u32;
        self.dist.clear();
        self.dist.resize(w * h, u32::MAX);
        let mut buckets: Vec<Vec<usize>> = vec![Vec::new(); limit as usize + 4];
        for i in 0..w * h {
            if layer.cells[i] && prev.cells[i] {
                self.dist[i] = 0;
                buckets[0].push(i);
            }
        }
        for d in 0..=limit as usize {
            while let Some(i) = buckets[d].pop() {
                if self.dist[i] != d as u32 {
                    continue;
                }
                let (x, y) = ((i % w) as i64, (i / w) as i64);
                for (dx, dy, cost) in [
                    (-1, 0, 2),
                    (1, 0, 2),
                    (0, -1, 2),
                    (0, 1, 2),
                    (-1, -1, 3),
                    (1, -1, 3),
                    (-1, 1, 3),
                    (1, 1, 3),
                ] {
                    let (nx, ny) = (x + dx, y + dy);
                    if nx < 0 || ny < 0 || nx >= w as i64 || ny >= h as i64 {
                        continue;
                    }
                    let j = ny as usize * w + nx as usize;
                    let nd = d as u32 + cost;
                    if layer.cells[j] && nd < self.dist[j] && nd <= limit + 3 {
                        self.dist[j] = nd;
                        buckets[nd as usize].push(j);
                    }
                }
            }
        }
        // Group far cells per supported region.
        let mut far_box: Vec<Option<CellBox>> = vec![None; n];
        let mut far_count = vec![0u64; n];
        for i in 0..w * h {
            let l = self.scratch[i];
            if l == u32::MAX || !supported[l as usize] || self.dist[i] <= limit {
                continue;
            }
            let l = l as usize;
            let (x, y) = (i % w + c.x0, i / w + c.y0);
            far_count[l] += 1;
            match &mut far_box[l] {
                Some(b) => b.add(x, y),
                b => *b = Some(CellBox::point(x, y)),
            }
        }
        for l in 0..n {
            if far_count[l] >= min_cells {
                self.issues.push(Issue {
                    kind: IssueKind::UnsupportedOverhang,
                    first_layer: self.layer,
                    last_layer: self.layer,
                    area: self.grid.cell_rect_mm(&far_box[l].unwrap()),
                    size: far_count[l] as f64 * self.cell_area(),
                });
            }
        }
    }

    fn track_hollows(&mut self, layer: &CoarseLayer, c: &Crop) {
        let (w, h) = (layer.width, layer.height);
        let n = label(layer, false, &mut self.air_scratch);
        // A hollow id for each air region in this layer.
        let base = self.hollows.len();
        for _ in 0..n {
            let id = self.hollows.len();
            self.hollows.push(Hollow {
                parent: id,
                vented: false,
                cells: 0,
                max_area_cells: 0,
                first_layer: self.layer,
                bbox: CellBox::point(0, 0),
                cup_first: None,
                cup_last: 0,
            });
        }
        let mut area = vec![0u64; n];
        let mut boxes: Vec<Option<CellBox>> = vec![None; n];
        let mut touches_edge = vec![false; n];
        for i in 0..w * h {
            let l = self.air_scratch[i];
            if l == u32::MAX {
                continue;
            }
            let l = l as usize;
            let (x, y) = (i % w, i / w);
            area[l] += 1;
            match &mut boxes[l] {
                Some(b) => b.add(x + c.x0, y + c.y0),
                b => *b = Some(CellBox::point(x + c.x0, y + c.y0)),
            }
            // The crop's edge is open air around the model (or the LCD edge).
            if x == 0 || y == 0 || x + 1 == w || y + 1 == h {
                touches_edge[l] = true;
            }
        }
        for l in 0..n {
            let hl = &mut self.hollows[base + l];
            hl.cells = area[l];
            hl.max_area_cells = area[l];
            hl.bbox = boxes[l].unwrap();
        }
        for l in 0..n {
            if touches_edge[l] {
                self.union(base + l, OPEN);
            }
        }
        // Join with the previous layer's air at the same cell (towards the
        // plate). Outside the previous crop there was only open air.
        let prev_crop = self.prev_crop;
        let had_prev = self.layer > 0;
        for i in 0..w * h {
            let cur = self.air_scratch[i];
            if cur == u32::MAX || !had_prev {
                continue;
            }
            let (gx, gy) = (i % w + c.x0, i / w + c.y0);
            let prev = match prev_crop {
                Some(p) if p.contains(gx, gy) => self.prev_air[(gy - p.y0) * p.w() + (gx - p.x0)],
                _ => OPEN as u32,
            };
            if prev != u32::MAX {
                self.union(base + cur as usize, prev as usize);
            }
        }
        // Each root's area in this layer, for cup size.
        let mut root_area: std::collections::HashMap<usize, u64> = Default::default();
        for l in 0..n {
            let r = self.find(base + l);
            *root_area.entry(r).or_default() += area[l];
        }
        // Hollows from the previous layer that did not continue were sealed.
        let mut prev_ids: Vec<u32> = self
            .prev_air
            .iter()
            .copied()
            .filter(|&p| p != u32::MAX)
            .collect();
        prev_ids.sort_unstable();
        prev_ids.dedup();
        let mut checked = std::collections::HashSet::new();
        for p in prev_ids {
            let r = self.find(p as usize);
            if !checked.insert(r) || root_area.contains_key(&r) {
                continue;
            }
            self.seal(r);
        }
        for (&r, &a) in &root_area {
            if r == OPEN {
                continue;
            }
            let layer = self.layer;
            let hl = &mut self.hollows[r];
            hl.max_area_cells = hl.max_area_cells.max(a);
            if !hl.vented {
                hl.cup_first.get_or_insert(layer);
                hl.cup_last = layer;
            }
        }
        // Remember this layer's air as hollow ids (roots resolved lazily).
        self.prev_air.clear();
        self.prev_air.extend(self.air_scratch.iter().map(|&l| {
            if l == u32::MAX {
                u32::MAX
            } else {
                (base + l as usize) as u32
            }
        }));
        self.prev_crop = Some(*c);
    }

    /// A hollow `r` has no air in the current layer: it is closed off.
    fn seal(&mut self, r: usize) {
        let hl = self.hollows[r].clone();
        if hl.vented {
            return;
        }
        let area = self.cell_area();
        self.report_cup(&hl);
        if hl.cells as f64 * area >= self.params.min_cup_area_mm2 {
            self.issues.push(Issue {
                kind: IssueKind::TrappedResin,
                first_layer: hl.first_layer,
                last_layer: self.layer.saturating_sub(1),
                area: self.grid.cell_rect_mm(&hl.bbox),
                // Volume is filled in by `finish` once the layer height is
                // known; store cell-layers for now.
                size: hl.cells as f64,
            });
        }
        // Mark as handled so it is not reported again.
        self.hollows[r].vented = true;
    }

    fn report_cup(&mut self, hl: &Hollow) {
        if let Some(first) = hl.cup_first {
            let a = hl.max_area_cells as f64 * self.cell_area();
            if a >= self.params.min_cup_area_mm2 {
                self.issues.push(Issue {
                    kind: IssueKind::SuctionCup,
                    first_layer: first,
                    last_layer: hl.cup_last,
                    area: self.grid.cell_rect_mm(&hl.bbox),
                    size: a,
                });
            }
        }
    }

    /// Finishes after the last layer. Hollows still open at the top are open
    /// to the vat once printing ends, but were cups while printing.
    pub fn finish(mut self, layer_mm: f64) -> Vec<Issue> {
        let mut roots: Vec<usize> = self
            .prev_air
            .iter()
            .filter(|&&p| p != u32::MAX)
            .map(|&p| p as usize)
            .collect();
        roots.sort_unstable();
        roots.dedup();
        let mut done = std::collections::HashSet::new();
        for p in roots {
            let r = self.find(p);
            if done.insert(r) && !self.hollows[r].vented {
                let hl = self.hollows[r].clone();
                self.report_cup(&hl);
            }
        }
        let cell_vol = self.cell_area() * layer_mm;
        for i in &mut self.issues {
            if i.kind == IssueKind::TrappedResin {
                i.size *= cell_vol;
            }
        }
        self.issues.sort_by_key(|i| (i.first_layer, i.kind as u8));
        self.issues
    }
}

/// Builds a coarse layer from lit pixel spans (rows in LCD order, not
/// mirrored). A cell is solid when any lit pixel's centre falls in it.
pub fn coarse_from_spans(
    spans: &crate::raster::LayerSpans,
    mm_per_px: (f64, f64),
    grid: &CoarseGrid,
) -> CoarseLayer {
    let mut l = CoarseLayer::empty(grid.width, grid.height);
    let cell = |px: u32, mm: f64, max: usize| {
        (((px as f64 + 0.5) * mm / grid.cell_mm) as usize).min(max - 1)
    };
    for (r, row) in spans.rows.iter().enumerate() {
        if row.is_empty() {
            continue;
        }
        let cy = cell(r as u32, mm_per_px.1, grid.height);
        for &(a, b) in row {
            let (c0, c1) = (
                cell(a, mm_per_px.0, grid.width),
                cell(b - 1, mm_per_px.0, grid.width),
            );
            for cx in c0..=c1 {
                l.set(cx, cy);
            }
        }
    }
    l
}

/// What [`check_goo`] found in an existing file.
#[derive(Debug, Clone, Default)]
pub struct GooAnalysis {
    pub issues: Vec<Issue>,
    /// Volume of lit pixels (grey pixels count by their level), mm^3.
    pub lit_volume_mm3: f64,
    pub empty_layers: Vec<u32>,
    /// Plate area covered by lit pixels in any layer, if any.
    pub bounds: Option<RectMm>,
}

/// Runs the print checks on an existing .goo file, for example one made by
/// another slicer. Layers are read as stored; X mirroring is undone using
/// the header's flag so positions match the plate.
pub fn check_goo(
    file: &goo::GooFile,
    params: CheckParams,
    progress: Option<&dyn Fn(u32, u32)>,
    cancel: Option<&std::sync::atomic::AtomicBool>,
) -> Result<GooAnalysis, String> {
    let h = &file.header;
    let (w, rows) = (h.resolution_x as u64, h.resolution_y as u64);
    let (mx, my) = (
        h.size_x as f64 / w as f64,
        h.size_y as f64 / rows as f64,
    );
    let grid = CoarseGrid::for_plate(h.size_x as f64, h.size_y as f64, params.cell_mm);
    let mut checker = PrintChecker::new(grid, params);
    let mut out = GooAnalysis::default();
    let n = file.layers.len() as u32;
    let mut bbox: Option<CellBox> = None;
    let cell = |px: u64, mm: f64, max: usize| (((px as f64 + 0.5) * mm / grid.cell_mm) as usize).min(max - 1);
    for (i, (_, data)) in file.layers.iter().enumerate() {
        if cancel.is_some_and(|c| c.load(std::sync::atomic::Ordering::Relaxed)) {
            return Err("cancelled".into());
        }
        let mut layer = CoarseLayer::empty(grid.width, grid.height);
        let mut lit = 0u64;
        let mut pos = 0u64;
        goo::rle::decode_runs(data, |v, len| {
            if v != 0 {
                lit += len * v as u64;
                let (mut p, end) = (pos, pos + len);
                while p < end {
                    let (y, x) = (p / w, p % w);
                    let row_end = ((y + 1) * w).min(end);
                    let x_end = x + (row_end - p);
                    let (a, b) = if h.mirror_x { (w - x_end, w - x) } else { (x, x_end) };
                    let cy = cell(y.min(rows - 1), my, grid.height);
                    for cx in cell(a, mx, grid.width)..=cell(b - 1, mx, grid.width) {
                        layer.set(cx, cy);
                    }
                    p = row_end;
                }
            }
            pos += len;
        })
        .map_err(|e| format!("layer {i}: {e}"))?;
        if lit == 0 {
            out.empty_layers.push(i as u32);
        }
        if let Some(b) = solid_bbox(&layer) {
            let cb = CellBox { x0: b.x0, y0: b.y0, x1: b.x1 - 1, y1: b.y1 - 1 };
            bbox = Some(match bbox {
                None => cb,
                Some(mut a) => {
                    a.union(&cb);
                    a
                }
            });
        }
        out.lit_volume_mm3 += lit as f64 / 255.0 * mx * my * h.layer_thickness as f64;
        checker.push_layer(layer);
        if let Some(p) = progress {
            if i % 16 == 0 || i as u32 + 1 == n {
                p(i as u32 + 1, n);
            }
        }
    }
    out.bounds = bbox.map(|b| grid.cell_rect_mm(&b));
    out.issues = checker.finish(h.layer_thickness as f64);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const W: usize = 20;
    const H: usize = 20;

    fn grid() -> CoarseGrid {
        CoarseGrid {
            width: W,
            height: H,
            cell_mm: 1.0,
            origin_mm: [-10.0, -10.0],
        }
    }

    fn params() -> CheckParams {
        CheckParams {
            cell_mm: 1.0,
            overhang_mm: 2.0,
            min_area_mm2: 1.0,
            min_cup_area_mm2: 1.0,
        }
    }

    fn rect(x0: usize, y0: usize, x1: usize, y1: usize) -> CoarseLayer {
        let mut l = CoarseLayer::empty(W, H);
        for y in y0..y1 {
            for x in x0..x1 {
                l.set(x, y);
            }
        }
        l
    }

    /// A square ring: solid from (x0,y0) to (x1,y1) minus the inner hole.
    fn ring(x0: usize, x1: usize, wall: usize) -> CoarseLayer {
        let mut l = rect(x0, x0, x1, x1);
        for y in x0 + wall..x1 - wall {
            for x in x0 + wall..x1 - wall {
                l.cells[y * W + x] = false;
            }
        }
        l
    }

    fn run(layers: Vec<CoarseLayer>) -> Vec<Issue> {
        let mut c = PrintChecker::new(grid(), params());
        for l in layers {
            c.push_layer(l);
        }
        c.finish(1.0)
    }

    fn kinds(issues: &[Issue]) -> Vec<IssueKind> {
        issues.iter().map(|i| i.kind).collect()
    }

    #[test]
    fn solid_column_is_fine() {
        let issues = run(vec![rect(5, 5, 10, 10); 5]);
        assert!(issues.is_empty(), "{issues:?}");
    }

    #[test]
    fn floating_part_is_an_island() {
        let mut layers = vec![rect(2, 2, 5, 5); 3];
        let mut l = rect(2, 2, 5, 5);
        for y in 10..13 {
            for x in 10..13 {
                l.set(x, y);
            }
        }
        layers.push(l);
        let issues = run(layers);
        assert_eq!(kinds(&issues), vec![IssueKind::Island]);
        assert_eq!(issues[0].first_layer, 3);
        assert_eq!(issues[0].size, 9.0);
        assert_eq!(issues[0].area.min, [0.0, 0.0]);
        assert_eq!(issues[0].area.max, [3.0, 3.0]);
    }

    #[test]
    fn wide_overhang_is_flagged_and_narrow_is_not() {
        // Grows by 1 cell (within the 2 mm limit): fine.
        let ok = run(vec![rect(5, 5, 10, 10), rect(4, 5, 11, 10)]);
        assert!(ok.is_empty(), "{ok:?}");
        // Grows by 6 cells to the right in one layer: flagged.
        let bad = run(vec![rect(5, 5, 10, 10), rect(5, 5, 16, 10)]);
        assert_eq!(kinds(&bad), vec![IssueKind::UnsupportedOverhang]);
        // Cells whose centre is more than 2 mm past the edge: x = 12..=15.
        assert_eq!(bad[0].size, 4.0 * 5.0);
    }

    #[test]
    fn open_ring_is_a_suction_cup() {
        // A cup open towards the vat: a floor on the plate, then ring walls.
        let mut layers = vec![rect(4, 4, 14, 14)];
        layers.extend(vec![ring(4, 14, 2); 4]);
        let issues = run(layers);
        assert_eq!(kinds(&issues), vec![IssueKind::SuctionCup]);
        assert_eq!((issues[0].first_layer, issues[0].last_layer), (1, 4));
        assert_eq!(issues[0].size, 36.0);
    }

    #[test]
    fn tube_open_at_the_plate_end_is_not_a_cup() {
        // A ring printed straight on the plate: the plate seals it, so it IS a
        // cup. A ring that starts with a vent through its wall is not.
        let mut vent = ring(4, 14, 2);
        for x in 4..6 {
            vent.cells[8 * W + x] = false; // gap through the left wall
        }
        let issues = run(vec![vent, ring(4, 14, 2), ring(4, 14, 2)]);
        assert!(issues.is_empty(), "{issues:?}");
    }

    #[test]
    fn closed_box_traps_resin() {
        let mut layers = vec![rect(4, 4, 14, 14)];
        layers.extend(vec![ring(4, 14, 2); 3]);
        layers.push(rect(4, 4, 14, 14));
        let issues = run(layers);
        // The lid over the 6 x 6 hollow is also an unsupported bridge.
        assert_eq!(
            kinds(&issues),
            vec![
                IssueKind::SuctionCup,
                IssueKind::TrappedResin,
                IssueKind::UnsupportedOverhang
            ]
        );
        let trap = &issues[1];
        assert_eq!((trap.first_layer, trap.last_layer), (1, 3));
        assert_eq!(trap.size, 36.0 * 3.0);
    }

    #[test]
    fn drain_hole_in_the_lid_side_does_not_vent_the_cup_until_it_appears() {
        // Walls with a vent appearing at layer 3: cup for layers 1-2 only.
        let mut layers = vec![rect(4, 4, 14, 14), ring(4, 14, 2), ring(4, 14, 2)];
        let mut vent = ring(4, 14, 2);
        for x in 4..6 {
            vent.cells[8 * W + x] = false;
        }
        layers.push(vent);
        layers.push(ring(4, 14, 2));
        let issues = run(layers);
        assert_eq!(kinds(&issues), vec![IssueKind::SuctionCup]);
        assert_eq!((issues[0].first_layer, issues[0].last_layer), (1, 2));
    }
}
