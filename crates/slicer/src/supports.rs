//! Automatic supports: straight columns with tapered tips, standing on a
//! raft or on the model itself.
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
//! What this does not do: branching or tree supports, moving supports off
//! the model to avoid marks, bracing between columns, or sizing tips by the
//! load they carry. Defaults are starting points, not tuned values.

use crate::mesh::{Mesh, Triangle, Vec3};

#[derive(Debug, Clone, Copy, PartialEq)]
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
    /// Also support local minima (downward points).
    pub support_minima: bool,
    /// Sides of each column's cross-section.
    pub segments: usize,
}

impl Default for SupportParams {
    fn default() -> Self {
        Self {
            lift_mm: 5.0,
            overhang_angle_deg: 45.0,
            spacing_mm: 3.0,
            tip_diameter_mm: 0.4,
            tip_length_mm: 2.0,
            tip_penetration_mm: 0.2,
            column_diameter_mm: 1.2,
            foot_diameter_mm: 2.5,
            foot_height_mm: 1.0,
            raft_mm: 1.0,
            raft_margin_mm: 2.0,
            support_minima: true,
            segments: 8,
        }
    }
}

impl SupportParams {
    pub fn validate(&self) -> Result<(), String> {
        let pos = [
            ("spacing", self.spacing_mm),
            ("tip diameter", self.tip_diameter_mm),
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
        if self.lift_mm < 0.0 || self.raft_mm < 0.0 {
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
    pub top: Vec3,
    pub bottom_z: f32,
    /// `true` when it ends on the model rather than the raft or plate.
    pub on_model: bool,
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
    p.validate()?;
    let mut model = placed.clone();
    let Some(b) = model.bounds() else {
        return Ok(Supported::default());
    };
    model.translate([0.0, 0.0, p.model_floor() - b.min[2]]);
    let points = find_points(&model, p);
    let supports = build(&points, p);
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
            hits.push(Hit {
                z,
                down: nz < 0.0,
                needs_support: -nz > self.cos_limit,
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
            for (k, h) in hits.iter().enumerate() {
                if !(h.down && h.needs_support) || h.z <= ground + 1e-3 {
                    continue;
                }
                // The surface below: the previous upward-facing hit.
                let below = hits[..k].iter().rev().find(|b| !b.down);
                let (bz, on_model) = match below {
                    Some(b) if b.z > ground => (b.z, true),
                    _ => (ground, false),
                };
                if h.z - bz < 0.3 {
                    continue; // the gap would just cure solid
                }
                points.push(SupportPoint {
                    top: [x, y, h.z],
                    bottom_z: bz,
                    on_model,
                });
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
            let (bz, on_model) = index.landing(v[0], v[1], v[2], ground);
            if v[2] - bz < 0.3 {
                continue;
            }
            points.push(SupportPoint {
                top: v,
                bottom_z: bz,
                on_model,
            });
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
        let (bz, on_model) = index.landing(c[0], c[1], c[2], ground);
        if c[2] - bz < 0.3 {
            continue;
        }
        near.insert(c, points.len());
        points.push(SupportPoint {
            top: c,
            bottom_z: bz,
            on_model,
        });
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
        ((x / self.cell).floor() as i64, (y / self.cell).floor() as i64)
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
    let (rt, rc, rf) = (
        p.tip_diameter_mm / 2.0,
        p.column_diameter_mm / 2.0,
        p.foot_diameter_mm / 2.0,
    );
    let mut feet: Vec<[f32; 2]> = Vec::new();
    for sp in points {
        let [x, y, zt] = sp.top;
        let zb = sp.bottom_z;
        let top = zt + p.tip_penetration_mm;
        let len = zt - zb;
        // Profile rings from top to bottom: (z, radius).
        let mut rings: Vec<(f32, f32)> = Vec::new();
        if sp.on_model {
            // Tip at both ends.
            if len <= 2.0 * p.tip_length_mm {
                let mid = (zt + zb) / 2.0;
                let rm = rt + (rc - rt) * ((len / 2.0) / p.tip_length_mm).min(1.0);
                rings.extend([(top, rt), (mid, rm), (zb - p.tip_penetration_mm, rt)]);
            } else {
                rings.extend([
                    (top, rt),
                    (zt - p.tip_length_mm, rc),
                    (zb + p.tip_length_mm, rc),
                    (zb - p.tip_penetration_mm, rt),
                ]);
            }
        } else {
            // Bury the foot slightly in the raft (or plate) so it bonds.
            let bottom = if p.raft_mm > 0.0 && p.lift_mm > 0.0 {
                zb - p.raft_mm.min(0.2)
            } else {
                zb
            };
            if len <= p.tip_length_mm + p.foot_height_mm {
                let k = len / (p.tip_length_mm + p.foot_height_mm);
                let mid = zb + p.foot_height_mm * k;
                rings.extend([(top, rt), (mid, rc), (bottom, rf.max(rc))]);
            } else {
                rings.extend([
                    (top, rt),
                    (zt - p.tip_length_mm, rc),
                    (zb + p.foot_height_mm, rc),
                    (bottom, rf.max(rc)),
                ]);
            }
            feet.push([x, y]);
        }
        out.append(lathe([x, y], &rings, p.segments.max(3)));
    }
    if p.raft_mm > 0.0 && p.lift_mm > 0.0 && !feet.is_empty() {
        let (x0, x1) = minmax(feet.iter().map(|f| f[0]));
        let (y0, y1) = minmax(feet.iter().map(|f| f[1]));
        let m = p.raft_margin_mm + rf;
        out.append(Mesh::cuboid(
            [x0 - m, y0 - m, 0.0],
            [x1 + m, y1 + m, p.raft_mm],
        ));
    }
    out
}

/// A closed solid of revolution around the vertical line at `c`. `rings`
/// go from top to bottom. Winding is outward.
fn lathe(c: [f32; 2], rings: &[(f32, f32)], n: usize) -> Mesh {
    let ring = |(z, r): (f32, f32)| -> Vec<Vec3> {
        (0..n)
            .map(|k| {
                let a = std::f32::consts::TAU * k as f32 / n as f32;
                [c[0] + r * a.cos(), c[1] + r * a.sin(), z]
            })
            .collect()
    };
    let rs: Vec<Vec<Vec3>> = rings.iter().map(|&r| ring(r)).collect();
    let mut tris = Vec::new();
    // Sides: going down from ring i (upper) to i+1 (lower), angles increase
    // counter-clockwise seen from above.
    for w in rs.windows(2) {
        let (u, l) = (&w[0], &w[1]);
        for k in 0..n {
            let k1 = (k + 1) % n;
            tris.push([l[k], l[k1], u[k1]]);
            tris.push([l[k], u[k1], u[k]]);
        }
    }
    // Caps.
    let (top, bot) = (&rs[0], &rs[rs.len() - 1]);
    let tc = [c[0], c[1], rings[0].0];
    let bc = [c[0], c[1], rings[rings.len() - 1].0];
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
            under_top.iter().all(|q| !q.on_model && q.bottom_z == p.raft_mm),
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
        assert!(shelf.iter().all(|q| q.on_model && (q.bottom_z - 2.0).abs() < 1e-3));
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
        m.append(Mesh::new(vec![[a, c, b], [a, b, tip], [b, c, tip], [c, a, tip]]));
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
}
