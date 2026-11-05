//! Triangle meshes and the transforms needed to place a model on the plate.

pub type Vec3 = [f32; 3];
pub type Triangle = [Vec3; 3];

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Mesh {
    pub triangles: Vec<Triangle>,
}

/// Axis-aligned bounding box.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Bounds {
    pub min: Vec3,
    pub max: Vec3,
}

impl Bounds {
    pub fn size(&self) -> Vec3 {
        [
            self.max[0] - self.min[0],
            self.max[1] - self.min[1],
            self.max[2] - self.min[2],
        ]
    }
}

impl Mesh {
    pub fn new(triangles: Vec<Triangle>) -> Self {
        Self { triangles }
    }

    /// `None` for an empty mesh.
    pub fn bounds(&self) -> Option<Bounds> {
        let mut it = self.triangles.iter().flatten();
        let first = *it.next()?;
        let mut b = Bounds {
            min: first,
            max: first,
        };
        for v in it {
            for (i, &c) in v.iter().enumerate() {
                b.min[i] = b.min[i].min(c);
                b.max[i] = b.max[i].max(c);
            }
        }
        Some(b)
    }

    pub fn translate(&mut self, d: Vec3) {
        for v in self.triangles.iter_mut().flatten() {
            for (c, dc) in v.iter_mut().zip(d) {
                *c += dc;
            }
        }
    }

    pub fn scale(&mut self, s: f32) {
        for v in self.triangles.iter_mut().flatten() {
            for c in v.iter_mut() {
                *c *= s;
            }
        }
    }

    /// Rotates about the X, then Y, then Z axis, in degrees.
    pub fn rotate_xyz(&mut self, deg: Vec3) {
        let (sx, cx) = deg[0].to_radians().sin_cos();
        let (sy, cy) = deg[1].to_radians().sin_cos();
        let (sz, cz) = deg[2].to_radians().sin_cos();
        for v in self.triangles.iter_mut().flatten() {
            let [x, y, z] = *v;
            let (y, z) = (y * cx - z * sx, y * sx + z * cx);
            let (x, z) = (x * cy + z * sy, -x * sy + z * cy);
            let (x, y) = (x * cz - y * sz, x * sz + y * cz);
            *v = [x, y, z];
        }
    }

    /// Centres the model on X/Y = 0 (the plate centre) plus `offset`, and
    /// drops it so its lowest point sits on Z = 0.
    pub fn place_on_plate(&mut self, offset: [f32; 2]) {
        if let Some(b) = self.bounds() {
            self.translate([
                offset[0] - (b.min[0] + b.max[0]) / 2.0,
                offset[1] - (b.min[1] + b.max[1]) / 2.0,
                -b.min[2],
            ]);
        }
    }

    /// Enclosed volume in mm^3 (absolute value of the signed volume, so it is
    /// right for closed meshes with consistent winding in either direction).
    pub fn volume(&self) -> f64 {
        let v: f64 = self
            .triangles
            .iter()
            .map(|[a, b, c]| {
                let (a, b, c) = (to64(a), to64(b), to64(c));
                (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                    + a[2] * (b[0] * c[1] - b[1] * c[0]))
                    / 6.0
            })
            .sum();
        v.abs()
    }

    /// An axis-aligned box from `min` to `max`, with outward-facing winding.
    pub fn cuboid(min: Vec3, max: Vec3) -> Self {
        let [x0, y0, z0] = min;
        let [x1, y1, z1] = max;
        let p = |x, y, z| [x, y, z];
        let quad = |a: Vec3, b: Vec3, c: Vec3, d: Vec3| [[a, b, c], [a, c, d]];
        let faces = [
            quad(p(x0, y0, z0), p(x0, y1, z0), p(x1, y1, z0), p(x1, y0, z0)), // bottom (-z)
            quad(p(x0, y0, z1), p(x1, y0, z1), p(x1, y1, z1), p(x0, y1, z1)), // top (+z)
            quad(p(x0, y0, z0), p(x1, y0, z0), p(x1, y0, z1), p(x0, y0, z1)), // front (-y)
            quad(p(x0, y1, z0), p(x0, y1, z1), p(x1, y1, z1), p(x1, y1, z0)), // back (+y)
            quad(p(x0, y0, z0), p(x0, y0, z1), p(x0, y1, z1), p(x0, y1, z0)), // left (-x)
            quad(p(x1, y0, z0), p(x1, y1, z0), p(x1, y1, z1), p(x1, y0, z1)), // right (+x)
        ];
        Self::new(faces.into_iter().flatten().collect())
    }

    pub fn append(&mut self, other: Mesh) {
        self.triangles.extend(other.triangles);
    }
}

fn to64(v: &Vec3) -> [f64; 3] {
    [v[0] as f64, v[1] as f64, v[2] as f64]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cuboid_volume_and_bounds() {
        let m = Mesh::cuboid([0.0, 0.0, 0.0], [2.0, 3.0, 4.0]);
        assert_eq!(m.triangles.len(), 12);
        assert!((m.volume() - 24.0).abs() < 1e-6);
        let b = m.bounds().unwrap();
        assert_eq!(b.size(), [2.0, 3.0, 4.0]);
    }

    #[test]
    fn cuboid_normals_point_outward() {
        let m = Mesh::cuboid([0.0, 0.0, 0.0], [1.0, 1.0, 1.0]);
        for [a, b, c] in &m.triangles {
            let u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
            let v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
            let n = [
                u[1] * v[2] - u[2] * v[1],
                u[2] * v[0] - u[0] * v[2],
                u[0] * v[1] - u[1] * v[0],
            ];
            let centroid = [
                (a[0] + b[0] + c[0]) / 3.0 - 0.5,
                (a[1] + b[1] + c[1]) / 3.0 - 0.5,
                (a[2] + b[2] + c[2]) / 3.0 - 0.5,
            ];
            let dot = n[0] * centroid[0] + n[1] * centroid[1] + n[2] * centroid[2];
            assert!(dot > 0.0, "inward face {a:?} {b:?} {c:?}");
        }
    }

    #[test]
    fn place_on_plate_centres_and_drops() {
        let mut m = Mesh::cuboid([10.0, 20.0, 5.0], [14.0, 22.0, 9.0]);
        m.place_on_plate([1.0, -1.0]);
        let b = m.bounds().unwrap();
        assert_eq!(b.min, [-1.0, -2.0, 0.0]);
        assert_eq!(b.max, [3.0, 0.0, 4.0]);
    }

    #[test]
    fn rotate_x_90_turns_y_into_z() {
        let mut m = Mesh::new(vec![[[0.0, 1.0, 0.0]; 3]]);
        m.rotate_xyz([90.0, 0.0, 0.0]);
        let v = m.triangles[0][0];
        assert!(v[1].abs() < 1e-6 && (v[2] - 1.0).abs() < 1e-6);
    }
}
