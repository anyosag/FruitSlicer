//! A small calibration print, built as a mesh and sent through the real
//! slicer.
//!
//! Five pads, symmetric about the plate centre so X/Y mirroring cannot
//! change the result:
//!
//! - a 20 x 20 mm square in the centre: measure both sides with calipers to
//!   check that the non-square 14 x 19 um pixels are scaled correctly;
//! - four 10 x 10 mm squares at (+-60 mm, +-35 mm) from the centre: check
//!   placement and that the whole LCD area is being addressed.

use slicer::Mesh;

/// (centre x, centre y, side) in mm.
const PADS: [(f32, f32, f32); 5] = [
    (0.0, 0.0, 20.0),
    (-60.0, -35.0, 10.0),
    (60.0, -35.0, 10.0),
    (-60.0, 35.0, 10.0),
    (60.0, 35.0, 10.0),
];

pub fn mesh(height_mm: f32) -> Mesh {
    let mut m = Mesh::default();
    for (cx, cy, side) in PADS {
        let r = side / 2.0;
        m.append(Mesh::cuboid(
            [cx - r, cy - r, 0.0],
            [cx + r, cy + r, height_mm],
        ));
    }
    m
}
