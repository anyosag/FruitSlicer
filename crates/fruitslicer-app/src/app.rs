//! The FruitSlicer window: plate, settings, slicing and the layer viewer.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;

use eframe::egui::{self, Color32, RichText, Vec2};
use eframe::egui_wgpu;
use slicer::checks::{CheckParams, Issue, IssueKind};
use slicer::mesh::Mesh;
use slicer::supports::SupportParams;
use slicer::{stl, PrintSettings, Printer, SATURN_4_ULTRA_16K};

use crate::camera::{ray_box, ray_plane_z, Camera, View};
use crate::jobs::{save_goo, SliceJob, SliceOutput};
use crate::layer_view::{self, LayerImage, PixelRect};
use crate::render::{self, Draw, DrawKind, Gpu3d, ViewCallback};
use crate::scene::{self, Object, Placement};
use crate::util;

const KEY_GRID: u64 = u64::MAX - 1;
const KEY_OUTLINE: u64 = u64::MAX - 2;
const KEY_ISSUES: u64 = u64::MAX - 3;
const KEY_LAYER: u64 = u64::MAX - 4;

fn model_key(id: u64) -> u64 {
    id * 4
}
fn support_key(id: u64) -> u64 {
    id * 4 + 1
}

/// A saved set of exposure settings.
#[derive(Clone, serde::Serialize, serde::Deserialize)]
pub struct ResinProfile {
    pub name: String,
    pub settings: PrintSettings,
}

/// Everything remembered between runs.
#[derive(Clone, serde::Serialize, serde::Deserialize)]
#[serde(default)]
pub struct Prefs {
    pub print: PrintSettings,
    pub mirror_x: bool,
    pub supports: SupportParams,
    pub auto_supports: bool,
    pub checks: bool,
    pub check_params: CheckParams,
    pub highlight_overhangs: bool,
    pub hollow: bool,
    pub hollow_params: slicer::hollow::HollowParams,
    pub profiles: Vec<ResinProfile>,
    pub last_dir: Option<PathBuf>,
}

impl Default for Prefs {
    fn default() -> Self {
        Self {
            print: PrintSettings::default(),
            mirror_x: SATURN_4_ULTRA_16K.mirror_x,
            supports: SupportParams::default(),
            auto_supports: true,
            checks: true,
            check_params: CheckParams::default(),
            highlight_overhangs: true,
            hollow: false,
            hollow_params: Default::default(),
            profiles: Vec::new(),
            last_dir: None,
        }
    }
}

#[derive(PartialEq, Clone, Copy)]
enum Tab {
    Plate,
    Layers,
}

/// What a drag in the 3D view is doing.
enum Drag {
    None,
    Orbit,
    Pan,
    /// Moving an object: its id, the plane height, the grab point and the
    /// offset when the drag started.
    Move {
        id: u64,
        z: f32,
        start: glam::Vec3,
        offset: [f32; 2],
    },
}

/// Undo snapshot: every object's id, name, mesh and placement.
type Snapshot = Vec<(u64, String, Arc<Mesh>, Placement, bool)>;

pub struct App {
    prefs: Prefs,
    printer: Printer,
    objects: Vec<Object>,
    bases: HashMap<u64, Arc<Mesh>>,
    next_id: u64,
    selected: Option<u64>,
    camera: Camera,
    drag: Drag,
    tab: Tab,
    /// GPU copy version per key: (object version, supports shown, highlight).
    uploaded: HashMap<u64, (u64, bool, u32)>,
    gpu_ready: bool,
    plate_uploaded: bool,
    job: Option<SliceJob>,
    result: Option<SliceOutput>,
    /// Object versions the current result was sliced from.
    result_versions: Vec<(u64, u64, Option<u64>)>,
    layer: usize,
    layer_tex: Option<(usize, egui::TextureHandle, LayerImage)>,
    layer_zoom: f32,
    layer_pan: Vec2,
    selected_issue: Option<usize>,
    issues_uploaded_for: Option<(usize, Option<usize>)>,
    status: String,
    error: Option<String>,
    undo: Vec<Snapshot>,
    redo: Vec<Snapshot>,
    new_profile_name: String,
    show_about: bool,
    show_file_info: bool,
    ctx: egui::Context,
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>, open: Vec<PathBuf>) -> Self {
        let prefs: Prefs = cc
            .storage
            .and_then(|s| eframe::get_value(s, "prefs"))
            .unwrap_or_default();
        let mut gpu_ready = false;
        if let Some(rs) = cc.wgpu_render_state.as_ref() {
            Gpu3d::install(rs);
            gpu_ready = true;
        }
        let mut printer = SATURN_4_ULTRA_16K;
        printer.mirror_x = prefs.mirror_x;
        let mut app = Self {
            prefs,
            printer,
            objects: Vec::new(),
            bases: HashMap::new(),
            next_id: 1,
            selected: None,
            camera: Camera::default(),
            drag: Drag::None,
            tab: Tab::Plate,
            uploaded: HashMap::new(),
            gpu_ready,
            plate_uploaded: false,
            job: None,
            result: None,
            result_versions: Vec::new(),
            layer: 0,
            layer_tex: None,
            layer_zoom: 1.0,
            layer_pan: Vec2::ZERO,
            selected_issue: None,
            issues_uploaded_for: None,
            status: "Open an STL file to start (File > Open, or drop it on the window).".into(),
            error: None,
            undo: Vec::new(),
            redo: Vec::new(),
            new_profile_name: String::new(),
            show_about: false,
            show_file_info: false,
            ctx: cc.egui_ctx.clone(),
        };
        for p in open {
            app.open_path(&p);
        }
        app
    }

    // ----- model management ------------------------------------------------

    fn snapshot(&self) -> Snapshot {
        self.objects
            .iter()
            .map(|o| {
                (
                    o.id,
                    o.name.clone(),
                    self.bases[&o.id].clone(),
                    o.placement,
                    o.supported.is_some(),
                )
            })
            .collect()
    }

    /// Call before any change that should be undoable.
    fn checkpoint(&mut self) {
        let s = self.snapshot();
        self.undo.push(s);
        if self.undo.len() > 50 {
            self.undo.remove(0);
        }
        self.redo.clear();
    }

    fn restore(&mut self, snap: Snapshot) {
        let mut objs = Vec::new();
        for (id, name, base, placement, had_supports) in snap {
            let mut o = Object::new(id, name, (*base).clone());
            o.placement = placement;
            o.rebuild();
            if had_supports {
                let _ = o.generate_supports(&self.prefs.supports);
            }
            self.bases.insert(id, base);
            objs.push(o);
        }
        self.objects = objs;
        if self
            .selected
            .is_some_and(|s| !self.objects.iter().any(|o| o.id == s))
        {
            self.selected = None;
        }
        self.uploaded.clear();
    }

    fn undo(&mut self) {
        if let Some(s) = self.undo.pop() {
            let cur = self.snapshot();
            self.redo.push(cur);
            self.restore(s);
        }
    }

    fn redo(&mut self) {
        if let Some(s) = self.redo.pop() {
            let cur = self.snapshot();
            self.undo.push(cur);
            self.restore(s);
        }
    }

    fn add_object(&mut self, name: String, mesh: Mesh) {
        self.checkpoint();
        let id = self.next_id;
        self.next_id += 1;
        let base = Arc::new(mesh);
        self.bases.insert(id, base.clone());
        let mut o = Object::new(id, name, (*base).clone());
        // Put a second model next to the first rather than on top of it.
        if !self.objects.is_empty() {
            let right = self
                .objects
                .iter()
                .filter_map(|o| o.print_bounds())
                .map(|b| b.max[0])
                .fold(f32::NEG_INFINITY, f32::max);
            if let Some(b) = o.placed_bounds {
                o.placement.offset[0] = right + 5.0 + (b.max[0] - b.min[0]) / 2.0;
                o.rebuild();
            }
        }
        if self.prefs.auto_supports {
            let _ = o.generate_supports(&self.prefs.supports);
        }
        self.objects.push(o);
        self.selected = Some(id);
        self.frame_all();
    }

    fn open_path(&mut self, path: &std::path::Path) {
        match std::fs::read(path) {
            Ok(data) => self.open_bytes(path, &data),
            Err(e) => self.error = Some(format!("{}: {e}", path.display())),
        }
        if let Some(dir) = path.parent() {
            self.prefs.last_dir = Some(dir.to_path_buf());
        }
    }

    fn open_bytes(&mut self, path: &std::path::Path, data: &[u8]) {
        let name = path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| "model".into());
        let ext = path
            .extension()
            .map(|e| e.to_string_lossy().to_lowercase())
            .unwrap_or_default();
        if ext == "goo" {
            if self.job.is_some() {
                self.error = Some("Wait for the current job to finish first.".into());
                return;
            }
            self.job = Some(SliceJob::open(
                data.to_vec(),
                name.clone(),
                self.prefs.checks.then_some(self.prefs.check_params),
                self.ctx.clone(),
            ));
            self.status = format!("Reading and checking {name}.goo…");
            return;
        }
        let parsed = match ext.as_str() {
            "obj" => crate::import::parse_obj(data),
            "3mf" => crate::import::parse_3mf(data),
            _ => stl::parse(data).map_err(|e| e.to_string()),
        };
        match parsed {
            Ok(m) if m.triangles.is_empty() => {
                self.error = Some(format!("{name}: the file has no triangles"));
            }
            Ok(m) => {
                let n = m.triangles.len();
                self.add_object(name.clone(), m);
                self.status = format!("Loaded {name} ({n} triangles).");
            }
            Err(e) => self.error = Some(format!("{name}: {e}")),
        }
    }

    fn open_dialog(&mut self) {
        let mut d = rfd::FileDialog::new()
            .add_filter("3D models", &["stl", "STL", "obj", "OBJ", "3mf", "3MF"])
            .add_filter("Sliced files (.goo)", &["goo", "GOO"])
            .add_filter("All files", &["*"]);
        if let Some(dir) = &self.prefs.last_dir {
            d = d.set_directory(dir);
        }
        if let Some(paths) = d.pick_files() {
            for p in paths {
                self.open_path(&p);
            }
        }
    }

    fn selected_mut(&mut self) -> Option<&mut Object> {
        let id = self.selected?;
        self.objects.iter_mut().find(|o| o.id == id)
    }

    fn delete_selected(&mut self) {
        if let Some(id) = self.selected {
            self.checkpoint();
            self.objects.retain(|o| o.id != id);
            self.selected = None;
        }
    }

    fn duplicate_selected(&mut self) {
        let Some(id) = self.selected else { return };
        let Some(o) = self.objects.iter().find(|o| o.id == id) else {
            return;
        };
        let (name, placement) = (format!("{} copy", o.name), o.placement);
        let base = self.bases[&id].clone();
        self.add_object(name, (*base).clone());
        if let Some(n) = self.selected_mut() {
            let mut p = placement;
            p.offset = n.placement.offset;
            n.placement = p;
            n.rebuild();
        }
        self.refresh_supports();
    }

    /// Regenerates supports that are out of date (when auto supports are on).
    fn refresh_supports(&mut self) {
        if !self.prefs.auto_supports {
            return;
        }
        let p = self.prefs.supports;
        for o in &mut self.objects {
            if !o.supports_current() {
                if let Err(e) = o.generate_supports(&p) {
                    self.error = Some(e);
                    break;
                }
            }
        }
    }

    fn regenerate_all_supports(&mut self) {
        let p = self.prefs.supports;
        for o in &mut self.objects {
            if let Err(e) = o.generate_supports(&p) {
                self.error = Some(e);
                break;
            }
        }
    }

    fn frame_all(&mut self) {
        let mut min = glam::Vec3::splat(f32::INFINITY);
        let mut max = glam::Vec3::splat(f32::NEG_INFINITY);
        for o in &self.objects {
            if let Some(b) = o.print_bounds() {
                min = min.min(b.min.into());
                max = max.max(b.max.into());
            }
        }
        if !min.is_finite() {
            min = glam::Vec3::new(-self.printer.size_x / 2.0, -self.printer.size_y / 2.0, 0.0);
            max = glam::Vec3::new(self.printer.size_x / 2.0, self.printer.size_y / 2.0, 40.0);
        }
        let c = (min + max) / 2.0;
        let r = ((max - min).length() / 2.0).max(20.0);
        let yaw = self.camera.yaw;
        let pitch = self.camera.pitch;
        self.camera.set_view(View::Iso, c.into(), r);
        self.camera.yaw = yaw;
        self.camera.pitch = pitch;
    }

    // ----- slicing -----------------------------------------------------------

    fn result_is_current(&self) -> bool {
        if self.result.as_ref().is_some_and(|r| r.opened.is_some()) {
            return true;
        }
        let now: Vec<_> = self
            .objects
            .iter()
            .map(|o| (o.id, o.version, o.supports_version))
            .collect();
        self.result.is_some() && now == self.result_versions
    }

    fn start_slice(&mut self, ctx: &egui::Context) {
        if self.job.is_some() {
            return;
        }
        if self.objects.is_empty() {
            self.error = Some("Nothing to slice: open a model first.".into());
            return;
        }
        if let Some(o) = self.objects.iter().find(|o| !o.fits(&self.printer)) {
            self.error = Some(format!(
                "{} doesn't fit on the plate. Move it, scale it down or rotate it.",
                o.name
            ));
            return;
        }
        if let Err(e) = self.prefs.print.validate() {
            self.error = Some(e);
            return;
        }
        self.refresh_supports();
        let mesh = scene::print_mesh(&self.objects);
        let profile = self
            .objects
            .iter()
            .map(|o| o.name.as_str())
            .collect::<Vec<_>>()
            .join("+");
        self.result_versions = self
            .objects
            .iter()
            .map(|o| (o.id, o.version, o.supports_version))
            .collect();
        self.printer.mirror_x = self.prefs.mirror_x;
        self.job = Some(SliceJob::start(
            mesh,
            self.printer.clone(),
            self.prefs.print.clone(),
            self.prefs.checks.then_some(self.prefs.check_params),
            self.prefs.hollow.then_some(self.prefs.hollow_params),
            profile,
            ctx.clone(),
        ));
        self.status = "Slicing…".into();
    }

    fn poll_job(&mut self) {
        let Some(job) = &self.job else { return };
        if let Some(r) = job.poll() {
            self.job = None;
            match r {
                Ok(out) => {
                    let r = &out.report;
                    self.status = format!(
                        "{} {} layers in {:.1} s. {}",
                        match &out.opened {
                            Some(n) => format!("Opened {n}.goo:"),
                            None => "Sliced".to_string(),
                        },
                        r.layer_count,
                        r.seconds,
                        if r.issues.is_empty() {
                            "No problems found.".to_string()
                        } else {
                            format!("{} possible problems; see Results.", r.issues.len())
                        }
                    );
                    self.layer = self.layer.min(r.layer_count.saturating_sub(1) as usize);
                    self.layer_tex = None;
                    self.selected_issue = None;
                    self.result = Some(out);
                    self.tab = Tab::Layers;
                }
                Err(e) => {
                    self.status = "Slicing stopped.".into();
                    if !e.contains("cancelled") {
                        self.error = Some(e);
                    }
                }
            }
        }
    }

    fn save_dialog(&mut self) {
        let Some(out) = &self.result else {
            self.error = Some("Slice first, then save.".into());
            return;
        };
        let name = self
            .objects
            .first()
            .map(|o| o.name.clone())
            .unwrap_or_else(|| "print".into());
        let mut d = rfd::FileDialog::new()
            .add_filter("ELEGOO GOO", &["goo"])
            .set_file_name(format!("{name}.goo"));
        if let Some(dir) = &self.prefs.last_dir {
            d = d.set_directory(dir);
        }
        if let Some(path) = d.save_file() {
            match save_goo(&out.goo, &path) {
                Ok(()) => self.status = format!("Saved {}.", path.display()),
                Err(e) => self.error = Some(e),
            }
        }
    }

    // ----- GPU sync ----------------------------------------------------------

    fn sync_gpu(&mut self, rs: &egui_wgpu::RenderState) {
        if !self.plate_uploaded {
            let (grid, outline) = render::plate_lines([
                self.printer.size_x,
                self.printer.size_y,
                self.printer.size_z,
            ]);
            Gpu3d::upload(rs, KEY_GRID, &grid);
            Gpu3d::upload(rs, KEY_OUTLINE, &outline);
            self.plate_uploaded = true;
        }
        let hl = if self.prefs.highlight_overhangs {
            self.prefs.supports.overhang_angle_deg.to_bits()
        } else {
            0
        };
        let flag = self
            .prefs
            .highlight_overhangs
            .then(|| self.prefs.supports.overhang_angle_deg.to_radians().cos());
        let mut live = std::collections::HashSet::new();
        for o in &self.objects {
            let sup = o.supports_current() && o.supported.is_some();
            let want = (o.version, sup, hl);
            live.insert(model_key(o.id));
            live.insert(support_key(o.id));
            if self.uploaded.get(&model_key(o.id)) != Some(&want) {
                let verts = render::mesh_vertices(o.print_model(), flag, o.orient);
                Gpu3d::upload(rs, model_key(o.id), &verts);
                let sverts = match (&o.supported, sup) {
                    (Some(s), true) => render::mesh_vertices(&s.supports, None, 1.0),
                    _ => Vec::new(),
                };
                Gpu3d::upload(rs, support_key(o.id), &sverts);
                self.uploaded.insert(model_key(o.id), want);
            }
        }
        let stale: Vec<u64> = self
            .uploaded
            .keys()
            .copied()
            .filter(|k| !live.contains(k))
            .collect();
        for k in stale {
            Gpu3d::remove(rs, k);
            Gpu3d::remove(rs, k + 1);
            self.uploaded.remove(&k);
        }
        // Issue boxes and the current layer plane.
        let key = (self.layer, self.selected_issue);
        if self.issues_uploaded_for != Some(key) {
            let mut lines = Vec::new();
            let mut plane = Vec::new();
            if let Some(out) = &self.result {
                let h = out.settings.layer_mm;
                for (i, issue) in out.report.issues.iter().enumerate() {
                    if self.selected_issue.is_some_and(|s| s != i) {
                        continue;
                    }
                    let z0 = issue.first_layer as f32 * h;
                    let z1 = (issue.last_layer + 1) as f32 * h;
                    lines.extend(render::box_lines(
                        [issue.area.min[0] as f32, issue.area.min[1] as f32, z0],
                        [
                            issue.area.max[0] as f32,
                            issue.area.max[1] as f32,
                            z1.max(z0 + 0.2),
                        ],
                    ));
                }
                let rad = self.prefs.hollow_params.hole_diameter_mm / 2.0;
                for d in &out.report.drain_holes {
                    let (x, y) = (d.center_mm[0] as f32, d.center_mm[1] as f32);
                    lines.extend(render::box_lines(
                        [x - rad, y - rad, d.first_layer as f32 * h],
                        [x + rad, y + rad, (d.last_layer + 1) as f32 * h],
                    ));
                }
                if self.tab == Tab::Layers {
                    let z = (self.layer as f32 + 1.0) * h;
                    let (hx, hy) = (self.printer.size_x / 2.0, self.printer.size_y / 2.0);
                    plane.extend(render::box_lines([-hx, -hy, z], [hx, hy, z]));
                }
            }
            Gpu3d::upload(rs, KEY_ISSUES, &lines);
            Gpu3d::upload(rs, KEY_LAYER, &plane);
            self.issues_uploaded_for = Some(key);
        }
    }

    // ----- UI ------------------------------------------------------------------

    fn menu_bar(&mut self, ui: &mut egui::Ui) {
        egui::MenuBar::new().ui(ui, |ui| {
            ui.menu_button("File", |ui| {
                if ui.button("Open model…  (Ctrl+O)").clicked() {
                    ui.close();
                    self.open_dialog();
                }
                if ui
                    .add_enabled(
                        self.result.is_some(),
                        egui::Button::new("Save .goo…  (Ctrl+S)"),
                    )
                    .clicked()
                {
                    ui.close();
                    self.save_dialog();
                }
                ui.separator();
                if ui.button("Clear plate").clicked() {
                    ui.close();
                    self.checkpoint();
                    self.objects.clear();
                    self.selected = None;
                }
                if ui.button("Add test pattern").clicked() {
                    ui.close();
                    self.add_object("test pattern".into(), test_pattern());
                }
            });
            ui.menu_button("Edit", |ui| {
                if ui
                    .add_enabled(!self.undo.is_empty(), egui::Button::new("Undo  (Ctrl+Z)"))
                    .clicked()
                {
                    ui.close();
                    self.undo();
                }
                if ui
                    .add_enabled(!self.redo.is_empty(), egui::Button::new("Redo  (Ctrl+Y)"))
                    .clicked()
                {
                    ui.close();
                    self.redo();
                }
                ui.separator();
                if ui.button("Duplicate  (Ctrl+D)").clicked() {
                    ui.close();
                    self.duplicate_selected();
                }
                if ui.button("Delete  (Del)").clicked() {
                    ui.close();
                    self.delete_selected();
                }
                if ui.button("Arrange all").clicked() {
                    ui.close();
                    self.checkpoint();
                    if !scene::arrange(&mut self.objects, &self.printer, 4.0) {
                        self.error = Some("Not everything fits on the plate.".into());
                    }
                    self.refresh_supports();
                }
            });
            ui.menu_button("View", |ui| {
                for (label, v) in [
                    ("Iso", View::Iso),
                    ("Front", View::Front),
                    ("Top", View::Top),
                    ("Bottom", View::Bottom),
                    ("Left", View::Left),
                    ("Right", View::Right),
                ] {
                    if ui.button(label).clicked() {
                        ui.close();
                        let (t, d) = (self.camera.target, self.camera.distance);
                        self.camera.set_view(v, t, 1.0);
                        self.camera.distance = d;
                    }
                }
                ui.separator();
                if ui.button("Frame all  (F)").clicked() {
                    ui.close();
                    self.frame_all();
                }
                ui.checkbox(&mut self.prefs.highlight_overhangs, "Highlight overhangs");
            });
            ui.menu_button("Help", |ui| {
                if ui.button("About and limits").clicked() {
                    ui.close();
                    self.show_about = true;
                }
            });
        });
    }

    fn toolbar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if ui.button("Open…").clicked() {
                self.open_dialog();
            }
            ui.separator();
            let slicing = self.job.is_some();
            if slicing {
                let p = self.job.as_ref().map(|j| j.progress()).unwrap_or(0.0);
                ui.add(
                    egui::ProgressBar::new(p)
                        .desired_width(200.0)
                        .show_percentage(),
                );
                if ui.button("Cancel").clicked() {
                    if let Some(j) = &self.job {
                        j.cancel.store(true, std::sync::atomic::Ordering::Relaxed);
                    }
                }
            } else {
                let label = if self.result.is_some() && !self.result_is_current() {
                    "Slice again"
                } else {
                    "Slice"
                };
                if ui
                    .add_enabled(
                        !self.objects.is_empty(),
                        egui::Button::new(RichText::new(label).strong()),
                    )
                    .clicked()
                {
                    let ctx = ui.ctx().clone();
                    self.start_slice(&ctx);
                }
            }
            if ui
                .add_enabled(
                    self.result.is_some() && !slicing,
                    egui::Button::new("Save .goo…"),
                )
                .clicked()
            {
                self.save_dialog();
            }
            ui.separator();
            ui.selectable_value(&mut self.tab, Tab::Plate, "Plate");
            ui.add_enabled_ui(self.result.is_some(), |ui| {
                ui.selectable_value(&mut self.tab, Tab::Layers, "Layers");
            });
            if self.result.is_some() && !self.result_is_current() && !slicing {
                ui.label(
                    RichText::new("⚠ plate changed since slicing")
                        .color(Color32::from_rgb(230, 170, 60)),
                );
            }
        });
    }

    fn objects_panel(&mut self, ui: &mut egui::Ui) {
        ui.heading("Models");
        if self.objects.is_empty() {
            ui.label("No models yet. Open an STL, OBJ or 3MF file, or drop one on the window.");
        }
        let mut select = None;
        for o in &self.objects {
            let fits = o.fits(&self.printer);
            let text = if fits {
                RichText::new(&o.name)
            } else {
                RichText::new(format!("{} (off the plate)", o.name)).color(Color32::LIGHT_RED)
            };
            if ui
                .selectable_label(self.selected == Some(o.id), text)
                .clicked()
            {
                select = Some(o.id);
            }
        }
        if let Some(s) = select {
            self.selected = Some(s);
        }
        ui.horizontal(|ui| {
            if ui
                .add_enabled(self.selected.is_some(), egui::Button::new("Duplicate"))
                .clicked()
            {
                self.duplicate_selected();
            }
            if ui
                .add_enabled(self.selected.is_some(), egui::Button::new("Delete"))
                .clicked()
            {
                self.delete_selected();
            }
            if ui
                .add_enabled(self.objects.len() > 1, egui::Button::new("Arrange"))
                .clicked()
            {
                self.checkpoint();
                if !scene::arrange(&mut self.objects, &self.printer, 4.0) {
                    self.error = Some("Not everything fits on the plate.".into());
                }
                self.refresh_supports();
            }
        });
        ui.separator();

        let Some(id) = self.selected else { return };
        let Some(idx) = self.objects.iter().position(|o| o.id == id) else {
            return;
        };
        ui.heading("Placement");
        let before = self.objects[idx].placement;
        let mut p = before;
        let mut commit = false;
        egui::Grid::new("placement").num_columns(4).show(ui, |ui| {
            ui.label("Scale %");
            let mut pct = p.scale * 100.0;
            let r = ui.add(
                egui::DragValue::new(&mut pct)
                    .speed(0.5)
                    .range(1.0..=2000.0)
                    .suffix(" %"),
            );
            p.scale = pct / 100.0;
            commit |= r.drag_stopped() || r.lost_focus();
            ui.end_row();
            for (axis, i) in [("Rotate X", 0), ("Rotate Y", 1), ("Rotate Z", 2)] {
                ui.label(axis);
                let r = ui.add(
                    egui::DragValue::new(&mut p.rotation[i])
                        .speed(1.0)
                        .suffix("°"),
                );
                commit |= r.drag_stopped() || r.lost_focus();
                if ui.small_button("-90").clicked() {
                    p.rotation[i] -= 90.0;
                    commit = true;
                }
                if ui.small_button("+90").clicked() {
                    p.rotation[i] += 90.0;
                    commit = true;
                }
                ui.end_row();
            }
            for (axis, i) in [("Move X", 0), ("Move Y", 1)] {
                ui.label(axis);
                let r = ui.add(
                    egui::DragValue::new(&mut p.offset[i])
                        .speed(0.5)
                        .suffix(" mm"),
                );
                commit |= r.drag_stopped() || r.lost_focus();
                ui.end_row();
            }
        });
        ui.horizontal(|ui| {
            if ui.button("Centre").clicked() {
                p.offset = [0.0, 0.0];
                commit = true;
            }
            if ui.button("Reset").clicked() {
                p = Placement::default();
                commit = true;
            }
            if ui
                .button("Lay flat")
                .on_hover_text("Rotate so the largest flat face is on the plate")
                .clicked()
            {
                self.checkpoint();
                self.objects[idx].lay_flat();
                p = self.objects[idx].placement;
                self.refresh_supports();
            }
        });
        if p != before {
            if commit
                && self
                    .undo
                    .last()
                    .map(|s| s.iter().find(|x| x.0 == id).map(|x| x.3))
                    != Some(Some(before))
            {
                self.checkpoint();
            }
            let o = &mut self.objects[idx];
            o.placement = p;
            o.rebuild();
        }
        if commit {
            self.refresh_supports();
        }
        let o = &self.objects[idx];
        if let Some(b) = o.placed_bounds {
            let s = b.size();
            ui.label(format!("Size: {:.2} × {:.2} × {:.2} mm", s[0], s[1], s[2]));
        }
        ui.label(format!(
            "{} triangles, {:.2} ml",
            o.placed.triangles.len(),
            o.placed.volume() / 1000.0
        ));
        if let Some(s) = o.supported.as_ref().filter(|_| o.supports_current()) {
            let on_model = s.points.iter().filter(|p| p.on_model).count();
            ui.label(format!(
                "{} supports ({} rest on the model)",
                s.points.len(),
                on_model
            ));
        } else if !self.prefs.auto_supports {
            ui.label("No supports.");
        }
    }

    fn settings_panel(&mut self, ui: &mut egui::Ui) {
        egui::ScrollArea::vertical().show(ui, |ui| {
            egui::CollapsingHeader::new(RichText::new("Print settings").heading())
                .default_open(true)
                .show(ui, |ui| self.print_settings_ui(ui));
            egui::CollapsingHeader::new(RichText::new("Supports").heading())
                .default_open(true)
                .show(ui, |ui| self.supports_ui(ui));
            egui::CollapsingHeader::new(RichText::new("Hollowing").heading())
                .default_open(false)
                .show(ui, |ui| self.hollow_ui(ui));
            egui::CollapsingHeader::new(RichText::new("Print checks").heading())
                .default_open(false)
                .show(ui, |ui| self.checks_ui(ui));
            egui::CollapsingHeader::new(RichText::new("Results").heading())
                .default_open(true)
                .show(ui, |ui| self.results_ui(ui));
        });
    }

    fn print_settings_ui(&mut self, ui: &mut egui::Ui) {
        ui.label(RichText::new("Printer: ELEGOO Saturn 4 Ultra 16K").weak());
        let s = &mut self.prefs.print;
        egui::Grid::new("print").num_columns(2).show(ui, |ui| {
            ui.label("Layer height");
            ui.add(
                egui::DragValue::new(&mut s.layer_mm)
                    .speed(0.005)
                    .range(0.01..=0.3)
                    .suffix(" mm"),
            );
            ui.end_row();
            ui.label("Exposure");
            ui.add(
                egui::DragValue::new(&mut s.exposure_s)
                    .speed(0.05)
                    .range(0.1..=60.0)
                    .suffix(" s"),
            );
            ui.end_row();
            ui.label("Bottom exposure");
            ui.add(
                egui::DragValue::new(&mut s.bottom_exposure_s)
                    .speed(0.5)
                    .range(1.0..=200.0)
                    .suffix(" s"),
            );
            ui.end_row();
            ui.label("Bottom layers");
            ui.add(egui::DragValue::new(&mut s.bottom_layers).range(0..=50));
            ui.end_row();
            ui.label("UV power");
            let mut pwm = s.light_pwm;
            ui.add(egui::Slider::new(&mut pwm, 1..=255));
            s.light_pwm = pwm;
            s.bottom_light_pwm = pwm;
            ui.end_row();
        });
        ui.label(
            RichText::new("Exposure defaults are placeholders. Use your resin maker's numbers.")
                .small()
                .color(Color32::from_rgb(230, 170, 60)),
        );
        ui.checkbox(
            &mut self.prefs.mirror_x,
            "Mirror X (as UVtools' profile does)",
        )
        .on_hover_text(
            "Not yet confirmed against a file from ELEGOO's own slicer. If text prints \
                 backwards, turn this off.",
        );

        // Resin profiles.
        ui.separator();
        ui.label("Resin profiles");
        let mut load = None;
        let mut delete = None;
        for (i, p) in self.prefs.profiles.iter().enumerate() {
            ui.horizontal(|ui| {
                if ui.button("Use").clicked() {
                    load = Some(i);
                }
                if ui.small_button("✖").on_hover_text("Delete").clicked() {
                    delete = Some(i);
                }
                ui.label(format!(
                    "{}: {} mm, {} s / {} s × {}",
                    p.name,
                    p.settings.layer_mm,
                    p.settings.exposure_s,
                    p.settings.bottom_exposure_s,
                    p.settings.bottom_layers
                ));
            });
        }
        if let Some(i) = load {
            self.prefs.print = self.prefs.profiles[i].settings.clone();
        }
        if let Some(i) = delete {
            self.prefs.profiles.remove(i);
        }
        ui.horizontal(|ui| {
            ui.add(
                egui::TextEdit::singleline(&mut self.new_profile_name)
                    .hint_text("Name")
                    .desired_width(120.0),
            );
            if ui
                .add_enabled(
                    !self.new_profile_name.trim().is_empty(),
                    egui::Button::new("Save current"),
                )
                .clicked()
            {
                let name = self.new_profile_name.trim().to_string();
                self.prefs.profiles.retain(|p| p.name != name);
                self.prefs.profiles.push(ResinProfile {
                    name,
                    settings: self.prefs.print.clone(),
                });
                self.new_profile_name.clear();
            }
        });
    }

    fn supports_ui(&mut self, ui: &mut egui::Ui) {
        let before = self.prefs.supports;
        let auto_before = self.prefs.auto_supports;
        ui.checkbox(&mut self.prefs.auto_supports, "Automatic supports");
        let s = &mut self.prefs.supports;
        egui::Grid::new("supports").num_columns(2).show(ui, |ui| {
            ui.label("Overhang angle");
            ui.add(egui::Slider::new(&mut s.overhang_angle_deg, 10.0..=80.0).suffix("°"))
                .on_hover_text("Downward faces flatter than this get supports");
            ui.end_row();
            ui.label("Spacing");
            ui.add(
                egui::DragValue::new(&mut s.spacing_mm)
                    .speed(0.1)
                    .range(0.8..=20.0)
                    .suffix(" mm"),
            );
            ui.end_row();
            ui.label("Lift off plate");
            ui.add(
                egui::DragValue::new(&mut s.lift_mm)
                    .speed(0.1)
                    .range(0.0..=30.0)
                    .suffix(" mm"),
            )
            .on_hover_text("0 puts the model straight on the plate and only supports overhangs");
            ui.end_row();
            ui.label("Tip diameter");
            ui.add(
                egui::DragValue::new(&mut s.tip_diameter_mm)
                    .speed(0.01)
                    .range(0.1..=2.0)
                    .suffix(" mm"),
            );
            ui.end_row();
            ui.label("Tip length");
            ui.add(
                egui::DragValue::new(&mut s.tip_length_mm)
                    .speed(0.05)
                    .range(0.3..=10.0)
                    .suffix(" mm"),
            );
            ui.end_row();
            ui.label("Column diameter");
            ui.add(
                egui::DragValue::new(&mut s.column_diameter_mm)
                    .speed(0.05)
                    .range(0.3..=5.0)
                    .suffix(" mm"),
            );
            ui.end_row();
            ui.label("Foot diameter");
            ui.add(
                egui::DragValue::new(&mut s.foot_diameter_mm)
                    .speed(0.05)
                    .range(0.3..=10.0)
                    .suffix(" mm"),
            );
            ui.end_row();
            ui.label("Raft thickness");
            ui.add(
                egui::DragValue::new(&mut s.raft_mm)
                    .speed(0.05)
                    .range(0.0..=5.0)
                    .suffix(" mm"),
            );
            ui.end_row();
            ui.label("Support low points");
            ui.checkbox(&mut s.support_minima, "");
            ui.end_row();
        });
        ui.horizontal(|ui| {
            if ui.button("Regenerate").clicked() {
                self.regenerate_all_supports();
            }
            if ui.button("Remove supports").clicked() {
                self.prefs.auto_supports = false;
                for o in &mut self.objects {
                    o.clear_supports();
                }
            }
        });
        ui.label(
            RichText::new(
                "Straight columns only: no tree or branching supports. Defaults are untested \
                 starting points.",
            )
            .small()
            .weak(),
        );
        let changed = self.prefs.supports != before;
        if self.prefs.auto_supports && (changed || !auto_before) {
            // Regenerate when the user lets go, not on every drag step.
            let dragging = ui.ctx().input(|i| i.pointer.any_down());
            if !dragging || !auto_before {
                self.regenerate_all_supports();
            } else {
                for o in &mut self.objects {
                    o.supports_version = None;
                }
            }
        }
        if self.prefs.auto_supports && !ui.ctx().input(|i| i.pointer.any_down()) {
            self.refresh_supports();
        }
    }

    fn hollow_ui(&mut self, ui: &mut egui::Ui) {
        ui.checkbox(&mut self.prefs.hollow, "Hollow the print (saves resin)");
        let h = &mut self.prefs.hollow_params;
        ui.add_enabled_ui(self.prefs.hollow, |ui| {
            egui::Grid::new("hollow").num_columns(2).show(ui, |ui| {
                ui.label("Wall thickness");
                ui.add(
                    egui::DragValue::new(&mut h.wall_mm)
                        .speed(0.05)
                        .range(0.8..=10.0)
                        .suffix(" mm"),
                );
                ui.end_row();
                ui.label("Drain holes");
                ui.checkbox(&mut h.drain_holes, "add automatically");
                ui.end_row();
                ui.label("Hole diameter");
                ui.add(
                    egui::DragValue::new(&mut h.hole_diameter_mm)
                        .speed(0.05)
                        .range(0.8..=10.0)
                        .suffix(" mm"),
                );
                ui.end_row();
                ui.label("Hole spacing");
                ui.add(
                    egui::DragValue::new(&mut h.hole_spacing_mm)
                        .speed(0.5)
                        .range(2.0..=100.0)
                        .suffix(" mm"),
                );
                ui.end_row();
            });
        });
        ui.label(
            RichText::new(
                "Holes go through the floor of each hollow, on the plate side, so it vents while \
                 printing and drains afterwards. Give the model some lift so the holes aren't \
                 against the raft. Hollow ceilings have no internal supports yet: check the \
                 results for islands and overhangs inside.",
            )
            .small()
            .weak(),
        );
    }

    fn checks_ui(&mut self, ui: &mut egui::Ui) {
        ui.checkbox(&mut self.prefs.checks, "Check the print while slicing");
        let c = &mut self.prefs.check_params;
        egui::Grid::new("checks").num_columns(2).show(ui, |ui| {
            ui.label("Grid cell");
            ui.add(
                egui::DragValue::new(&mut c.cell_mm)
                    .speed(0.01)
                    .range(0.1..=1.0)
                    .suffix(" mm"),
            );
            ui.end_row();
            ui.label("Max overhang");
            ui.add(
                egui::DragValue::new(&mut c.overhang_mm)
                    .speed(0.05)
                    .range(0.2..=10.0)
                    .suffix(" mm"),
            )
            .on_hover_text("How far a layer may stick out past the one before it");
            ui.end_row();
            ui.label("Ignore areas under");
            ui.add(
                egui::DragValue::new(&mut c.min_area_mm2)
                    .speed(0.01)
                    .range(0.0..=10.0)
                    .suffix(" mm²"),
            );
            ui.end_row();
            ui.label("Ignore cups under");
            ui.add(
                egui::DragValue::new(&mut c.min_cup_area_mm2)
                    .speed(0.1)
                    .range(0.0..=100.0)
                    .suffix(" mm²"),
            );
            ui.end_row();
        });
        ui.label(
            RichText::new(
                "Checks use a coarse grid, so features smaller than a cell can be missed and \
                 narrow gaps can look closed.",
            )
            .small()
            .weak(),
        );
    }

    fn results_ui(&mut self, ui: &mut egui::Ui) {
        let Some(out) = &self.result else {
            ui.label("Slice to see layer count, print time, resin use and problems.");
            return;
        };
        let r = &out.report;
        let s = &out.settings;
        let secs = s.estimated_seconds(r.layer_count);
        if let Some(name) = &out.opened {
            ui.label(
                RichText::new(format!("Showing {name}.goo (opened, not sliced here)")).strong(),
            );
        }
        if ui.small_button("File details…").clicked() {
            self.show_file_info = true;
        }
        egui::Grid::new("results").num_columns(2).show(ui, |ui| {
            ui.label("Layers");
            ui.label(format!("{} × {} mm", r.layer_count, s.layer_mm));
            ui.end_row();
            ui.label("Print time");
            ui.label(format!("about {} (rough)", util::duration(secs)));
            ui.end_row();
            ui.label("Resin");
            ui.label(format!(
                "{:.1} ml (≈ {:.0} g)",
                r.sliced_volume_mm3 / 1000.0,
                r.sliced_volume_mm3 / 1000.0 * 1.1
            ));
            ui.end_row();
            ui.label("File size");
            ui.label(format!("{:.1} MB", r.encoded_bytes as f64 / 1e6));
            ui.end_row();
        });
        if !r.empty_layers.is_empty() {
            ui.label(
                RichText::new(format!(
                    "⚠ {} empty layers: something is floating or the mesh is broken.",
                    r.empty_layers.len()
                ))
                .color(Color32::LIGHT_RED),
            );
        }
        let diff = (r.sliced_volume_mm3 - r.mesh_volume_mm3).abs() / r.mesh_volume_mm3.max(1e-9);
        if diff > 0.05 {
            ui.label(
                RichText::new(format!(
                    "⚠ Sliced volume is {:.0}% off the mesh volume: the mesh may have holes or flipped faces.",
                    diff * 100.0
                ))
                .color(Color32::from_rgb(230, 170, 60)),
            );
        }
        if !r.drain_holes.is_empty() {
            ui.label(format!("{} drain holes cut:", r.drain_holes.len()));
            for d in &r.drain_holes {
                ui.label(
                    RichText::new(format!(
                        "  at ({:.1}, {:.1}) mm, layers {}–{}",
                        d.center_mm[0],
                        d.center_mm[1],
                        d.first_layer + 1,
                        d.last_layer + 1
                    ))
                    .small(),
                );
            }
        }
        ui.separator();
        if r.issues.is_empty() {
            if self.prefs.checks {
                ui.label(RichText::new("✔ No problems found.").color(Color32::LIGHT_GREEN));
            }
            return;
        }
        let mut counts: HashMap<IssueKind, usize> = HashMap::new();
        for i in &r.issues {
            *counts.entry(i.kind).or_default() += 1;
        }
        for kind in [
            IssueKind::Island,
            IssueKind::UnsupportedOverhang,
            IssueKind::SuctionCup,
            IssueKind::TrappedResin,
        ] {
            if let Some(n) = counts.get(&kind) {
                ui.label(
                    RichText::new(format!("{} × {}", n, kind.label())).color(issue_color(kind)),
                )
                .on_hover_text(issue_help(kind));
            }
        }
        ui.label(RichText::new("Click one to jump to it:").small().weak());
        let mut jump = None;
        egui::ScrollArea::vertical()
            .max_height(260.0)
            .id_salt("issues")
            .show(ui, |ui| {
                for (i, issue) in r.issues.iter().enumerate() {
                    let text = issue_text(issue);
                    let sel = self.selected_issue == Some(i);
                    if ui
                        .selectable_label(sel, RichText::new(text).color(issue_color(issue.kind)))
                        .on_hover_text(issue_help(issue.kind))
                        .clicked()
                    {
                        jump = Some(i);
                    }
                }
            });
        if let Some(i) = jump {
            if self.selected_issue == Some(i) {
                self.selected_issue = None;
            } else {
                self.selected_issue = Some(i);
                let first = out.report.issues[i].first_layer as usize;
                self.layer = first;
                self.tab = Tab::Layers;
                self.center_layer_on_issue(i);
            }
        }
        if self.selected_issue.is_some() && ui.button("Show all problems").clicked() {
            self.selected_issue = None;
        }
    }

    fn center_layer_on_issue(&mut self, i: usize) {
        let Some(out) = &self.result else { return };
        let issue = &out.report.issues[i];
        if let Some((_, _, img)) = &self.layer_tex {
            let c = issue.area.center();
            let uv = img.mm_to_uv(&out.goo.header, [c[0] as f32, c[1] as f32]);
            self.layer_zoom = self.layer_zoom.max(3.0);
            // layer_pan is in units of the fitted image size.
            self.layer_pan = Vec2::new(0.5 - uv[0], 0.5 - uv[1]);
        }
    }

    fn view_3d(&mut self, ui: &mut egui::Ui) {
        let (rect, resp) =
            ui.allocate_exact_size(ui.available_size(), egui::Sense::click_and_drag());
        let aspect = rect.width() / rect.height().max(1.0);
        let to_ndc = |p: egui::Pos2| {
            [
                (p.x - rect.left()) / rect.width() * 2.0 - 1.0,
                1.0 - (p.y - rect.top()) / rect.height() * 2.0,
            ]
        };

        // Picking.
        let pick = |app: &App, p: egui::Pos2| -> Option<(u64, f32)> {
            let (o, d) = app.camera.ray(to_ndc(p), aspect);
            app.objects
                .iter()
                .filter_map(|ob| {
                    let b = ob.print_bounds()?;
                    ray_box(o, d, b.min.into(), b.max.into()).map(|t| (ob.id, t))
                })
                .min_by(|a, b| a.1.total_cmp(&b.1))
        };

        if resp.drag_started() {
            let (primary, secondary, middle) = ui.input(|i| {
                (
                    i.pointer.button_down(egui::PointerButton::Primary),
                    i.pointer.button_down(egui::PointerButton::Secondary),
                    i.pointer.button_down(egui::PointerButton::Middle),
                )
            });
            let shift = ui.input(|i| i.modifiers.shift);
            self.drag = if secondary || middle || (primary && shift) {
                Drag::Pan
            } else if let Some((id, t)) = resp
                .interact_pointer_pos()
                .and_then(|p| pick(self, p))
                .filter(|_| self.tab == Tab::Plate)
            {
                let (o, d) = self
                    .camera
                    .ray(to_ndc(resp.interact_pointer_pos().unwrap()), aspect);
                let hit = o + d * t;
                self.selected = Some(id);
                self.checkpoint();
                let offset = self
                    .objects
                    .iter()
                    .find(|o| o.id == id)
                    .unwrap()
                    .placement
                    .offset;
                Drag::Move {
                    id,
                    z: hit.z,
                    start: hit,
                    offset,
                }
            } else {
                Drag::Orbit
            };
        }
        if resp.dragged() {
            let d = resp.drag_delta();
            match &self.drag {
                Drag::Orbit => self.camera.orbit(d.x, d.y),
                Drag::Pan => self.camera.pan(d.x, d.y, rect.height()),
                Drag::Move {
                    id,
                    z,
                    start,
                    offset,
                } => {
                    if let Some(p) = resp.interact_pointer_pos() {
                        let (o, dir) = self.camera.ray(to_ndc(p), aspect);
                        if let Some(hit) = ray_plane_z(o, dir, *z) {
                            let delta = hit - *start;
                            let (id, offset) = (*id, *offset);
                            if let Some(ob) = self.objects.iter_mut().find(|o| o.id == id) {
                                ob.placement.offset = [offset[0] + delta.x, offset[1] + delta.y];
                                ob.rebuild();
                            }
                        }
                    }
                }
                Drag::None => {}
            }
        }
        if resp.drag_stopped() {
            if matches!(self.drag, Drag::Move { .. }) {
                self.refresh_supports();
            }
            self.drag = Drag::None;
        }
        if resp.clicked() {
            self.selected = resp
                .interact_pointer_pos()
                .and_then(|p| pick(self, p))
                .map(|x| x.0);
        }
        if resp.hovered() {
            let scroll = ui.input(|i| i.smooth_scroll_delta.y);
            if scroll != 0.0 {
                self.camera.zoom(scroll / 50.0);
            }
        }

        // Draw list.
        let mut draws = vec![
            Draw {
                key: KEY_GRID,
                color: [0.35, 0.37, 0.42, 0.5],
                kind: DrawKind::Lines,
                highlight_overhangs: false,
                clip_z: None,
            },
            Draw {
                key: KEY_OUTLINE,
                color: [0.55, 0.6, 0.7, 0.9],
                kind: DrawKind::Lines,
                highlight_overhangs: false,
                clip_z: None,
            },
        ];
        let layer_mode = self.tab == Tab::Layers;
        let clip = if layer_mode {
            self.result
                .as_ref()
                .map(|r| (self.layer as f32 + 1.0) * r.settings.layer_mm)
        } else {
            None
        };
        for o in &self.objects {
            let fits = o.fits(&self.printer);
            let sel = self.selected == Some(o.id);
            let color = if !fits {
                [0.85, 0.3, 0.3, 1.0]
            } else if sel {
                [0.98, 0.72, 0.30, 1.0]
            } else {
                [0.85, 0.62, 0.32, 1.0]
            };
            draws.push(Draw {
                key: model_key(o.id),
                color,
                kind: DrawKind::Solid,
                highlight_overhangs: self.prefs.highlight_overhangs && !layer_mode,
                clip_z: clip,
            });
            draws.push(Draw {
                key: support_key(o.id),
                color: [0.55, 0.75, 0.9, 1.0],
                kind: DrawKind::Solid,
                highlight_overhangs: false,
                clip_z: clip,
            });
            if layer_mode {
                // The part not yet printed, faint.
                draws.push(Draw {
                    key: model_key(o.id),
                    color: [color[0], color[1], color[2], 0.12],
                    kind: DrawKind::Ghost,
                    highlight_overhangs: false,
                    clip_z: None,
                });
            }
        }
        if self.result.is_some() && self.result_is_current() {
            draws.push(Draw {
                key: KEY_ISSUES,
                color: [1.0, 0.3, 0.9, 1.0],
                kind: DrawKind::Lines,
                highlight_overhangs: false,
                clip_z: None,
            });
            if layer_mode {
                draws.push(Draw {
                    key: KEY_LAYER,
                    color: [0.3, 1.0, 0.5, 1.0],
                    kind: DrawKind::Lines,
                    highlight_overhangs: false,
                    clip_z: None,
                });
            }
        }
        let cb = ViewCallback {
            view_proj: self.camera.proj(aspect) * self.camera.view(),
            light_dir: (self.camera.eye() - glam::Vec3::from(self.camera.target)).normalize()
                + glam::Vec3::new(0.2, -0.1, 0.4),
            draws,
        };
        ui.painter()
            .rect_filled(rect, 0.0, Color32::from_rgb(28, 30, 36));
        ui.painter()
            .add(egui_wgpu::Callback::new_paint_callback(rect, cb));

        // Overlay help.
        ui.painter().text(
            rect.left_bottom() + Vec2::new(8.0, -8.0),
            egui::Align2::LEFT_BOTTOM,
            "Left drag: orbit (or move a model)   Right drag / Shift+drag: pan   Wheel: zoom",
            egui::FontId::proportional(12.0),
            Color32::from_gray(140),
        );
    }

    fn layers_view(&mut self, ui: &mut egui::Ui) {
        let Some(out) = &self.result else {
            ui.label("Slice first.");
            return;
        };
        let n = out.goo.layers.len();
        if n == 0 {
            return;
        }
        let h = out.goo.header.clone();
        let layer_mm = out.settings.layer_mm;
        ui.horizontal(|ui| {
            ui.label("Layer");
            let mut l = self.layer + 1;
            ui.add(egui::DragValue::new(&mut l).range(1..=n));
            ui.style_mut().spacing.slider_width = (ui.available_width() - 190.0).max(100.0);
            ui.add(egui::Slider::new(&mut l, 1..=n).show_value(false));
            self.layer = l.clamp(1, n) - 1;
            ui.label(format!(
                "of {n}, Z = {:.2} mm",
                (self.layer + 1) as f32 * layer_mm
            ));
            if ui.button("Fit").clicked() {
                self.layer_zoom = 1.0;
                self.layer_pan = Vec2::ZERO;
            }
        });
        let (up, down) = ui.input(|i| {
            (
                i.key_pressed(egui::Key::ArrowUp) || i.key_pressed(egui::Key::PageUp),
                i.key_pressed(egui::Key::ArrowDown) || i.key_pressed(egui::Key::PageDown),
            )
        });
        if up {
            self.layer = (self.layer + 1).min(n - 1);
        }
        if down {
            self.layer = self.layer.saturating_sub(1);
        }

        // Render the layer image if needed. Crop to the print's footprint.
        if self.layer_tex.as_ref().map(|t| t.0) != Some(self.layer) {
            let mut min = [f32::INFINITY; 2];
            let mut max = [f32::NEG_INFINITY; 2];
            for o in &self.objects {
                if let Some(b) = o.print_bounds() {
                    min = [min[0].min(b.min[0]), min[1].min(b.min[1])];
                    max = [max[0].max(b.max[0]), max[1].max(b.max[1])];
                }
            }
            let rect = if let Some((a, b)) = out.opened_bounds {
                PixelRect::around_mm(&h, a, b, 3.0)
            } else if min[0].is_finite() && out.opened.is_none() {
                PixelRect::around_mm(&h, min, max, 3.0)
            } else {
                PixelRect::full(&h)
            };
            match layer_view::render(&h, &out.goo.layers[self.layer].1, rect, 2048) {
                Ok(img) => {
                    let rgba: Vec<Color32> = img
                        .pixels
                        .iter()
                        .map(|&v| {
                            if v == 0 {
                                Color32::from_rgb(16, 18, 24)
                            } else {
                                let k = v as f32 / 255.0;
                                Color32::from_rgb(
                                    (16.0 + k * 214.0) as u8,
                                    (18.0 + k * 150.0) as u8,
                                    (24.0 + k * 40.0) as u8,
                                )
                            }
                        })
                        .collect();
                    let ci = egui::ColorImage::new([img.width, img.height], rgba);
                    let tex = ui
                        .ctx()
                        .load_texture("layer", ci, egui::TextureOptions::NEAREST);
                    self.layer_tex = Some((self.layer, tex, img));
                }
                Err(e) => {
                    self.error = Some(format!("layer {}: {e}", self.layer));
                    return;
                }
            }
        }
        let Some((_, tex, img)) = &self.layer_tex else {
            return;
        };

        let (rect, resp) =
            ui.allocate_exact_size(ui.available_size(), egui::Sense::click_and_drag());
        ui.painter()
            .rect_filled(rect, 0.0, Color32::from_rgb(10, 11, 14));
        // Physical aspect of the image.
        let mm_w = (img.rect.x1 - img.rect.x0) as f32 * h.size_x / h.resolution_x as f32;
        let mm_h = (img.rect.y1 - img.rect.y0) as f32 * h.size_y / h.resolution_y as f32;
        let fit = (rect.width() / mm_w).min(rect.height() / mm_h) * 0.95;
        if resp.hovered() {
            let scroll = ui.input(|i| i.smooth_scroll_delta.y);
            if scroll != 0.0 {
                self.layer_zoom = (self.layer_zoom * (1.0 + scroll / 400.0)).clamp(0.5, 64.0);
            }
        }
        let size = Vec2::new(mm_w, mm_h) * fit * self.layer_zoom;
        if resp.dragged() {
            self.layer_pan += resp.drag_delta() / size;
        }
        let center = rect.center() + self.layer_pan * size;
        let img_rect = egui::Rect::from_center_size(center, size);
        let painter = ui.painter_at(rect);
        painter.image(
            tex.id(),
            img_rect,
            egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
            Color32::WHITE,
        );
        // Issues on this layer.
        for (i, issue) in out.report.issues.iter().enumerate() {
            let on_layer =
                (issue.first_layer as usize..=issue.last_layer as usize).contains(&self.layer);
            if !on_layer && self.selected_issue != Some(i) {
                continue;
            }
            let a = img.mm_to_uv(&h, [issue.area.min[0] as f32, issue.area.max[1] as f32]);
            let b = img.mm_to_uv(&h, [issue.area.max[0] as f32, issue.area.min[1] as f32]);
            let r = egui::Rect::from_min_max(
                img_rect.min + Vec2::new(a[0], a[1]) * size,
                img_rect.min + Vec2::new(b[0], b[1]) * size,
            )
            .expand(2.0);
            let c = issue_color(issue.kind);
            let width = if self.selected_issue == Some(i) {
                3.0
            } else {
                1.5
            };
            let stroke = if on_layer {
                egui::Stroke::new(width, c)
            } else {
                egui::Stroke::new(1.0, c.gamma_multiply(0.4))
            };
            painter.rect_stroke(r, 2.0, stroke, egui::StrokeKind::Outside);
            if on_layer && (r.width() > 60.0 || self.selected_issue == Some(i)) {
                painter.text(
                    r.left_top() - Vec2::new(0.0, 2.0),
                    egui::Align2::LEFT_BOTTOM,
                    issue.kind.label(),
                    egui::FontId::proportional(12.0),
                    c,
                );
            }
        }
        // Cursor position readout.
        if let Some(p) = resp.hover_pos() {
            let uv = (p - img_rect.min) / size;
            if (0.0..=1.0).contains(&uv.x) && (0.0..=1.0).contains(&uv.y) {
                let mm = img.uv_to_mm(&h, uv.x, uv.y);
                painter.text(
                    rect.right_top() + Vec2::new(-8.0, 8.0),
                    egui::Align2::RIGHT_TOP,
                    format!("X {:.2}  Y {:.2} mm", mm[0], mm[1]),
                    egui::FontId::monospace(12.0),
                    Color32::from_gray(170),
                );
            }
        }
        painter.text(
            rect.left_bottom() + Vec2::new(8.0, -8.0),
            egui::Align2::LEFT_BOTTOM,
            "Seen from above the plate (X mirror undone).  Up/Down: layer   Wheel: zoom   Drag: pan",
            egui::FontId::proportional(12.0),
            Color32::from_gray(140),
        );
    }

    fn file_info_window(&mut self, ctx: &egui::Context) {
        let Some(out) = &self.result else { return };
        let h = &out.goo.header;
        let mut open = self.show_file_info;
        egui::Window::new("File details")
            .open(&mut open)
            .default_width(420.0)
            .vscroll(true)
            .show(ctx, |ui| {
                ui.label("Every header field, as stored. Compare these with a file from ELEGOO's own slicer to check the unverified ones.");
                egui::Grid::new("hdr").striped(true).num_columns(2).show(ui, |ui| {
                    let mut row = |k: &str, v: String| {
                        ui.label(k);
                        ui.label(v);
                        ui.end_row();
                    };
                    row("Version", h.version.clone());
                    row("Software", format!("{} {}", h.software_info, h.software_version));
                    row("Created", h.file_time.clone());
                    row("Printer", format!("{} ({})", h.printer_name, h.printer_type));
                    row("Profile", h.profile_name.clone());
                    row("Resolution", format!("{} × {} px", h.resolution_x, h.resolution_y));
                    row("Mirror X / Y", format!("{} / {}", h.mirror_x, h.mirror_y));
                    row("Plate", format!("{} × {} × {} mm", h.size_x, h.size_y, h.size_z));
                    row("Layers", format!("{} × {} mm", h.layer_count, h.layer_thickness));
                    row("Bottom layers", format!("{} (transition {})", h.bottom_layers, h.transition_layers));
                    row("Exposure", format!("{} s (bottom {} s)", h.exposure_time, h.bottom_exposure_time));
                    row("Light PWM", format!("{} (bottom {})", h.light_pwm, h.bottom_light_pwm));
                    row("Delay mode", format!("{:?}", h.delay_mode));
                    row("Lift", format!("{} mm @ {}, then {} mm @ {}", h.lift_distance, h.lift_speed, h.lift_distance_2, h.lift_speed_2));
                    row("Retract", format!("{} mm @ {}, then {} mm @ {}", h.retract_distance, h.retract_speed, h.retract_distance_2, h.retract_speed_2));
                    row("Bottom lift", format!("{} mm @ {}, then {} mm @ {}", h.bottom_lift_distance, h.bottom_lift_speed, h.bottom_lift_distance_2, h.bottom_lift_speed_2));
                    row("Bottom retract", format!("{} mm @ {}, then {} mm @ {}", h.bottom_retract_distance, h.bottom_retract_speed, h.bottom_retract_distance_2, h.bottom_retract_speed_2));
                    row("Waits", format!("before lift {} s, after lift {} s, after retract {} s", h.before_lift_time, h.after_lift_time, h.after_retract_time));
                    row("Bottom waits", format!("before lift {} s, after lift {} s, after retract {} s", h.bottom_before_lift_time, h.bottom_after_lift_time, h.bottom_after_retract_time));
                    row("Turn-off time", format!("{} s", h.turn_off_time));
                    row("Anti-aliasing", format!("level {}, grey {}, blur {}, 8-bit {}", h.anti_aliasing_level, h.grey_level, h.blur_level, h.grey_scale_level));
                    row("Advance mode", format!("{}", h.advance_mode));
                    row("Estimates", format!("{} s, {:.1} mm³, {:.1} g", h.printing_time, h.total_volume, h.total_weight));
                });
                if let Some((def, _)) = out.goo.layers.get(self.layer) {
                    ui.separator();
                    ui.label(RichText::new(format!("Layer {} fields", self.layer + 1)).strong());
                    ui.label(format!("{def:#?}"));
                }
            });
        self.show_file_info = open;
    }

    fn about_window(&mut self, ctx: &egui::Context) {
        let mut open = self.show_about;
        egui::Window::new("About FruitSlicer")
            .open(&mut open)
            .default_width(460.0)
            .show(ctx, |ui| {
                ui.label(format!("FruitSlicer {}", env!("CARGO_PKG_VERSION")));
                ui.label("A resin slicer for the ELEGOO Saturn 4 Ultra 16K, written from scratch in Rust.");
                ui.separator();
                ui.label(RichText::new("Not verified on a real printer yet:").strong());
                for line in [
                    "X mirroring (on by default, from UVtools' profile).",
                    "Tilt/lift fields in the .goo file (0.05 placeholders from UVtools).",
                    "Exposure defaults (placeholders, set your resin's values).",
                    "Support sizes and spacing (untested starting points).",
                    "The print-time estimate (exposure plus about 5 s per layer).",
                ] {
                    ui.label(format!("• {line}"));
                }
                ui.label("Open a file in UVtools to double-check it before printing.");
            });
        self.show_about = open;
    }
}

fn issue_color(k: IssueKind) -> Color32 {
    match k {
        IssueKind::Island => Color32::from_rgb(255, 90, 90),
        IssueKind::UnsupportedOverhang => Color32::from_rgb(255, 170, 60),
        IssueKind::SuctionCup => Color32::from_rgb(120, 180, 255),
        IssueKind::TrappedResin => Color32::from_rgb(220, 120, 255),
    }
}

fn issue_help(k: IssueKind) -> &'static str {
    match k {
        IssueKind::Island => "Starts printing in mid-air with nothing under it. It will fall into the vat. Add supports.",
        IssueKind::UnsupportedOverhang => "A layer sticks out too far past the one before it. It may sag or peel off. Add supports or tilt the model.",
        IssueKind::SuctionCup => "A hollow open only towards the vat floor. Each lift pulls a vacuum, which can rip the print off or crack it. Tilt the model or add a vent hole.",
        IssueKind::TrappedResin => "A hollow the print closes off completely. Uncured resin stays inside and can crack the print later. Add a drain hole.",
    }
}

fn issue_text(i: &Issue) -> String {
    let layers = if i.first_layer == i.last_layer {
        format!("layer {}", i.first_layer + 1)
    } else {
        format!("layers {}–{}", i.first_layer + 1, i.last_layer + 1)
    };
    let c = i.area.center();
    let size = if i.kind == IssueKind::TrappedResin {
        format!("{:.1} mm³", i.size)
    } else {
        format!("{:.1} mm²", i.size)
    };
    format!(
        "{}: {layers}, at ({:.1}, {:.1}), {size}",
        i.kind.label(),
        c[0],
        c[1]
    )
}

/// The CLI's calibration pattern: a 20 x 20 mm square and four pads.
fn test_pattern() -> Mesh {
    let h = 3.0;
    let mut m = Mesh::cuboid([-10.0, -10.0, 0.0], [10.0, 10.0, h]);
    for (x, y) in [(-60.0, -35.0), (60.0, -35.0), (-60.0, 35.0), (60.0, 35.0)] {
        m.append(Mesh::cuboid([x - 5.0, y - 5.0, 0.0], [x + 5.0, y + 5.0, h]));
    }
    m
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.poll_job();
        if let Some(rs) = frame.wgpu_render_state() {
            if self.gpu_ready {
                self.sync_gpu(rs);
            }
        }

        // Dropped files.
        let dropped = ctx.input(|i| i.raw.dropped_files.clone());
        for f in dropped {
            let path = f.path().to_path_buf();
            match f.bytes() {
                Ok(bytes) => {
                    self.open_bytes(&path, &bytes);
                    if let Some(dir) = path.parent() {
                        self.prefs.last_dir = Some(dir.to_path_buf());
                    }
                }
                Err(e) => self.error = Some(format!("{}: {e}", path.display())),
            }
        }

        // Shortcuts.
        let typing = ctx.egui_wants_keyboard_input();
        let (open, save, undo, redo, dup, del, frame_all) = ctx.input(|i| {
            let c = i.modifiers.command;
            (
                c && i.key_pressed(egui::Key::O),
                c && i.key_pressed(egui::Key::S),
                c && !i.modifiers.shift && i.key_pressed(egui::Key::Z),
                c && (i.key_pressed(egui::Key::Y)
                    || (i.modifiers.shift && i.key_pressed(egui::Key::Z))),
                c && i.key_pressed(egui::Key::D),
                !typing && i.key_pressed(egui::Key::Delete),
                !typing && !c && i.key_pressed(egui::Key::F),
            )
        });
        if open {
            self.open_dialog();
        }
        if save && self.result.is_some() {
            self.save_dialog();
        }
        if undo && !typing {
            self.undo();
        }
        if redo && !typing {
            self.redo();
        }
        if dup {
            self.duplicate_selected();
        }
        if del && self.tab == Tab::Plate {
            self.delete_selected();
        }
        if frame_all {
            self.frame_all();
        }

        egui::Panel::top("menu").show(ui, |ui| {
            self.menu_bar(ui);
            self.toolbar(ui);
        });
        egui::Panel::bottom("status").show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(&self.status);
            });
        });
        egui::Panel::left("objects")
            .default_size(270.0)
            .show(ui, |ui| {
                egui::ScrollArea::vertical()
                    .id_salt("left")
                    .show(ui, |ui| self.objects_panel(ui));
            });
        egui::Panel::right("settings")
            .default_size(330.0)
            .show(ui, |ui| self.settings_panel(ui));
        egui::CentralPanel::default()
            .frame(egui::Frame::NONE)
            .show(ui, |ui| match self.tab {
                Tab::Plate => self.view_3d(ui),
                Tab::Layers if self.result.as_ref().is_some_and(|r| r.opened.is_some()) => {
                    ui.vertical(|ui| self.layers_view(ui));
                }
                Tab::Layers => {
                    // Split: layer image on the left, a small 3D view on the right.
                    let avail = ui.available_size();
                    ui.horizontal(|ui| {
                        ui.allocate_ui(Vec2::new(avail.x * 0.62, avail.y), |ui| {
                            ui.vertical(|ui| self.layers_view(ui));
                        });
                        ui.allocate_ui(Vec2::new(ui.available_width(), avail.y), |ui| {
                            self.view_3d(ui);
                        });
                    });
                }
            });

        if let Some(e) = self.error.clone() {
            let mut open = true;
            egui::Window::new("Problem")
                .collapsible(false)
                .resizable(false)
                .open(&mut open)
                .anchor(egui::Align2::CENTER_CENTER, Vec2::ZERO)
                .show(&ctx, |ui| {
                    ui.label(&e);
                    if ui.button("OK").clicked() {
                        self.error = None;
                    }
                });
            if !open {
                self.error = None;
            }
        }
        self.about_window(&ctx);
        self.file_info_window(&ctx);
        if self.job.is_some() {
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }
    }

    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        eframe::set_value(storage, "prefs", &self.prefs);
    }
}
