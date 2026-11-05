//! Exposure settings, and turning them into .goo header and layer fields.

use goo::{DelayMode, Header, LayerDef, Preview, HEADER_SIZE};

use crate::printer::Printer;

/// Per-print exposure settings. The defaults are placeholders, not tuned for
/// any resin: use your resin maker's numbers.
#[derive(Debug, Clone, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(feature = "serde", serde(default))]
pub struct PrintSettings {
    pub layer_mm: f32,
    pub exposure_s: f32,
    pub bottom_exposure_s: f32,
    pub bottom_layers: u32,
    /// UV power, 0..=255.
    pub light_pwm: u16,
    pub bottom_light_pwm: u16,
    /// Anti-aliasing: 1 = off, otherwise sub-rows sampled per pixel row
    /// (2, 4 or 8). Edge pixels get grey levels by how much they are covered.
    pub anti_alias: u32,
}

impl Default for PrintSettings {
    fn default() -> Self {
        Self {
            layer_mm: 0.05,
            exposure_s: 2.5,
            bottom_exposure_s: 30.0,
            bottom_layers: 4,
            light_pwm: 255,
            bottom_light_pwm: 255,
            anti_alias: 1,
        }
    }
}

impl PrintSettings {
    pub fn validate(&self) -> Result<(), String> {
        if !(self.layer_mm > 0.0 && self.layer_mm <= 0.5) {
            return Err(format!(
                "layer height {} mm is outside 0..0.5 mm",
                self.layer_mm
            ));
        }
        if !(self.exposure_s > 0.0 && self.bottom_exposure_s > 0.0) {
            return Err("exposure times must be positive".into());
        }
        if !(1..=16).contains(&self.anti_alias) {
            return Err("anti-aliasing must be 1 (off) to 16".into());
        }
        if self.light_pwm > 255 || self.bottom_light_pwm > 255 {
            return Err("light PWM must be 0..=255".into());
        }
        Ok(())
    }

    fn is_bottom(&self, layer: u32) -> bool {
        layer < self.bottom_layers
    }

    /// Movement values for the printer. Tilting-vat values copy UVtools'
    /// Saturn 4 Ultra 16K profile (0.05 everywhere), which look like
    /// placeholders the firmware replaces with its tilt modes. Unverified
    /// against a file from ELEGOO's own slicer.
    fn movement(printer: &Printer) -> (f32, f32) {
        if printer.tilting_vat {
            (0.05, 0.05)
        } else {
            (6.0, 60.0)
        }
    }

    /// The layer definition for layer `i` (0-based).
    pub fn layer_def(&self, printer: &Printer, i: u32) -> LayerDef {
        let (dist, speed) = Self::movement(printer);
        let bottom = self.is_bottom(i);
        LayerDef {
            pause: false,
            pause_position_z: printer.size_z,
            position_z: (i + 1) as f32 * self.layer_mm,
            exposure_time: if bottom {
                self.bottom_exposure_s
            } else {
                self.exposure_s
            },
            off_time: 0.0,
            before_lift_time: 0.0,
            after_lift_time: 0.0,
            after_retract_time: 0.0,
            lift_distance: dist,
            lift_speed: speed,
            lift_distance_2: 0.0,
            lift_speed_2: 0.0,
            retract_distance: dist,
            retract_speed: speed,
            retract_distance_2: 0.0,
            retract_speed_2: 0.0,
            light_pwm: if bottom {
                self.bottom_light_pwm
            } else {
                self.light_pwm
            },
        }
    }

    /// Rough print time: exposure plus about 5 s per layer for the tilt
    /// (the profile's "fast" tilt time).
    pub fn estimated_seconds(&self, layer_count: u32) -> u32 {
        let bottom = self.bottom_layers.min(layer_count);
        (bottom as f32 * self.bottom_exposure_s
            + (layer_count - bottom) as f32 * self.exposure_s
            + layer_count as f32 * 5.0) as u32
    }
}

/// Everything the header needs that comes from the model.
#[derive(Debug, Clone)]
pub struct HeaderInputs<'a> {
    pub profile_name: &'a str,
    pub timestamp: &'a str,
    pub layer_count: u32,
    pub volume_mm3: f64,
    pub small_preview: Preview,
    pub big_preview: Preview,
}

pub fn build_header(printer: &Printer, s: &PrintSettings, inp: HeaderInputs) -> Header {
    let (dist, speed) = PrintSettings::movement(printer);
    Header {
        version: "V3.0".into(),
        software_info: "FruitSlicer".into(),
        software_version: env!("CARGO_PKG_VERSION").into(),
        file_time: inp.timestamp.into(),
        printer_name: printer.name.into(),
        printer_type: "DLP".into(),
        profile_name: truncate(inp.profile_name, 32),
        anti_aliasing_level: s.anti_alias.max(1) as u16,
        grey_level: 0,
        blur_level: 0,
        small_preview: inp.small_preview,
        big_preview: inp.big_preview,
        layer_count: inp.layer_count,
        resolution_x: printer.resolution_x,
        resolution_y: printer.resolution_y,
        mirror_x: printer.mirror_x,
        mirror_y: false,
        size_x: printer.size_x,
        size_y: printer.size_y,
        size_z: printer.size_z,
        layer_thickness: s.layer_mm,
        exposure_time: s.exposure_s,
        delay_mode: DelayMode::StaticTime,
        turn_off_time: 0.0,
        bottom_before_lift_time: 0.0,
        bottom_after_lift_time: 0.0,
        bottom_after_retract_time: 0.0,
        before_lift_time: 0.0,
        after_lift_time: 0.0,
        after_retract_time: 0.0,
        bottom_exposure_time: s.bottom_exposure_s,
        bottom_layers: s.bottom_layers.min(inp.layer_count),
        bottom_lift_distance: dist,
        bottom_lift_speed: speed,
        lift_distance: dist,
        lift_speed: speed,
        bottom_retract_distance: dist,
        bottom_retract_speed: speed,
        retract_distance: dist,
        retract_speed: speed,
        bottom_lift_distance_2: 0.0,
        bottom_lift_speed_2: 0.0,
        lift_distance_2: 0.0,
        lift_speed_2: 0.0,
        bottom_retract_distance_2: 0.0,
        bottom_retract_speed_2: 0.0,
        retract_distance_2: 0.0,
        retract_speed_2: 0.0,
        bottom_light_pwm: s.bottom_light_pwm,
        light_pwm: s.light_pwm,
        advance_mode: false,
        printing_time: s.estimated_seconds(inp.layer_count),
        total_volume: inp.volume_mm3 as f32,
        total_weight: (inp.volume_mm3 / 1000.0 * 1.1) as f32, // ~1.1 g/ml, typical resin
        total_price: 0.0,
        price_unit: "$".into(),
        layer_content_offset: HEADER_SIZE,
        grey_scale_level: true,
        transition_layers: 0,
    }
}

/// Cuts a string to at most `max` bytes on a character boundary.
fn truncate(s: &str, max: usize) -> String {
    let mut end = s.len().min(max);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_string()
}
