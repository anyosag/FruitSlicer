//! Slicing on a background thread.

use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::mpsc::{channel, Receiver};
use std::sync::Arc;

use goo::GooFile;
use slicer::checks::CheckParams;
use slicer::mesh::Mesh;
use slicer::{slice_to_goo_with, PrintSettings, Printer, SliceHooks, SliceReport};

pub struct SliceOutput {
    pub report: SliceReport,
    /// Set when this is a file that was opened rather than sliced here.
    pub opened: Option<String>,
    /// Lit area of an opened file, mm.
    pub opened_bounds: Option<([f32; 2], [f32; 2])>,
    /// The written file, parsed back (this also checks it).
    pub goo: GooFile,
    pub settings: PrintSettings,
}

pub struct SliceJob {
    pub cancel: Arc<AtomicBool>,
    done: Arc<AtomicU32>,
    total: Arc<AtomicU32>,
    rx: Receiver<Result<SliceOutput, String>>,
}

impl SliceJob {
    pub fn start(
        mesh: Mesh,
        printer: Printer,
        settings: PrintSettings,
        checks: Option<CheckParams>,
        profile: String,
        ctx: eframe::egui::Context,
    ) -> Self {
        let cancel = Arc::new(AtomicBool::new(false));
        let done = Arc::new(AtomicU32::new(0));
        let total = Arc::new(AtomicU32::new(0));
        let (tx, rx) = channel();
        let (c, d, t) = (cancel.clone(), done.clone(), total.clone());
        std::thread::spawn(move || {
            let threads = std::thread::available_parallelism()
                .map(|n| n.get())
                .unwrap_or(1);
            let progress = |a: u32, b: u32| {
                d.store(a, Ordering::Relaxed);
                t.store(b, Ordering::Relaxed);
                ctx.request_repaint();
            };
            let mut bytes = Vec::new();
            let result = slice_to_goo_with(
                &mesh,
                &printer,
                &settings,
                &profile,
                &crate::util::timestamp(),
                &mut bytes,
                threads,
                &SliceHooks {
                    progress: Some(&progress),
                    cancel: Some(&c),
                    checks,
                },
            )
            .map_err(|e| e.to_string())
            .and_then(|report| {
                let goo = GooFile::read(bytes.as_slice())
                    .map_err(|e| format!("the sliced file did not read back: {e}"))?;
                Ok(SliceOutput {
                    report,
                    opened: None,
                    opened_bounds: None,
                    goo,
                    settings,
                })
            });
            let _ = tx.send(result);
            ctx.request_repaint();
        });
        Self {
            cancel,
            done,
            total,
            rx,
        }
    }

    /// Reads an existing .goo file and runs the print checks on it.
    pub fn open(
        bytes: Vec<u8>,
        name: String,
        checks: Option<CheckParams>,
        ctx: eframe::egui::Context,
    ) -> Self {
        let cancel = Arc::new(AtomicBool::new(false));
        let done = Arc::new(AtomicU32::new(0));
        let total = Arc::new(AtomicU32::new(0));
        let (tx, rx) = channel();
        let (c, d, t) = (cancel.clone(), done.clone(), total.clone());
        std::thread::spawn(move || {
            let started = std::time::Instant::now();
            let result = (|| {
                let goo = GooFile::read(bytes.as_slice()).map_err(|e| format!("{name}: {e}"))?;
                let progress = |a: u32, b: u32| {
                    d.store(a, Ordering::Relaxed);
                    t.store(b, Ordering::Relaxed);
                    ctx.request_repaint();
                };
                let params = checks.unwrap_or_default();
                let a = slicer::checks::check_goo(&goo, params, Some(&progress), Some(&c))?;
                let h = &goo.header;
                let settings = PrintSettings {
                    layer_mm: h.layer_thickness,
                    exposure_s: h.exposure_time,
                    bottom_exposure_s: h.bottom_exposure_time,
                    bottom_layers: h.bottom_layers,
                    light_pwm: h.light_pwm,
                    bottom_light_pwm: h.bottom_light_pwm,
                };
                let opened_bounds = a.bounds.map(|b| {
                    (
                        [b.min[0] as f32, b.min[1] as f32],
                        [b.max[0] as f32, b.max[1] as f32],
                    )
                });
                let report = SliceReport {
                    layer_count: goo.layers.len() as u32,
                    model_size_mm: [0.0; 3],
                    mesh_volume_mm3: a.lit_volume_mm3,
                    sliced_volume_mm3: a.lit_volume_mm3,
                    empty_layers: a.empty_layers,
                    encoded_bytes: goo.layers.iter().map(|l| l.1.len() as u64).sum(),
                    seconds: started.elapsed().as_secs_f64(),
                    issues: if checks.is_some() { a.issues } else { Vec::new() },
                };
                Ok(SliceOutput {
                    report,
                    opened: Some(name),
                    opened_bounds,
                    goo,
                    settings,
                })
            })();
            let _ = tx.send(result);
            ctx.request_repaint();
        });
        Self {
            cancel,
            done,
            total,
            rx,
        }
    }

    pub fn progress(&self) -> f32 {
        let t = self.total.load(Ordering::Relaxed);
        if t == 0 {
            0.0
        } else {
            self.done.load(Ordering::Relaxed) as f32 / t as f32
        }
    }

    pub fn poll(&self) -> Option<Result<SliceOutput, String>> {
        self.rx.try_recv().ok()
    }
}

/// Writes a parsed .goo back to disk.
pub fn save_goo(goo: &GooFile, path: &std::path::Path) -> Result<(), String> {
    let f = std::fs::File::create(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut w = goo::GooWriter::new(f, &goo.header).map_err(|e| e.to_string())?;
    for (def, data) in &goo.layers {
        w.write_layer(def, data).map_err(|e| e.to_string())?;
    }
    w.finish().map_err(|e| e.to_string())?;
    Ok(())
}
