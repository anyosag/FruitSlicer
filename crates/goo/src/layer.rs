//! Per-layer definition ("Layer definition" child field in the spec).

use std::io::{Read, Write};

use crate::bytes::*;
use crate::{Error, DELIMITER};

/// Settings for one layer. With `Header::advance_mode` set, the printer uses
/// these instead of the header's global values. Writers should fill them in
/// either way, as ELEGOO's slicers do.
#[derive(Debug, Clone, PartialEq)]
pub struct LayerDef {
    pub pause: bool,             // 1, stored as u16
    pub pause_position_z: f32,   // 2
    pub position_z: f32,         // 3, height of this layer in mm
    pub exposure_time: f32,      // 4
    pub off_time: f32,           // 5
    pub before_lift_time: f32,   // 6
    pub after_lift_time: f32,    // 7
    pub after_retract_time: f32, // 8
    pub lift_distance: f32,      // 9
    pub lift_speed: f32,         // 10
    pub lift_distance_2: f32,    // 11
    pub lift_speed_2: f32,       // 12
    pub retract_distance: f32,   // 13
    pub retract_speed: f32,      // 14
    pub retract_distance_2: f32, // 15
    pub retract_speed_2: f32,    // 16
    pub light_pwm: u16,          // 17
}

impl LayerDef {
    /// Writes the definition, the data size, the encoded image and the
    /// trailing delimiter. `data` must be a full encoded layer (magic byte
    /// through checksum), as returned by `LayerEncoder::finish`.
    pub fn write_with_data(&self, w: &mut impl Write, data: &[u8]) -> Result<(), Error> {
        write_u16(w, self.pause as u16)?;
        for v in [
            self.pause_position_z,
            self.position_z,
            self.exposure_time,
            self.off_time,
            self.before_lift_time,
            self.after_lift_time,
            self.after_retract_time,
            self.lift_distance,
            self.lift_speed,
            self.lift_distance_2,
            self.lift_speed_2,
            self.retract_distance,
            self.retract_speed,
            self.retract_distance_2,
            self.retract_speed_2,
        ] {
            write_f32(w, v)?;
        }
        write_u16(w, self.light_pwm)?;
        w.write_all(DELIMITER)?;
        write_u32(w, data.len() as u32)?;
        w.write_all(data)?;
        w.write_all(DELIMITER)?;
        Ok(())
    }

    /// Reads a definition and its encoded image data.
    pub fn read_with_data(r: &mut impl Read) -> Result<(Self, Vec<u8>), Error> {
        let def = Self {
            pause: read_u16(r)? != 0,
            pause_position_z: read_f32(r)?,
            position_z: read_f32(r)?,
            exposure_time: read_f32(r)?,
            off_time: read_f32(r)?,
            before_lift_time: read_f32(r)?,
            after_lift_time: read_f32(r)?,
            after_retract_time: read_f32(r)?,
            lift_distance: read_f32(r)?,
            lift_speed: read_f32(r)?,
            lift_distance_2: read_f32(r)?,
            lift_speed_2: read_f32(r)?,
            retract_distance: read_f32(r)?,
            retract_speed: read_f32(r)?,
            retract_distance_2: read_f32(r)?,
            retract_speed_2: read_f32(r)?,
            light_pwm: read_u16(r)?,
        };
        expect(r, DELIMITER, "layer definition delimiter")?;
        let size = read_u32(r)? as usize;
        let mut data = vec![0u8; size];
        r.read_exact(&mut data)?;
        expect(r, DELIMITER, "layer data delimiter")?;
        Ok((def, data))
    }
}
