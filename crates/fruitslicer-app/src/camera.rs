//! An orbit camera around a target point, with Z up.

use glam::{Mat4, Vec3};

#[derive(Debug, Clone, Copy, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct Camera {
    pub target: [f32; 3],
    /// Angle around Z, radians. 0 looks along +Y (from the front).
    pub yaw: f32,
    /// Angle above the plate, radians.
    pub pitch: f32,
    pub distance: f32,
    pub fov_y: f32,
}

impl Default for Camera {
    fn default() -> Self {
        Self {
            target: [0.0, 0.0, 20.0],
            yaw: -0.5,
            pitch: 0.55,
            distance: 300.0,
            fov_y: 35f32.to_radians(),
        }
    }
}

/// Preset views.
#[derive(Debug, Clone, Copy)]
pub enum View {
    Iso,
    Front,
    Top,
    Bottom,
    Left,
    Right,
}

impl Camera {
    pub fn eye(&self) -> Vec3 {
        let (sy, cy) = self.yaw.sin_cos();
        let (sp, cp) = self.pitch.sin_cos();
        // yaw = 0: camera at -Y looking towards +Y.
        let dir = Vec3::new(sy * cp, -cy * cp, sp);
        Vec3::from(self.target) + dir * self.distance
    }

    pub fn view(&self) -> Mat4 {
        let up = if self.pitch.abs() > 1.5 {
            // Looking straight down/up: use Y as up so the view is stable.
            Vec3::new(-self.yaw.sin(), self.yaw.cos(), 0.0)
        } else {
            Vec3::Z
        };
        Mat4::look_at_rh(self.eye(), Vec3::from(self.target), up)
    }

    pub fn proj(&self, aspect: f32) -> Mat4 {
        let near = (self.distance * 0.01).max(0.1);
        Mat4::perspective_rh(
            self.fov_y,
            aspect.max(0.01),
            near,
            self.distance * 20.0 + 1000.0,
        )
    }

    pub fn orbit(&mut self, dx: f32, dy: f32) {
        self.yaw -= dx * 0.008;
        self.pitch = (self.pitch + dy * 0.008).clamp(-1.55, 1.55);
    }

    /// Moves the target in the view plane by a drag of (dx, dy) points in a
    /// viewport `height` points tall.
    pub fn pan(&mut self, dx: f32, dy: f32, height: f32) {
        let scale = 2.0 * self.distance * (self.fov_y / 2.0).tan() / height.max(1.0);
        let v = self.view().inverse();
        let right = v.transform_vector3(Vec3::X);
        let up = v.transform_vector3(Vec3::Y);
        let t = Vec3::from(self.target) - right * dx * scale + up * dy * scale;
        self.target = t.into();
    }

    pub fn zoom(&mut self, steps: f32) {
        self.distance = (self.distance * (1.0 - steps * 0.1).clamp(0.5, 1.5)).clamp(5.0, 3000.0);
    }

    pub fn set_view(&mut self, v: View, target: [f32; 3], radius: f32) {
        let (yaw, pitch) = match v {
            View::Iso => (-0.5, 0.55),
            View::Front => (0.0, 0.0),
            View::Top => (0.0, 1.55),
            View::Bottom => (0.0, -1.55),
            View::Left => (-std::f32::consts::FRAC_PI_2, 0.0),
            View::Right => (std::f32::consts::FRAC_PI_2, 0.0),
        };
        self.yaw = yaw;
        self.pitch = pitch;
        self.target = target;
        self.distance = (radius / (self.fov_y / 2.0).tan() * 1.3).clamp(20.0, 3000.0);
    }

    /// A ray from the eye through a point in the viewport, given in
    /// normalised device coordinates (-1..1, y up).
    pub fn ray(&self, ndc: [f32; 2], aspect: f32) -> (Vec3, Vec3) {
        let inv = (self.proj(aspect) * self.view()).inverse();
        let near = inv.project_point3(Vec3::new(ndc[0], ndc[1], 0.0));
        let far = inv.project_point3(Vec3::new(ndc[0], ndc[1], 1.0));
        (near, (far - near).normalize())
    }
}

/// Where a ray meets the plane z = `z`, if it does (in front of the eye).
pub fn ray_plane_z(origin: Vec3, dir: Vec3, z: f32) -> Option<Vec3> {
    if dir.z.abs() < 1e-6 {
        return None;
    }
    let t = (z - origin.z) / dir.z;
    (t > 0.0).then(|| origin + dir * t)
}

/// Distance along a ray to an axis-aligned box, if it hits.
pub fn ray_box(origin: Vec3, dir: Vec3, min: Vec3, max: Vec3) -> Option<f32> {
    let inv = Vec3::ONE / dir;
    let t0 = (min - origin) * inv;
    let t1 = (max - origin) * inv;
    let tmin = t0.min(t1).max_element();
    let tmax = t0.max(t1).min_element();
    (tmax >= tmin.max(0.0)).then_some(tmin.max(0.0))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ray_through_centre_hits_target() {
        let c = Camera::default();
        let (o, d) = c.ray([0.0, 0.0], 1.5);
        let to_target = (Vec3::from(c.target) - o).normalize();
        assert!(d.dot(to_target) > 0.9999);
    }

    #[test]
    fn ray_box_hits_and_misses() {
        let (min, max) = (Vec3::splat(-1.0), Vec3::splat(1.0));
        assert!(ray_box(Vec3::new(0.0, -5.0, 0.0), Vec3::Y, min, max).is_some());
        assert!(ray_box(Vec3::new(3.0, -5.0, 0.0), Vec3::Y, min, max).is_none());
    }
}
