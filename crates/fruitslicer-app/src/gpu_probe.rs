//! Picks a graphics adapter that can actually create a device, before the
//! window opens. Without this, a machine whose preferred backend is broken
//! (an old driver, a VM, remote desktop) gets a crash at startup.
//!
//! Order: the usual hardware backends (DirectX 12 / Vulkan / Metal), then
//! OpenGL, then a software adapter (WARP on Windows).

use std::sync::Arc;

use eframe::egui_wgpu::{self, wgpu};

pub struct Choice {
    pub backends: wgpu::Backends,
    pub adapter_name: String,
    pub backend: wgpu::Backend,
    pub software: bool,
}

fn instance(backends: wgpu::Backends) -> wgpu::Instance {
    let mut desc = egui_wgpu::WgpuSetupCreateNew::without_display_handle().instance_descriptor;
    desc.backends = backends;
    wgpu::Instance::new(desc)
}

fn device_works(adapter: &wgpu::Adapter) -> bool {
    pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).is_ok()
}

/// Returns the first adapter that can make a device, and a log of what was
/// tried (for the error message if nothing works).
pub fn probe() -> (Option<Choice>, Vec<String>) {
    let mut log = Vec::new();
    if std::env::var_os("WGPU_BACKEND").is_some() {
        log.push("WGPU_BACKEND is set; not probing.".into());
        return (None, log);
    }
    for backends in [wgpu::Backends::PRIMARY, wgpu::Backends::GL] {
        let inst = instance(backends);
        let mut adapters = pollster::block_on(inst.enumerate_adapters(backends));
        // Prefer real GPUs over software ones.
        adapters.sort_by_key(|a| (a.get_info().device_type == wgpu::DeviceType::Cpu) as u8);
        for a in adapters {
            let info = a.get_info();
            let ok = device_works(&a);
            log.push(format!(
                "{:?} {} ({:?}): {}",
                info.backend,
                info.name,
                info.device_type,
                if ok { "ok" } else { "failed" }
            ));
            if ok {
                return (
                    Some(Choice {
                        backends,
                        adapter_name: info.name,
                        backend: info.backend,
                        software: info.device_type == wgpu::DeviceType::Cpu,
                    }),
                    log,
                );
            }
        }
    }
    // Software fallback (WARP on Windows).
    let inst = instance(wgpu::Backends::PRIMARY);
    if let Ok(a) = pollster::block_on(inst.request_adapter(&wgpu::RequestAdapterOptions {
        force_fallback_adapter: true,
        ..Default::default()
    })) {
        let info = a.get_info();
        let ok = device_works(&a);
        log.push(format!(
            "fallback {:?} {}: {}",
            info.backend,
            info.name,
            if ok { "ok" } else { "failed" }
        ));
        if ok {
            return (
                Some(Choice {
                    backends: wgpu::Backends::PRIMARY,
                    adapter_name: info.name,
                    backend: info.backend,
                    software: true,
                }),
                log,
            );
        }
    }
    (None, log)
}

/// eframe setup that uses the probed adapter.
pub fn setup(choice: &Choice) -> egui_wgpu::WgpuSetup {
    let mut s = egui_wgpu::WgpuSetupCreateNew::without_display_handle();
    s.instance_descriptor.backends = choice.backends;
    let (name, backend) = (choice.adapter_name.clone(), choice.backend);
    s.native_adapter_selector = Some(Arc::new(move |adapters: &[wgpu::Adapter], _surface| {
        adapters
            .iter()
            .find(|a| {
                let i = a.get_info();
                i.name == name && i.backend == backend
            })
            .or_else(|| adapters.first())
            .cloned()
            .ok_or_else(|| "no graphics adapter".to_string())
    }));
    egui_wgpu::WgpuSetup::CreateNew(s)
}
