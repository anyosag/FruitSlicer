//! Slicing a placed mesh into a .goo file.

use std::io::Write;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

use goo::{GooWriter, LayerEncoder, Preview};

use crate::mesh::{Mesh, Triangle};
use crate::printer::Printer;
use crate::raster::{slice_layer, LayerSpans, PixelGrid};
use crate::settings::{build_header, HeaderInputs, PrintSettings};
use crate::{preview, SliceError};

/// An encoded layer and its lit pixel count.
type EncodedLayer = (Vec<u8>, u64);

/// What happened during a slice, for the user.
#[derive(Debug, Clone)]
pub struct SliceReport {
    pub layer_count: u32,
    pub model_size_mm: [f32; 3],
    pub mesh_volume_mm3: f64,
    /// Volume of lit pixels across all layers; differs from the mesh volume
    /// by pixel rounding, and a lot if the mesh is not closed.
    pub sliced_volume_mm3: f64,
    /// 0-based indices of layers with no lit pixels.
    pub empty_layers: Vec<u32>,
    pub encoded_bytes: u64,
    pub seconds: f64,
}

/// Slices `mesh`, which must already be placed: X/Y relative to the plate
/// centre, resting on Z = 0. Writes the .goo file to `out`.
pub fn slice_to_goo(
    mesh: &Mesh,
    printer: &Printer,
    settings: &PrintSettings,
    profile_name: &str,
    timestamp: &str,
    out: impl Write,
    threads: usize,
) -> Result<SliceReport, SliceError> {
    let started = Instant::now();
    settings.validate().map_err(SliceError::Settings)?;
    let bounds = mesh.bounds().ok_or(SliceError::EmptyModel)?;
    check_fits(&bounds.min, &bounds.max, printer)?;

    let h = settings.layer_mm as f64;
    let height = bounds.max[2] as f64;
    let layer_count = ((height / h) - 1e-6).ceil().max(0.0) as u32;
    if layer_count == 0 {
        return Err(SliceError::EmptyModel);
    }

    let header = build_header(
        printer,
        settings,
        HeaderInputs {
            profile_name,
            timestamp,
            layer_count,
            volume_mm3: mesh.volume(),
            small_preview: preview::render(mesh, printer, Preview::SMALL),
            big_preview: preview::render(mesh, printer, Preview::BIG),
        },
    );
    let mut writer = GooWriter::new(out, &header)?;

    let buckets = bucket_by_layer(&mesh.triangles, layer_count, h);
    let (mm_x, mm_y) = printer.pixel_mm();
    let grid = PixelGrid {
        width: printer.resolution_x as u32,
        height: printer.resolution_y as u32,
        mm_per_px_x: mm_x,
        mm_per_px_y: mm_y,
    };

    let threads = threads.max(1);
    let batch = threads * 4;
    let mut lit_total = 0u64;
    let mut empty_layers = Vec::new();
    let mut encoded_bytes = 0u64;

    for start in (0..layer_count).step_by(batch) {
        let end = (start + batch as u32).min(layer_count);
        let results: Vec<Mutex<Option<EncodedLayer>>> =
            (start..end).map(|_| Mutex::new(None)).collect();
        let next = AtomicUsize::new(0);
        std::thread::scope(|scope| {
            for _ in 0..threads.min((end - start) as usize) {
                scope.spawn(|| loop {
                    let k = next.fetch_add(1, Ordering::Relaxed);
                    if k >= results.len() {
                        break;
                    }
                    let layer = start as usize + k;
                    let z = (layer as f64 + 0.5) * h;
                    let tris = buckets[layer].iter().map(|&t| &mesh.triangles[t as usize]);
                    let spans = slice_layer(tris, z, &grid);
                    let lit = spans.lit_pixels();
                    let data = encode(&spans, grid.width, printer.mirror_x);
                    *results[k].lock().unwrap() = Some((data, lit));
                });
            }
        });
        for (k, cell) in results.into_iter().enumerate() {
            let layer = start + k as u32;
            let (data, lit) = cell.into_inner().unwrap().expect("every layer is computed");
            if lit == 0 {
                empty_layers.push(layer);
            }
            lit_total += lit;
            encoded_bytes += data.len() as u64;
            writer.write_layer(&settings.layer_def(printer, layer), &data)?;
        }
    }
    writer.finish()?;

    Ok(SliceReport {
        layer_count,
        model_size_mm: bounds.size(),
        mesh_volume_mm3: mesh.volume(),
        sliced_volume_mm3: lit_total as f64 * mm_x * mm_y * h,
        empty_layers,
        encoded_bytes,
        seconds: started.elapsed().as_secs_f64(),
    })
}

fn check_fits(min: &[f32; 3], max: &[f32; 3], p: &Printer) -> Result<(), SliceError> {
    let (hx, hy) = (p.size_x / 2.0, p.size_y / 2.0);
    let eps = 1e-3;
    if min[0] < -hx - eps || max[0] > hx + eps || min[1] < -hy - eps || max[1] > hy + eps {
        return Err(SliceError::DoesNotFit(format!(
            "model spans X {:.2}..{:.2} mm and Y {:.2}..{:.2} mm; the plate is ±{hx:.2} x ±{hy:.2} mm",
            min[0], max[0], min[1], max[1]
        )));
    }
    if min[2] < -eps {
        return Err(SliceError::DoesNotFit(format!(
            "model goes below the plate (Z min {:.3} mm)",
            min[2]
        )));
    }
    if max[2] > p.size_z + eps {
        return Err(SliceError::DoesNotFit(format!(
            "model is {:.2} mm tall; the printer's Z limit is {} mm",
            max[2], p.size_z
        )));
    }
    Ok(())
}

/// For each layer, the triangles that can cross its cutting plane at
/// `(i + 0.5) * h`.
fn bucket_by_layer(tris: &[Triangle], layer_count: u32, h: f64) -> Vec<Vec<u32>> {
    let mut buckets = vec![Vec::new(); layer_count as usize];
    for (i, t) in tris.iter().enumerate() {
        let zmin = t.iter().map(|v| v[2]).fold(f32::INFINITY, f32::min) as f64;
        let zmax = t.iter().map(|v| v[2]).fold(f32::NEG_INFINITY, f32::max) as f64;
        if zmax <= zmin {
            continue; // flat triangles never cross a plane
        }
        // Planes with zmin <= z < zmax, widened by one layer for rounding.
        let first = ((zmin / h - 0.5).floor() as i64 - 1).max(0);
        let last = ((zmax / h - 0.5).ceil() as i64 + 1).min(layer_count as i64 - 1);
        for layer in first..=last {
            buckets[layer as usize].push(i as u32);
        }
    }
    buckets
}

/// Encodes spans row by row, mirroring X if the printer needs it.
fn encode(spans: &LayerSpans, width: u32, mirror_x: bool) -> Vec<u8> {
    let mut enc = LayerEncoder::new();
    let mut row_runs: Vec<(u32, u32)> = Vec::new();
    for row in &spans.rows {
        row_runs.clear();
        if mirror_x {
            row_runs.extend(row.iter().rev().map(|&(a, b)| (width - b, width - a)));
        } else {
            row_runs.extend_from_slice(row);
        }
        let mut x = 0;
        for &(a, b) in &row_runs {
            enc.push_run(0x00, (a - x) as u64);
            enc.push_run(0xFF, (b - a) as u64);
            x = b;
        }
        enc.push_run(0x00, (width - x) as u64);
    }
    enc.finish()
}
