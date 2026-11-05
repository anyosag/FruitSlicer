//! FruitSlicer desktop app.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod camera;
mod gpu_probe;
mod import;
mod jobs;
mod layer_view;
mod project;
mod render;
mod scene;
mod util;

use eframe::egui;

fn main() {
    let open: Vec<std::path::PathBuf> = std::env::args_os().skip(1).map(Into::into).collect();
    let (choice, log) = gpu_probe::probe();
    let mut options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("FruitSlicer")
            .with_inner_size([1400.0, 860.0])
            .with_min_inner_size([900.0, 600.0])
            .with_drag_and_drop(true),
        depth_buffer: 24,
        multisampling: render::MSAA as u16,
        renderer: eframe::Renderer::Wgpu,
        ..Default::default()
    };
    let mut gpu_note = None;
    match &choice {
        Some(c) => {
            options.wgpu_options.wgpu_setup = gpu_probe::setup(c);
            if c.software {
                gpu_note = Some(format!(
                    "Using software rendering ({}): the 3D view may be slow.",
                    c.adapter_name
                ));
            }
        }
        None if std::env::var_os("WGPU_BACKEND").is_none() => {
            fail(&format!(
                "FruitSlicer couldn't find a graphics adapter it can use.\n\nTried:\n{}\n\n\
                 Updating your graphics driver usually fixes this.",
                log.join("\n")
            ));
            return;
        }
        None => {}
    }
    let result = eframe::run_native(
        "FruitSlicer",
        options,
        Box::new(move |cc| Ok(Box::new(app::App::new(cc, open, gpu_note)))),
    );
    if let Err(e) = result {
        fail(&format!(
            "FruitSlicer stopped with an error:\n\n{e}\n\nGraphics adapters tried:\n{}",
            log.join("\n")
        ));
    }
}

/// Shows an error the user can see even without a console.
fn fail(msg: &str) {
    eprintln!("{msg}");
    let _ = rfd::MessageDialog::new()
        .set_title("FruitSlicer")
        .set_description(msg)
        .set_level(rfd::MessageLevel::Error)
        .show();
}
