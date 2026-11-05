//! Slices meshes onto the real Saturn 4 Ultra 16K grid and reads the .goo back.

use goo::{rle, GooFile};
use slicer::{slice_to_goo, stl, Mesh, PrintSettings, SliceError, SATURN_4_ULTRA_16K};

/// A UV sphere with outward winding.
fn sphere(r: f32, rings: usize, segs: usize) -> Mesh {
    let p = |i: usize, j: usize| {
        let th = std::f32::consts::PI * i as f32 / rings as f32;
        let ph = 2.0 * std::f32::consts::PI * j as f32 / segs as f32;
        [
            r * th.sin() * ph.cos(),
            r * th.sin() * ph.sin(),
            r * th.cos(),
        ]
    };
    let mut tris = Vec::new();
    for i in 0..rings {
        for j in 0..segs {
            let (a, b, c, d) = (p(i, j), p(i + 1, j), p(i + 1, j + 1), p(i, j + 1));
            if i != 0 {
                tris.push([a, b, d]);
            }
            if i != rings - 1 {
                tris.push([b, c, d]);
            }
        }
    }
    Mesh::new(tris)
}

fn slice(mesh: &Mesh, printer: &slicer::Printer) -> (slicer::SliceReport, GooFile) {
    let mut out = Vec::new();
    let r = slice_to_goo(
        mesh,
        printer,
        &PrintSettings::default(),
        "test",
        "2026-11-05 00:00:00",
        &mut out,
        4,
    )
    .unwrap();
    (r, GooFile::read(out.as_slice()).unwrap())
}

/// Bounding box of lit pixels in a decoded layer: (x0, x1, y0, y1), half-open.
fn lit_bbox(data: &[u8], w: u64) -> (u64, u64, u64, u64) {
    let (mut x0, mut x1, mut y0, mut y1) = (u64::MAX, 0, u64::MAX, 0);
    let mut pos = 0u64;
    rle::decode_runs(data, |v, len| {
        let mut p = pos;
        while v != 0 && p < pos + len {
            // Split the run at row ends.
            let (y, x) = (p / w, p % w);
            let end = (pos + len).min((y + 1) * w);
            x0 = x0.min(x);
            x1 = x1.max(x + (end - p));
            y0 = y0.min(y);
            y1 = y1.max(y + 1);
            p = end;
        }
        pos += len;
    })
    .unwrap();
    (x0, x1, y0, y1)
}

#[test]
fn cube_slices_to_exact_size_and_layer_count() {
    let p = SATURN_4_ULTRA_16K;
    let mut m = Mesh::cuboid([0.0, 0.0, 0.0], [10.0, 10.0, 2.0]);
    m.place_on_plate([0.0, 0.0]);
    let (r, goo) = slice(&m, &p);
    assert_eq!(r.layer_count, 40);
    assert_eq!(goo.layers.len(), 40);
    assert!(r.empty_layers.is_empty());
    assert!(
        (r.sliced_volume_mm3 - 200.0).abs() / 200.0 < 0.01,
        "{}",
        r.sliced_volume_mm3
    );
    let (x0, x1, y0, y1) = lit_bbox(&goo.layers[10].1, p.resolution_x as u64);
    let (mx, my) = p.pixel_mm();
    assert!(((x1 - x0) as f64 * mx - 10.0).abs() <= mx);
    assert!(((y1 - y0) as f64 * my - 10.0).abs() <= my);
    // Layer 0 uses bottom exposure, later layers the normal one.
    assert_eq!(goo.layers[0].0.exposure_time, 30.0);
    assert_eq!(goo.layers[39].0.exposure_time, 2.5);
    assert!((goo.layers[39].0.position_z - 2.0).abs() < 1e-5);
}

#[test]
fn sphere_volume_matches_mesh() {
    let mut m = sphere(8.0, 48, 96);
    m.place_on_plate([30.0, -20.0]);
    let (r, goo) = slice(&m, &SATURN_4_ULTRA_16K);
    assert_eq!(goo.layers.len(), 320);
    let err = (r.sliced_volume_mm3 - r.mesh_volume_mm3).abs() / r.mesh_volume_mm3;
    assert!(
        err < 0.01,
        "sliced {} vs mesh {}",
        r.sliced_volume_mm3,
        r.mesh_volume_mm3
    );
    assert!(r.empty_layers.is_empty());
}

#[test]
fn mirror_x_flips_columns() {
    let mut p = SATURN_4_ULTRA_16K;
    let mut m = Mesh::cuboid([0.0, 0.0, 0.0], [5.0, 5.0, 0.1]);
    m.place_on_plate([50.0, 0.0]); // right of centre
    let w = p.resolution_x as u64;

    p.mirror_x = false;
    let (_, plain) = slice(&m, &p);
    let (x0, _, _, _) = lit_bbox(&plain.layers[0].1, w);
    assert!(x0 > w / 2, "unmirrored model should be on the right");

    p.mirror_x = true;
    let (_, mirrored) = slice(&m, &p);
    let (_, x1, _, _) = lit_bbox(&mirrored.layers[0].1, w);
    assert!(x1 < w / 2, "mirrored model should be on the left");
    assert!(mirrored.header.mirror_x);
}

#[test]
fn stl_round_trip_through_slicer() {
    let mut m = stl::parse(&stl::to_binary(&Mesh::cuboid(
        [0.0, 0.0, 0.0],
        [4.0, 4.0, 1.0],
    )))
    .unwrap();
    m.place_on_plate([0.0, 0.0]);
    let (r, _) = slice(&m, &SATURN_4_ULTRA_16K);
    assert_eq!(r.layer_count, 20);
}

#[test]
fn rejects_model_bigger_than_plate() {
    let mut m = Mesh::cuboid([0.0, 0.0, 0.0], [150.0, 10.0, 1.0]);
    m.place_on_plate([0.0, 0.0]);
    m.translate([50.0, 0.0, 0.0]);
    let err = slice_to_goo(
        &m,
        &SATURN_4_ULTRA_16K,
        &PrintSettings::default(),
        "t",
        "t",
        Vec::new(),
        1,
    )
    .unwrap_err();
    assert!(matches!(err, SliceError::DoesNotFit(_)), "{err}");
}

#[test]
fn single_thread_and_multi_thread_agree() {
    let mut m = sphere(5.0, 24, 48);
    m.place_on_plate([0.0, 0.0]);
    let run = |threads| {
        let mut out = Vec::new();
        slice_to_goo(
            &m,
            &SATURN_4_ULTRA_16K,
            &PrintSettings::default(),
            "t",
            "t",
            &mut out,
            threads,
        )
        .unwrap();
        out
    };
    assert_eq!(run(1), run(8));
}

#[test]
fn checks_find_a_cup_and_an_island_on_the_real_grid() {
    use slicer::checks::{CheckParams, IssueKind};
    use slicer::{slice_to_goo_with, SliceHooks};
    // A 20 x 20 mm cup: 1 mm floor on the plate, 2 mm walls, 6 mm tall.
    let mut m = Mesh::cuboid([-10.0, -10.0, 0.0], [10.0, 10.0, 1.0]);
    m.append(Mesh::cuboid([-10.0, -10.0, 1.0], [10.0, -8.0, 6.0]));
    m.append(Mesh::cuboid([-10.0, 8.0, 1.0], [10.0, 10.0, 6.0]));
    m.append(Mesh::cuboid([-10.0, -8.0, 1.0], [-8.0, 8.0, 6.0]));
    m.append(Mesh::cuboid([8.0, -8.0, 1.0], [10.0, 8.0, 6.0]));
    // A floating 3 x 3 mm block starting at Z = 3 mm, away from the cup.
    m.append(Mesh::cuboid([40.0, 0.0, 3.0], [43.0, 3.0, 4.0]));
    let mut out = Vec::new();
    let progress_calls = std::sync::atomic::AtomicU32::new(0);
    let progress = |done: u32, total: u32| {
        assert!(done <= total);
        progress_calls.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    };
    let r = slice_to_goo_with(
        &m,
        &SATURN_4_ULTRA_16K,
        &PrintSettings::default(),
        "t",
        "2026-11-05 00:00:00",
        &mut out,
        4,
        &SliceHooks {
            progress: Some(&progress),
            cancel: None,
            checks: Some(CheckParams::default()),
        },
    )
    .unwrap();
    assert!(progress_calls.into_inner() > 0);
    let cups: Vec<_> = r
        .issues
        .iter()
        .filter(|i| i.kind == IssueKind::SuctionCup)
        .collect();
    assert_eq!(cups.len(), 1, "{:?}", r.issues);
    // Inner hollow is 16 x 16 mm = 256 mm^2, give or take a cell.
    assert!((cups[0].size - 256.0).abs() < 20.0, "{:?}", cups[0]);
    assert_eq!(cups[0].first_layer, 20); // first layer above the 1 mm floor
    let c = cups[0].area.center();
    assert!(c[0].abs() < 0.5 && c[1].abs() < 0.5, "{c:?}");
    let islands: Vec<_> = r
        .issues
        .iter()
        .filter(|i| i.kind == IssueKind::Island)
        .collect();
    assert_eq!(islands.len(), 1, "{:?}", r.issues);
    assert_eq!(islands[0].first_layer, 60);
    let c = islands[0].area.center();
    assert!(
        (c[0] - 41.5).abs() < 0.5 && (c[1] - 1.5).abs() < 0.5,
        "{c:?}"
    );
}

#[test]
fn cancelling_stops_the_slice() {
    use slicer::{slice_to_goo_with, SliceHooks};
    let m = Mesh::cuboid([-5.0, -5.0, 0.0], [5.0, 5.0, 5.0]);
    let cancel = std::sync::atomic::AtomicBool::new(true);
    let r = slice_to_goo_with(
        &m,
        &SATURN_4_ULTRA_16K,
        &PrintSettings::default(),
        "t",
        "2026-11-05 00:00:00",
        Vec::new(),
        2,
        &SliceHooks {
            cancel: Some(&cancel),
            ..Default::default()
        },
    );
    assert!(matches!(r, Err(SliceError::Cancelled)));
}

/// Slices with checks on and returns the islands found.
fn islands_after_supports(
    model: &Mesh,
    p: &slicer::supports::SupportParams,
) -> Vec<slicer::checks::Issue> {
    use slicer::checks::{CheckParams, IssueKind};
    use slicer::{slice_to_goo_with, SliceHooks};
    let mut placed = model.clone();
    placed.place_on_plate([0.0, 0.0]);
    let s = slicer::supports::generate(&placed, p).unwrap();
    let r = slice_to_goo_with(
        &s.combined(),
        &SATURN_4_ULTRA_16K,
        &PrintSettings::default(),
        "t",
        "2026-11-05 00:00:00",
        Vec::new(),
        4,
        &SliceHooks {
            checks: Some(CheckParams::default()),
            ..Default::default()
        },
    )
    .unwrap();
    r.issues
        .into_iter()
        .filter(|i| i.kind == IssueKind::Island)
        .collect()
}

#[test]
fn supported_sphere_has_no_islands_and_unsupported_one_does() {
    let ball = sphere(8.0, 48, 96);
    let p = slicer::supports::SupportParams::default();
    let islands = islands_after_supports(&ball, &p);
    assert!(islands.is_empty(), "{islands:?}");
    // Without supports a lifted sphere's first layers float.
    let mut lifted = ball.clone();
    lifted.place_on_plate([0.0, 0.0]);
    lifted.translate([0.0, 0.0, 5.0]);
    let (r, _) = slice(&lifted, &SATURN_4_ULTRA_16K);
    assert!(!r.empty_layers.is_empty());
}

#[test]
fn supported_table_has_no_islands() {
    let mut m = Mesh::cuboid([-10.0, -10.0, 10.0], [10.0, 10.0, 12.0]);
    for (x, y) in [(-10.0, -10.0), (8.0, -10.0), (-10.0, 8.0), (8.0, 8.0)] {
        m.append(Mesh::cuboid([x, y, 0.0], [x + 2.0, y + 2.0, 10.0]));
    }
    let islands = islands_after_supports(&m, &Default::default());
    assert!(islands.is_empty(), "{islands:?}");
}
