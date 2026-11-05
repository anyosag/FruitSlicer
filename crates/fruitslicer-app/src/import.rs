//! OBJ and 3MF import. STL is handled by the slicer crate.

use slicer::mesh::{Mesh, Triangle};

/// Parses a Wavefront OBJ: `v` lines and `f` lines (polygons are fanned
/// into triangles). Negative (relative) indices are supported.
pub fn parse_obj(data: &[u8]) -> Result<Mesh, String> {
    let text = String::from_utf8_lossy(data);
    let mut verts: Vec<[f32; 3]> = Vec::new();
    let mut tris: Vec<Triangle> = Vec::new();
    for (n, line) in text.lines().enumerate() {
        let mut w = line.split_whitespace();
        match w.next() {
            Some("v") => {
                let mut v = [0f32; 3];
                for c in &mut v {
                    *c = w
                        .next()
                        .and_then(|s| s.parse().ok())
                        .ok_or(format!("OBJ line {}: bad vertex", n + 1))?;
                }
                verts.push(v);
            }
            Some("f") => {
                let idx: Result<Vec<usize>, String> = w
                    .map(|s| {
                        let i: i64 = s
                            .split('/')
                            .next()
                            .and_then(|s| s.parse().ok())
                            .ok_or(format!("OBJ line {}: bad face", n + 1))?;
                        let i = if i < 0 { verts.len() as i64 + i } else { i - 1 };
                        if i < 0 || i as usize >= verts.len() {
                            return Err(format!(
                                "OBJ line {}: vertex {} out of range",
                                n + 1,
                                i + 1
                            ));
                        }
                        Ok(i as usize)
                    })
                    .collect();
                let idx = idx?;
                for k in 1..idx.len().saturating_sub(1) {
                    tris.push([verts[idx[0]], verts[idx[k]], verts[idx[k + 1]]]);
                }
            }
            _ => {}
        }
    }
    if tris.is_empty() {
        return Err("no faces found".into());
    }
    Ok(Mesh::new(tris))
}

/// Parses a 3MF: every `<object>` mesh in 3D/3dmodel.model, with each
/// `<item>` in `<build>` placed by its transform (component references are
/// followed one level deep). Units other than millimetres are converted.
pub fn parse_3mf(data: &[u8]) -> Result<Mesh, String> {
    use std::io::Read;
    let mut zip =
        zip::ZipArchive::new(std::io::Cursor::new(data)).map_err(|e| format!("3MF: {e}"))?;
    let name = (0..zip.len())
        .filter_map(|i| zip.by_index(i).ok().map(|f| f.name().to_string()))
        .find(|n| n.to_lowercase().ends_with(".model"))
        .ok_or("3MF: no .model file inside")?;
    let mut xml = String::new();
    zip.by_name(&name)
        .map_err(|e| format!("3MF: {e}"))?
        .read_to_string(&mut xml)
        .map_err(|e| format!("3MF: {e}"))?;
    parse_3mf_model(&xml)
}

type Mat = [f32; 12];
const IDENTITY: Mat = [1., 0., 0., 0., 1., 0., 0., 0., 1., 0., 0., 0.];

fn parse_transform(s: Option<&str>) -> Mat {
    let Some(s) = s else { return IDENTITY };
    let v: Vec<f32> = s
        .split_whitespace()
        .filter_map(|x| x.parse().ok())
        .collect();
    if v.len() == 12 {
        v.try_into().unwrap()
    } else {
        IDENTITY
    }
}

/// 3MF matrices are row vectors: p' = p * M, with M given as m00 m01 m02 m10
/// ... m32 (the last row is the translation).
fn apply(m: &Mat, p: [f32; 3]) -> [f32; 3] {
    [
        p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9],
        p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
        p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11],
    ]
}

fn mul(a: &Mat, b: &Mat) -> Mat {
    // Apply a, then b.
    let r =
        |i: usize, j: usize| a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    let mut m = [0f32; 12];
    for i in 0..3 {
        for j in 0..3 {
            m[i * 3 + j] = r(i, j);
        }
    }
    let t = apply(b, [a[9], a[10], a[11]]);
    m[9..12].copy_from_slice(&t);
    m
}

/// A tiny attribute reader for one XML start tag.
fn attr<'a>(tag: &'a str, name: &str) -> Option<&'a str> {
    let mut rest = tag;
    while let Some(i) = rest.find(name) {
        let before = rest[..i].chars().last();
        let after = &rest[i + name.len()..];
        let after_trim = after.trim_start();
        if before.is_some_and(|c| c.is_whitespace()) && after_trim.starts_with('=') {
            let v = after_trim[1..].trim_start();
            let q = v.chars().next()?;
            let v = &v[1..];
            let end = v.find(q)?;
            return Some(&v[..end]);
        }
        rest = &rest[i + name.len()..];
    }
    None
}

fn parse_3mf_model(xml: &str) -> Result<Mesh, String> {
    use std::collections::HashMap;
    let scale = match xml.find("<model").and_then(|i| {
        attr(
            &xml[i..xml[i..].find('>').map(|e| i + e).unwrap_or(xml.len())],
            "unit",
        )
    }) {
        Some("micron") => 0.001,
        Some("centimeter") => 10.0,
        Some("inch") => 25.4,
        Some("foot") => 304.8,
        Some("meter") => 1000.0,
        _ => 1.0,
    };
    // Objects: id -> (triangles, components).
    let mut objects: HashMap<String, (Vec<Triangle>, Vec<(String, Mat)>)> = HashMap::new();
    let mut items: Vec<(String, Mat)> = Vec::new();
    let mut cur: Option<String> = None;
    let mut verts: Vec<[f32; 3]> = Vec::new();
    let mut pos = 0;
    while let Some(start) = xml[pos..].find('<') {
        let s = pos + start;
        let Some(end) = xml[s..].find('>') else { break };
        let tag = &xml[s + 1..s + end];
        pos = s + end + 1;
        let tname = tag
            .trim_start_matches('/')
            .split(|c: char| c.is_whitespace() || c == '/')
            .next()
            .unwrap_or("");
        let tname = tname.rsplit(':').next().unwrap_or(tname);
        let closing = tag.starts_with('/');
        match (tname, closing) {
            ("object", false) => {
                cur = attr(tag, "id").map(str::to_string);
                verts.clear();
                if let Some(id) = &cur {
                    objects.entry(id.clone()).or_default();
                }
            }
            ("object", true) => cur = None,
            ("vertex", false) => {
                let g = |n| {
                    attr(tag, n)
                        .and_then(|v| v.parse::<f32>().ok())
                        .unwrap_or(0.0)
                };
                verts.push([g("x"), g("y"), g("z")]);
            }
            ("triangle", false) => {
                let g = |n| attr(tag, n).and_then(|v| v.parse::<usize>().ok());
                if let (Some(a), Some(b), Some(c), Some(id)) = (g("v1"), g("v2"), g("v3"), &cur) {
                    if a < verts.len() && b < verts.len() && c < verts.len() {
                        objects
                            .get_mut(id)
                            .unwrap()
                            .0
                            .push([verts[a], verts[b], verts[c]]);
                    }
                }
            }
            ("component", false) => {
                if let (Some(oid), Some(id)) = (attr(tag, "objectid"), &cur) {
                    let t = parse_transform(attr(tag, "transform"));
                    objects.get_mut(id).unwrap().1.push((oid.to_string(), t));
                }
            }
            ("item", false) => {
                if let Some(oid) = attr(tag, "objectid") {
                    items.push((oid.to_string(), parse_transform(attr(tag, "transform"))));
                }
            }
            _ => {}
        }
    }
    if items.is_empty() {
        items = objects.keys().map(|k| (k.clone(), IDENTITY)).collect();
    }
    let mut tris = Vec::new();
    fn emit(
        id: &str,
        m: &Mat,
        objects: &HashMap<String, (Vec<Triangle>, Vec<(String, Mat)>)>,
        depth: u32,
        out: &mut Vec<Triangle>,
    ) {
        let Some((t, comps)) = objects.get(id) else {
            return;
        };
        for tri in t {
            out.push([apply(m, tri[0]), apply(m, tri[1]), apply(m, tri[2])]);
        }
        if depth < 8 {
            for (cid, cm) in comps {
                emit(cid, &mul(cm, m), objects, depth + 1, out);
            }
        }
    }
    for (id, m) in &items {
        emit(id, m, &objects, 0, &mut tris);
    }
    if tris.is_empty() {
        return Err("3MF: no triangles found".into());
    }
    if scale != 1.0 {
        for t in &mut tris {
            for v in t.iter_mut() {
                for c in v.iter_mut() {
                    *c *= scale;
                }
            }
        }
    }
    Ok(Mesh::new(tris))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn obj_quad_becomes_two_triangles() {
        let s = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1/1 2/2 3/3 4/4\nf -4 -3 -2\n";
        let m = parse_obj(s.as_bytes()).unwrap();
        assert_eq!(m.triangles.len(), 3);
        assert_eq!(
            m.triangles[1],
            [[0.0, 0.0, 0.0], [1.0, 1.0, 0.0], [0.0, 1.0, 0.0]]
        );
    }

    #[test]
    fn three_mf_model_with_transform_and_units() {
        let xml = r#"<?xml version="1.0"?>
<model unit="centimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
 <resources>
  <object id="1" type="model"><mesh>
   <vertices><vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/></vertices>
   <triangles><triangle v1="0" v2="1" v3="2"/></triangles>
  </mesh></object>
  <object id="2"><components><component objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 5"/></components></object>
 </resources>
 <build><item objectid="2" transform="1 0 0 0 1 0 0 0 1 2 0 0"/></build>
</model>"#;
        let m = parse_3mf_model(xml).unwrap();
        assert_eq!(m.triangles.len(), 1);
        // Component lifts z by 5, item moves x by 2, then cm -> mm.
        assert_eq!(m.triangles[0][0], [20.0, 0.0, 50.0]);
        assert_eq!(m.triangles[0][1], [30.0, 0.0, 50.0]);
    }
}
