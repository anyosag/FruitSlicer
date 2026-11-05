//! Printer profiles. Only the Saturn 4 Ultra 16K for now.

/// Physical facts about a printer's LCD and build volume.
#[derive(Debug, Clone)]
pub struct Printer {
    pub name: &'static str,
    pub resolution_x: u16,
    pub resolution_y: u16,
    /// Active LCD area in mm.
    pub size_x: f32,
    pub size_y: f32,
    /// Usable Z height in mm.
    pub size_z: f32,
    pub mirror_x: bool,
    pub tilting_vat: bool,
}

impl Printer {
    /// Pixel pitch in mm. The 16K's pixels are not square.
    pub fn pixel_mm(&self) -> (f64, f64) {
        (
            self.size_x as f64 / self.resolution_x as f64,
            self.size_y as f64 / self.resolution_y as f64,
        )
    }
}

/// ELEGOO Saturn 4 Ultra 16K.
///
/// Resolution and size are from ELEGOO's manual (15120 x 6230 px,
/// 211.68 x 118.37 x 220 mm). `mirror_x` follows UVtools' PrusaSlicer
/// profile for this printer; it has not been confirmed against a file from
/// ELEGOO's own slicer.
pub const SATURN_4_ULTRA_16K: Printer = Printer {
    name: "ELEGOO Saturn 4 Ultra 16K",
    resolution_x: 15120,
    resolution_y: 6230,
    size_x: 211.68,
    size_y: 118.37,
    size_z: 220.0,
    mirror_x: true,
    tilting_vat: true,
};
