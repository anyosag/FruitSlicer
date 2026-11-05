//! Project files (.fsproj): the plate and its settings in one zip.
//!
//! Layout: `project.json` with the settings and each object's placement and
//! hand-edited supports, plus `models/<n>.stl` with each object's mesh as it
//! was loaded. Supports are regenerated on load from the saved settings and
//! edits rather than stored.

use std::io::{Read, Write};

use slicer::checks::CheckParams;
use slicer::hollow::HollowParams;
use slicer::mesh::Mesh;
use slicer::supports::SupportParams;
use slicer::{stl, PrintSettings};

use crate::scene::Placement;

pub const EXTENSION: &str = "fsproj";
const FORMAT: u32 = 1;

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct ProjectSettings {
    pub print: PrintSettings,
    pub supports: SupportParams,
    pub checks: bool,
    pub check_params: CheckParams,
    pub hollow: bool,
    pub hollow_params: HollowParams,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct ProjectObject {
    pub name: String,
    pub placement: Placement,
    pub supports: bool,
    #[serde(default)]
    pub added: Vec<[f32; 3]>,
    #[serde(default)]
    pub removed: Vec<[f32; 3]>,
    /// Path of the mesh inside the zip.
    pub mesh: String,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
struct Manifest {
    format: u32,
    app: String,
    settings: ProjectSettings,
    objects: Vec<ProjectObject>,
}

#[derive(Debug)]
pub struct Project {
    pub settings: ProjectSettings,
    pub objects: Vec<(ProjectObject, Mesh)>,
}

/// Writes a project. The `mesh` field of each object is filled in here.
pub fn write(
    settings: &ProjectSettings,
    objects: &[(ProjectObject, &Mesh)],
) -> Result<Vec<u8>, String> {
    let mut buf = std::io::Cursor::new(Vec::new());
    let mut zip = zip::ZipWriter::new(&mut buf);
    let opts = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    let mut manifest = Manifest {
        format: FORMAT,
        app: format!("FruitSlicer {}", env!("CARGO_PKG_VERSION")),
        settings: settings.clone(),
        objects: Vec::new(),
    };
    for (i, (o, m)) in objects.iter().enumerate() {
        let path = format!("models/{i}.stl");
        zip.start_file(path.as_str(), opts)
            .map_err(|e| e.to_string())?;
        zip.write_all(&stl::to_binary(m))
            .map_err(|e| e.to_string())?;
        let mut o = o.clone();
        o.mesh = path;
        manifest.objects.push(o);
    }
    zip.start_file("project.json", opts)
        .map_err(|e| e.to_string())?;
    let json = serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?;
    zip.write_all(&json).map_err(|e| e.to_string())?;
    zip.finish().map_err(|e| e.to_string())?;
    Ok(buf.into_inner())
}

pub fn read(data: &[u8]) -> Result<Project, String> {
    let mut zip = zip::ZipArchive::new(std::io::Cursor::new(data))
        .map_err(|e| format!("not a FruitSlicer project: {e}"))?;
    let mut json = Vec::new();
    zip.by_name("project.json")
        .map_err(|_| "not a FruitSlicer project: project.json is missing".to_string())?
        .read_to_end(&mut json)
        .map_err(|e| e.to_string())?;
    let manifest: Manifest =
        serde_json::from_slice(&json).map_err(|e| format!("project.json: {e}"))?;
    if manifest.format > FORMAT {
        return Err(format!(
            "this project was saved by a newer FruitSlicer (format {})",
            manifest.format
        ));
    }
    let mut objects = Vec::new();
    for o in manifest.objects {
        let mut bytes = Vec::new();
        zip.by_name(&o.mesh)
            .map_err(|_| format!("{} is missing from the project", o.mesh))?
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        let m = stl::parse(&bytes).map_err(|e| format!("{}: {e}", o.mesh))?;
        objects.push((o, m));
    }
    Ok(Project {
        settings: manifest.settings,
        objects,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let mut m = Mesh::default();
        m.triangles
            .push([[0.0, 0.0, 0.0], [10.0, 0.0, 0.0], [0.0, 10.0, 0.0]]);
        m.triangles
            .push([[0.0, 0.0, 0.0], [0.0, 10.0, 0.0], [0.0, 0.0, 5.0]]);
        let settings = ProjectSettings {
            print: PrintSettings {
                layer_mm: 0.03,
                anti_alias: 4,
                ..Default::default()
            },
            supports: SupportParams {
                spacing_mm: 2.0,
                ..Default::default()
            },
            checks: false,
            check_params: CheckParams::default(),
            hollow: true,
            hollow_params: HollowParams::default(),
        };
        let o = ProjectObject {
            name: "part".into(),
            placement: Placement {
                scale: 2.0,
                rotation: [10.0, 0.0, 90.0],
                offset: [5.0, -3.0],
            },
            supports: true,
            added: vec![[1.0, 2.0, 3.0]],
            removed: vec![[4.0, 5.0, 6.0]],
            mesh: String::new(),
        };
        let bytes = write(&settings, &[(o.clone(), &m), (o.clone(), &m)]).unwrap();
        let p = read(&bytes).unwrap();
        assert_eq!(p.settings, settings);
        assert_eq!(p.objects.len(), 2);
        let (o2, m2) = &p.objects[1];
        assert_eq!(o2.placement, o.placement);
        assert_eq!(o2.added, o.added);
        assert_eq!(o2.removed, o.removed);
        assert_eq!(o2.mesh, "models/1.stl");
        assert_eq!(m2.triangles, m.triangles);
    }

    #[test]
    fn rejects_other_zips() {
        let mut buf = std::io::Cursor::new(Vec::new());
        let mut z = zip::ZipWriter::new(&mut buf);
        z.start_file("x.txt", zip::write::SimpleFileOptions::default())
            .unwrap();
        z.finish().unwrap();
        assert!(read(buf.get_ref()).unwrap_err().contains("project.json"));
        assert!(read(b"garbage").is_err());
    }
}
