//! Models on the plate, their placement and their supports.

use slicer::mesh::{Bounds, Mesh};
use slicer::supports::{self, SupportParams, Supported};
use slicer::Printer;

#[derive(Debug, Clone, Copy, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct Placement {
    /// Uniform scale, 1 = as loaded.
    pub scale: f32,
    /// Degrees about X, then Y, then Z.
    pub rotation: [f32; 3],
    /// Centre offset from the plate centre, mm.
    pub offset: [f32; 2],
}

impl Default for Placement {
    fn default() -> Self {
        Self {
            scale: 1.0,
            rotation: [0.0; 3],
            offset: [0.0; 2],
        }
    }
}

pub struct Object {
    pub id: u64,
    pub name: String,
    /// As loaded from the file.
    pub base: Mesh,
    pub placement: Placement,
    /// Scaled, rotated, centred on its offset and resting on Z = 0.
    pub placed: Mesh,
    pub placed_bounds: Option<Bounds>,
    /// Supports for `placed`, if generated and still current.
    pub supported: Option<Supported>,
    /// Bumped whenever the placed mesh changes, so the GPU copy is refreshed.
    pub version: u64,
    /// Version the supports were made for.
    pub supports_version: Option<u64>,
    /// +1 if the mesh winds outward, -1 if inward.
    pub orient: f32,
}

impl Object {
    pub fn new(id: u64, name: String, base: Mesh) -> Self {
        let mut o = Self {
            id,
            name,
            base,
            placement: Placement::default(),
            placed: Mesh::default(),
            placed_bounds: None,
            supported: None,
            version: 0,
            supports_version: None,
            orient: 1.0,
        };
        o.orient = if signed_volume(&o.base) < 0.0 {
            -1.0
        } else {
            1.0
        };
        o.rebuild();
        o
    }

    pub fn rebuild(&mut self) {
        let mut m = self.base.clone();
        let p = &self.placement;
        if p.scale != 1.0 {
            m.scale(p.scale);
        }
        if p.rotation != [0.0; 3] {
            m.rotate_xyz(p.rotation);
        }
        m.place_on_plate(p.offset);
        self.placed_bounds = m.bounds();
        self.placed = m;
        self.version += 1;
    }

    pub fn supports_current(&self) -> bool {
        self.supports_version == Some(self.version)
    }

    pub fn generate_supports(&mut self, p: &SupportParams) -> Result<(), String> {
        self.supported = Some(supports::generate(&self.placed, p)?);
        self.supports_version = Some(self.version);
        Ok(())
    }

    pub fn clear_supports(&mut self) {
        self.supported = None;
        self.supports_version = None;
    }

    /// The model as it will print: lifted if it has supports.
    pub fn print_model(&self) -> &Mesh {
        match &self.supported {
            Some(s) if self.supports_current() => &s.model,
            _ => &self.placed,
        }
    }

    /// Bounds of everything this object prints (model and supports).
    pub fn print_bounds(&self) -> Option<Bounds> {
        let mut b = self.print_model().bounds()?;
        if let Some(s) = self.supported.as_ref().filter(|_| self.supports_current()) {
            if let Some(sb) = s.supports.bounds() {
                for i in 0..3 {
                    b.min[i] = b.min[i].min(sb.min[i]);
                    b.max[i] = b.max[i].max(sb.max[i]);
                }
            }
        }
        Some(b)
    }

    pub fn fits(&self, printer: &Printer) -> bool {
        let Some(b) = self.print_bounds() else {
            return true;
        };
        let (hx, hy) = (printer.size_x / 2.0, printer.size_y / 2.0);
        let e = 1e-3;
        b.min[0] >= -hx - e
            && b.max[0] <= hx + e
            && b.min[1] >= -hy - e
            && b.max[1] <= hy + e
            && b.max[2] <= printer.size_z + e
    }

    /// Lays the model flat on its largest roughly-flat face: rotates so that
    /// face points straight down.
    pub fn lay_flat(&mut self) {
        // Group faces of the scaled+rotated mesh by normal direction.
        let mut m = self.base.clone();
        m.scale(self.placement.scale);
        m.rotate_xyz(self.placement.rotation);
        let mut best: Option<(glam::Vec3, f64)> = None;
        let mut buckets: std::collections::HashMap<(i32, i32, i32), (glam::Vec3, f64)> =
            Default::default();
        for t in &m.triangles {
            let a = glam::Vec3::from(t[0]);
            let n = (glam::Vec3::from(t[1]) - a).cross(glam::Vec3::from(t[2]) - a) * self.orient;
            let area = n.length() as f64 / 2.0;
            if area == 0.0 {
                continue;
            }
            let n = n.normalize();
            let key = (
                (n.x * 20.0).round() as i32,
                (n.y * 20.0).round() as i32,
                (n.z * 20.0).round() as i32,
            );
            let e = buckets.entry(key).or_insert((glam::Vec3::ZERO, 0.0));
            e.0 += n * area as f32;
            e.1 += area;
        }
        for (_, (n, area)) in buckets {
            if best.is_none_or(|(_, a)| area > a) {
                best = Some((n.normalize_or_zero(), area));
            }
        }
        let Some((n, _)) = best else {
            return;
        };
        // Extra rotation taking n to -Z, applied after the current one.
        let q = glam::Quat::from_rotation_arc(n, -glam::Vec3::Z);
        let current = rotation_quat(self.placement.rotation);
        let (z, y, x) = (q * current).to_euler(glam::EulerRot::ZYX);
        self.placement.rotation = [x.to_degrees(), y.to_degrees(), z.to_degrees()];
        self.rebuild();
    }
}

/// The rotation `Mesh::rotate_xyz` applies (X first, then Y, then Z).
pub fn rotation_quat(deg: [f32; 3]) -> glam::Quat {
    glam::Quat::from_rotation_z(deg[2].to_radians())
        * glam::Quat::from_rotation_y(deg[1].to_radians())
        * glam::Quat::from_rotation_x(deg[0].to_radians())
}

pub fn signed_volume(m: &Mesh) -> f64 {
    m.triangles
        .iter()
        .map(|[a, b, c]| {
            let f = |v: &[f32; 3]| [v[0] as f64, v[1] as f64, v[2] as f64];
            let (a, b, c) = (f(a), f(b), f(c));
            (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                + a[2] * (b[0] * c[1] - b[1] * c[0]))
                / 6.0
        })
        .sum()
}

/// Everything on the plate, merged into one mesh ready to slice.
pub fn print_mesh(objects: &[Object]) -> Mesh {
    let mut m = Mesh::default();
    for o in objects {
        m.append(o.print_model().clone());
        if let Some(s) = o.supported.as_ref().filter(|_| o.supports_current()) {
            m.append(s.supports.clone());
        }
    }
    m
}

/// Places objects in rows across the plate, largest first, with `gap` mm
/// between them. Returns false if they don't all fit.
pub fn arrange(objects: &mut [Object], printer: &Printer, gap: f32) -> bool {
    let mut order: Vec<usize> = (0..objects.len()).collect();
    let size = |o: &Object| {
        o.print_bounds()
            .map(|b| [b.max[0] - b.min[0], b.max[1] - b.min[1]])
            .unwrap_or([0.0, 0.0])
    };
    order.sort_by(|&a, &b| {
        let (sa, sb) = (size(&objects[a]), size(&objects[b]));
        (sb[1] * sb[0]).total_cmp(&(sa[1] * sa[0]))
    });
    let (w, h) = (printer.size_x - 2.0 * gap, printer.size_y - 2.0 * gap);
    let (mut x, mut y, mut row_h) = (0.0f32, 0.0f32, 0.0f32);
    let mut ok = true;
    for i in order {
        let s = size(&objects[i]);
        if x > 0.0 && x + s[0] > w {
            x = 0.0;
            y += row_h + gap;
            row_h = 0.0;
        }
        if y + s[1] > h || s[0] > w {
            ok = false;
        }
        let centre = [-w / 2.0 + x + s[0] / 2.0, -h / 2.0 + y + s[1] / 2.0];
        // Offsets centre the placed model's bounds; supports may extend
        // the footprint, so shift by the difference.
        let o = &mut objects[i];
        let (pb, mb) = (o.print_bounds(), o.placed_bounds);
        let shift = match (pb, mb) {
            (Some(pb), Some(mb)) => [
                (mb.min[0] + mb.max[0]) / 2.0 - (pb.min[0] + pb.max[0]) / 2.0,
                (mb.min[1] + mb.max[1]) / 2.0 - (pb.min[1] + pb.max[1]) / 2.0,
            ],
            _ => [0.0, 0.0],
        };
        o.placement.offset = [centre[0] + shift[0], centre[1] + shift[1]];
        o.rebuild();
        x += s[0] + gap;
        row_h = row_h.max(s[1]);
    }
    ok
}

#[cfg(test)]
mod tests {
    use super::*;
    use slicer::SATURN_4_ULTRA_16K;

    #[test]
    fn rotation_quat_matches_mesh_rotate() {
        let deg = [30.0, -45.0, 110.0];
        let mut m = Mesh::new(vec![[[1.0, 2.0, 3.0]; 3]]);
        m.rotate_xyz(deg);
        let q = rotation_quat(deg) * glam::Vec3::new(1.0, 2.0, 3.0);
        let v = m.triangles[0][0];
        assert!((q - glam::Vec3::from(v)).length() < 1e-4, "{q:?} {v:?}");
    }

    #[test]
    fn lay_flat_puts_a_tilted_box_on_its_face() {
        let mut o = Object::new(1, "box".into(), Mesh::cuboid([0.0; 3], [10.0, 20.0, 2.0]));
        o.placement.rotation = [37.0, 12.0, 0.0];
        o.rebuild();
        o.lay_flat();
        let b = o.placed_bounds.unwrap();
        let s = b.size();
        // Lies on one of its two large faces: 2 mm tall.
        assert!((s[2] - 2.0).abs() < 1e-3, "{s:?}");
    }

    #[test]
    fn arrange_keeps_objects_apart_and_on_the_plate() {
        let mut objs: Vec<Object> = (0..6)
            .map(|i| Object::new(i, "c".into(), Mesh::cuboid([0.0; 3], [30.0, 20.0, 10.0])))
            .collect();
        assert!(arrange(&mut objs, &SATURN_4_ULTRA_16K, 5.0));
        for o in &objs {
            assert!(o.fits(&SATURN_4_ULTRA_16K));
        }
        for (i, a) in objs.iter().enumerate() {
            for b in &objs[i + 1..] {
                let (a, b) = (a.placed_bounds.unwrap(), b.placed_bounds.unwrap());
                let overlap = a.min[0] < b.max[0] - 1e-3
                    && b.min[0] < a.max[0] - 1e-3
                    && a.min[1] < b.max[1] - 1e-3
                    && b.min[1] < a.max[1] - 1e-3;
                assert!(!overlap);
            }
        }
    }
}
