//! Automatic supports: tapered tips on tree branches or straight columns,
//! standing on a raft or on the model itself.
//!
//! How points are chosen:
//!
//! 1. Vertical rays on a square grid (`spacing_mm`) are cast through the
//!    model. Wherever a ray leaves a downward-facing surface that is flatter
//!    than `overhang_angle_deg`, that spot gets a support.
//! 2. Every vertex that is lower than all of its neighbours (a local
//!    minimum, such as the tip of a downward spike) gets a support, since
//!    the grid can miss it and it would otherwise start as an island.
//!
//! Each support goes straight down to the first surface below it: the raft,
//! or an upward-facing part of the model (then it ends in a small tip on the
//! model, which leaves a mark there).
//!
//! Made to come off cleanly:
//!
//! - Tips are thin where they touch (0.35 mm) and only reach 0.12 mm into
//!   the model, so they snap at the contact and leave a small mark.
//! - On a sloped face the tip leans to meet the surface head-on (up to 30°
//!   from vertical), so it breaks off square instead of tearing a sliver.
//! - Low points, which carry the most pull early in the print, get a
//!   bigger tip (0.5 mm).
//! - The raft's edge is undercut (it flares out at 45° from a smaller
//!   footprint), so there's a gap at the plate for a scraper to slip into.
//!
//! With `tree` on, supports that go to the raft lean in and merge pairwise
//! into thicker trunks wherever the branches stay clear of the model.
//!
//! What this does not do: moving supports off the model to avoid marks,
//! or bracing between columns. Defaults are
//! starting points from slicer guides, not values tested on this printer.

use crate::mesh::{Mesh, Triangle, Vec3};

#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(default))]
pub struct SupportParams {
    /// Gap between the raft and the model's lowest point, in mm.
    pub lift_mm: f32,
    /// Downward faces flatter than this (degrees from horizontal) get
    /// supports.
    pub overhang_angle_deg: f32,
    /// Grid spacing for support points, in mm.
    pub spacing_mm: f32,
    /// Diameter where the tip touches the model.
    pub tip_diameter_mm: f32,
    /// Tip diameter at low points (local minima), which hold the most.
    pub strong_tip_diameter_mm: f32,
    /// Lean tips to meet sloped faces head-on (at most 30° from vertical).
    pub angled_tips: bool,
    /// Length of the tapered tip.
    pub tip_length_mm: f32,
    /// How far the tip reaches into the model so it bonds.
    pub tip_penetration_mm: f32,
    pub column_diameter_mm: f32,
    /// Diameter of the cone foot on the raft.
    pub foot_diameter_mm: f32,
    pub foot_height_mm: f32,
    /// Raft thickness; 0 for no raft.
    pub raft_mm: f32,
    /// How far the raft extends past the outermost feet.
    pub raft_margin_mm: f32,
    /// Height of the raft's undercut edge; 0 for a square edge.
    pub raft_lip_mm: f32,
    /// Angle of the undercut from the plate. It overhangs while printing,
    /// so it can't be much flatter than 45°.
    pub raft_lip_angle_deg: f32,
    /// Also support local minima (downward points).
    pub support_minima: bool,
    /// Tree supports: neighbouring supports lean in and merge into shared
    /// trunks, so there are fewer feet and they come off in clusters.
    pub tree: bool,
    /// Most a branch leans from vertical, degrees.
    pub branch_angle_deg: f32,
    /// Furthest apart (sideways) two branches can be and still merge.
    pub max_branch_mm: f32,
    /// Trunks thicken as branches join, up to this diameter.
    pub trunk_diameter_mm: f32,
    /// Sides of each column's cross-section.
    pub segments: usize,
}

impl Default for SupportParams {
    fn default() -> Self {
        Self {
            lift_mm: 5.0,
            overhang_angle_deg: 45.0,
            spacing_mm: 3.0,
            tip_diameter_mm: 0.35,
            strong_tip_diameter_mm: 0.5,
            angled_tips: true,
            tip_length_mm: 2.0,
            tip_penetration_mm: 0.12,
            column_diameter_mm: 1.2,
            foot_diameter_mm: 2.5,
            foot_height_mm: 1.0,
            raft_mm: 1.5,
            raft_margin_mm: 2.0,
            raft_lip_mm: 0.8,
            raft_lip_angle_deg: 45.0,
            support_minima: true,
            tree: true,
            branch_angle_deg: 35.0,
            max_branch_mm: 10.0,
            trunk_diameter_mm: 3.0,
            segments: 8,
        }
    }
}

impl SupportParams {
    pub fn validate(&self) -> Result<(), String> {
        let pos = [
            ("spacing", self.spacing_mm),
            ("tip diameter", self.tip_diameter_mm),
            ("low-point tip diameter", self.strong_tip_diameter_mm),
            ("column diameter", self.column_diameter_mm),
            ("foot diameter", self.foot_diameter_mm),
        ];
        for (n, v) in pos {
            if !(v > 0.0) {
                return Err(format!("support {n} must be positive"));
            }
        }
        if self.spacing_mm < 0.5 {
            return Err("support spacing below 0.5 mm would make a solid block".into());
        }
        if !(0.0..90.0).contains(&self.overhang_angle_deg) {
            return Err("overhang angle must be 0..90 degrees".into());
        }
        if !(30.0..=90.0).contains(&self.raft_lip_angle_deg) {
            return Err("raft lip angle must be 30..90 degrees (flatter won't print)".into());
        }
        if self.lift_mm < 0.0 || self.raft_mm < 0.0 || self.raft_lip_mm < 0.0 {
            return Err("lift and raft thickness can't be negative".into());
        }
        Ok(())
    }

    /// Z where the model's lowest point ends up.
    pub fn model_floor(&self) -> f32 {
        if self.lift_mm > 0.0 {
            self.raft_mm + self.lift_mm
        } else {
            0.0
        }
    }

    /// Z of the surface feet stand on (raft top, or the plate).
    fn ground(&self) -> f32 {
        if self.lift_mm > 0.0 {
            self.raft_mm
        } else {
            0.0
        }
    }
}

/// One support: from `top` on the model down to `bottom_z`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SupportPoint {
    /// Where the tip touches the model.
    pub top: Vec3,
    pub bottom_z: f32,
    /// `true` when it ends on the model rather than the raft or plate.
    pub on_model: bool,
    /// Unit direction from `top` along the tip, away from the model.
    /// Straight down unless the tip leans to meet a sloped face.
    pub tip_dir: Vec3,
    /// Uses the bigger low-point tip.
    pub strong: bool,
}

const DOWN: Vec3 = [0.0, 0.0, -1.0];
/// Most a tip leans from vertical.
const MAX_TIP_LEAN_DEG: f32 = 30.0;

impl SupportPoint {
    /// Where the tip ends and the vertical column starts.
    pub fn column_top(&self, tip_length: f32) -> Vec3 {
        if self.tip_dir == DOWN {
            return [self.top[0], self.top[1], self.top[2] - tip_length];
        }
        let d = self.tip_dir;
        [
            self.top[0] + d[0] * tip_length,
            self.top[1] + d[1] * tip_length,
            self.top[2] + d[2] * tip_length,
        ]
    }
}

/// Result of [`generate`].
#[derive(Debug, Clone, Default)]
pub struct Supported {
    /// The model, raised by the lift.
    pub model: Mesh,
    /// Columns and raft, as one mesh.
    pub supports: Mesh,
    pub points: Vec<SupportPoint>,
}

impl Supported {
    /// Model plus supports, ready to slice.
    pub fn combined(&self) -> Mesh {
        let mut m = self.model.clone();
        m.append(self.supports.clone());
        m
    }
}

/// Raises `placed` (resting on Z = 0) by the lift and adds supports.
pub fn generate(placed: &Mesh, p: &SupportParams) -> Result<Supported, String> {
    generate_edited(placed, p, &[], &[])
}

/// Like [`generate`], with hand edits. Points are in `placed` coordinates
/// (before the lift). Automatic supports whose tip is within 1.5 mm of a
/// `removed` point are dropped; each `added` point gets a support going
/// straight down from it.
pub fn generate_edited(
    placed: &Mesh,
    p: &SupportParams,
    added: &[Vec3],
    removed: &[Vec3],
) -> Result<Supported, String> {
    p.validate()?;
    let mut model = placed.clone();
    let Some(b) = model.bounds() else {
        return Ok(Supported::default());
    };
    let dz = p.model_floor() - b.min[2];
    model.translate([0.0, 0.0, dz]);
    let mut points = find_points(&model, p);
    let near = |a: Vec3, b: Vec3| {
        let d = [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
        d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1.5 * 1.5
    };
    points.retain(|q| !removed.iter().any(|r| near(q.top, [r[0], r[1], r[2] + dz])));
    if !added.is_empty() {
        let index = RayIndex::new(&model, p.spacing_mm.max(1.0), p.overhang_angle_deg);
        for a in added {
            let top = [a[0], a[1], a[2] + dz];
            points.extend(index.place(top, None, false, p));
        }
    }
    let supports = if p.tree && p.lift_mm > 0.0 {
        let index = RayIndex::new(&model, p.spacing_mm.max(1.0), p.overhang_angle_deg);
        build_tree(&index, &points, p)
    } else {
        build(&points, p)
    };
    Ok(Supported {
        model,
        supports,
        points,
    })
}

/// A ray hit: height and whether the surface faces down.
#[derive(Debug, Clone, Copy)]
struct Hit {
    z: f32,
    down: bool,
    needs_support: bool,
    /// Outward unit normal.
    n: Vec3,
}

/// Triangles binned into an XY grid, for vertical ray casts.
struct RayIndex<'a> {
    tris: &'a [Triangle],
    /// +1 if the mesh winds outward, -1 if inward.
    orient: f32,
    min: [f32; 2],
    cell: f32,
    w: usize,
    h: usize,
    bins: Vec<Vec<u32>>,
    /// Faces whose normal's z is below this (as a fraction of the normal's
    /// length) need support.
    cos_limit: f32,
}

impl<'a> RayIndex<'a> {
    fn new(mesh: &'a Mesh, cell: f32, overhang_angle_deg: f32) -> Self {
        let b = mesh.bounds().expect("non-empty mesh");
        let w = (((b.max[0] - b.min[0]) / cell).ceil() as usize).max(1);
        let h = (((b.max[1] - b.min[1]) / cell).ceil() as usize).max(1);
        let mut bins = vec![Vec::new(); w * h];
        let ix = |v: f32, lo: f32, n: usize| (((v - lo) / cell) as usize).min(n - 1);
        for (i, t) in mesh.triangles.iter().enumerate() {
            let (x0, x1) = minmax(t.iter().map(|v| v[0]));
            let (y0, y1) = minmax(t.iter().map(|v| v[1]));
            for cy in ix(y0, b.min[1], h)..=ix(y1, b.min[1], h) {
                for cx in ix(x0, b.min[0], w)..=ix(x1, b.min[0], w) {
                    bins[cy * w + cx].push(i as u32);
                }
            }
        }
        let orient = if signed_volume(mesh) < 0.0 { -1.0 } else { 1.0 };
        // A face tilted `a` from horizontal has |n_z| = cos(a).
        let cos_limit = overhang_angle_deg.to_radians().cos();
        Self {
            tris: &mesh.triangles,
            orient,
            min: [b.min[0], b.min[1]],
            cell,
            w,
            h,
            bins,
            cos_limit,
        }
    }

    /// All surfaces the vertical line at (x, y) crosses, sorted by z.
    fn hits(&self, x: f32, y: f32) -> Vec<Hit> {
        let cx = ((x - self.min[0]) / self.cell).floor();
        let cy = ((y - self.min[1]) / self.cell).floor();
        if cx < 0.0 || cy < 0.0 || cx as usize >= self.w || cy as usize >= self.h {
            return Vec::new();
        }
        let mut hits = Vec::new();
        for &ti in &self.bins[cy as usize * self.w + cx as usize] {
            let t = &self.tris[ti as usize];
            let Some(z) = vertical_hit(t, x, y) else {
                continue;
            };
            let n = normal(t);
            let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
            if len == 0.0 {
                continue;
            }
            let nz = n[2] * self.orient / len;
            let k = self.orient / len;
            hits.push(Hit {
                z,
                down: nz < 0.0,
                needs_support: -nz > self.cos_limit,
                n: [n[0] * k, n[1] * k, n[2] * k],
            });
        }
        hits.sort_by(|a, b| a.z.total_cmp(&b.z));
        // A ray through a shared edge hits both triangles: merge near-equal.
        hits.dedup_by(|b, a| (a.z - b.z).abs() < 1e-4 && a.down == b.down);
        hits
    }

    /// The first surface below `z` at (x, y): (its z, whether it's model).
    fn landing(&self, x: f32, y: f32, z: f32, ground: f32) -> (f32, bool) {
        let hits = self.hits(x, y);
        hits.iter()
            .rev()
            .find(|h| h.z < z - 1e-3 && !h.down)
            .filter(|h| h.z > ground)
            .map(|h| (h.z, true))
            .unwrap_or((ground, false))
    }

    /// Whether (x, y, z) is outside the model: the nearest surface above it
    /// faces down, or there is none.
    fn outside(&self, x: f32, y: f32, z: f32) -> bool {
        self.hits(x, y)
            .iter()
            .find(|h| h.z > z)
            .is_none_or(|h| h.down)
    }

    /// A support touching the model at `contact`. With `normal` (the
    /// surface's outward normal) and angled tips on, the tip leans to meet
    /// the surface head-on if there's room; otherwise it goes straight down.
    /// `None` when there's no room for a support at all.
    fn place(
        &self,
        contact: Vec3,
        normal: Option<Vec3>,
        strong: bool,
        p: &SupportParams,
    ) -> Option<SupportPoint> {
        let ground = p.ground();
        let [x, y, z] = contact;
        let vertical = || {
            let (bz, on_model) = self.landing(x, y, z, ground);
            (z - bz >= 0.3).then_some(SupportPoint {
                top: contact,
                bottom_z: bz,
                on_model,
                tip_dir: DOWN,
                strong,
            })
        };
        let Some(n) = normal.filter(|_| p.angled_tips) else {
            return vertical();
        };
        let d = lean(n);
        if d == DOWN {
            return vertical();
        }
        let l = p.tip_length_mm;
        let at = |t: f32| [x + d[0] * t, y + d[1] * t, z + d[2] * t];
        let (e, mid) = (at(l), at(l * 0.5));
        // The tip's end and middle, and a ring around the column's top
        // (so the column doesn't graze the model and close off a pocket),
        // must all be clear of the model.
        let r = p.column_diameter_mm;
        let ring = [[r, 0.0], [-r, 0.0], [0.0, r], [0.0, -r]];
        let clear = e[2] > ground + 0.3
            && self.outside(e[0], e[1], e[2])
            && self.outside(mid[0], mid[1], mid[2])
            && ring
                .iter()
                .all(|o| self.outside(e[0] + o[0], e[1] + o[1], e[2] + r));
        if !clear {
            return vertical();
        }
        let (bz, on_model) = self.landing(e[0], e[1], e[2], ground);
        if e[2] - bz < 0.3 {
            return vertical();
        }
        Some(SupportPoint {
            top: contact,
            bottom_z: bz,
            on_model,
            tip_dir: d,
            strong,
        })
    }
}

/// Direction for a tip leaving a downward face with outward normal `n`:
/// along the normal, leaning at most [`MAX_TIP_LEAN_DEG`] from vertical.
fn lean(n: Vec3) -> Vec3 {
    let h = (n[0] * n[0] + n[1] * n[1]).sqrt();
    let a = (-n[2]).clamp(-1.0, 1.0).acos();
    if h < 1e-6 || a < 1f32.to_radians() || n[2] >= 0.0 {
        return DOWN;
    }
    let a = a.min(MAX_TIP_LEAN_DEG.to_radians());
    let (sa, ca) = a.sin_cos();
    [n[0] / h * sa, n[1] / h * sa, -ca]
}

fn minmax(it: impl Iterator<Item = f32>) -> (f32, f32) {
    it.fold((f32::INFINITY, f32::NEG_INFINITY), |(a, b), v| {
        (a.min(v), b.max(v))
    })
}

fn signed_volume(m: &Mesh) -> f64 {
    m.triangles
        .iter()
        .map(|[a, b, c]| {
            let f = |v: &Vec3| [v[0] as f64, v[1] as f64, v[2] as f64];
            let (a, b, c) = (f(a), f(b), f(c));
            (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                + a[2] * (b[0] * c[1] - b[1] * c[0]))
                / 6.0
        })
        .sum()
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

/// Height where the vertical line at (x, y) meets triangle `t`, if it does.
/// Edges are half-open so a line through a shared edge usually hits once.
fn vertical_hit(t: &Triangle, x: f32, y: f32) -> Option<f32> {
    let (ax, ay) = (t[0][0] as f64, t[0][1] as f64);
    let (bx, by) = (t[1][0] as f64, t[1][1] as f64);
    let (cx, cy) = (t[2][0] as f64, t[2][1] as f64);
    let (x, y) = (x as f64, y as f64);
    let d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if d.abs() < 1e-12 {
        return None; // vertical face
    }
    let l0 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d;
    let l1 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d;
    let l2 = 1.0 - l0 - l1;
    let eps = -1e-9;
    if l0 < eps || l1 < eps || l2 < eps {
        return None;
    }
    Some((l0 * t[0][2] as f64 + l1 * t[1][2] as f64 + l2 * t[2][2] as f64) as f32)
}

/// Picks support points on a model that is already raised.
pub fn find_points(model: &Mesh, p: &SupportParams) -> Vec<SupportPoint> {
    let Some(b) = model.bounds() else {
        return Vec::new();
    };
    let index = RayIndex::new(model, p.spacing_mm.max(1.0), p.overhang_angle_deg);
    let ground = p.ground();
    let mut points = Vec::new();

    // 1. Grid rays. Offset by half a step so a box's edges aren't sampled.
    let s = p.spacing_mm;
    let nx = ((b.max[0] - b.min[0]) / s).floor() as i64;
    let ny = ((b.max[1] - b.min[1]) / s).floor() as i64;
    let x0 = (b.min[0] + b.max[0]) / 2.0 - nx as f32 * s / 2.0;
    let y0 = (b.min[1] + b.max[1]) / 2.0 - ny as f32 * s / 2.0;
    for j in 0..=ny {
        for i in 0..=nx {
            let (x, y) = (x0 + i as f32 * s, y0 + j as f32 * s);
            let hits = index.hits(x, y);
            for h in &hits {
                if !(h.down && h.needs_support) || h.z <= ground + 1e-3 {
                    continue;
                }
                // None when the gap below would just cure solid.
                points.extend(index.place([x, y, h.z], Some(h.n), false, p));
            }
        }
    }

    // 2. Local minima.
    if p.support_minima {
        let min_gap = s * 0.5;
        for v in local_minima(model) {
            if v[2] <= ground + 1e-3 {
                continue; // resting on the plate
            }
            let near = points.iter().any(|q: &SupportPoint| {
                let (dx, dy) = (q.top[0] - v[0], q.top[1] - v[1]);
                dx * dx + dy * dy < min_gap * min_gap && (q.top[2] - v[2]).abs() < min_gap
            });
            if near {
                continue;
            }
            points.extend(index.place(v, None, true, p));
        }
    }

    // 3. Coverage: any overhang face with no support near it (a small flat
    // bottom the grid stepped over) gets one at its centroid.
    let reach = s * 0.75;
    let mut near = PointHash::new(reach);
    for (i, q) in points.iter().enumerate() {
        near.insert(q.top, i);
    }
    for t in &model.triangles {
        let n = normal(t);
        let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
        if len == 0.0 {
            continue;
        }
        let nz = n[2] * index.orient / len;
        if !(-nz > index.cos_limit) {
            continue;
        }
        let c = [
            (t[0][0] + t[1][0] + t[2][0]) / 3.0,
            (t[0][1] + t[1][1] + t[2][1]) / 3.0,
            (t[0][2] + t[1][2] + t[2][2]) / 3.0,
        ];
        if c[2] <= ground + 1e-3 || near.any_within(c, reach, &points) {
            continue;
        }
        let k = index.orient / len;
        let nn = [n[0] * k, n[1] * k, n[2] * k];
        if let Some(sp) = index.place(c, Some(nn), false, p) {
            near.insert(c, points.len());
            points.push(sp);
        }
    }
    points
}

/// A spatial hash of support points for "is there one nearby?" queries.
struct PointHash {
    cell: f32,
    map: std::collections::HashMap<(i64, i64), Vec<usize>>,
}

impl PointHash {
    fn new(cell: f32) -> Self {
        Self {
            cell,
            map: Default::default(),
        }
    }

    fn key(&self, x: f32, y: f32) -> (i64, i64) {
        (
            (x / self.cell).floor() as i64,
            (y / self.cell).floor() as i64,
        )
    }

    fn insert(&mut self, v: Vec3, i: usize) {
        let k = self.key(v[0], v[1]);
        self.map.entry(k).or_default().push(i);
    }

    /// Whether a point lies within `r` in XY and within `r` in Z of `v`.
    fn any_within(&self, v: Vec3, r: f32, pts: &[SupportPoint]) -> bool {
        let (kx, ky) = self.key(v[0], v[1]);
        for dy in -1..=1 {
            for dx in -1..=1 {
                if let Some(ids) = self.map.get(&(kx + dx, ky + dy)) {
                    for &i in ids {
                        let q = pts[i].top;
                        let (ex, ey, ez) = (q[0] - v[0], q[1] - v[1], q[2] - v[2]);
                        if ex * ex + ey * ey <= r * r && ez.abs() <= r {
                            return true;
                        }
                    }
                }
            }
        }
        false
    }
}

/// Vertices strictly lower than every vertex they share an edge with.
fn local_minima(m: &Mesh) -> Vec<Vec3> {
    use std::collections::HashMap;
    let key = |v: &Vec3| {
        let q = |c: f32| (c * 1e4).round() as i64;
        (q(v[0]), q(v[1]), q(v[2]))
    };
    let mut ids: HashMap<(i64, i64, i64), usize> = HashMap::new();
    let mut verts: Vec<Vec3> = Vec::new();
    let mut is_min: Vec<bool> = Vec::new();
    let mut id = |v: &Vec3, verts: &mut Vec<Vec3>, is_min: &mut Vec<bool>| {
        *ids.entry(key(v)).or_insert_with(|| {
            verts.push(*v);
            is_min.push(true);
            verts.len() - 1
        })
    };
    for t in &m.triangles {
        let i = [
            id(&t[0], &mut verts, &mut is_min),
            id(&t[1], &mut verts, &mut is_min),
            id(&t[2], &mut verts, &mut is_min),
        ];
        for a in 0..3 {
            for b in 0..3 {
                if a != b && verts[i[b]][2] <= verts[i[a]][2] + 1e-4 {
                    is_min[i[a]] = false;
                }
            }
        }
    }
    verts
        .into_iter()
        .zip(is_min)
        .filter_map(|(v, m)| m.then_some(v))
        .collect()
}

/// Builds the support meshes and the raft.
pub fn build(points: &[SupportPoint], p: &SupportParams) -> Mesh {
    let mut out = Mesh::default();
    let (rc, rf) = (p.column_diameter_mm / 2.0, p.foot_diameter_mm / 2.0);
    let rt_plain = p.tip_diameter_mm / 2.0;
    let on_raft = p.raft_mm > 0.0 && p.lift_mm > 0.0;
    let n = p.segments.max(3);
    let mut feet: Vec<[f32; 2]> = Vec::new();
    for sp in points {
        let rt = if sp.strong {
            p.strong_tip_diameter_mm / 2.0
        } else {
            rt_plain
        };
        let zb = sp.bottom_z;
        // Bury the foot slightly in the raft (or plate) so it bonds.
        let foot_bottom = if on_raft { zb - p.raft_mm.min(0.2) } else { zb };
        // Profile rings from top to bottom: (z, radius).
        let mut rings: Vec<(f32, f32)> = Vec::new();
        let [x, y] = if sp.tip_dir == DOWN {
            let [x, y, zt] = sp.top;
            let top = zt + p.tip_penetration_mm;
            let len = zt - zb;
            if sp.on_model {
                // Tip at both ends.
                if len <= 2.0 * p.tip_length_mm {
                    let mid = (zt + zb) / 2.0;
                    let rm = rt + (rc - rt) * ((len / 2.0) / p.tip_length_mm).min(1.0);
                    rings.extend([(top, rt), (mid, rm), (zb - p.tip_penetration_mm, rt_plain)]);
                } else {
                    rings.extend([
                        (top, rt),
                        (zt - p.tip_length_mm, rc),
                        (zb + p.tip_length_mm, rc),
                        (zb - p.tip_penetration_mm, rt_plain),
                    ]);
                }
            } else if len <= p.tip_length_mm + p.foot_height_mm {
                let k = len / (p.tip_length_mm + p.foot_height_mm);
                let mid = zb + p.foot_height_mm * k;
                rings.extend([(top, rt), (mid, rc), (foot_bottom, rf.max(rc))]);
            } else {
                rings.extend([
                    (top, rt),
                    (zt - p.tip_length_mm, rc),
                    (zb + p.foot_height_mm, rc),
                    (foot_bottom, rf.max(rc)),
                ]);
            }
            [x, y]
        } else {
            // A leaning tip, then a vertical column from where it ends.
            let d = sp.tip_dir;
            let start = [
                sp.top[0] - d[0] * p.tip_penetration_mm,
                sp.top[1] - d[1] * p.tip_penetration_mm,
                sp.top[2] - d[2] * p.tip_penetration_mm,
            ];
            let e = sp.column_top(p.tip_length_mm);
            let axis = [start[0] - e[0], start[1] - e[1], start[2] - e[2]];
            let l = (axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]).sqrt();
            let up = [axis[0] / l, axis[1] / l, axis[2] / l];
            out.append(lathe_axis(e, up, &[(l, rt), (0.0, rc)], n));
            let len = e[2] - zb;
            if sp.on_model {
                if len <= p.tip_length_mm {
                    rings.extend([(e[2], rc), (zb - p.tip_penetration_mm, rt_plain)]);
                } else {
                    rings.extend([
                        (e[2], rc),
                        (zb + p.tip_length_mm, rc),
                        (zb - p.tip_penetration_mm, rt_plain),
                    ]);
                }
            } else if len <= p.foot_height_mm {
                rings.extend([(e[2], rc), (foot_bottom, rf.max(rc))]);
            } else {
                rings.extend([
                    (e[2], rc),
                    (zb + p.foot_height_mm, rc),
                    (foot_bottom, rf.max(rc)),
                ]);
            }
            [e[0], e[1]]
        };
        if !sp.on_model {
            feet.push([x, y]);
        }
        out.append(lathe([x, y], &rings, n));
    }
    if on_raft && !feet.is_empty() {
        let (x0, x1) = minmax(feet.iter().map(|f| f[0]));
        let (y0, y1) = minmax(feet.iter().map(|f| f[1]));
        let lip = p.raft_lip_mm.min(p.raft_mm);
        let inset = if p.raft_lip_angle_deg >= 89.9 {
            0.0
        } else {
            lip / p.raft_lip_angle_deg.to_radians().tan()
        };
        let m = p.raft_margin_mm + rf;
        out.append(raft(
            [x0 - m, y0 - m],
            [x1 + m, y1 + m],
            p.raft_mm,
            lip,
            inset,
        ));
    }
    out
}

/// A node in a support tree: where a branch ends, or where two meet.
struct TreeNode {
    p: Vec3,
    r: f32,
    parent: Option<usize>,
    alive: bool,
}

/// Where two branches tipped at `a` and `b` would meet, leaning at most
/// `tan` (tangent of the branch angle) from vertical.
fn merge_point(a: Vec3, b: Vec3, tan: f32) -> Vec3 {
    let (a, b) = if a[2] >= b[2] { (a, b) } else { (b, a) };
    let d = ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)).sqrt();
    if d < 1e-6 {
        return [a[0], a[1], b[2] - 0.5];
    }
    // Sideways distance from a, chosen so both branches lean equally far.
    let ha = ((d + (a[2] - b[2]) * tan) / 2.0).clamp(0.0, d);
    let hb = d - ha;
    let z = (a[2] - ha / tan).min(b[2] - hb / tan).min(b[2] - 0.5);
    let t = ha / d;
    [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, z]
}

impl RayIndex<'_> {
    /// Whether a rod of radius `r` from `a` to `b` stays clear of the model.
    fn rod_clear(&self, a: Vec3, b: Vec3, r: f32) -> bool {
        let d = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
        let len = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
        let n = (len / 0.5).ceil().max(1.0) as usize;
        let o = r + 0.3;
        (1..=n).all(|k| {
            let t = k as f32 / n as f32;
            let [x, y, z] = [a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t];
            [[0.0, 0.0], [o, 0.0], [-o, 0.0], [0.0, o], [0.0, -o]]
                .iter()
                .all(|q| self.outside(x + q[0], y + q[1], z))
        })
    }
}

/// Tree supports: supports that go to the raft lean in and merge pairwise
/// into thicker trunks wherever the branches stay clear of the model.
/// Supports that end on the model stay straight.
fn build_tree(index: &RayIndex, points: &[SupportPoint], p: &SupportParams) -> Mesh {
    use std::cmp::Reverse;
    use std::collections::{BinaryHeap, HashMap};

    let (on_model, to_raft): (Vec<SupportPoint>, Vec<SupportPoint>) =
        points.iter().partition(|q| q.on_model);
    let mut out = build(&on_model, p);
    let n = p.segments.max(3);
    let ground = p.ground();
    let r_leaf = p.column_diameter_mm / 2.0;
    let r_max = (p.trunk_diameter_mm / 2.0).max(r_leaf);
    let rf = p.foot_diameter_mm / 2.0;
    let tan = p.branch_angle_deg.clamp(5.0, 60.0).to_radians().tan();
    let min_z = ground + p.foot_height_mm + 0.5;

    // Tips, and a leaf node where each tip ends.
    let mut nodes: Vec<TreeNode> = Vec::new();
    for sp in &to_raft {
        let rt = if sp.strong {
            p.strong_tip_diameter_mm / 2.0
        } else {
            p.tip_diameter_mm / 2.0
        };
        let d = sp.tip_dir;
        let start = [
            sp.top[0] - d[0] * p.tip_penetration_mm,
            sp.top[1] - d[1] * p.tip_penetration_mm,
            sp.top[2] - d[2] * p.tip_penetration_mm,
        ];
        let e = sp.column_top(p.tip_length_mm);
        out.append(rod(e, start, r_leaf, rt, n));
        nodes.push(TreeNode {
            p: e,
            r: r_leaf,
            parent: None,
            alive: true,
        });
    }

    // Merge the pair that can meet highest, again and again.
    let cell = p.max_branch_mm.max(1.0);
    let key = |v: Vec3| ((v[0] / cell).floor() as i64, (v[1] / cell).floor() as i64);
    let mut grid: HashMap<(i64, i64), Vec<usize>> = HashMap::new();
    for (i, nd) in nodes.iter().enumerate() {
        grid.entry(key(nd.p)).or_default().push(i);
    }
    let mut heap: BinaryHeap<(i64, Reverse<usize>, Reverse<usize>)> = BinaryHeap::new();
    let candidates = |i: usize, nodes: &[TreeNode], grid: &HashMap<(i64, i64), Vec<usize>>| {
        let a = nodes[i].p;
        let (kx, ky) = key(a);
        let mut found = Vec::new();
        if nodes[i].r >= r_max - 1e-4 {
            return found;
        }
        for dy in -1..=1 {
            for dx in -1..=1 {
                for &j in grid.get(&(kx + dx, ky + dy)).into_iter().flatten() {
                    // A full-thickness trunk takes no more branches, so
                    // trees stay small and don't pile up into a knot.
                    if j == i || !nodes[j].alive || nodes[j].r >= r_max - 1e-4 {
                        continue;
                    }
                    let b = nodes[j].p;
                    if (a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) > cell * cell {
                        continue;
                    }
                    let m = merge_point(a, b, tan);
                    if m[2] >= min_z {
                        found.push(((m[2] * 1000.0) as i64, Reverse(i.min(j)), Reverse(i.max(j))));
                    }
                }
            }
        }
        found
    };
    for i in 0..nodes.len() {
        for c in candidates(i, &nodes, &grid) {
            if c.1 .0 == i {
                heap.push(c);
            }
        }
    }
    while let Some((_, Reverse(i), Reverse(j))) = heap.pop() {
        if !nodes[i].alive || !nodes[j].alive {
            continue;
        }
        let (a, b) = (nodes[i].p, nodes[j].p);
        let m = merge_point(a, b, tan);
        let r = (nodes[i].r.powi(2) + nodes[j].r.powi(2)).sqrt().min(r_max);
        let clear = !index.landing(m[0], m[1], m[2], ground).1
            && index.rod_clear(a, m, nodes[i].r)
            && index.rod_clear(b, m, nodes[j].r)
            && index.rod_clear(m, [m[0], m[1], ground], r);
        if !clear {
            continue;
        }
        let k = nodes.len();
        nodes.push(TreeNode {
            p: m,
            r,
            parent: None,
            alive: true,
        });
        for c in [i, j] {
            nodes[c].alive = false;
            nodes[c].parent = Some(k);
        }
        grid.entry(key(m)).or_default().push(k);
        heap.extend(candidates(k, &nodes, &grid));
    }

    // Branches, joints and trunks.
    let on_raft = p.raft_mm > 0.0;
    let mut feet = Vec::new();
    for nd in &nodes {
        out.append(ball(nd.p, nd.r, n));
        match nd.parent {
            Some(q) => out.append(rod(nodes[q].p, nd.p, nodes[q].r, nd.r, n)),
            None => {
                let [x, y, z] = nd.p;
                let zb = ground;
                let bottom = if on_raft { zb - p.raft_mm.min(0.2) } else { zb };
                let rfoot = rf.max(nd.r * 1.5);
                let rings: Vec<(f32, f32)> = if z - zb > p.foot_height_mm {
                    vec![(z, nd.r), (zb + p.foot_height_mm, nd.r), (bottom, rfoot)]
                } else {
                    vec![(z, nd.r), (bottom, rfoot)]
                };
                out.append(lathe([x, y], &rings, n));
                feet.push(([x, y], rfoot));
            }
        }
    }
    if on_raft && !feet.is_empty() {
        let rmax = feet.iter().map(|f| f.1).fold(0.0, f32::max);
        let (x0, x1) = minmax(feet.iter().map(|f| f.0[0]));
        let (y0, y1) = minmax(feet.iter().map(|f| f.0[1]));
        let lip = p.raft_lip_mm.min(p.raft_mm);
        let inset = if p.raft_lip_angle_deg >= 89.9 {
            0.0
        } else {
            lip / p.raft_lip_angle_deg.to_radians().tan()
        };
        let m = p.raft_margin_mm + rmax;
        out.append(raft(
            [x0 - m, y0 - m],
            [x1 + m, y1 + m],
            p.raft_mm,
            lip,
            inset,
        ));
    }
    out
}

/// A tapered rod from `lo` (radius `rl`) to `hi` (radius `rh`).
fn rod(lo: Vec3, hi: Vec3, rl: f32, rh: f32, n: usize) -> Mesh {
    let d = [hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]];
    let len = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
    if len < 1e-5 {
        return Mesh::default();
    }
    let up = [d[0] / len, d[1] / len, d[2] / len];
    lathe_axis(lo, up, &[(len, rh), (0.0, rl)], n)
}

/// A rough ball, to round off joints.
fn ball(c: Vec3, r: f32, n: usize) -> Mesh {
    let rings: Vec<(f32, f32)> = [1.0f32, 0.7, 0.0, -0.7, -1.0]
        .iter()
        .map(|&k| (c[2] + k * r, r * (1.0 - k * k).sqrt().max(0.05)))
        .collect();
    lathe([c[0], c[1]], &rings, n)
}

/// A raft over `min`..`max`, `t` thick. Its edge is undercut: the
/// bottom is inset by `inset`, and the sides flare out to the full size at
/// height `lip`, so there's a gap at the plate for a scraper to slip into.
fn raft(min: [f32; 2], max: [f32; 2], t: f32, lip: f32, inset: f32) -> Mesh {
    let inset = inset
        .min((max[0] - min[0]) / 2.0 - 0.5)
        .min((max[1] - min[1]) / 2.0 - 0.5);
    if inset <= 1e-3 || lip <= 1e-3 {
        return Mesh::cuboid([min[0], min[1], 0.0], [max[0], max[1], t]);
    }
    let lip = lip.min(t);
    let rect = |i: f32, z: f32| -> [Vec3; 4] {
        [
            [min[0] + i, min[1] + i, z],
            [max[0] - i, min[1] + i, z],
            [max[0] - i, max[1] - i, z],
            [min[0] + i, max[1] - i, z],
        ]
    };
    let mut rings = vec![rect(inset, 0.0), rect(0.0, lip)];
    if t > lip + 1e-4 {
        rings.push(rect(0.0, t));
    }
    let mut tris = Vec::new();
    for w in rings.windows(2) {
        let (l, u) = (&w[0], &w[1]);
        for k in 0..4 {
            let k1 = (k + 1) % 4;
            tris.push([l[k], l[k1], u[k1]]);
            tris.push([l[k], u[k1], u[k]]);
        }
    }
    let (b, t) = (&rings[0], &rings[rings.len() - 1]);
    tris.extend([[b[0], b[2], b[1]], [b[0], b[3], b[2]]]);
    tris.extend([[t[0], t[1], t[2]], [t[0], t[2], t[3]]]);
    Mesh::new(tris)
}

/// A closed solid of revolution around the vertical line at `c`. `rings`
/// are (z, radius) from top to bottom. Winding is outward.
fn lathe(c: [f32; 2], rings: &[(f32, f32)], n: usize) -> Mesh {
    lathe_axis([c[0], c[1], 0.0], [0.0, 0.0, 1.0], rings, n)
}

/// A closed solid of revolution around the line through `base` along the
/// unit vector `up`. `rings` are (distance along `up`, radius), in
/// decreasing distance. Winding is outward.
fn lathe_axis(base: Vec3, up: Vec3, rings: &[(f32, f32)], n: usize) -> Mesh {
    let cross = |a: Vec3, b: Vec3| -> Vec3 {
        [
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0],
        ]
    };
    // u, v with u x v = up, so angles run counter-clockwise seen from `up`.
    let helper = if up[2].abs() < 0.9 {
        [0.0, 0.0, 1.0]
    } else {
        [1.0, 0.0, 0.0]
    };
    let u = cross(up, helper);
    let ul = (u[0] * u[0] + u[1] * u[1] + u[2] * u[2]).sqrt();
    let u = [u[0] / ul, u[1] / ul, u[2] / ul];
    let v = cross(up, u);
    let at = |t: f32, r: f32, a: f32| -> Vec3 {
        let (s, c) = a.sin_cos();
        std::array::from_fn(|i| base[i] + up[i] * t + r * (u[i] * c + v[i] * s))
    };
    let ring = |(t, r): (f32, f32)| -> Vec<Vec3> {
        (0..n)
            .map(|k| at(t, r, std::f32::consts::TAU * k as f32 / n as f32))
            .collect()
    };
    let rs: Vec<Vec<Vec3>> = rings.iter().map(|&r| ring(r)).collect();
    let mut tris = Vec::new();
    for w in rs.windows(2) {
        let (hi, lo) = (&w[0], &w[1]);
        for k in 0..n {
            let k1 = (k + 1) % n;
            tris.push([lo[k], lo[k1], hi[k1]]);
            tris.push([lo[k], hi[k1], hi[k]]);
        }
    }
    let (top, bot) = (&rs[0], &rs[rs.len() - 1]);
    let tc = at(rings[0].0, 0.0, 0.0);
    let bc = at(rings[rings.len() - 1].0, 0.0, 0.0);
    for k in 0..n {
        let k1 = (k + 1) % n;
        tris.push([tc, top[k], top[k1]]);
        tris.push([bc, bot[k1], bot[k]]);
    }
    Mesh::new(tris)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hand_edits_add_and_remove_supports() {
        let m = Mesh::cuboid([-10.0, -10.0, 0.0], [10.0, 10.0, 5.0]);
        let p = SupportParams::default();
        let auto = generate(&m, &p).unwrap();
        let first = auto.points[0].top;
        let removed = [[first[0], first[1], first[2] - p.model_floor()]];
        let added = [[0.1, 0.2, 0.0]];
        let s = generate_edited(&m, &p, &added, &removed).unwrap();
        assert_eq!(s.points.len(), auto.points.len());
        assert!(!s.points.iter().any(|q| q.top == first));
        assert!(s
            .points
            .iter()
            .any(|q| q.top == [0.1, 0.2, p.model_floor()]));
    }

    #[test]
    fn lathe_is_closed_and_outward() {
        let m = lathe([0.0, 0.0], &[(2.0, 1.0), (0.0, 1.0)], 64);
        let v = signed_volume(&m);
        // A 64-gon cylinder of r=1, h=2 is close to 2*pi.
        assert!(v > 0.0, "inward winding: {v}");
        assert!((v - 2.0 * std::f64::consts::PI).abs() < 0.02, "{v}");
    }

    #[test]
    fn table_gets_supports_under_its_top_and_raft() {
        // A table: 20x20x2 top resting on four 2x2 legs 10 mm tall.
        let mut m = Mesh::cuboid([-10.0, -10.0, 10.0], [10.0, 10.0, 12.0]);
        for (x, y) in [(-10.0, -10.0), (8.0, -10.0), (-10.0, 8.0), (8.0, 8.0)] {
            m.append(Mesh::cuboid([x, y, 0.0], [x + 2.0, y + 2.0, 10.0]));
        }
        let p = SupportParams {
            spacing_mm: 4.0,
            ..Default::default()
        };
        let s = generate(&m, &p).unwrap();
        let floor = p.model_floor();
        assert_eq!(s.model.bounds().unwrap().min[2], floor);
        // The legs' bottoms (at the floor) and the underside of the top.
        let under_top: Vec<_> = s
            .points
            .iter()
            .filter(|q| (q.top[2] - (floor + 10.0)).abs() < 1e-3)
            .collect();
        let under_legs: Vec<_> = s
            .points
            .iter()
            .filter(|q| (q.top[2] - floor).abs() < 1e-3)
            .collect();
        assert!(under_top.len() >= 9, "{} under the top", under_top.len());
        assert!(
            under_top
                .iter()
                .all(|q| !q.on_model && q.bottom_z == p.raft_mm),
            "supports under the table top go to the raft"
        );
        assert!(!under_legs.is_empty());
        // Raft spans the feet and sits on the plate.
        let sb = s.supports.bounds().unwrap();
        assert_eq!(sb.min[2], 0.0);
        assert!(sb.min[0] < -10.0 && sb.max[0] > 10.0);
        assert!(signed_volume(&s.supports) > 0.0);
    }

    #[test]
    fn support_lands_on_model_below() {
        // An L-shelf: a base block and a ledge above it, sticking out over
        // the base so the gap is between two parts of the model.
        let mut m = Mesh::cuboid([-10.0, -5.0, 0.0], [10.0, 5.0, 2.0]); // base
        m.append(Mesh::cuboid([8.0, -5.0, 2.0], [10.0, 5.0, 12.0])); // post
        m.append(Mesh::cuboid([-10.0, -5.0, 12.0], [10.0, 5.0, 14.0])); // shelf
        let p = SupportParams {
            lift_mm: 0.0,
            spacing_mm: 3.0,
            ..Default::default()
        };
        let s = generate(&m, &p).unwrap();
        let shelf: Vec<_> = s
            .points
            .iter()
            .filter(|q| (q.top[2] - 12.0).abs() < 1e-3)
            .collect();
        assert!(!shelf.is_empty());
        assert!(shelf
            .iter()
            .all(|q| q.on_model && (q.bottom_z - 2.0).abs() < 1e-3));
        // Lift 0: the base sits on the plate and needs nothing.
        assert!(s.points.iter().all(|q| q.top[2] > 1.0));
    }

    #[test]
    fn steep_faces_need_no_support() {
        // An upside-down pyramid would need support; a cube standing on the
        // plate with lift 0 needs none.
        let m = Mesh::cuboid([-5.0, -5.0, 0.0], [5.0, 5.0, 10.0]);
        let p = SupportParams {
            lift_mm: 0.0,
            ..Default::default()
        };
        assert!(generate(&m, &p).unwrap().points.is_empty());
    }

    #[test]
    fn downward_spike_gets_a_minimum_support() {
        // A narrow downward-pointing tetra-like spike hanging under a block,
        // thinner than the grid spacing.
        let mut m = Mesh::cuboid([-10.0, -10.0, 10.0], [10.0, 10.0, 12.0]);
        let (a, b, c, tip) = (
            [0.0, 0.0, 10.0],
            [0.4, 0.0, 10.0],
            [0.2, 0.4, 10.0],
            [0.2, 0.15, 5.0],
        );
        m.append(Mesh::new(vec![
            [a, c, b],
            [a, b, tip],
            [b, c, tip],
            [c, a, tip],
        ]));
        let p = SupportParams {
            lift_mm: 0.0,
            spacing_mm: 7.0,
            ..Default::default()
        };
        let s = generate(&m, &p).unwrap();
        assert!(
            s.points.iter().any(|q| (q.top[2] - 5.0).abs() < 1e-3),
            "{:?}",
            s.points
        );
    }

    #[test]
    fn tilted_lathe_is_closed_and_outward() {
        let up = lean([0.5, 0.0, -0.866]);
        let m = lathe_axis([1.0, 2.0, 3.0], up, &[(2.0, 1.0), (0.0, 1.0)], 64);
        let v = signed_volume(&m);
        assert!((v - 2.0 * std::f64::consts::PI).abs() < 0.02, "{v}");
    }

    #[test]
    fn lean_follows_the_normal_up_to_30_degrees() {
        let (s20, c20) = 20f32.to_radians().sin_cos();
        let d = lean([s20, 0.0, -c20]);
        assert!(
            (d[0] - s20).abs() < 1e-5 && (d[2] + c20).abs() < 1e-5,
            "{d:?}"
        );
        let (s40, c40) = 40f32.to_radians().sin_cos();
        let d = lean([0.0, -s40, -c40]);
        let (s30, c30) = 30f32.to_radians().sin_cos();
        assert!(
            (d[1] + s30).abs() < 1e-5 && (d[2] + c30).abs() < 1e-5,
            "{d:?}"
        );
        assert_eq!(lean([0.0, 0.0, -1.0]), DOWN);
    }

    #[test]
    fn tips_lean_to_meet_a_sloped_underside() {
        let mut m = Mesh::cuboid([-10.0, -5.0, 0.0], [10.0, 5.0, 4.0]);
        m.rotate_xyz([0.0, 20.0, 0.0]);
        let p = SupportParams::default();
        let s = generate(&m, &p).unwrap();
        let leaning: Vec<_> = s.points.iter().filter(|q| q.tip_dir != DOWN).collect();
        assert!(
            leaning.len() >= 4,
            "{} leaning of {}",
            leaning.len(),
            s.points.len()
        );
        let (s20, c20) = 20f32.to_radians().sin_cos();
        let index = RayIndex::new(&s.model, 1.0, p.overhang_angle_deg);
        for q in &leaning {
            let d = q.tip_dir;
            assert!(
                (d[0].abs() - s20).abs() < 0.01 && d[1].abs() < 0.01,
                "{d:?}"
            );
            assert!((d[2] + c20).abs() < 0.01, "{d:?}");
            let e = q.column_top(p.tip_length_mm);
            assert!(index.outside(e[0], e[1], e[2]), "tip ends inside the model");
            assert!(!q.on_model && q.bottom_z == p.raft_mm);
        }
        // Without angled tips everything goes straight down.
        let flat = SupportParams {
            angled_tips: false,
            ..p
        };
        assert!(generate(&m, &flat)
            .unwrap()
            .points
            .iter()
            .all(|q| q.tip_dir == DOWN));
    }

    #[test]
    fn low_points_get_strong_tips() {
        let mut m = Mesh::cuboid([-10.0, -10.0, 10.0], [10.0, 10.0, 12.0]);
        let (a, b, c, tip) = (
            [0.0, 0.0, 10.0],
            [0.4, 0.0, 10.0],
            [0.2, 0.4, 10.0],
            [0.2, 0.15, 5.0],
        );
        m.append(Mesh::new(vec![
            [a, c, b],
            [a, b, tip],
            [b, c, tip],
            [c, a, tip],
        ]));
        let s = generate(&m, &SupportParams::default()).unwrap();
        let low = s.points.iter().filter(|q| q.strong).count();
        assert_eq!(low, 1, "{:?}", s.points);
    }

    #[test]
    fn raft_edge_is_undercut_for_a_scraper() {
        let (t, lip) = (1.5f32, 0.8f32);
        let m = raft([0.0, 0.0], [20.0, 20.0], t, lip, lip);
        let v = signed_volume(&m);
        let (a1, a2) = (((20.0 - 2.0 * lip) as f64).powi(2), 400.0f64);
        let want = lip as f64 / 3.0 * (a1 + a2 + (a1 * a2).sqrt()) + 400.0 * (t - lip) as f64;
        assert!((v - want).abs() < 1e-3, "{v} vs {want}");
        // The footprint on the plate is smaller than the raft's top.
        let bottom: Vec<_> = m
            .triangles
            .iter()
            .flatten()
            .filter(|p| p[2] == 0.0)
            .map(|p| p[0])
            .collect();
        let lo = bottom.iter().cloned().fold(f32::INFINITY, f32::min);
        assert!((lo - lip).abs() < 1e-5, "{lo}");
        assert_eq!(m.bounds().unwrap().min[0], 0.0);
        // A 90° edge is a plain box.
        let sq = raft([0.0, 0.0], [20.0, 20.0], t, lip, 0.0);
        assert!((signed_volume(&sq) - 400.0 * t as f64).abs() < 1e-3);
    }

    #[test]
    fn flat_raft_lips_are_rejected() {
        let p = SupportParams {
            raft_lip_angle_deg: 20.0,
            ..Default::default()
        };
        assert!(p.validate().is_err());
    }

    /// Number of support feet: lathe bottoms buried 0.2 mm in the raft.
    fn feet(m: &Mesh, p: &SupportParams) -> usize {
        let z = p.raft_mm - 0.2;
        let mut v: Vec<(i64, i64)> = m
            .triangles
            .iter()
            .flatten()
            .filter(|q| (q[2] - z).abs() < 1e-4)
            .map(|q| ((q[0] * 1e3).round() as i64, (q[1] * 1e3).round() as i64))
            .collect();
        v.sort();
        v.dedup();
        v.len() / (p.segments + 1)
    }

    #[test]
    fn tree_supports_share_trunks() {
        // A table: its top is 10 mm up, so there's room for branches.
        let mut m = Mesh::cuboid([-15.0, -15.0, 10.0], [15.0, 15.0, 12.0]);
        m.append(Mesh::cuboid([-1.0, -1.0, 0.0], [1.0, 1.0, 10.0]));
        let tree = SupportParams::default();
        let cols = SupportParams {
            tree: false,
            ..tree
        };
        let (t, c) = (generate(&m, &tree).unwrap(), generate(&m, &cols).unwrap());
        assert_eq!(t.points.len(), c.points.len());
        let (ft, fc) = (feet(&t.supports, &tree), feet(&c.supports, &cols));
        let raft = c.points.iter().filter(|q| !q.on_model).count();
        assert!(fc + 5 >= raft, "{fc} feet for {raft} columns");
        assert!(ft * 3 <= fc, "{ft} trunks for {fc} columns");
        assert!(signed_volume(&t.supports) > 0.0);
    }

    #[test]
    fn merge_point_respects_the_branch_angle() {
        let tan = 35f32.to_radians().tan();
        for (a, b) in [
            ([0.0, 0.0, 20.0], [6.0, 0.0, 20.0]),
            ([0.0, 0.0, 20.0], [3.0, 4.0, 15.0]),
            ([0.0, 0.0, 20.0], [1.0, 0.0, 5.0]),
        ] {
            let m = merge_point(a, b, tan);
            for q in [a, b] {
                let h = ((q[0] - m[0]).powi(2) + (q[1] - m[1]).powi(2)).sqrt();
                assert!(q[2] - m[2] >= 0.5 - 1e-4);
                assert!(h <= (q[2] - m[2]) * tan + 1e-3, "{q:?} -> {m:?}");
            }
        }
    }
}
