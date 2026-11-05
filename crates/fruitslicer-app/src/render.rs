//! The 3D viewport: a small wgpu renderer drawn inside egui's render pass
//! through a paint callback.
//!
//! Meshes are uploaded once (when they change) into [`Gpu3d`], which lives in
//! egui-wgpu's callback resources. Each frame the app sends a [`ViewCallback`]
//! that lists which meshes to draw, in which colour.

use std::collections::HashMap;

use eframe::egui_wgpu::{self, wgpu};
use glam::Mat4;
use slicer::mesh::Mesh;

/// Must match `NativeOptions::multisampling` and `depth_buffer` in main.rs.
pub const MSAA: u32 = 4;
pub const DEPTH_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Depth24Plus;

/// Uniform slots per frame (one per draw call).
const SLOTS: usize = 256;
const SLOT_STRIDE: u64 = 256;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Vertex {
    pub pos: [f32; 3],
    pub normal: [f32; 3],
    /// 1.0 on faces that need support, for the overhang highlight.
    pub flag: f32,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Uniforms {
    view_proj: [[f32; 4]; 4],
    color: [f32; 4],
    /// xyz: light direction; w: 1 = shaded, 0 = flat colour.
    light: [f32; 4],
    /// x: overhang highlight on (1) or off (0); y: clip height; z: clip on.
    params: [f32; 4],
}

const SHADER: &str = r#"
struct U {
    view_proj: mat4x4<f32>,
    color: vec4<f32>,
    light: vec4<f32>,
    params: vec4<f32>,
};
@group(0) @binding(0) var<uniform> u: U;

struct VOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) n: vec3<f32>,
    @location(1) flag: f32,
    @location(2) z: f32,
};

@vertex
fn vs(@location(0) p: vec3<f32>, @location(1) n: vec3<f32>, @location(2) flag: f32) -> VOut {
    var o: VOut;
    o.pos = u.view_proj * vec4<f32>(p, 1.0);
    o.n = n;
    o.flag = flag;
    o.z = p.z;
    return o;
}

@fragment
fn fs(i: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4<f32> {
    if (u.params.z > 0.5 && i.z > u.params.y) {
        discard;
    }
    if (u.light.w < 0.5) {
        return u.color;
    }
    var base = u.color.rgb;
    if (u.params.x > 0.5 && i.flag > 0.5) {
        base = mix(base, vec3<f32>(0.95, 0.25, 0.2), 0.75);
    }
    var n = normalize(i.n);
    if (!ff) {
        // Inside of the mesh (a hole or flipped faces): darker.
        n = -n;
        base = base * 0.45;
    }
    let key = max(dot(n, normalize(u.light.xyz)), 0.0);
    let fill = max(dot(n, normalize(vec3<f32>(-0.4, -0.3, 0.8))), 0.0) * 0.25;
    let c = base * (0.28 + 0.62 * key + fill);
    return vec4<f32>(c, u.color.a);
}
"#;

struct GpuMesh {
    buffer: wgpu::Buffer,
    count: u32,
}

/// GPU state, stored in egui-wgpu's callback resources.
pub struct Gpu3d {
    tri_pipeline: wgpu::RenderPipeline,
    tri_blend_pipeline: wgpu::RenderPipeline,
    line_pipeline: wgpu::RenderPipeline,
    uniforms: wgpu::Buffer,
    bind_group: wgpu::BindGroup,
    meshes: HashMap<u64, GpuMesh>,
}

impl Gpu3d {
    pub fn install(rs: &egui_wgpu::RenderState) {
        let device = &rs.device;
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("fruitslicer 3d"),
            source: wgpu::ShaderSource::Wgsl(SHADER.into()),
        });
        let bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("fruitslicer 3d"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    has_dynamic_offset: true,
                    min_binding_size: wgpu::BufferSize::new(std::mem::size_of::<Uniforms>() as u64),
                },
                count: None,
            }],
        });
        let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("fruitslicer 3d"),
            bind_group_layouts: &[Some(&bgl)],
            immediate_size: 0,
        });
        let attrs = wgpu::vertex_attr_array![0 => Float32x3, 1 => Float32x3, 2 => Float32];
        let make = |topology, blend: bool, depth_write: bool| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("fruitslicer 3d"),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: Some("vs"),
                    compilation_options: Default::default(),
                    buffers: &[Some(wgpu::VertexBufferLayout {
                        array_stride: std::mem::size_of::<Vertex>() as u64,
                        step_mode: wgpu::VertexStepMode::Vertex,
                        attributes: &attrs,
                    })],
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: Some("fs"),
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: rs.target_format,
                        blend: Some(if blend {
                            wgpu::BlendState::ALPHA_BLENDING
                        } else {
                            wgpu::BlendState::REPLACE
                        }),
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                primitive: wgpu::PrimitiveState {
                    topology,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: Some(wgpu::DepthStencilState {
                    format: DEPTH_FORMAT,
                    depth_write_enabled: Some(depth_write),
                    depth_compare: Some(wgpu::CompareFunction::LessEqual),
                    stencil: Default::default(),
                    bias: Default::default(),
                }),
                multisample: wgpu::MultisampleState {
                    count: MSAA,
                    ..Default::default()
                },
                multiview_mask: None,
                cache: None,
            })
        };
        let tri_pipeline = make(wgpu::PrimitiveTopology::TriangleList, false, true);
        let tri_blend_pipeline = make(wgpu::PrimitiveTopology::TriangleList, true, false);
        let line_pipeline = make(wgpu::PrimitiveTopology::LineList, true, true);
        let uniforms = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("fruitslicer 3d uniforms"),
            size: SLOT_STRIDE * SLOTS as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("fruitslicer 3d"),
            layout: &bgl,
            entries: &[wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                    buffer: &uniforms,
                    offset: 0,
                    size: wgpu::BufferSize::new(std::mem::size_of::<Uniforms>() as u64),
                }),
            }],
        });
        rs.renderer.write().callback_resources.insert(Gpu3d {
            tri_pipeline,
            tri_blend_pipeline,
            line_pipeline,
            uniforms,
            bind_group,
            meshes: HashMap::new(),
        });
    }

    /// Uploads (or replaces) a vertex list under `key`.
    pub fn upload(rs: &egui_wgpu::RenderState, key: u64, verts: &[Vertex]) {
        use wgpu::util::DeviceExt;
        let mut r = rs.renderer.write();
        let Some(gpu) = r.callback_resources.get_mut::<Gpu3d>() else {
            return;
        };
        if verts.is_empty() {
            gpu.meshes.remove(&key);
            return;
        }
        let buffer = rs
            .device
            .create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("fruitslicer mesh"),
                contents: bytemuck::cast_slice(verts),
                usage: wgpu::BufferUsages::VERTEX,
            });
        gpu.meshes.insert(
            key,
            GpuMesh {
                buffer,
                count: verts.len() as u32,
            },
        );
    }

    pub fn remove(rs: &egui_wgpu::RenderState, key: u64) {
        if let Some(gpu) = rs.renderer.write().callback_resources.get_mut::<Gpu3d>() {
            gpu.meshes.remove(&key);
        }
    }
}

/// Flat-shaded vertices for a mesh. `flag_cos` marks faces whose downward
/// normal is steeper than this (faces that need support); `None` = no flags.
pub fn mesh_vertices(m: &Mesh, flag_cos: Option<f32>, orient: f32) -> Vec<Vertex> {
    let mut v = Vec::with_capacity(m.triangles.len() * 3);
    for t in &m.triangles {
        let a = glam::Vec3::from(t[0]);
        let b = glam::Vec3::from(t[1]);
        let c = glam::Vec3::from(t[2]);
        let n = (b - a).cross(c - a) * orient;
        let n = n.normalize_or_zero();
        let flag = match flag_cos {
            Some(limit) if -n.z > limit => 1.0,
            _ => 0.0,
        };
        for p in [a, b, c] {
            v.push(Vertex {
                pos: p.into(),
                normal: n.into(),
                flag,
            });
        }
    }
    v
}

/// Line vertices (pairs) for the plate grid and build volume outline.
pub fn plate_lines(size: [f32; 3]) -> (Vec<Vertex>, Vec<Vertex>) {
    let (hx, hy, hz) = (size[0] / 2.0, size[1] / 2.0, size[2]);
    let lv = |x: f32, y: f32, z: f32| Vertex {
        pos: [x, y, z],
        normal: [0.0, 0.0, 1.0],
        flag: 0.0,
    };
    let mut grid = Vec::new();
    let step = 10.0;
    let mut x = 0.0f32;
    while x <= hx {
        for s in [x, -x] {
            grid.push(lv(s, -hy, 0.0));
            grid.push(lv(s, hy, 0.0));
        }
        x += step;
    }
    let mut y = 0.0f32;
    while y <= hy {
        for s in [y, -y] {
            grid.push(lv(-hx, s, 0.0));
            grid.push(lv(hx, s, 0.0));
        }
        y += step;
    }
    let mut outline = Vec::new();
    let corners = [(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)];
    for z in [0.0, hz] {
        for i in 0..4 {
            let (a, b) = (corners[i], corners[(i + 1) % 4]);
            outline.push(lv(a.0, a.1, z));
            outline.push(lv(b.0, b.1, z));
        }
    }
    for c in corners {
        outline.push(lv(c.0, c.1, 0.0));
        outline.push(lv(c.0, c.1, hz));
    }
    (grid, outline)
}

/// Line vertices for an axis-aligned box.
pub fn box_lines(min: [f32; 3], max: [f32; 3]) -> Vec<Vertex> {
    let lv = |x: f32, y: f32, z: f32| Vertex {
        pos: [x, y, z],
        normal: [0.0, 0.0, 1.0],
        flag: 0.0,
    };
    let c = |i: usize| {
        lv(
            if i & 1 == 0 { min[0] } else { max[0] },
            if i & 2 == 0 { min[1] } else { max[1] },
            if i & 4 == 0 { min[2] } else { max[2] },
        )
    };
    let edges = [
        (0, 1),
        (2, 3),
        (4, 5),
        (6, 7),
        (0, 2),
        (1, 3),
        (4, 6),
        (5, 7),
        (0, 4),
        (1, 5),
        (2, 6),
        (3, 7),
    ];
    edges.iter().flat_map(|&(a, b)| [c(a), c(b)]).collect()
}

#[derive(Clone, Copy, PartialEq)]
pub enum DrawKind {
    /// Opaque, shaded.
    Solid,
    /// Translucent, shaded, no depth write. Drawn after solids.
    Ghost,
    /// Flat-coloured lines.
    Lines,
}

#[derive(Clone)]
pub struct Draw {
    pub key: u64,
    pub color: [f32; 4],
    pub kind: DrawKind,
    pub highlight_overhangs: bool,
    /// Hide everything above this height.
    pub clip_z: Option<f32>,
}

/// One frame's 3D scene.
pub struct ViewCallback {
    pub view_proj: Mat4,
    pub light_dir: glam::Vec3,
    pub draws: Vec<Draw>,
}

impl ViewCallback {
    fn ordered(&self) -> Vec<&Draw> {
        let mut d: Vec<&Draw> = self.draws.iter().take(SLOTS).collect();
        d.sort_by_key(|d| match d.kind {
            DrawKind::Solid => 0,
            DrawKind::Lines => 1,
            DrawKind::Ghost => 2,
        });
        d
    }
}

impl egui_wgpu::CallbackTrait for ViewCallback {
    fn prepare(
        &self,
        _device: &wgpu::Device,
        queue: &wgpu::Queue,
        _screen: &egui_wgpu::ScreenDescriptor,
        _encoder: &mut wgpu::CommandEncoder,
        res: &mut egui_wgpu::CallbackResources,
    ) -> Vec<wgpu::CommandBuffer> {
        let Some(gpu) = res.get::<Gpu3d>() else {
            return Vec::new();
        };
        for (slot, d) in self.ordered().into_iter().enumerate() {
            let u = Uniforms {
                view_proj: self.view_proj.to_cols_array_2d(),
                color: d.color,
                light: [
                    self.light_dir.x,
                    self.light_dir.y,
                    self.light_dir.z,
                    if d.kind == DrawKind::Lines { 0.0 } else { 1.0 },
                ],
                params: [
                    d.highlight_overhangs as u8 as f32,
                    d.clip_z.unwrap_or(0.0),
                    d.clip_z.is_some() as u8 as f32,
                    0.0,
                ],
            };
            queue.write_buffer(
                &gpu.uniforms,
                slot as u64 * SLOT_STRIDE,
                bytemuck::bytes_of(&u),
            );
        }
        Vec::new()
    }

    fn paint(
        &self,
        _info: egui::PaintCallbackInfo,
        pass: &mut wgpu::RenderPass<'static>,
        res: &egui_wgpu::CallbackResources,
    ) {
        let Some(gpu) = res.get::<Gpu3d>() else {
            return;
        };
        for (slot, d) in self.ordered().into_iter().enumerate() {
            let Some(m) = gpu.meshes.get(&d.key) else {
                continue;
            };
            pass.set_pipeline(match d.kind {
                DrawKind::Solid => &gpu.tri_pipeline,
                DrawKind::Ghost => &gpu.tri_blend_pipeline,
                DrawKind::Lines => &gpu.line_pipeline,
            });
            pass.set_bind_group(0, &gpu.bind_group, &[(slot as u64 * SLOT_STRIDE) as u32]);
            pass.set_vertex_buffer(0, m.buffer.slice(..));
            pass.draw(0..m.count, 0..1);
        }
    }
}

use eframe::egui;
