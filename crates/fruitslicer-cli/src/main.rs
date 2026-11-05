//! FruitSlicer command-line tools. Run with no arguments for usage.

mod test_pattern;

use std::fs::File;
use std::io::{BufWriter, Write};
use std::process::ExitCode;
use std::time::{SystemTime, UNIX_EPOCH};

use goo::GooFile;
use slicer::checks::CheckParams;
use slicer::{
    slice_to_goo_with, stl, Mesh, PrintSettings, Printer, SliceHooks, SATURN_4_ULTRA_16K,
};

const USAGE: &str = "usage:
  fruitslicer slice <model.stl> [--out file.goo] [placement] [settings]
  fruitslicer test-pattern <out.goo> [--height MM] [settings]
  fruitslicer info <file.goo> [--layer N --pgm out.pgm [--scale S]]

placement (slice only; the model is centred on the plate and dropped to Z=0):
  --scale F  --rotate-x DEG --rotate-y DEG --rotate-z DEG  --x MM --y MM

settings (defaults are placeholders: use your resin's numbers):
  --layer-height MM (0.05)  --exposure S (2.5)  --bottom-exposure S (30)
  --bottom-layers N (4)  --pwm 0-255 (255)  --mirror-x on|off (on)  --threads N

supports and checks (slice only):
  --supports on|off (off)  --lift MM (5)  --overhang-angle DEG (45)  --support-spacing MM (3)
  --checks on|off (on): report islands, overhangs, suction cups and trapped resin
  --hollow WALL_MM: hollow the print  --drain-holes on|off (on)  --hole-diameter MM (2.5)";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.first().map(String::as_str) {
        Some("info") => info(&args[1..]),
        Some("slice") => slice_cmd(&args[1..]),
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

/// Reads the exposure flags shared by `slice` and `test-pattern`.
fn print_settings(flags: &[(String, String)]) -> Result<PrintSettings, String> {
    let mut s = PrintSettings::default();
    if let Some(v) = flag(flags, "exposure")? {
        s.exposure_s = v;
    }
    if let Some(v) = flag(flags, "bottom-exposure")? {
        s.bottom_exposure_s = v;
    }
    if let Some(v) = flag(flags, "bottom-layers")? {
        s.bottom_layers = v;
    }
    if let Some(v) = flag(flags, "layer-height")? {
        s.layer_mm = v;
    }
    if let Some(v) = flag(flags, "pwm")? {
        s.light_pwm = v;
        s.bottom_light_pwm = v;
    }
    s.validate()?;
    Ok(s)
}

fn hollow_params(
    flags: &[(String, String)],
) -> Result<Option<slicer::hollow::HollowParams>, String> {
    let Some(wall) = flag::<f32>(flags, "hollow")? else {
        return Ok(None);
    };
    let mut p = slicer::hollow::HollowParams {
        wall_mm: wall,
        ..Default::default()
    };
    if let Some(d) = flag(flags, "hole-diameter")? {
        p.hole_diameter_mm = d;
    }
    p.drain_holes = flag::<String>(flags, "drain-holes")?.as_deref() != Some("off");
    Ok(Some(p))
}

fn printer(flags: &[(String, String)]) -> Result<Printer, String> {
    let mut p = SATURN_4_ULTRA_16K;
    if let Some(m) = flag::<String>(flags, "mirror-x")? {
        p.mirror_x = match m.as_str() {
            "on" => true,
            "off" => false,
            _ => return Err("--mirror-x takes on or off".into()),
        };
    }
    Ok(p)
}

fn threads(flags: &[(String, String)]) -> Result<usize, String> {
    Ok(flag(flags, "threads")?.unwrap_or_else(|| {
        std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(1)
    }))
}

fn run_slice(
    mesh: &Mesh,
    out_path: &str,
    profile: &str,
    flags: &[(String, String)],
) -> Result<(), String> {
    let settings = print_settings(flags)?;
    let p = printer(flags)?;
    let out = File::create(out_path).map_err(|e| format!("{out_path}: {e}"))?;
    let checks = flag::<String>(flags, "checks")?.as_deref() != Some("off");
    let r = slice_to_goo_with(
        mesh,
        &p,
        &settings,
        profile,
        &timestamp(),
        out,
        threads(flags)?,
        &SliceHooks {
            checks: checks.then(CheckParams::default),
            hollow: hollow_params(flags)?,
            ..Default::default()
        },
    )
    .map_err(|e| e.to_string())?;
    let [sx, sy, sz] = r.model_size_mm;
    println!("wrote {out_path} for {}", p.name);
    println!(
        "  model           {sx:.2} x {sy:.2} x {sz:.2} mm, {} triangles",
        mesh.triangles.len()
    );
    println!(
        "  layers          {} x {} mm ({} bottom at {} s, rest at {} s)",
        r.layer_count,
        settings.layer_mm,
        settings.bottom_layers.min(r.layer_count),
        settings.bottom_exposure_s,
        settings.exposure_s
    );
    println!(
        "  volume          {:.1} mm3 from the mesh, {:.1} mm3 sliced",
        r.mesh_volume_mm3, r.sliced_volume_mm3
    );
    println!("  mirror X        {}", p.mirror_x);
    println!(
        "  took            {:.2} s, {:.1} MB",
        r.seconds,
        r.encoded_bytes as f64 / 1e6
    );
    if !r.empty_layers.is_empty() {
        let shown: Vec<String> = r.empty_layers.iter().take(10).map(u32::to_string).collect();
        eprintln!(
            "warning: {} empty layer(s) (0-based: {}{}); the model may float or be broken",
            r.empty_layers.len(),
            shown.join(", "),
            if r.empty_layers.len() > 10 {
                ", ..."
            } else {
                ""
            }
        );
    }
    let diff = (r.sliced_volume_mm3 - r.mesh_volume_mm3).abs() / r.mesh_volume_mm3.max(1e-9);
    if diff > 0.05 {
        eprintln!(
            "warning: sliced volume differs from mesh volume by {:.0}%; the mesh may have holes or flipped faces",
            diff * 100.0
        );
    }
    for d in &r.drain_holes {
        println!(
            "  drain hole      at ({:.1}, {:.1}) mm through layers {}-{}",
            d.center_mm[0], d.center_mm[1], d.first_layer, d.last_layer
        );
    }
    for i in &r.issues {
        let c = i.area.center();
        let unit = if i.kind == slicer::checks::IssueKind::TrappedResin {
            "mm3"
        } else {
            "mm2"
        };
        eprintln!(
            "check: {} at layers {}-{}, around ({:.1}, {:.1}) mm, {:.1} {unit}",
            i.kind.label(),
            i.first_layer,
            i.last_layer,
            c[0],
            c[1],
            i.size
        );
    }
    eprintln!("note: exposure defaults are placeholders; set them for your resin before printing.");
    Ok(())
}

fn slice_cmd(args: &[String]) -> Result<(), String> {
    let (input, flags) = parse(args)?;
    let output = flag::<String>(&flags, "out")?.unwrap_or_else(|| {
        format!(
            "{}.goo",
            input.trim_end_matches(".stl").trim_end_matches(".STL")
        )
    });
    let data = std::fs::read(&input).map_err(|e| format!("{input}: {e}"))?;
    let mut mesh = stl::parse(&data).map_err(|e| format!("{input}: {e}"))?;
    if let Some(s) = flag::<f32>(&flags, "scale")? {
        mesh.scale(s);
    }
    let rot = [
        flag(&flags, "rotate-x")?.unwrap_or(0.0),
        flag(&flags, "rotate-y")?.unwrap_or(0.0),
        flag(&flags, "rotate-z")?.unwrap_or(0.0),
    ];
    if rot != [0.0; 3] {
        mesh.rotate_xyz(rot);
    }
    mesh.place_on_plate([
        flag(&flags, "x")?.unwrap_or(0.0),
        flag(&flags, "y")?.unwrap_or(0.0),
    ]);
    if flag::<String>(&flags, "supports")?.as_deref() == Some("on") {
        let mut sp = slicer::supports::SupportParams::default();
        if let Some(v) = flag(&flags, "lift")? {
            sp.lift_mm = v;
        }
        if let Some(v) = flag(&flags, "overhang-angle")? {
            sp.overhang_angle_deg = v;
        }
        if let Some(v) = flag(&flags, "support-spacing")? {
            sp.spacing_mm = v;
        }
        let s = slicer::supports::generate(&mesh, &sp)?;
        println!("added {} supports", s.points.len());
        mesh = s.combined();
    }
    let profile = std::path::Path::new(&input)
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    run_slice(&mesh, &output, &profile, &flags)
}

fn test_pattern_cmd(args: &[String]) -> Result<(), String> {
    let (path, flags) = parse(args)?;
    let height = flag(&flags, "height")?.unwrap_or(3.0);
    run_slice(
        &test_pattern::mesh(height),
        &path,
        "FruitSlicer test pattern",
        &flags,
    )
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
