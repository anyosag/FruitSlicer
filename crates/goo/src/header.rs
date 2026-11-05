//! The fixed 195,477-byte file header ("header info" in the spec).

use std::io::{Read, Write};

use crate::bytes::*;
use crate::preview::Preview;
use crate::{Error, DELIMITER, MAGIC_TAG};

/// Size of the V3 header in bytes; layer data starts right after it.
pub const HEADER_SIZE: u32 = 195_477;

/// Spec field 25: 0 = "turn off time" mode (uses `turn_off_time`),
/// 1 = "static time" mode (uses the before/after lift and retract waits).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DelayMode {
    TurnOffTime = 0,
    StaticTime = 1,
}

/// Every header field, in file order. Field numbers in comments match the
/// spec's "Index" column. Times are seconds, distances mm, speeds mm/min.
#[derive(Debug, Clone, PartialEq)]
pub struct Header {
    pub version: String,                // 0, 4 bytes, e.g. "V3.0"
    pub software_info: String,          // 2, 32 bytes
    pub software_version: String,       // 3, 24 bytes
    pub file_time: String,              // 4, 24 bytes
    pub printer_name: String,           // 5, 32 bytes
    pub printer_type: String,           // 6, 32 bytes
    pub profile_name: String,           // 7, 32 bytes
    pub anti_aliasing_level: u16,       // 8
    pub grey_level: u16,                // 9
    pub blur_level: u16,                // 10
    pub small_preview: Preview,         // 11, 116 x 116 RGB565
    pub big_preview: Preview,           // 13, 290 x 290 RGB565
    pub layer_count: u32,               // 15
    pub resolution_x: u16,              // 16
    pub resolution_y: u16,              // 17
    pub mirror_x: bool,                 // 18
    pub mirror_y: bool,                 // 19
    pub size_x: f32,                    // 20
    pub size_y: f32,                    // 21
    pub size_z: f32,                    // 22
    pub layer_thickness: f32,           // 23
    pub exposure_time: f32,             // 24
    pub delay_mode: DelayMode,          // 25
    pub turn_off_time: f32,             // 26
    pub bottom_before_lift_time: f32,   // 27
    pub bottom_after_lift_time: f32,    // 28
    pub bottom_after_retract_time: f32, // 29
    pub before_lift_time: f32,          // 30
    pub after_lift_time: f32,           // 31
    pub after_retract_time: f32,        // 32
    pub bottom_exposure_time: f32,      // 33
    pub bottom_layers: u32,             // 34
    pub bottom_lift_distance: f32,      // 35
    pub bottom_lift_speed: f32,         // 36
    pub lift_distance: f32,             // 37
    pub lift_speed: f32,                // 38
    pub bottom_retract_distance: f32,   // 39
    pub bottom_retract_speed: f32,      // 40
    pub retract_distance: f32,          // 41
    pub retract_speed: f32,             // 42
    pub bottom_lift_distance_2: f32,    // 43
    pub bottom_lift_speed_2: f32,       // 44
    pub lift_distance_2: f32,           // 45
    pub lift_speed_2: f32,              // 46
    pub bottom_retract_distance_2: f32, // 47
    pub bottom_retract_speed_2: f32,    // 48
    pub retract_distance_2: f32,        // 49
    pub retract_speed_2: f32,           // 50
    pub bottom_light_pwm: u16,          // 51, 0..=255
    pub light_pwm: u16,                 // 52, 0..=255
    pub advance_mode: bool,             // 53, true = use per-layer settings
    pub printing_time: u32,             // 54, seconds
    pub total_volume: f32,              // 55, mm^3
    pub total_weight: f32,              // 56, g
    pub total_price: f32,               // 57
    pub price_unit: String,             // 58, 8 bytes
    pub layer_content_offset: u32,      // 59, normally HEADER_SIZE
    pub grey_scale_level: bool,         // 60, false = 4-bit (0..=0xF), true = 8-bit
    pub transition_layers: u16,         // 61
}

impl Header {
    pub fn write_to(&self, w: &mut impl Write) -> Result<(), Error> {
        write_str(w, &self.version, 4, "version")?;
        w.write_all(MAGIC_TAG)?;
        write_str(w, &self.software_info, 32, "software_info")?;
        write_str(w, &self.software_version, 24, "software_version")?;
        write_str(w, &self.file_time, 24, "file_time")?;
        write_str(w, &self.printer_name, 32, "printer_name")?;
        write_str(w, &self.printer_type, 32, "printer_type")?;
        write_str(w, &self.profile_name, 32, "profile_name")?;
        write_u16(w, self.anti_aliasing_level)?;
        write_u16(w, self.grey_level)?;
        write_u16(w, self.blur_level)?;
        self.small_preview.write_to(w, Preview::SMALL)?;
        w.write_all(DELIMITER)?;
        self.big_preview.write_to(w, Preview::BIG)?;
        w.write_all(DELIMITER)?;
        write_u32(w, self.layer_count)?;
        write_u16(w, self.resolution_x)?;
        write_u16(w, self.resolution_y)?;
        write_bool(w, self.mirror_x)?;
        write_bool(w, self.mirror_y)?;
        for v in [
            self.size_x,
            self.size_y,
            self.size_z,
            self.layer_thickness,
            self.exposure_time,
        ] {
            write_f32(w, v)?;
        }
        write_u8(w, self.delay_mode as u8)?;
        for v in [
            self.turn_off_time,
            self.bottom_before_lift_time,
            self.bottom_after_lift_time,
            self.bottom_after_retract_time,
            self.before_lift_time,
            self.after_lift_time,
            self.after_retract_time,
            self.bottom_exposure_time,
        ] {
            write_f32(w, v)?;
        }
        write_u32(w, self.bottom_layers)?;
        for v in [
            self.bottom_lift_distance,
            self.bottom_lift_speed,
            self.lift_distance,
            self.lift_speed,
            self.bottom_retract_distance,
            self.bottom_retract_speed,
            self.retract_distance,
            self.retract_speed,
            self.bottom_lift_distance_2,
            self.bottom_lift_speed_2,
            self.lift_distance_2,
            self.lift_speed_2,
            self.bottom_retract_distance_2,
            self.bottom_retract_speed_2,
            self.retract_distance_2,
            self.retract_speed_2,
        ] {
            write_f32(w, v)?;
        }
        write_u16(w, self.bottom_light_pwm)?;
        write_u16(w, self.light_pwm)?;
        write_bool(w, self.advance_mode)?;
        write_u32(w, self.printing_time)?;
        write_f32(w, self.total_volume)?;
        write_f32(w, self.total_weight)?;
        write_f32(w, self.total_price)?;
        write_str(w, &self.price_unit, 8, "price_unit")?;
        write_u32(w, self.layer_content_offset)?;
        write_bool(w, self.grey_scale_level)?;
        write_u16(w, self.transition_layers)?;
        Ok(())
    }

    pub fn read_from(r: &mut impl Read) -> Result<Self, Error> {
        let version = read_str(r, 4)?;
        expect(r, MAGIC_TAG, "header magic tag")?;
        let software_info = read_str(r, 32)?;
        let software_version = read_str(r, 24)?;
        let file_time = read_str(r, 24)?;
        let printer_name = read_str(r, 32)?;
        let printer_type = read_str(r, 32)?;
        let profile_name = read_str(r, 32)?;
        let anti_aliasing_level = read_u16(r)?;
        let grey_level = read_u16(r)?;
        let blur_level = read_u16(r)?;
        let small_preview = Preview::read_from(r, Preview::SMALL)?;
        expect(r, DELIMITER, "small preview delimiter")?;
        let big_preview = Preview::read_from(r, Preview::BIG)?;
        expect(r, DELIMITER, "big preview delimiter")?;
        let layer_count = read_u32(r)?;
        let resolution_x = read_u16(r)?;
        let resolution_y = read_u16(r)?;
        let mirror_x = read_bool(r)?;
        let mirror_y = read_bool(r)?;
        let size_x = read_f32(r)?;
        let size_y = read_f32(r)?;
        let size_z = read_f32(r)?;
        let layer_thickness = read_f32(r)?;
        let exposure_time = read_f32(r)?;
        let delay_mode = match read_u8(r)? {
            0 => DelayMode::TurnOffTime,
            _ => DelayMode::StaticTime,
        };
        Ok(Self {
            version,
            software_info,
            software_version,
            file_time,
            printer_name,
            printer_type,
            profile_name,
            anti_aliasing_level,
            grey_level,
            blur_level,
            small_preview,
            big_preview,
            layer_count,
            resolution_x,
            resolution_y,
            mirror_x,
            mirror_y,
            size_x,
            size_y,
            size_z,
            layer_thickness,
            exposure_time,
            delay_mode,
            turn_off_time: read_f32(r)?,
            bottom_before_lift_time: read_f32(r)?,
            bottom_after_lift_time: read_f32(r)?,
            bottom_after_retract_time: read_f32(r)?,
            before_lift_time: read_f32(r)?,
            after_lift_time: read_f32(r)?,
            after_retract_time: read_f32(r)?,
            bottom_exposure_time: read_f32(r)?,
            bottom_layers: read_u32(r)?,
            bottom_lift_distance: read_f32(r)?,
            bottom_lift_speed: read_f32(r)?,
            lift_distance: read_f32(r)?,
            lift_speed: read_f32(r)?,
            bottom_retract_distance: read_f32(r)?,
            bottom_retract_speed: read_f32(r)?,
            retract_distance: read_f32(r)?,
            retract_speed: read_f32(r)?,
            bottom_lift_distance_2: read_f32(r)?,
            bottom_lift_speed_2: read_f32(r)?,
            lift_distance_2: read_f32(r)?,
            lift_speed_2: read_f32(r)?,
            bottom_retract_distance_2: read_f32(r)?,
            bottom_retract_speed_2: read_f32(r)?,
            retract_distance_2: read_f32(r)?,
            retract_speed_2: read_f32(r)?,
            bottom_light_pwm: read_u16(r)?,
            light_pwm: read_u16(r)?,
            advance_mode: read_bool(r)?,
            printing_time: read_u32(r)?,
            total_volume: read_f32(r)?,
            total_weight: read_f32(r)?,
            total_price: read_f32(r)?,
            price_unit: read_str(r, 8)?,
            layer_content_offset: read_u32(r)?,
            grey_scale_level: read_bool(r)?,
            transition_layers: read_u16(r)?,
        })
    }

    pub fn pixel_count(&self) -> u64 {
        self.resolution_x as u64 * self.resolution_y as u64
    }
}
