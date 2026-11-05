//! FruitSlicer command-line tools.
//!
//! ```text
//! fruitslicer info <file.goo> [--layer N --pgm out.pgm --scale S]
//! fruitslicer test-pattern <out.goo> [--exposure S] [--bottom-exposure S]
//!                                    [--bottom-layers N] [--layer-height MM] [--height MM]
//! ```

mod printer;
mod test_pattern;

use std::fs::File;
use std::io::{BufWriter, Write};
use std::process::ExitCode;
use std::time::{SystemTime, UNIX_EPOCH};

use goo::GooFile;

const USAGE: &str = "usage:
  fruitslicer info <file.goo> [--layer N --pgm out.pgm [--scale S]]
  fruitslicer test-pattern <out.goo> [--exposure S] [--bottom-exposure S]
                                     [--bottom-layers N] [--layer-height MM] [--height MM]";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.first().map(String::as_str) {
        Some("info") => info(&args[1..]),
        Some("test-pattern") => test_pattern_cmd(&args[1..]),
        _ => Err(USAGE.to_string()),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}

/// Splits arguments into one positional path and `--flag value` pairs.
fn parse(args: &[String]) -> Result<(String, Vec<(String, String)>), String> {
    let mut path = None;
    let mut flags = vec![];
    let mut it = args.iter();
    while let Some(a) = it.next() {
        if let Some(name) = a.strip_prefix("--") {
            let v = it
                .next()
                .ok_or(format!("--{name} needs a value\n\n{USAGE}"))?;
            flags.push((name.to_string(), v.clone()));
        } else if path.is_none() {
            path = Some(a.clone());
        } else {
            return Err(format!("unexpected argument {a}\n\n{USAGE}"));
        }
    }
    Ok((path.ok_or(USAGE.to_string())?, flags))
}

fn flag<T: std::str::FromStr>(flags: &[(String, String)], name: &str) -> Result<Option<T>, String> {
    flags
        .iter()
        .find(|(n, _)| n == name)
        .map(|(_, v)| {
            v.parse()
                .map_err(|_| format!("--{name}: cannot parse {v:?}"))
        })
        .transpose()
}

fn info(args: &[String]) -> Result<(), String> {
    let (path, flags) = parse(args)?;
    let file = File::open(&path).map_err(|e| format!("{path}: {e}"))?;
    let goo = GooFile::read(file).map_err(|e| format!("{path}: {e}"))?;
    let h = &goo.header;
    println!("{path}: valid GOO {}", h.version);
    println!(
        "  software        {} {}",
        h.software_info, h.software_version
    );
    println!("  created         {}", h.file_time);
    println!("  printer         {} ({})", h.printer_name, h.printer_type);
    println!("  profile         {}", h.profile_name);
    println!(
        "  resolution      {} x {} px, mirror x={} y={}",
        h.resolution_x, h.resolution_y, h.mirror_x, h.mirror_y
    );
    println!(
        "  platform        {} x {} x {} mm",
        h.size_x, h.size_y, h.size_z
    );
    println!(
        "  layers          {} x {} mm ({} bottom, {} transition)",
        h.layer_count, h.layer_thickness, h.bottom_layers, h.transition_layers
    );
    println!(
        "  exposure        {} s, bottom {} s, delay mode {:?}",
        h.exposure_time, h.bottom_exposure_time, h.delay_mode
    );
    println!(
        "  lift            {} mm @ {} mm/min, then {} mm @ {}",
        h.lift_distance, h.lift_speed, h.lift_distance_2, h.lift_speed_2
    );
    println!(
        "  retract         {} mm @ {} mm/min, then {} mm @ {}",
        h.retract_distance, h.retract_speed, h.retract_distance_2, h.retract_speed_2
    );
    println!(
        "  bottom lift     {} mm @ {} mm/min, then {} mm @ {}",
        h.bottom_lift_distance,
        h.bottom_lift_speed,
        h.bottom_lift_distance_2,
        h.bottom_lift_speed_2
    );
    println!(
        "  bottom retract  {} mm @ {} mm/min, then {} mm @ {}",
        h.bottom_retract_distance,
        h.bottom_retract_speed,
        h.bottom_retract_distance_2,
        h.bottom_retract_speed_2
    );
    println!(
        "  waits (bottom)  before lift {} s, after lift {} s, after retract {} s",
        h.bottom_before_lift_time, h.bottom_after_lift_time, h.bottom_after_retract_time
    );
    println!(
        "  waits           before lift {} s, after lift {} s, after retract {} s, turn-off {} s",
        h.before_lift_time, h.after_lift_time, h.after_retract_time, h.turn_off_time
    );
    println!(
        "  light PWM       {} (bottom {})",
        h.light_pwm, h.bottom_light_pwm
    );
    println!(
        "  anti-aliasing   level {}, grey {}, blur {}, 8-bit grey {}",
        h.anti_aliasing_level, h.grey_level, h.blur_level, h.grey_scale_level
    );
    println!("  advance mode    {}", h.advance_mode);
    println!(
        "  estimates       {} s, {} mm3, {} g, {} {}",
        h.printing_time, h.total_volume, h.total_weight, h.total_price, h.price_unit
    );
    let total: usize = goo.layers.iter().map(|(_, d)| d.len()).sum();
    println!("  layer data      {total} bytes encoded");

    if let Some(n) = flag::<usize>(&flags, "layer")? {
        let (def, data) = goo.layers.get(n).ok_or(format!("no layer {n}"))?;
        println!("layer {n}: {def:?}, {} bytes", data.len());
        if let Some(out) = flag::<String>(&flags, "pgm")? {
            let scale = flag::<usize>(&flags, "scale")?.unwrap_or(8).max(1);
            write_pgm(&out, h, data, scale)?;
            println!("wrote {out} at 1/{scale} scale");
        }
    }
    Ok(())
}

/// Writes a downscaled greyscale preview of one layer (max of each block).
fn write_pgm(out: &str, h: &goo::Header, data: &[u8], scale: usize) -> Result<(), String> {
    let (w, hgt) = (h.resolution_x as usize, h.resolution_y as usize);
    let (ow, oh) = (w.div_ceil(scale), hgt.div_ceil(scale));
    let mut img = vec![0u8; ow * oh];
    let mut pos = 0usize;
    goo::rle::decode_runs(data, |v, len| {
        if v != 0 {
            for p in pos..pos + len as usize {
                let (x, y) = (p % w, p / w);
                let o = &mut img[(y / scale) * ow + x / scale];
                *o = (*o).max(v);
            }
        }
        pos += len as usize;
    })
    .map_err(|e| e.to_string())?;
    let mut f = BufWriter::new(File::create(out).map_err(|e| format!("{out}: {e}"))?);
    write!(f, "P5\n{ow} {oh}\n255\n")
        .and_then(|_| f.write_all(&img))
        .map_err(|e| e.to_string())
}

fn test_pattern_cmd(args: &[String]) -> Result<(), String> {
    let (path, flags) = parse(args)?;
    let mut s = test_pattern::Settings::default();
    if let Some(v) = flag(&flags, "exposure")? {
        s.exposure_s = v;
    }
    if let Some(v) = flag(&flags, "bottom-exposure")? {
        s.bottom_exposure_s = v;
    }
    if let Some(v) = flag(&flags, "bottom-layers")? {
        s.bottom_layers = v;
    }
    if let Some(v) = flag(&flags, "layer-height")? {
        s.layer_mm = v;
    }
    if let Some(v) = flag(&flags, "height")? {
        s.height_mm = v;
    }

    let p = printer::SATURN_4_ULTRA_16K;
    let out = File::create(&path).map_err(|e| format!("{path}: {e}"))?;
    let layers = test_pattern::write(out, &p, &s, &timestamp()).map_err(|e| e.to_string())?;
    println!("wrote {path}: {layers} layers for {}", p.name);
    println!(
        "settings: {} mm layers, {} s exposure, {} s x {} bottom layers",
        s.layer_mm, s.exposure_s, s.bottom_exposure_s, s.bottom_layers
    );
    eprintln!("note: exposure defaults are placeholders; set them for your resin before printing.");
    Ok(())
}

/// UTC time as "YYYY-MM-DD HH:MM:SS", without pulling in a date crate.
fn timestamp() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let (days, rem) = (secs / 86_400, secs % 86_400);
    // Civil-from-days (Howard Hinnant's algorithm).
    let z = days as i64 + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + (m <= 2) as i64;
    format!(
        "{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02}",
        rem / 3600,
        rem % 3600 / 60,
        rem % 60
    )
}
