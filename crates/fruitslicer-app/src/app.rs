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
use crate::theme;
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
enum SettingsTab {
    Print,
    Supports,
    Hollow,
    Checks,
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
#[derive(Clone)]
struct SavedObject {
    id: u64,
    name: String,
    base: Arc<Mesh>,
    placement: Placement,
    had_supports: bool,
    added: Vec<[f32; 3]>,
    removed: Vec<[f32; 3]>,
}
type Snapshot = Vec<SavedObject>;

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
    settings_tab: SettingsTab,
    /// Show the 3D cut-away next to the layer image.
    show_cutaway: bool,
    /// A problem to zoom the layer image onto, next frame.
    focus_issue: Option<usize>,
    /// GPU copy version per key: (object version, supports shown, highlight).
    uploaded: HashMap<u64, (u64, bool, u32)>,
    gpu_ready: bool,
    plate_uploaded: bool,
    job: Option<SliceJob>,
    result: Option<SliceOutput>,
    /// Where the plate was last saved or opened as a project.
    project_path: Option<PathBuf>,
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
    edit_supports: bool,
    /// Open and non-manifold edges per object id.
    mesh_errors: HashMap<u64, (usize, usize)>,
}

impl App {
    pub fn new(
        cc: &eframe::CreationContext<'_>,
        open: Vec<PathBuf>,
        gpu_note: Option<String>,
    ) -> Self {
        let prefs: Prefs = cc
            .storage
            .and_then(|s| eframe::get_value(s, "prefs"))
            .unwrap_or_default();
        theme::install(&cc.egui_ctx);
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
            settings_tab: SettingsTab::Print,
            show_cutaway: true,
            focus_issue: None,
            uploaded: HashMap::new(),
            gpu_ready,
            plate_uploaded: false,
            job: None,
            result: None,
            project_path: None,
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
            edit_supports: false,
            mesh_errors: HashMap::new(),
        };
        for p in open {
            app.open_path(&p);
        }
        if let Some(n) = gpu_note {
            app.status = n;
        }
        app
    }

    // ----- model management ------------------------------------------------

    fn snapshot(&self) -> Snapshot {
        self.objects
            .iter()
            .map(|o| SavedObject {
                id: o.id,
                name: o.name.clone(),
                base: self.bases[&o.id].clone(),
                placement: o.placement,
                had_supports: o.supported.is_some(),
                added: o.added.clone(),
                removed: o.removed.clone(),
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
        for so in snap {
            let mut o = Object::new(so.id, so.name, (*so.base).clone());
            o.placement = so.placement;
            o.added = so.added;
            o.removed = so.removed;
            o.rebuild();
            if so.had_supports {
                let _ = o.generate_supports(&self.prefs.supports);
            }
            self.bases.insert(so.id, so.base);
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
        self.mesh_errors.insert(id, scene::mesh_errors(&base));
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
        if ext == crate::project::EXTENSION {
            self.load_project(path, data);
            return;
        }
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
            .add_filter("FruitSlicer projects", &[crate::project::EXTENSION])
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

    fn project_settings(&self) -> crate::project::ProjectSettings {
        let p = &self.prefs;
        crate::project::ProjectSettings {
            print: p.print.clone(),
            supports: p.supports,
            checks: p.checks,
            check_params: p.check_params,
            hollow: p.hollow,
            hollow_params: p.hollow_params,
        }
    }

    fn save_project_dialog(&mut self) {
        if self.objects.is_empty() {
            return;
        }
        let objs: Vec<_> = self
            .objects
            .iter()
            .map(|o| {
                (
                    crate::project::ProjectObject {
                        name: o.name.clone(),
                        placement: o.placement,
                        supports: o.supported.is_some(),
                        added: o.added.clone(),
                        removed: o.removed.clone(),
                        mesh: String::new(),
                    },
                    &o.base,
                )
            })
            .collect();
        let bytes = match crate::project::write(&self.project_settings(), &objs) {
            Ok(b) => b,
            Err(e) => {
                self.error = Some(format!("Could not save the project: {e}"));
                return;
            }
        };
        let name = self
            .project_path
            .as_ref()
            .and_then(|p| p.file_name())
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| format!("plate.{}", crate::project::EXTENSION));
        let mut d = rfd::FileDialog::new()
            .add_filter("FruitSlicer project", &[crate::project::EXTENSION])
            .set_file_name(name);
        if let Some(dir) = &self.prefs.last_dir {
            d = d.set_directory(dir);
        }
        if let Some(mut path) = d.save_file() {
            if path.extension().is_none() {
                path.set_extension(crate::project::EXTENSION);
            }
            match std::fs::write(&path, bytes) {
                Ok(()) => {
                    self.status = format!("Saved project {}.", path.display());
                    self.project_path = Some(path);
                }
                Err(e) => self.error = Some(format!("{}: {e}", path.display())),
            }
        }
    }

    /// Replaces the plate and the project settings with a saved project.
    fn load_project(&mut self, path: &std::path::Path, data: &[u8]) {
        let proj = match crate::project::read(data) {
            Ok(p) => p,
            Err(e) => {
                self.error = Some(format!("{}: {e}", path.display()));
                return;
            }
        };
        self.checkpoint();
        let s = proj.settings;
        self.prefs.print = s.print;
        self.prefs.supports = s.supports;
        self.prefs.checks = s.checks;
        self.prefs.check_params = s.check_params;
        self.prefs.hollow = s.hollow;
        self.prefs.hollow_params = s.hollow_params;
        self.objects.clear();
        self.selected = None;
        let n = proj.objects.len();
        for (po, mesh) in proj.objects {
            let id = self.next_id;
            self.next_id += 1;
            let base = Arc::new(mesh);
            self.mesh_errors.insert(id, scene::mesh_errors(&base));
            self.bases.insert(id, base.clone());
            let mut o = Object::new(id, po.name, (*base).clone());
            o.placement = po.placement;
            o.added = po.added;
            o.removed = po.removed;
            o.rebuild();
            if po.supports {
                let _ = o.generate_supports(&self.prefs.supports);
            }
            self.objects.push(o);
        }
        self.result = None;
        self.uploaded.clear();
        self.project_path = Some(path.to_path_buf());
        self.frame_all();
        self.status = format!("Opened project {} ({n} objects).", path.display());
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
                if ui.button("Open model or project…  (Ctrl+O)").clicked() {
                    ui.close();
                    self.open_dialog();
                }
                if ui
                    .add_enabled(
                        !self.objects.is_empty(),
                        egui::Button::new("Save project…  (Ctrl+Shift+S)"),
                    )
                    .clicked()
                {
                    ui.close();
                    self.save_project_dialog();
                }
                ui.separator();
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
                if ui
                    .add_enabled(
                        !self.objects.is_empty(),
                        egui::Button::new("Export plate as STL (with supports)…"),
                    )
                    .clicked()
                {
                    ui.close();
                    self.export_stl();
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

    fn top_bar(&mut self, ui: &mut egui::Ui) {
        let bar = ui.max_rect();
        ui.horizontal_centered(|ui| {
            let (r, _) = ui.allocate_exact_size(Vec2::splat(22.0), egui::Sense::hover());
            theme::logo(ui.painter(), r);
            ui.label(
                RichText::new("FruitSlicer")
                    .font(egui::FontId::new(15.0, theme::semibold()))
                    .color(theme::TEXT),
            );
            ui.add_space(10.0);
            self.menu_bar(ui);

            // Right side: the main actions.
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                let slicing = self.job.is_some();
                if slicing {
                    if theme::secondary_button(ui, true, "Cancel").clicked() {
                        if let Some(j) = &self.job {
                            j.cancel.store(true, std::sync::atomic::Ordering::Relaxed);
                        }
                    }
                    let p = self.job.as_ref().map(|j| j.progress()).unwrap_or(0.0);
                    ui.add(
                        egui::ProgressBar::new(p)
                            .desired_width(180.0)
                            .desired_height(8.0)
                            .fill(theme::ACCENT)
                            .corner_radius(4.0),
                    );
                    ui.label(
                        RichText::new(format!("Slicing {:.0}%", p * 100.0)).color(theme::TEXT_WEAK),
                    );
                } else {
                    let stale = self.result.is_some() && !self.result_is_current();
                    let label = if stale { "Slice again" } else { "Slice" };
                    if theme::primary_button(ui, !self.objects.is_empty(), label)
                        .on_hover_text("Slice the plate and check it for problems")
                        .clicked()
                    {
                        let ctx = ui.ctx().clone();
                        self.start_slice(&ctx);
                    }
                    if theme::secondary_button(ui, self.result.is_some(), "Save .goo")
                        .on_hover_text("Save the sliced file for the printer (Ctrl+S)")
                        .clicked()
                    {
                        self.save_dialog();
                    }
                    if stale {
                        ui.label(
                            RichText::new("Plate changed since slicing")
                                .small()
                                .color(theme::WARN),
                        );
                    }
                }
            });
        });

        // Centre: the two modes, centred on the window.
        let size = Vec2::new(190.0, 34.0);
        let rect = egui::Rect::from_center_size(bar.center(), size);
        let has_result = self.result.is_some();
        ui.scope_builder(egui::UiBuilder::new().max_rect(rect), |ui| {
            theme::segmented(
                ui,
                &mut self.tab,
                &[
                    (Tab::Plate, "Plate", true),
                    (Tab::Layers, "Layers", has_result),
                ],
                true,
            );
        });
    }

    fn objects_panel(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            theme::section_title(ui, "Models");
            if !self.objects.is_empty() {
                ui.label(
                    RichText::new(self.objects.len().to_string())
                        .small()
                        .color(theme::TEXT_FAINT),
                );
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui
                    .small_button("+ Add")
                    .on_hover_text("Open a model or project (Ctrl+O)")
                    .clicked()
                {
                    self.open_dialog();
                }
            });
        });
        if self.objects.is_empty() {
            theme::card(ui, None, |ui| {
                ui.label(RichText::new("No models yet").strong());
                theme::hint(
                    ui,
                    "Open an STL, OBJ or 3MF file, or drop one on the window.",
                );
            });
            return;
        }
        let mut select = None;
        theme::card(ui, None, |ui| {
            ui.spacing_mut().item_spacing.y = 2.0;
            for o in &self.objects {
                let fits = o.fits(&self.printer);
                let sel = self.selected == Some(o.id);
                let h = 30.0;
                let (rect, resp) = ui
                    .allocate_exact_size(Vec2::new(ui.available_width(), h), egui::Sense::click());
                let p = ui.painter();
                if sel {
                    p.rect_filled(rect, 6.0, theme::ACCENT.gamma_multiply(0.16));
                } else if resp.hovered() {
                    p.rect_filled(rect, 6.0, theme::FIELD);
                }
                let dot = if !fits {
                    theme::ERROR
                } else if sel {
                    theme::ACCENT
                } else {
                    Color32::from_rgb(217, 158, 82)
                };
                p.circle_filled(rect.left_center() + Vec2::new(12.0, 0.0), 4.5, dot);
                p.text(
                    rect.left_center() + Vec2::new(26.0, 0.0),
                    egui::Align2::LEFT_CENTER,
                    &o.name,
                    egui::FontId::new(
                        13.5,
                        if sel {
                            theme::semibold()
                        } else {
                            egui::FontFamily::Proportional
                        },
                    ),
                    if sel { Color32::WHITE } else { theme::TEXT },
                );
                let right = if fits {
                    format!("{:.1} ml", o.placed.volume() / 1000.0)
                } else {
                    "off the plate".into()
                };
                p.text(
                    rect.right_center() - Vec2::new(10.0, 0.0),
                    egui::Align2::RIGHT_CENTER,
                    right,
                    egui::FontId::proportional(11.5),
                    if fits {
                        theme::TEXT_FAINT
                    } else {
                        theme::ERROR
                    },
                );
                if resp.clicked() {
                    select = Some(o.id);
                }
            }
        });
        if let Some(s) = select {
            self.selected = Some(s);
        }
        ui.horizontal(|ui| {
            if ui
                .add_enabled(self.selected.is_some(), egui::Button::new("Duplicate"))
                .on_hover_text("Ctrl+D")
                .clicked()
            {
                self.duplicate_selected();
            }
            if ui
                .add_enabled(self.selected.is_some(), egui::Button::new("Delete"))
                .on_hover_text("Del")
                .clicked()
            {
                self.delete_selected();
            }
            if ui
                .add_enabled(self.objects.len() > 1, egui::Button::new("Arrange"))
                .on_hover_text("Spread the models out over the plate")
                .clicked()
            {
                self.checkpoint();
                if !scene::arrange(&mut self.objects, &self.printer, 4.0) {
                    self.error = Some("Not everything fits on the plate.".into());
                }
                self.refresh_supports();
            }
        });
        ui.add_space(6.0);

        let Some(id) = self.selected else {
            theme::hint(ui, "Select a model to move, rotate or scale it.");
            return;
        };
        let Some(idx) = self.objects.iter().position(|o| o.id == id) else {
            return;
        };
        let before = self.objects[idx].placement;
        let mut p = before;
        let mut commit = false;
        theme::card(ui, Some("Placement"), |ui| {
            egui::Grid::new("placement")
                .num_columns(4)
                .spacing([8.0, 6.0])
                .show(ui, |ui| {
                    theme::row_label(ui, "Scale");
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
                        theme::row_label(ui, axis);
                        let r = ui.add(
                            egui::DragValue::new(&mut p.rotation[i])
                                .speed(1.0)
                                .suffix("°"),
                        );
                        commit |= r.drag_stopped() || r.lost_focus();
                        if ui.small_button("−90°").clicked() {
                            p.rotation[i] -= 90.0;
                            commit = true;
                        }
                        if ui.small_button("+90°").clicked() {
                            p.rotation[i] += 90.0;
                            commit = true;
                        }
                        ui.end_row();
                    }
                    for (axis, i) in [("Move X", 0), ("Move Y", 1)] {
                        theme::row_label(ui, axis);
                        let r = ui.add(
                            egui::DragValue::new(&mut p.offset[i])
                                .speed(0.5)
                                .suffix(" mm"),
                        );
                        commit |= r.drag_stopped() || r.lost_focus();
                        ui.end_row();
                    }
                });
            ui.add_space(2.0);
            ui.horizontal(|ui| {
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
                if ui.button("Centre").clicked() {
                    p.offset = [0.0, 0.0];
                    commit = true;
                }
                if ui.button("Reset").clicked() {
                    p = Placement::default();
                    commit = true;
                }
            });
        });
        if p != before {
            if commit
                && self
                    .undo
                    .last()
                    .map(|s| s.iter().find(|x| x.id == id).map(|x| x.placement))
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
        ui.add_space(4.0);
        theme::card(ui, Some("Details"), |ui| {
            egui::Grid::new("details")
                .num_columns(2)
                .spacing([12.0, 4.0])
                .show(ui, |ui| {
                    if let Some(b) = o.placed_bounds {
                        let s = b.size();
                        theme::row_label(ui, "Size");
                        ui.label(format!("{:.1} × {:.1} × {:.1} mm", s[0], s[1], s[2]));
                        ui.end_row();
                    }
                    theme::row_label(ui, "Volume");
                    ui.label(format!("{:.2} ml", o.placed.volume() / 1000.0));
                    ui.end_row();
                    theme::row_label(ui, "Triangles");
                    ui.label(o.placed.triangles.len().to_string());
                    ui.end_row();
                    if let Some(s) = o.supported.as_ref().filter(|_| o.supports_current()) {
                        let on_model = s.points.iter().filter(|p| p.on_model).count();
                        theme::row_label(ui, "Supports");
                        ui.label(format!("{} ({} on the model)", s.points.len(), on_model));
                        ui.end_row();
                    } else if !self.prefs.auto_supports {
                        theme::row_label(ui, "Supports");
                        ui.label("none");
                        ui.end_row();
                    }
                    if !o.added.is_empty() || !o.removed.is_empty() {
                        theme::row_label(ui, "Hand edits");
                        ui.label(format!(
                            "{} added, {} removed",
                            o.added.len(),
                            o.removed.len()
                        ));
                        ui.end_row();
                    }
                });
            if let Some(&(open, nm)) = self.mesh_errors.get(&o.id) {
                if open > 0 || nm > 0 {
                    theme::callout(
                        ui,
                        theme::WARN,
                        format!(
                            "Mesh problems: {open} open edges, {nm} non-manifold edges. Holes or \
                             overlaps can slice wrongly; check the layers, or repair the mesh \
                             first (for example in Blender's 3D-Print toolbox)."
                        ),
                    );
                }
            }
            if o.orient < 0.0 {
                theme::hint(
                    ui,
                    "Faces point inwards (the mesh is inside out). Slicing handles it.",
                );
            }
        });
    }

    /// Right panel on the Plate tab: settings, one page at a time.
    fn settings_panel(&mut self, ui: &mut egui::Ui) {
        ui.add_space(2.0);
        theme::segmented(
            ui,
            &mut self.settings_tab,
            &[
                (SettingsTab::Print, "Print", true),
                (SettingsTab::Supports, "Supports", true),
                (SettingsTab::Hollow, "Hollow", true),
                (SettingsTab::Checks, "Checks", true),
            ],
            true,
        );
        ui.add_space(4.0);
        if self.result.is_some() {
            egui::Panel::bottom("last-slice")
                .frame(egui::Frame::NONE.inner_margin(egui::Margin {
                    top: 8,
                    ..Default::default()
                }))
                .show_separator_line(false)
                .show(ui, |ui| self.slice_summary(ui));
        }
        egui::ScrollArea::vertical()
            .id_salt("settings")
            .auto_shrink([false, false])
            .show(ui, |ui| match self.settings_tab {
                SettingsTab::Print => self.print_settings_ui(ui),
                SettingsTab::Supports => self.supports_ui(ui),
                SettingsTab::Hollow => self.hollow_ui(ui),
                SettingsTab::Checks => self.checks_ui(ui),
            });
    }

    /// A short summary of the last slice, with a way into the layers.
    fn slice_summary(&mut self, ui: &mut egui::Ui) {
        let Some(out) = &self.result else { return };
        let r = &out.report;
        let secs = out.settings.estimated_seconds(r.layer_count);
        let stale = !self.result_is_current();
        let n = r.issues.len();
        let mut open = false;
        theme::card(ui, Some("Last slice"), |ui| {
            ui.label(
                RichText::new(format!(
                    "{}  ·  {:.1} ml  ·  {} layers",
                    util::duration(secs),
                    r.sliced_volume_mm3 / 1000.0,
                    r.layer_count
                ))
                .color(theme::TEXT),
            );
            let (text, color) = if n == 0 {
                ("No problems found".to_string(), theme::OK)
            } else {
                (
                    format!("{n} possible problem{}", if n == 1 { "" } else { "s" }),
                    theme::WARN,
                )
            };
            theme::dot_label(ui, color, RichText::new(text).color(color));
            if stale {
                theme::hint(
                    ui,
                    "The plate changed since this slice. Slice again to update it.",
                );
            }
            open = ui
                .add_sized(
                    [ui.available_width(), 28.0],
                    egui::Button::new("View layers  →"),
                )
                .clicked();
        });
        if open {
            self.tab = Tab::Layers;
        }
    }

    fn print_settings_ui(&mut self, ui: &mut egui::Ui) {
        let s = &mut self.prefs.print;
        theme::card(ui, Some("Exposure"), |ui| {
            egui::Grid::new("print")
                .num_columns(2)
                .spacing([12.0, 7.0])
                .min_col_width(120.0)
                .show(ui, |ui| {
                    theme::row_label(ui, "Layer height");
                    ui.add(
                        egui::DragValue::new(&mut s.layer_mm)
                            .speed(0.005)
                            .range(0.01..=0.3)
                            .suffix(" mm"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Exposure");
                    ui.add(
                        egui::DragValue::new(&mut s.exposure_s)
                            .speed(0.05)
                            .range(0.1..=60.0)
                            .suffix(" s"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Bottom exposure");
                    ui.add(
                        egui::DragValue::new(&mut s.bottom_exposure_s)
                            .speed(0.5)
                            .range(1.0..=200.0)
                            .suffix(" s"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Bottom layers");
                    ui.add(egui::DragValue::new(&mut s.bottom_layers).range(0..=50));
                    ui.end_row();
                    theme::row_label(ui, "UV power");
                    let mut pwm = s.light_pwm;
                    ui.add(egui::Slider::new(&mut pwm, 1..=255));
                    s.light_pwm = pwm;
                    s.bottom_light_pwm = pwm;
                    ui.end_row();
                    theme::row_label(ui, "Anti-aliasing");
                    egui::ComboBox::from_id_salt("aa")
                        .selected_text(if s.anti_alias <= 1 {
                            "Off".to_string()
                        } else {
                            format!("{}×", s.anti_alias)
                        })
                        .show_ui(ui, |ui| {
                            ui.selectable_value(&mut s.anti_alias, 1, "Off");
                            ui.selectable_value(&mut s.anti_alias, 2, "2×");
                            ui.selectable_value(&mut s.anti_alias, 4, "4×");
                            ui.selectable_value(&mut s.anti_alias, 8, "8×");
                        })
                        .response
                        .on_hover_text("Grey edge pixels for smoother surfaces. Not yet confirmed on the printer: try a small test print first.");
                    ui.end_row();
                });
            theme::callout(
                ui,
                theme::WARN,
                "Exposure defaults are placeholders. Use your resin maker's numbers.",
            );
        });
        ui.add_space(4.0);

        // Resin profiles.
        let mut load = None;
        let mut delete = None;
        theme::card(ui, Some("Resin profiles"), |ui| {
            if self.prefs.profiles.is_empty() {
                theme::hint(ui, "Save the settings above under a name to reuse them.");
            }
            for (i, p) in self.prefs.profiles.iter().enumerate() {
                ui.horizontal(|ui| {
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(RichText::new(&p.name).strong());
                        theme::hint(
                            ui,
                            format!(
                                "{} mm · {} s · bottom {} s × {}",
                                p.settings.layer_mm,
                                p.settings.exposure_s,
                                p.settings.bottom_exposure_s,
                                p.settings.bottom_layers
                            ),
                        );
                    });
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if ui.small_button("×").on_hover_text("Delete").clicked() {
                            delete = Some(i);
                        }
                        if ui.small_button("Use").clicked() {
                            load = Some(i);
                        }
                    });
                });
            }
            ui.horizontal(|ui| {
                ui.add(
                    egui::TextEdit::singleline(&mut self.new_profile_name)
                        .hint_text("Profile name")
                        .desired_width(ui.available_width() - 70.0),
                );
                if ui
                    .add_enabled(
                        !self.new_profile_name.trim().is_empty(),
                        egui::Button::new("Save"),
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
        });
        if let Some(i) = load {
            self.prefs.print = self.prefs.profiles[i].settings.clone();
        }
        if let Some(i) = delete {
            self.prefs.profiles.remove(i);
        }
        ui.add_space(4.0);

        let mirror = &mut self.prefs.mirror_x;
        theme::card(ui, Some("Printer"), |ui| {
            ui.label("ELEGOO Saturn 4 Ultra 16K");
            ui.checkbox(mirror, "Mirror X (as UVtools' profile does)")
                .on_hover_text(
                    "Not yet confirmed against a file from ELEGOO's own slicer. If text prints \
                     backwards, turn this off.",
                );
        });
    }

    fn supports_ui(&mut self, ui: &mut egui::Ui) {
        let before = self.prefs.supports;
        let auto_before = self.prefs.auto_supports;
        let mut regenerate = false;
        let mut clear_edits = false;
        let mut remove = false;
        let edit = &mut self.edit_supports;
        let auto = &mut self.prefs.auto_supports;
        let s = &mut self.prefs.supports;
        theme::card(ui, Some("Placement"), |ui| {
            ui.checkbox(auto, "Automatic supports");
            egui::Grid::new("supports")
                .num_columns(2)
                .spacing([12.0, 7.0])
                .min_col_width(120.0)
                .show(ui, |ui| {
                    theme::row_label(ui, "Style");
                    ui.horizontal(|ui| {
                        ui.selectable_value(&mut s.tree, true, "Tree")
                            .on_hover_text("Neighbouring supports lean in and merge into shared trunks: fewer feet, and they come off in clusters");
                        ui.selectable_value(&mut s.tree, false, "Columns");
                    });
                    ui.end_row();
                    if s.tree {
                        theme::row_label(ui, "Branch angle");
                        ui.add(egui::Slider::new(&mut s.branch_angle_deg, 10.0..=50.0).suffix("°"))
                            .on_hover_text("Most a branch leans from vertical. Steeper branches merge less.");
                        ui.end_row();
                    }
                    theme::row_label(ui, "Overhang angle");
                    ui.add(egui::Slider::new(&mut s.overhang_angle_deg, 10.0..=80.0).suffix("°"))
                        .on_hover_text("Downward faces flatter than this get supports");
                    ui.end_row();
                    theme::row_label(ui, "Spacing");
                    ui.add(
                        egui::DragValue::new(&mut s.spacing_mm)
                            .speed(0.1)
                            .range(0.8..=20.0)
                            .suffix(" mm"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Lift off plate");
                    ui.add(
                        egui::DragValue::new(&mut s.lift_mm)
                            .speed(0.1)
                            .range(0.0..=30.0)
                            .suffix(" mm"),
                    )
                    .on_hover_text(
                        "0 puts the model straight on the plate and only supports overhangs",
                    );
                    ui.end_row();
                    theme::row_label(ui, "Support low points");
                    ui.checkbox(&mut s.support_minima, "");
                    ui.end_row();
                });
            ui.horizontal(|ui| {
                regenerate = ui.button("Regenerate").clicked();
                remove = ui.button("Remove all").clicked();
            });
        });
        ui.add_space(4.0);
        theme::card(ui, Some("Edit by hand"), |ui| {
            ui.horizontal(|ui| {
                ui.toggle_value(edit, if *edit { "Editing…" } else { "Start editing" })
                    .on_hover_text(
                        "Click a model to add a support there. Right-click a support to remove it.",
                    );
                clear_edits = ui.button("Clear edits").clicked();
            });
            if *edit {
                theme::callout(
                    ui,
                    theme::INFO,
                    "Click a model to add a support, right-click a support to remove it.",
                );
            }
        });
        ui.add_space(4.0);
        theme::card(ui, Some("Tips"), |ui| {
            egui::Grid::new("support-tips")
                .num_columns(2)
                .spacing([12.0, 7.0])
                .min_col_width(120.0)
                .show(ui, |ui| {
                    theme::row_label(ui, "Tip diameter");
                    ui.add(
                        egui::DragValue::new(&mut s.tip_diameter_mm)
                            .speed(0.01)
                            .range(0.1..=2.0)
                            .suffix(" mm"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Low-point tip");
                    ui.add(
                        egui::DragValue::new(&mut s.strong_tip_diameter_mm)
                            .speed(0.01)
                            .range(0.1..=2.0)
                            .suffix(" mm"),
                    )
                    .on_hover_text("Tip diameter at the lowest points, which hold the most at the start of the print");
                    ui.end_row();
                    theme::row_label(ui, "Tip length");
                    ui.add(
                        egui::DragValue::new(&mut s.tip_length_mm)
                            .speed(0.05)
                            .range(0.3..=10.0)
                            .suffix(" mm"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Angle tips");
                    ui.checkbox(&mut s.angled_tips, "")
                        .on_hover_text("Lean tips (up to 30°) to meet sloped faces head-on, so they snap off cleanly");
                    ui.end_row();
                });
        });
        ui.add_space(4.0);
        theme::card(ui, Some("Columns and raft"), |ui| {
            egui::Grid::new("support-columns")
                .num_columns(2)
                .spacing([12.0, 7.0])
                .min_col_width(120.0)
                .show(ui, |ui| {
                    theme::row_label(ui, "Column diameter");
                    ui.add(
                        egui::DragValue::new(&mut s.column_diameter_mm)
                            .speed(0.05)
                            .range(0.3..=5.0)
                            .suffix(" mm"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Foot diameter");
                    ui.add(
                        egui::DragValue::new(&mut s.foot_diameter_mm)
                            .speed(0.05)
                            .range(0.3..=10.0)
                            .suffix(" mm"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Raft thickness");
                    ui.add(
                        egui::DragValue::new(&mut s.raft_mm)
                            .speed(0.05)
                            .range(0.0..=5.0)
                            .suffix(" mm"),
                    );
                    ui.end_row();
                    theme::row_label(ui, "Raft lip");
                    ui.add(
                        egui::DragValue::new(&mut s.raft_lip_mm)
                            .speed(0.05)
                            .range(0.0..=3.0)
                            .suffix(" mm"),
                    )
                    .on_hover_text("Height of the undercut at the raft's edge, where a scraper slips in. 0 for a square edge.");
                    ui.end_row();
                    theme::row_label(ui, "Raft lip angle");
                    ui.add(egui::Slider::new(&mut s.raft_lip_angle_deg, 30.0..=90.0).suffix("°"))
                        .on_hover_text("How steeply the undercut flares out from the plate. It overhangs while printing, so stay near 45° or steeper.");
                    ui.end_row();
                });
        });
        ui.horizontal(|ui| {
            if ui
                .small_button("Reset to defaults")
                .on_hover_text("Put every support setting back to its default")
                .clicked()
            {
                self.prefs.supports = SupportParams::default();
            }
        });
        theme::hint(
            ui,
            "Thin, shallow tips that lean to meet sloped faces, on a raft with an \
             undercut edge. Supports that land on the model stay straight. Defaults come from \
             slicer guides and are untested on this printer.",
        );

        if clear_edits {
            self.checkpoint();
            for o in &mut self.objects {
                o.added.clear();
                o.removed.clear();
                o.supports_version = None;
            }
            self.refresh_supports();
        }
        if regenerate {
            self.regenerate_all_supports();
        }
        if remove {
            self.prefs.auto_supports = false;
            for o in &mut self.objects {
                o.clear_supports();
            }
        }
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
        let on = &mut self.prefs.hollow;
        let h = &mut self.prefs.hollow_params;
        theme::card(ui, Some("Hollowing"), |ui| {
            ui.checkbox(on, "Hollow the print (saves resin)");
            ui.add_enabled_ui(*on, |ui| {
                egui::Grid::new("hollow")
                    .num_columns(2)
                    .spacing([12.0, 7.0])
                    .min_col_width(120.0)
                    .show(ui, |ui| {
                        theme::row_label(ui, "Wall thickness");
                        ui.add(
                            egui::DragValue::new(&mut h.wall_mm)
                                .speed(0.05)
                                .range(0.8..=10.0)
                                .suffix(" mm"),
                        );
                        ui.end_row();
                    });
            });
        });
        ui.add_space(4.0);
        theme::card(ui, Some("Drain holes"), |ui| {
            ui.add_enabled_ui(*on, |ui| {
                ui.checkbox(&mut h.drain_holes, "Add drain holes automatically");
                egui::Grid::new("holes")
                    .num_columns(2)
                    .spacing([12.0, 7.0])
                    .min_col_width(120.0)
                    .show(ui, |ui| {
                        theme::row_label(ui, "Hole diameter");
                        ui.add(
                            egui::DragValue::new(&mut h.hole_diameter_mm)
                                .speed(0.05)
                                .range(0.8..=10.0)
                                .suffix(" mm"),
                        );
                        ui.end_row();
                        theme::row_label(ui, "Hole spacing");
                        ui.add(
                            egui::DragValue::new(&mut h.hole_spacing_mm)
                                .speed(0.5)
                                .range(2.0..=100.0)
                                .suffix(" mm"),
                        );
                        ui.end_row();
                    });
            });
            theme::hint(
                ui,
                "Holes go through the floor of each hollow, on the plate side, so it vents while \
                 printing and drains afterwards. Give the model some lift so the holes aren't \
                 against the raft.",
            );
        });
        if *on {
            ui.add_space(4.0);
            theme::callout(
                ui,
                theme::WARN,
                "Hollow ceilings have no internal supports yet: check the results for islands \
                 and overhangs inside.",
            );
        }
    }

    fn checks_ui(&mut self, ui: &mut egui::Ui) {
        let on = &mut self.prefs.checks;
        let c = &mut self.prefs.check_params;
        theme::card(ui, Some("Print checks"), |ui| {
            ui.checkbox(on, "Check the print while slicing");
            ui.add_enabled_ui(*on, |ui| {
                egui::Grid::new("checks")
                    .num_columns(2)
                    .spacing([12.0, 7.0])
                    .min_col_width(120.0)
                    .show(ui, |ui| {
                        theme::row_label(ui, "Grid cell");
                        ui.add(
                            egui::DragValue::new(&mut c.cell_mm)
                                .speed(0.01)
                                .range(0.1..=1.0)
                                .suffix(" mm"),
                        );
                        ui.end_row();
                        theme::row_label(ui, "Max overhang");
                        ui.add(
                            egui::DragValue::new(&mut c.overhang_mm)
                                .speed(0.05)
                                .range(0.2..=10.0)
                                .suffix(" mm"),
                        )
                        .on_hover_text("How far a layer may stick out past the one before it");
                        ui.end_row();
                        theme::row_label(ui, "Ignore areas under");
                        ui.add(
                            egui::DragValue::new(&mut c.min_area_mm2)
                                .speed(0.01)
                                .range(0.0..=10.0)
                                .suffix(" mm²"),
                        );
                        ui.end_row();
                        theme::row_label(ui, "Ignore cups under");
                        ui.add(
                            egui::DragValue::new(&mut c.min_cup_area_mm2)
                                .speed(0.1)
                                .range(0.0..=100.0)
                                .suffix(" mm²"),
                        );
                        ui.end_row();
                    });
            });
            theme::hint(
                ui,
                "Checks use a coarse grid, so features smaller than a cell can be missed and \
                 narrow gaps can look closed.",
            );
        });
        ui.add_space(4.0);
        theme::card(ui, Some("What it looks for"), |ui| {
            for kind in [
                IssueKind::Island,
                IssueKind::UnsupportedOverhang,
                IssueKind::SuctionCup,
                IssueKind::TrappedResin,
            ] {
                theme::dot_label(ui, issue_color(kind), RichText::new(kind.label()).strong());
                ui.indent(kind.label(), |ui| theme::hint(ui, issue_help(kind)));
            }
        });
    }

    /// Right panel on the Layers tab: what the slice produced and its problems.
    fn results_ui(&mut self, ui: &mut egui::Ui) {
        let Some(out) = &self.result else {
            theme::hint(
                ui,
                "Slice to see layer count, print time, resin use and problems.",
            );
            return;
        };
        let r = &out.report;
        let s = &out.settings;
        let secs = s.estimated_seconds(r.layer_count);
        let mut show_info = false;
        theme::card(ui, Some("Print"), |ui| {
            if let Some(name) = &out.opened {
                ui.label(RichText::new(format!("{name}.goo (opened, not sliced here)")).strong());
            }
            ui.columns(2, |c| {
                theme::stat(
                    &mut c[0],
                    "Print time",
                    &util::duration(secs),
                    Some("rough"),
                );
                theme::stat(
                    &mut c[1],
                    "Resin",
                    &format!("{:.1} ml", r.sliced_volume_mm3 / 1000.0),
                    Some(&format!("≈ {:.0} g", r.sliced_volume_mm3 / 1000.0 * 1.1)),
                );
            });
            ui.columns(2, |c| {
                theme::stat(
                    &mut c[0],
                    "Layers",
                    &r.layer_count.to_string(),
                    Some(&format!("{} mm each", s.layer_mm)),
                );
                theme::stat(
                    &mut c[1],
                    "File size",
                    &format!("{:.1} MB", r.encoded_bytes as f64 / 1e6),
                    None,
                );
            });
            show_info = ui.small_button("File details…").clicked();
            if !r.empty_layers.is_empty() {
                theme::callout(
                    ui,
                    theme::ERROR,
                    format!(
                        "{} empty layers: something is floating or the mesh is broken.",
                        r.empty_layers.len()
                    ),
                );
            }
            let diff =
                (r.sliced_volume_mm3 - r.mesh_volume_mm3).abs() / r.mesh_volume_mm3.max(1e-9);
            if diff > 0.05 {
                theme::callout(
                    ui,
                    theme::WARN,
                    format!(
                        "Sliced volume is {:.0}% off the mesh volume: the mesh may have holes or flipped faces.",
                        diff * 100.0
                    ),
                );
            }
            if !r.drain_holes.is_empty() {
                ui.label(format!("{} drain holes cut", r.drain_holes.len()));
                for d in &r.drain_holes {
                    theme::hint(
                        ui,
                        format!(
                            "at ({:.1}, {:.1}) mm, layers {}–{}",
                            d.center_mm[0],
                            d.center_mm[1],
                            d.first_layer + 1,
                            d.last_layer + 1
                        ),
                    );
                }
            }
        });
        if show_info {
            self.show_file_info = true;
        }
        ui.add_space(4.0);

        let mut fix = false;
        let mut jump = None;
        theme::card(ui, Some("Problems"), |ui| {
            if r.issues.is_empty() {
                if self.prefs.checks {
                    theme::dot_label(
                        ui,
                        theme::OK,
                        RichText::new("No problems found").color(theme::OK),
                    );
                } else {
                    theme::hint(ui, "Print checks are off.");
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
                    let c = issue_color(kind);
                    ui.horizontal(|ui| {
                        theme::dot_label(ui, c, RichText::new(kind.label()).color(c));
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            ui.label(
                                RichText::new(n.to_string())
                                    .font(egui::FontId::new(13.0, theme::semibold()))
                                    .color(c),
                            );
                        });
                    })
                    .response
                    .on_hover_text(issue_help(kind));
                }
            }
            ui.separator();
            let islands = counts.get(&IssueKind::Island).copied().unwrap_or(0);
            if islands > 0 && self.result_is_current() && out.opened.is_none() {
                fix = ui
                    .button(format!("Add supports under {islands} island(s)"))
                    .on_hover_text(
                        "Adds a hand-placed support under each island, then you slice again",
                    )
                    .clicked();
            }
            theme::hint(ui, "Click one to jump to it.");
            ui.spacing_mut().item_spacing.y = 2.0;
            for (i, issue) in r.issues.iter().enumerate() {
                let sel = self.selected_issue == Some(i);
                let c = issue_color(issue.kind);
                let (rect, resp) = ui.allocate_exact_size(
                    Vec2::new(ui.available_width(), 40.0),
                    egui::Sense::click(),
                );
                let p = ui.painter();
                if sel {
                    p.rect_filled(rect, 6.0, c.gamma_multiply(0.16));
                    p.rect_stroke(
                        rect,
                        6.0,
                        egui::Stroke::new(1.0, c.gamma_multiply(0.6)),
                        egui::StrokeKind::Inside,
                    );
                } else if resp.hovered() {
                    p.rect_filled(rect, 6.0, theme::FIELD);
                }
                p.circle_filled(rect.left_center() + Vec2::new(12.0, 0.0), 4.5, c);
                p.text(
                    rect.left_top() + Vec2::new(26.0, 6.0),
                    egui::Align2::LEFT_TOP,
                    issue.kind.label(),
                    egui::FontId::new(13.0, theme::semibold()),
                    if sel { Color32::WHITE } else { theme::TEXT },
                );
                p.text(
                    rect.left_bottom() + Vec2::new(26.0, -5.0),
                    egui::Align2::LEFT_BOTTOM,
                    issue_detail(issue),
                    egui::FontId::proportional(11.5),
                    theme::TEXT_WEAK,
                );
                if resp.on_hover_text(issue_help(issue.kind)).clicked() {
                    jump = Some(i);
                }
            }
        });
        if fix {
            self.support_islands();
            return;
        }
        if let Some(i) = jump {
            if self.selected_issue == Some(i) {
                self.selected_issue = None;
            } else {
                self.selected_issue = Some(i);
                let first = out.report.issues[i].first_layer as usize;
                self.layer = first;
                self.tab = Tab::Layers;
                self.focus_issue = Some(i);
            }
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
        if self.edit_supports
            && self.tab == Tab::Plate
            && (resp.clicked() || resp.secondary_clicked())
        {
            if let Some(p) = resp.interact_pointer_pos() {
                let (o, d) = self.camera.ray(to_ndc(p), aspect);
                self.edit_support_at(o, d, resp.secondary_clicked());
            }
        } else if resp.clicked() {
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
        theme::gradient(
            ui.painter(),
            rect,
            Color32::from_rgb(30, 34, 43),
            Color32::from_rgb(13, 15, 19),
        );
        ui.painter()
            .add(egui_wgpu::Callback::new_paint_callback(rect, cb));

        // Overlays.
        let painter = ui.painter_at(rect);
        if self.objects.is_empty() && !layer_mode {
            let c = rect.center();
            painter.text(
                c - Vec2::new(0.0, 14.0),
                egui::Align2::CENTER_CENTER,
                "Drop a model here",
                egui::FontId::new(20.0, theme::semibold()),
                theme::TEXT,
            );
            painter.text(
                c + Vec2::new(0.0, 14.0),
                egui::Align2::CENTER_CENTER,
                "STL, OBJ, 3MF, a FruitSlicer project or a .goo file  ·  or use File › Open (Ctrl+O)",
                egui::FontId::proportional(13.0),
                theme::TEXT_WEAK,
            );
        }
        if self.edit_supports && !layer_mode {
            theme::pill(
                &painter,
                rect.center_top() + Vec2::new(0.0, 12.0),
                egui::Align2::CENTER_TOP,
                "Editing supports: click a model to add one, right-click a support to remove it",
                theme::INFO,
                egui::FontId::proportional(12.0),
            );
        }
        if layer_mode {
            theme::pill(
                &painter,
                rect.left_top() + Vec2::new(10.0, 10.0),
                egui::Align2::LEFT_TOP,
                "3D cut-away",
                theme::TEXT_WEAK,
                egui::FontId::proportional(11.5),
            );
        }

        // Camera presets, top right.
        let mut set_view = None;
        let mut frame = false;
        let bar = egui::Rect::from_min_size(
            rect.right_top() + Vec2::new(-262.0, 10.0),
            Vec2::new(252.0, 32.0),
        );
        if rect.width() > 360.0 {
            ui.scope_builder(
                egui::UiBuilder::new()
                    .max_rect(bar)
                    .layout(egui::Layout::right_to_left(egui::Align::Center)),
                |ui| {
                    egui::Frame::new()
                        .fill(Color32::from_black_alpha(150))
                        .corner_radius(8)
                        .inner_margin(egui::Margin::same(3))
                        .show(ui, |ui| {
                            ui.spacing_mut().item_spacing.x = 10.0;
                            frame = ui
                                .add(egui::Button::new("Fit").frame(false))
                                .on_hover_text("Frame everything (F)")
                                .clicked();
                            for (label, v) in [
                                ("Right", View::Right),
                                ("Front", View::Front),
                                ("Top", View::Top),
                                ("Iso", View::Iso),
                            ] {
                                if ui
                                    .add(
                                        egui::Button::new(
                                            RichText::new(label).color(theme::TEXT_WEAK),
                                        )
                                        .frame(false),
                                    )
                                    .clicked()
                                {
                                    set_view = Some(v);
                                }
                            }
                        });
                },
            );
        }
        if let Some(v) = set_view {
            let (t, d) = (self.camera.target, self.camera.distance);
            self.camera.set_view(v, t, 1.0);
            self.camera.distance = d;
        }
        if frame {
            self.frame_all();
        }

        let hints: &[(&str, &str)] = if layer_mode || rect.width() < 760.0 {
            &[("Drag", "orbit"), ("Right drag", "pan"), ("Wheel", "zoom")]
        } else {
            &[
                ("Drag", "orbit or move a model"),
                ("Right drag", "pan"),
                ("Wheel", "zoom"),
                ("F", "frame all"),
            ]
        };
        theme::hints(&painter, rect.left_bottom() + Vec2::new(14.0, -10.0), hints);
    }

    /// Adds a support where the ray hits a model, or (with `remove`) removes
    /// the support whose tip is nearest the ray.
    fn edit_support_at(&mut self, o: glam::Vec3, d: glam::Vec3, remove: bool) {
        if !remove {
            let hit = self
                .objects
                .iter()
                .enumerate()
                .filter_map(|(i, ob)| {
                    scene::ray_mesh(ob.print_model(), o, d).map(|(t, p)| (i, t, p))
                })
                .min_by(|a, b| a.1.total_cmp(&b.1));
            let Some((i, _, p)) = hit else { return };
            self.checkpoint();
            let ob = &mut self.objects[i];
            let placed = [p.x, p.y, p.z - ob.lift()];
            let local = ob.to_model(placed);
            ob.added.push(local);
            ob.supports_version = None;
            self.selected = Some(ob.id);
            self.status = "Added a support.".into();
        } else {
            // Nearest support tip to the ray, within 2 mm.
            let mut best: Option<(usize, f32, [f32; 3])> = None;
            for (i, ob) in self.objects.iter().enumerate() {
                let Some(s) = ob.supported.as_ref().filter(|_| ob.supports_current()) else {
                    continue;
                };
                for q in &s.points {
                    let v = glam::Vec3::from(q.top) - o;
                    let dist = (v - d * v.dot(d)).length();
                    if dist < 2.0 && best.is_none_or(|b| dist < b.1) {
                        best = Some((i, dist, q.top));
                    }
                }
            }
            let Some((i, _, top)) = best else { return };
            self.checkpoint();
            let ob = &mut self.objects[i];
            let placed = [top[0], top[1], top[2] - ob.lift()];
            let local = ob.to_model(placed);
            // Undo a hand-added one if that's what it was.
            let before = ob.added.len();
            ob.added
                .retain(|a| (glam::Vec3::from(*a) - glam::Vec3::from(local)).length() > 0.05);
            if ob.added.len() == before {
                ob.removed.push(local);
            }
            ob.supports_version = None;
            self.status = "Removed a support.".into();
        }
        let p = self.prefs.supports;
        for ob in &mut self.objects {
            if !ob.supports_current() {
                if let Err(e) = ob.generate_supports(&p) {
                    self.error = Some(e);
                }
            }
        }
    }

    /// Adds a hand-placed support under each island found by the last slice.
    fn support_islands(&mut self) {
        let Some(out) = &self.result else { return };
        let h = out.settings.layer_mm;
        let islands: Vec<Issue> = out
            .report
            .issues
            .iter()
            .filter(|i| i.kind == IssueKind::Island)
            .cloned()
            .collect();
        self.checkpoint();
        let mut added = 0;
        for isl in islands {
            let z = isl.first_layer as f32 * h;
            let (min, max) = (isl.area.min, isl.area.max);
            // Try the centre first, then a grid over the island's box.
            let mut tries = vec![isl.area.center()];
            for j in 0..3 {
                for i in 0..3 {
                    tries.push([
                        min[0] + (max[0] - min[0]) * (i as f64 + 0.5) / 3.0,
                        min[1] + (max[1] - min[1]) * (j as f64 + 0.5) / 3.0,
                    ]);
                }
            }
            'found: for c in tries {
                let o = glam::Vec3::new(c[0] as f32, c[1] as f32, z - 2.0);
                for ob in &mut self.objects {
                    if let Some((_, p)) = scene::ray_mesh(ob.print_model(), o, glam::Vec3::Z) {
                        if (p.z - z).abs() < 1.0 {
                            let placed = [p.x, p.y, p.z - ob.lift()];
                            let local = ob.to_model(placed);
                            ob.added.push(local);
                            ob.supports_version = None;
                            added += 1;
                            break 'found;
                        }
                    }
                }
            }
        }
        let p = self.prefs.supports;
        for ob in &mut self.objects {
            if !ob.supports_current() {
                let _ = ob.generate_supports(&p);
            }
        }
        self.tab = Tab::Plate;
        self.status = format!("Added {added} supports under islands. Slice again to check.");
    }

    fn export_stl(&mut self) {
        if self.objects.is_empty() {
            return;
        }
        self.refresh_supports();
        let mesh = scene::print_mesh(&self.objects);
        let mut d = rfd::FileDialog::new()
            .add_filter("STL", &["stl"])
            .set_file_name("plate.stl");
        if let Some(dir) = &self.prefs.last_dir {
            d = d.set_directory(dir);
        }
        if let Some(path) = d.save_file() {
            match std::fs::write(&path, stl::to_binary(&mesh)) {
                Ok(()) => self.status = format!("Exported {}.", path.display()),
                Err(e) => self.error = Some(format!("{}: {e}", path.display())),
            }
        }
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
        let can_cutaway = out.opened.is_none();

        // Header: layer number, height and view options.
        egui::Frame::new()
            .fill(theme::PANEL)
            .inner_margin(egui::Margin::symmetric(12, 6))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.horizontal(|ui| {
                    theme::section_title(ui, "Layer");
                    let mut l = self.layer + 1;
                    ui.add(egui::DragValue::new(&mut l).range(1..=n));
                    self.layer = l.clamp(1, n) - 1;
                    ui.label(RichText::new(format!("of {n}")).color(theme::TEXT_WEAK));
                    ui.add_space(6.0);
                    ui.label(
                        RichText::new(format!("Z {:.2} mm", (self.layer + 1) as f32 * layer_mm))
                            .font(egui::FontId::new(13.0, theme::semibold())),
                    );
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if can_cutaway {
                            ui.toggle_value(&mut self.show_cutaway, "3D cut-away")
                                .on_hover_text(
                                    "Show the model cut at this layer next to the image",
                                );
                        }
                        if ui
                            .button("Fit")
                            .on_hover_text("Fit the layer in the view")
                            .clicked()
                        {
                            self.layer_zoom = 1.0;
                            self.layer_pan = Vec2::ZERO;
                        }
                    });
                });
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

        // Body: the image, with the layer scrubber down its right edge.
        let body = ui.available_rect_before_wrap();
        let scrub_w = 46.0;
        let view = egui::Rect::from_min_max(body.min, egui::pos2(body.max.x - scrub_w, body.max.y));
        let scrub = egui::Rect::from_min_max(egui::pos2(view.max.x, body.min.y), body.max);
        ui.allocate_rect(body, egui::Sense::hover());
        self.layer_scrubber(ui, scrub, n);

        // Render the layer image if needed. Crop to the print's footprint.
        let Some(out) = &self.result else { return };
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
                                Color32::from_rgb(12, 14, 18)
                            } else {
                                let k = v as f32 / 255.0;
                                Color32::from_rgb(
                                    (12.0 + k * 220.0) as u8,
                                    (14.0 + k * 154.0) as u8,
                                    (18.0 + k * 52.0) as u8,
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

        let rect = view;
        let resp = ui.interact(
            rect,
            ui.scope_id().with("layer-image"),
            egui::Sense::click_and_drag(),
        );
        let painter = ui.painter_at(rect);
        painter.rect_filled(rect, 0.0, Color32::from_rgb(9, 10, 13));
        // Physical aspect of the image.
        let mm_w = (img.rect.x1 - img.rect.x0) as f32 * h.size_x / h.resolution_x as f32;
        let mm_h = (img.rect.y1 - img.rect.y0) as f32 * h.size_y / h.resolution_y as f32;
        let fit = (rect.width() / mm_w).min(rect.height() / mm_h) * 0.92;

        // Zoom to a problem the user picked: centre it, and make it about
        // half the view, so the whole box and its label stay on screen.
        if let Some(i) = self.focus_issue.take() {
            if let Some(issue) = out.report.issues.get(i) {
                let a = img.mm_to_uv(&h, [issue.area.min[0] as f32, issue.area.max[1] as f32]);
                let b = img.mm_to_uv(&h, [issue.area.max[0] as f32, issue.area.min[1] as f32]);
                let w = ((b[0] - a[0]).abs() * mm_w * fit).max(1.0);
                let hh = ((b[1] - a[1]).abs() * mm_h * fit).max(1.0);
                let z = (rect.width() * 0.5 / w).min(rect.height() * 0.5 / hh);
                self.layer_zoom = z.clamp(1.0, 12.0);
                let c = [(a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0];
                self.layer_pan = Vec2::new(0.5 - c[0], 0.5 - c[1]);
            }
        }

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
        // A faint outline of the cropped area, so empty layers still read.
        painter.rect_stroke(
            img_rect,
            2.0,
            egui::Stroke::new(1.0, Color32::from_rgb(34, 38, 47)),
            egui::StrokeKind::Outside,
        );
        painter.image(
            tex.id(),
            img_rect,
            egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
            Color32::WHITE,
        );

        // Issues on this layer. Boxes first, then labels, so labels sit on
        // top and never hide behind another box.
        let inner = rect.shrink(8.0);
        let mut labels: Vec<(egui::Rect, Color32, &'static str, bool)> = Vec::new();
        for (i, issue) in out.report.issues.iter().enumerate() {
            let on_layer =
                (issue.first_layer as usize..=issue.last_layer as usize).contains(&self.layer);
            let sel = self.selected_issue == Some(i);
            if !on_layer && !sel {
                continue;
            }
            let a = img.mm_to_uv(&h, [issue.area.min[0] as f32, issue.area.max[1] as f32]);
            let b = img.mm_to_uv(&h, [issue.area.max[0] as f32, issue.area.min[1] as f32]);
            let r = egui::Rect::from_min_max(
                img_rect.min + Vec2::new(a[0], a[1]) * size,
                img_rect.min + Vec2::new(b[0], b[1]) * size,
            )
            .expand(3.0);
            let c = issue_color(issue.kind);
            let stroke = if on_layer {
                egui::Stroke::new(if sel { 2.5 } else { 1.5 }, c)
            } else {
                egui::Stroke::new(1.0, c.gamma_multiply(0.4))
            };
            if sel && on_layer {
                painter.rect_filled(r, 4.0, c.gamma_multiply(0.10));
            }
            painter.rect_stroke(r, 4.0, stroke, egui::StrokeKind::Outside);
            if on_layer && (r.width() > 40.0 || sel) {
                labels.push((r, c, issue.kind.label(), sel));
            }
        }
        // Selected first so it gets the best spot.
        labels.sort_by_key(|l| !l.3);
        let font = egui::FontId::new(11.5, theme::semibold());
        let mut placed: Vec<egui::Rect> = Vec::new();
        for (r, c, text, _) in labels {
            let w = painter
                .layout_no_wrap(text.to_string(), font.clone(), c)
                .size()
                .x
                + 14.0;
            let lh = 20.0;
            // Above the box if there is room, else just inside its top edge;
            // always kept inside the view.
            let mut y = if r.top() - lh - 4.0 >= inner.top() {
                r.top() - lh - 4.0
            } else {
                (r.top() + 4.0).max(inner.top())
            };
            let x = r
                .left()
                .clamp(inner.left(), (inner.right() - w).max(inner.left()));
            let mut lr = egui::Rect::from_min_size(egui::pos2(x, y), Vec2::new(w, lh));
            // Step down past any label already placed there.
            while placed.iter().any(|p| p.intersects(lr)) && lr.bottom() < inner.bottom() {
                y += lh + 3.0;
                lr = egui::Rect::from_min_size(egui::pos2(x, y), Vec2::new(w, lh));
            }
            if lr.bottom() > inner.bottom() {
                continue;
            }
            placed.push(lr);
            theme::pill(
                &painter,
                lr.min,
                egui::Align2::LEFT_TOP,
                text,
                c,
                font.clone(),
            );
        }

        // Overlays: orientation note, cursor position, hints.
        theme::pill(
            &painter,
            rect.left_top() + Vec2::new(10.0, 10.0),
            egui::Align2::LEFT_TOP,
            "Seen from above the plate (X mirror undone)",
            theme::TEXT_WEAK,
            egui::FontId::proportional(11.5),
        );
        if let Some(p) = resp.hover_pos() {
            let uv = (p - img_rect.min) / size;
            if (0.0..=1.0).contains(&uv.x) && (0.0..=1.0).contains(&uv.y) {
                let mm = img.uv_to_mm(&h, uv.x, uv.y);
                theme::pill(
                    &painter,
                    rect.right_top() + Vec2::new(-10.0, 10.0),
                    egui::Align2::RIGHT_TOP,
                    &format!("X {:.2}  Y {:.2} mm", mm[0], mm[1]),
                    theme::TEXT,
                    egui::FontId::monospace(11.5),
                );
            }
        }
        theme::hints(
            &painter,
            rect.left_bottom() + Vec2::new(14.0, -10.0),
            &[("↑ ↓", "layer"), ("Wheel", "zoom"), ("Drag", "pan")],
        );

        // The picked problem, explained.
        let picked = self
            .selected_issue
            .and_then(|i| out.report.issues.get(i).map(|x| (i, x.clone())));
        if let Some((_, issue)) = picked {
            let c = issue_color(issue.kind);
            let w = 340.0_f32.min(rect.width() - 24.0);
            let area = egui::Rect::from_min_size(
                egui::pos2(rect.right() - w - 12.0, rect.bottom() - 44.0 - 130.0),
                Vec2::new(w, 130.0),
            );
            let mut clear = false;
            let mut jump = false;
            ui.scope_builder(
                egui::UiBuilder::new()
                    .max_rect(area)
                    .layout(egui::Layout::bottom_up(egui::Align::LEFT)),
                |ui| {
                    egui::Frame::new()
                        .fill(Color32::from_rgba_unmultiplied(24, 27, 34, 240))
                        .stroke(egui::Stroke::new(1.0, c.gamma_multiply(0.5)))
                        .corner_radius(10)
                        .inner_margin(egui::Margin::same(12))
                        .shadow(egui::Shadow {
                            offset: [0, 4],
                            blur: 16,
                            spread: 0,
                            color: Color32::from_black_alpha(120),
                        })
                        .show(ui, |ui| {
                            ui.set_width(w - 24.0);
                            ui.with_layout(egui::Layout::top_down(egui::Align::LEFT), |ui| {
                                ui.horizontal(|ui| {
                                    theme::dot_label(
                                        ui,
                                        c,
                                        RichText::new(issue.kind.label())
                                            .font(egui::FontId::new(14.0, theme::semibold()))
                                            .color(Color32::WHITE),
                                    );
                                    ui.with_layout(
                                        egui::Layout::right_to_left(egui::Align::Center),
                                        |ui| {
                                            clear = ui
                                                .small_button("×")
                                                .on_hover_text("Close")
                                                .clicked();
                                        },
                                    );
                                });
                                theme::hint(ui, issue_detail(&issue));
                                ui.label(
                                    RichText::new(issue_help(issue.kind))
                                        .small()
                                        .color(theme::TEXT),
                                );
                                if issue.first_layer as usize != self.layer {
                                    jump = ui.small_button("Go to its first layer").clicked();
                                }
                            });
                        });
                },
            );
            if clear {
                self.selected_issue = None;
            }
            if jump {
                self.layer = issue.first_layer as usize;
            }
        }
    }

    /// A vertical layer slider with the problems marked along it.
    fn layer_scrubber(&mut self, ui: &mut egui::Ui, rect: egui::Rect, n: usize) {
        let painter = ui.painter_at(rect);
        painter.rect_filled(rect, 0.0, theme::PANEL);
        let track = egui::Rect::from_center_size(
            rect.center(),
            Vec2::new(6.0, (rect.height() - 72.0).max(20.0)),
        );
        let resp = ui.interact(
            rect,
            ui.scope_id().with("scrubber"),
            egui::Sense::click_and_drag(),
        );
        let y_of = |l: usize| -> f32 {
            let t = if n > 1 {
                l as f32 / (n - 1) as f32
            } else {
                0.0
            };
            track.bottom() - t * track.height()
        };
        if let Some(p) = resp.interact_pointer_pos() {
            if resp.dragged() || resp.clicked() || resp.drag_started() {
                let t = ((track.bottom() - p.y) / track.height()).clamp(0.0, 1.0);
                self.layer = (t * (n - 1) as f32).round() as usize;
            }
        }
        if resp.hovered() {
            let scroll = ui.input(|i| i.smooth_scroll_delta.y);
            if scroll > 0.0 {
                self.layer = (self.layer + 1).min(n - 1);
            } else if scroll < 0.0 {
                self.layer = self.layer.saturating_sub(1);
            }
        }
        painter.rect_filled(track, 3.0, theme::FIELD);
        let y = y_of(self.layer);
        painter.rect_filled(
            egui::Rect::from_min_max(egui::pos2(track.left(), y), track.right_bottom()),
            3.0,
            theme::ACCENT.gamma_multiply(0.75),
        );
        // Problem markers.
        if let Some(out) = &self.result {
            for (i, issue) in out.report.issues.iter().enumerate() {
                let y0 = y_of(issue.first_layer as usize);
                let y1 = y_of(issue.last_layer as usize);
                let c = issue_color(issue.kind);
                let sel = self.selected_issue == Some(i);
                let x = track.right() + 4.0;
                painter.rect_filled(
                    egui::Rect::from_min_max(
                        egui::pos2(x, y1 - 1.5),
                        egui::pos2(x + if sel { 8.0 } else { 6.0 }, y0 + 1.5),
                    ),
                    1.0,
                    c,
                );
            }
        }
        let handle = egui::pos2(track.center().x, y);
        painter.circle_filled(handle, 8.0, Color32::WHITE);
        painter.circle_stroke(handle, 8.0, egui::Stroke::new(2.0, theme::ACCENT));
        painter.text(
            egui::pos2(rect.center().x, rect.top() + 10.0),
            egui::Align2::CENTER_TOP,
            n.to_string(),
            egui::FontId::proportional(10.5),
            theme::TEXT_FAINT,
        );
        painter.text(
            egui::pos2(rect.center().x, rect.bottom() - 10.0),
            egui::Align2::CENTER_BOTTOM,
            "1",
            egui::FontId::proportional(10.5),
            theme::TEXT_FAINT,
        );
        if resp.hovered() || resp.dragged() {
            resp.on_hover_text_at_pointer(format!("Layer {}", self.layer + 1));
        }
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

/// Where and how big a problem is, without its kind.
fn issue_detail(i: &Issue) -> String {
    let layers = if i.first_layer == i.last_layer {
        format!("Layer {}", i.first_layer + 1)
    } else {
        format!("Layers {}–{}", i.first_layer + 1, i.last_layer + 1)
    };
    let c = i.area.center();
    let size = if i.kind == IssueKind::TrappedResin {
        format!("{:.1} mm³", i.size)
    } else {
        format!("{:.1} mm²", i.size)
    };
    format!("{layers}  ·  ({:.1}, {:.1}) mm  ·  {size}", c[0], c[1])
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
                c && !i.modifiers.shift && i.key_pressed(egui::Key::S),
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
        if ctx.input(|i| i.modifiers.command && i.modifiers.shift && i.key_pressed(egui::Key::S)) {
            self.save_project_dialog();
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

        egui::Panel::top("top-bar")
            .exact_size(52.0)
            .frame(
                egui::Frame::new()
                    .fill(theme::BG)
                    .inner_margin(egui::Margin::symmetric(14, 0))
                    .stroke(egui::Stroke::new(1.0, theme::BORDER)),
            )
            .show(ui, |ui| self.top_bar(ui));
        egui::Panel::bottom("status")
            .exact_size(26.0)
            .frame(
                egui::Frame::new()
                    .fill(theme::BG)
                    .inner_margin(egui::Margin::symmetric(14, 0)),
            )
            .show(ui, |ui| {
                ui.horizontal_centered(|ui| {
                    let busy = self.job.is_some();
                    let dot = if busy { theme::ACCENT } else { theme::OK };
                    let (r, _) = ui.allocate_exact_size(Vec2::splat(8.0), egui::Sense::hover());
                    ui.painter().circle_filled(r.center(), 3.5, dot);
                    ui.label(RichText::new(&self.status).small().color(theme::TEXT_WEAK));
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        ui.label(
                            RichText::new(format!(
                                "{}  ·  {} × {} mm",
                                self.printer.name, self.printer.size_x, self.printer.size_y
                            ))
                            .small()
                            .color(theme::TEXT_FAINT),
                        );
                    });
                });
            });
        let side = egui::Frame::new()
            .fill(theme::PANEL)
            .inner_margin(egui::Margin::same(12));
        let layers = self.tab == Tab::Layers && self.result.is_some();
        if !layers {
            egui::Panel::left("objects")
                .default_size(290.0)
                .size_range(240.0..=420.0)
                .frame(side)
                .show(ui, |ui| {
                    egui::ScrollArea::vertical()
                        .id_salt("left")
                        .auto_shrink([false, false])
                        .show(ui, |ui| self.objects_panel(ui));
                });
        }
        egui::Panel::right("settings")
            .default_size(340.0)
            .size_range(300.0..=480.0)
            .frame(side)
            .show(ui, |ui| {
                if layers {
                    egui::ScrollArea::vertical()
                        .id_salt("results")
                        .auto_shrink([false, false])
                        .show(ui, |ui| self.results_ui(ui));
                } else {
                    self.settings_panel(ui);
                }
            });
        egui::CentralPanel::default()
            .frame(egui::Frame::NONE)
            .show(ui, |ui| {
                if !layers {
                    self.view_3d(ui);
                    return;
                }
                let cutaway =
                    self.show_cutaway && self.result.as_ref().is_some_and(|r| r.opened.is_none());
                if cutaway {
                    // The 3D cut-away gets a resizable share of the space.
                    let w = ui.available_width();
                    egui::Panel::right("cutaway")
                        .resizable(true)
                        .default_size(w * 0.42)
                        .size_range(240.0..=(w * 0.7).max(260.0))
                        .frame(egui::Frame::NONE)
                        .show(ui, |ui| self.view_3d(ui));
                }
                egui::CentralPanel::default()
                    .frame(egui::Frame::NONE)
                    .show(ui, |ui| self.layers_view(ui));
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
