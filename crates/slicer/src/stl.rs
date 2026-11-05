//! STL loading (binary and ASCII). Units are assumed to be millimetres.

use std::fmt;

use crate::mesh::{Mesh, Triangle};

#[derive(Debug, PartialEq)]
pub struct StlError(pub String);

impl fmt::Display for StlError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "STL: {}", self.0)
    }
}

impl std::error::Error for StlError {}

/// Parses an STL file from memory, detecting binary or ASCII.
///
/// Binary STLs may also start with "solid" in their 80-byte header, so the
/// binary size check wins whenever it matches exactly.
pub fn parse(data: &[u8]) -> Result<Mesh, StlError> {
    if data.len() >= 84 {
        let n = u32::from_le_bytes([data[80], data[81], data[82], data[83]]) as usize;
        if data.len() == 84 + n * 50 {
            return Ok(parse_binary(data, n));
        }
    }
    let text = std::str::from_utf8(data).map_err(|_| {
        StlError("not a valid binary STL (size doesn't match triangle count) and not ASCII".into())
    })?;
    if !text.trim_start().starts_with("solid") {
        return Err(StlError(
            "unrecognised file: no binary layout and no `solid` keyword".into(),
        ));
    }
    parse_ascii(text)
}

fn parse_binary(data: &[u8], n: usize) -> Mesh {
    let f = |o: usize| f32::from_le_bytes([data[o], data[o + 1], data[o + 2], data[o + 3]]);
    let triangles = (0..n)
        .map(|i| {
            let base = 84 + i * 50 + 12; // skip the stored normal
            let v = |k: usize| [f(base + k * 12), f(base + k * 12 + 4), f(base + k * 12 + 8)];
            [v(0), v(1), v(2)]
        })
        .collect();
    Mesh::new(triangles)
}

fn parse_ascii(text: &str) -> Result<Mesh, StlError> {
    let mut triangles: Vec<Triangle> = Vec::new();
    let mut verts = Vec::with_capacity(3);
    for (line_no, line) in text.lines().enumerate() {
        let mut words = line.split_whitespace();
        match words.next() {
            Some("vertex") => {
                let mut v = [0f32; 3];
                for c in v.iter_mut() {
                    *c = words
                        .next()
                        .and_then(|w| w.parse().ok())
                        .ok_or_else(|| StlError(format!("line {}: bad vertex", line_no + 1)))?;
                }
                verts.push(v);
            }
            Some("endloop") => {
                if verts.len() != 3 {
                    return Err(StlError(format!(
                        "line {}: facet has {} vertices, expected 3",
                        line_no + 1,
                        verts.len()
                    )));
                }
                triangles.push([verts[0], verts[1], verts[2]]);
                verts.clear();
            }
            _ => {}
        }
    }
    if triangles.is_empty() {
        return Err(StlError("no triangles found".into()));
    }
    Ok(Mesh::new(triangles))
}

/// Writes a binary STL (used by tests and for exporting placed models).
pub fn to_binary(mesh: &Mesh) -> Vec<u8> {
    let mut out = vec![0u8; 80];
    out.extend_from_slice(&(mesh.triangles.len() as u32).to_le_bytes());
    for t in &mesh.triangles {
        out.extend_from_slice(&[0u8; 12]);
        for v in t {
            for c in v {
                out.extend_from_slice(&c.to_le_bytes());
            }
        }
        out.extend_from_slice(&[0u8; 2]);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn binary_round_trip() {
        let m = Mesh::cuboid([0.0, 0.0, 0.0], [1.0, 2.0, 3.0]);
        assert_eq!(parse(&to_binary(&m)).unwrap(), m);
    }

    #[test]
    fn binary_with_solid_header_is_still_binary() {
        let m = Mesh::cuboid([0.0, 0.0, 0.0], [1.0, 1.0, 1.0]);
        let mut b = to_binary(&m);
        b[..5].copy_from_slice(b"solid");
        assert_eq!(parse(&b).unwrap(), m);
    }

    #[test]
    fn ascii() {
        let s = "solid t\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0.5\n  endloop\n endfacet\nendsolid t\n";
        let m = parse(s.as_bytes()).unwrap();
        assert_eq!(
            m.triangles,
            vec![[[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.5]]]
        );
    }

    #[test]
    fn rejects_garbage() {
        assert!(parse(b"hello world").is_err());
    }
}
