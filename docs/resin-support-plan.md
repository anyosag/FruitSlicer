# FruitSlicer — resin support generator plan (Elegoo Saturn 4 Ultra 16K)

## Context
`anyosag/FruitSlicer` is empty (one-line README), so this is a greenfield project. The user asked for a support generator that:
- **(A)** leaves good-looking surfaces and produces tidy supports,
- **(B)** is easy to remove,
- **(C)** uses the fewest supports per surface.

The target is **resin MSLA**, not FDM (the user corrected this mid-session).

- **Printer:** Elegoo Saturn 4 Ultra 16K, the only Elegoo printer with "16K" in its name.
  - 15120×6230 px with **14×19 µm non-square pixels**.
  - Build volume 211.68×118.37×220 mm; layers 0.01–0.2 mm.
  - **Tilt release**, 30 °C heated vat, PFA film.
  - Official slicers: Elegoo SatelLite (`.goo`) and Chitubox.
- **User decisions:** C++ and Clipper2, plus a minimal end-to-end pipeline to printable output.

**How the goals are reframed** (agreed critique of the literal ask):
- **(C) is a constrained minimisation.** Find the fewest contacts such that three things hold:
  1. every downward-facing sample is covered through material;
  2. every connected part, at every layer, has enough tip *capacity* for its peel load;
  3. every island is anchored.

  Pure coverage is not enough. Resin parts fail when the peel and suction force on a layer exceeds the tips' capacity, and that peaks at large-area layers and on the first layers of flat faces.
- **(A) is mostly about scars and where they land**: tip size and shape, avoiding cosmetic faces and edges, and orientation. Orientation is the biggest single lever on A, B and C (Formlabs, Chitubox and Anycubic guides; Vanek, Zhang).
- **A trades against C through tip size.** Smaller tips leave smaller scars but need more contacts (Formlabs: "raise density to compensate"). The model therefore couples tip class with capacity and coverage.

## Evidence base (read by research agents; key URLs)
| Topic | Finding |
|---|---|
| PrusaSlicer SLA (AGPL-3.0: re-implement, don't copy) | **v2.9.2 point generator:** deterministic and bottom-up; influence radius grows with height above the point (3.2 mm at 0, 4.0 at 3.9 mm, 5.0 at 15 mm, 6.0 at 40 mm); islands placed by Voronoi skeleton; tiny islands ≤ 0.4 mm are erased. **v2.6 generator:** force model, about 1 tip per 11.1 mm² at density 1, island demand ×(1 + aspect/2). **Pinhead:** Ø0.4 front, 0.2 mm penetration, 1.0 mm width, pillar Ø1.0, ≤ 45° tilt. **Default tree:** ≤ 3 heads per pillar, bridges ≤ 15 mm at 45°, zig-zag bracing, reversed-pinhead anchors, 5 mm elevation, pad. `raw.githubusercontent.com/prusa3d/PrusaSlicer/version_2.9.2/src/libslic3r/SLA/SupportIslands/SampleConfigFactory.cpp` |
| Formlabs PreForm | Categories: minima / overhang / stabilization / reinforcement. Larger touchpoints on minima, smaller on edges. **Light-touch:** no cross-links near the part, so the tip flexes and breaks at the surface. Mini rafts; ≥ 45° self-supporting in rigid resins. formlabs.com/global/support/Advanced-Support-Structure-Settings/ and /Tearaway-Supports/ |
| Lychee 7.5 | Islands classed by first-layer area. Reinforcement based on volume above an island until it merges. Overhang interval varies linearly with angle. Edge offset. Auto-parent/optimise. **Auto-regeneration keeps manual supports.** One example: 470 islands detected, printed with 346 tips. lychee.co/changelogs/lychee-slicer-7-5-0 |
| Chitubox, Photon Workshop | Density expressed as tip spacing, plus a separate pillar spacing. Break points, MST bracing, raft slope, modes +All/+Platform/+Overhang. docs.chitubox.com …/configure-support-parameters |
| UVtools (AGPL-3.0) | Pixel-based lints for islands (<25% overlap with the layer below at brightness ≥ 150), overhangs, resin traps and suction cups. *Use it as an external checker.* |
| Peel physics | Pan 2017: F ∝ μV·R⁴/h³, scales with area/perimeter, centre-peaked pressure, drifts up with height. Wu 2016: tilt release ≈ 20% below pull-up, **stress highest far from the hinge**, sizing rule n·A_tip·σ_b ≥ S·T₀·A. Flexible films cut force by more than 48% (abstract). eprints.whiterose.ac.uk/109861/ |
| Contacts and cure | Grayscale or slight under-cure at contacts weakens them and improves finish (Liao 2021; Artemeva 2024, abstract). Jacobs working curve, with D_p and E_c varying up to 10× by resin. **No published data on MSLA lateral over-cure, or on cone vs sphere vs neck tip shapes vs scar size**, so these are calibrated. |
| Output format | Elegoo GOO spec V1.2 (GPL-3.0 PDF; RLE, big-endian, no tilt fields). UVtools also implements **GOO V5**. *Which version the 16K needs is UNVERIFIED.* |

## Design

### 1. Pipeline and output
1. Load an STL (3MF later).
2. Apply user rotation and elevation (default 5 mm).
3. Slice every shell with a **nonzero union** (Clipper2). Multi-shell, self-intersecting minis must not produce fake holes or fake islands.
4. Build the geometry front end (§2), then points (§3), heads (§4), pillars, bracing and raft (§5).
5. **MVP output:** a supported model as STL or 3MF, written streaming, then sliced in **SatelLite or Chitubox**.
   - **Week-1 blocker test:** an overlap coupon (tip spheres penetrating a cube) checks whether those slicers fill overlapping shells correctly.
   - If they don't, export as separate 3MF bodies or do a real union with the Manifold library.
   - Tessellate to ≤ 7 µm chord error (half a pixel): about 9 segments for a 0.2 mm tip, 19 for a 1 mm shaft.
6. **Assert raft thickness ≥ (bottom + transition layers) × layer height,** so the long bottom exposures never land on tips.
7. **Native rasterisation and `.goo` are optional and late** (M6). The main payoff is grayscale contact weakening. Pin the version first by inspecting a real SatelLite 16K file.

### 2. Geometry front end
- **Minima from the mesh, not the slices.** A vertex with no lower neighbours is a minimum; flat plateaus are merged. This is independent of layer height and offset. Contacts are ray-projected onto the true surface.
- **IslandGraph per layer:** parts linked when they overlap the parent part grown by the cure width, *or* when they are connected in the mesh. Thin shallow features (a 0.1 mm wire at 20°) then become overhangs, not one island per layer.
- **Local thickness:** an inward ray along the normal, using the AABB tree.
- **Cavity detection:** closed or drained internal voids. Their ceilings must be self-supporting, or the user is warned. Never place model anchors inside a cavity.

### 3. Point generator: capacitated, material-aware cover (`PointGenerator`, `CoverageModel`, `CapacityModel`)
**Samples (demand)**
- Mesh minima.
- New-overhang edges, about every 1 mm, plus interior samples.
- Peninsulas that reach more than 2 mm beyond their support.
- Island first layers.
- Near-vertical walls of thin features that grow downward (spears and limbs): **stabilization runs in M1, not later.**

**Coverage constraint**
- Each sample needs ≥ 1 tip within r(min(Δz, local thickness), tip class).
- The tip must sit on an **ancestor part** in the IslandGraph, and distance is measured **through material** (geodesic on the layer polygons). A point can no longer cover across a slot or onto a neighbouring finger.
- **Seed for r:** the Prusa curve. It is **re-calibrated from scratch** for this printer, with no extra tilt factor stacked on top.

**Capacity constraint**
- For each connected component C at each layer L: Σ c(tip class) over the tips in C's ancestry ≥ A(C, L) · k_peel(position relative to the hinge) + suction term (when C is flagged as a cup).
- **Seed:** a 0.4 mm tip ≈ 11.1 mm² of layer area (Prusa v2.6), scaled by tip area (Wu).
- The peel-axis vector and hinge side live in `PrinterProfile`. **Measure them on the real printer.**

**Island minimums**
- Size classes follow Prusa v2.9.2:

  | Island length | Tips |
  |---|---|
  | ≤ ~1.7 mm | 1 |
  | ≤ ~6.5 mm | 2 |
  | larger | skeleton plus outline |

  Seeded for a 0.4 mm head; then calibrated.
- Islands that merge with existing material within the cure width over a few layers are skipped.
- An island below the printable size is **warned about, never silently dropped.**

**Optimiser**
1. Run deterministic weighted greedy for coverage. Score = (newly covered samples + capacity deficit reduced) / cost.
   - cost = 1 + w_cos·cosmetic + w_edge·edge proximity.
   - Cosmetic input: a JSON keep-out list of faces or regions, or "avoid faces visible from direction v".
   - Edge proximity: within tip radius + margin of a sharp edge, mirroring Lychee's edge offset.
2. Upgrade tip class where capacity is short and space is limited (larger on minima, smaller near edges).
3. Run reverse-delete pruning, highest cost first. Every deletion re-checks coverage, capacity and island minimums.
4. **No Lloyd relaxation.** It breaks coverage after pruning and drags points off minima.

**Warnings**
- Peak single-layer area.
- Large horizontal first-layer faces: the green 0.05 mm skin is the worst case. Suggest a tilt.
- Suction cups.
- Unreachable cavity ceilings.

**Settings exposed:** density (light / normal / heavy), tip class, critical angle, elevation, build-plate only, cosmetic keep-outs, removal timing (green or post-cure, which biases tip class).

### 4. Heads (scars and removal)
- **Pinhead:** a contact sphere of Ø d_tip, a cone to the shaft, and penetration p.
  - **Starting values to calibrate:** tip classes 0.2 / 0.3 / 0.4 / 0.6 mm; p = 0.1–0.2 mm.
  - Head length ≥ 1 mm, so flush cutters can reach the neck.
  - No vendor publishes defaults; Prusa uses 0.4 / 0.2 / 1.0.
- **Direction:** the surface normal, clamped to ≤ 45° from straight down.
  - Collision test: a ray fan against the mesh AABB.
  - On collision, a deterministic search over tilt, azimuth and length, falling back to a smaller back radius.
- **Flex zone:** no bracing or merging within ~2 mm of the contact (Formlabs light-touch).

### 5. Pillars, bracing, raft
- **Phase 1 (M2):**
  - Mostly vertical pillars, clustering ≤ 3 heads per pillar with ≤ 45° bridges.
  - Simple cross-bracing on tall pillars (≥ 1 link above 15 mm, ≥ 2 above 35 mm).
  - Raft options: none, mini, or full with a chamfer for the scraper. Plus a **flat-on-plate, no supports** mode for flat-backed parts.
- **Phase 2 (M5), for a tidier look and fewer shafts:**
  - Branching merges where 45° cones meet, with radius growing with load.
  - Model anchors (reversed small pinhead) only under a weight limit, and reported.
  - Zig-zag bracing.
  - **No-trap rule:** reject any support cross-section that forms a ring around part geometry.

### 6. Orientation search (M3, pulled forward)
- Run the generator over about 50 orientations, including tilts of 10–45°.
- Rank them by contacts, contacts on cosmetic faces, peak layer area, area change between layers, height, and large sections near the hinge (Wu).
- Output the top 3, each with its report.

### 7. Validation
**Polygon lints, run in-house in CI**
- zero unsupported islands;
- coverage through material;
- capacity per component and layer;
- tip angle ≤ 45°;
- no penetration beyond p;
- every pillar ends on the raft or on a counted anchor;
- raft thickness ≥ bottom layers.

**External check:** run UVtools on SatelLite's `.goo` for islands, suction cups and resin traps on the real rasterisation. This invokes the tool; it copies no AGPL code.

**Determinism:** hash points quantised to 1 µm, build with `-ffp-contract=off`, and use fixed-order reductions.

### 8. Manual control (resin-slicer model, CLI first)
- `--points-out points.json` writes stable IDs, category, tip class, position, normal and a lock flag.
- `--points-in edits.json` adds, deletes, moves or resizes points.
- **Manual and locked points survive regeneration.** Auto points re-flow around them, as in Lychee.

## Code layout (C++20, CMake + FetchContent)
**Dependencies:** Clipper2 (Boost licence, verified), nanoflann, Catch2, CLI11, nlohmann/json, stb_image_write, and a small BVH (own, or madmann91/bvh).
- **Dev only:** HiGHS, to solve small models exactly as an integer program and measure the gap between greedy and optimal.
- **Fallback:** Manifold, for mesh union.
- Licences other than Clipper2's were noted from memory or by the reviewer. Verify each when integrating.

```
src/geometry/  Vec, BBox, Polygon (Clipper2 Paths64, µm fixed point), Transform
src/mesh/      TriangleMesh, StlIO, AabbTree, MeshSlicer (nonzero union), MeshAnalysis (minima, thickness, sharp edges, cavities)
src/layers/    LayerStack, IslandGraph (cure-width + mesh-connectivity linking, peninsulas)
src/support/   SupportConfig, SupportPoint, CoverageModel, CapacityModel, CosmeticField, PointGenerator,
               HeadPlacer, PillarRouter (M2) → TreeRouter (M5), Bracing, Raft, SupportGeometry (primitives,
               tessellation), SupportValidator, SupportMetrics, ManualEdits, OrientationSearch (M3)
src/printer/   PrinterProfile (16K constants, peel axis/hinge side, bottom/transition layers)
src/formats/   StlWriter/ThreeMfWriter (streaming); GooWriter + raster/ (M6, optional)
src/app/       fruitslicer CLI: support | orient | calibrate | lint
tests/         unit/, models/ (procedural generators), golden/ (metric snapshots), ilp/ (HiGHS gap tests)
```

## Milestones and exit criteria
- **M0 — Foundation:** CMake and dependencies, STL I/O, AABB tree, nonzero slicer, mesh minima and thickness, IslandGraph, PrinterProfile, PNG debug dumps.
  - *Exit:* slice areas within 0.5% of analytic values on procedural solids; the IslandGraph is correct on the suite; **the week-1 overlap coupon has decided the export strategy.**
- **M0.5 — Calibration coupons** (primitives + raft + export only), about 60 coupons per plate at randomised positions:
  - `tips`: diameter × penetration × **load (supported area)**, giving capacity c(tip class);
  - `spacing`: plates at 0–60° plus unsupported wedges, giving r(Δz) and the self-supporting angle;
  - `islands`: size × count, giving island minimums.
  
  Tables are keyed by resin, layer height, exposure, vat/ambient temperature and plate position. Sweep each variable until failures appear, fit a logistic curve, and operate at about 70% of the failure threshold. Count failed tips from residue left on the film.
  - *Exit:* filled tables for the user's resin.
- **M1 — Point generator** (§2–3, including stabilization and warnings), points JSON, PLY output coloured by category.
  - *Exit:* all invariants pass, and the greedy/optimal gap is ≤ 15% on small models (HiGHS).
- **M2 — Heads, phase-1 pillars, raft and export, then the GATE.**
  - **Setup:** FruitSlicer and SatelLite/Chitubox auto supports (and Lychee, if available) on the **same plate**, at matched tip diameter. The coupon is the unit of trial (zero failures in 60 means a failure rate ≤ 5% at 95% confidence).
  - **Scars:** judged by blind ranking under raking light at a fixed magnification.
  - **Go:** at a matched failure rate, ≥ 25% fewer contacts *or* measurably smaller scars.
  - **No-go:** stop optimiser work and ship the calibrated-density generator, keeping the capacity and coverage model.
- **M3 — Orientation search** (§6).
  - *Exit:* on the benchmark set, the top-ranked orientation beats the user's manual orientation on contacts or cosmetic hits.
- **M4 — Manual edits that persist, metrics report, polygon lints, UVtools external check.**
  - *Exit:* edit round-trip tests and golden reports in CI.
- **M5 — Phase-2 routing** (§5).
  - *Exit:* fewer shafts and less resin than M2 with invariants intact, and a passing plate.
- **M6 (optional) — Native output:** inspect a real 16K `.goo`, rasterise analytically with coverage anti-aliasing and anisotropic pixels, streaming one layer at a time; GOO writer; grayscale contact weakening.
  - *Exit:* UVtools opens the file with layers and areas matching SatelLite's output, and it prints.

## Verification
- **Procedural test models:**
  - flat plate; plates tilted 15–60°
  - downward cone; hemisphere
  - mushroom (island over a stem)
  - floating islands of 0.2–5 mm
  - slot and fingers (coverage through gaps)
  - thin cloak or shell (thickness-aware r)
  - spear (stabilization)
  - 0.1 mm wire at 20° (island linking)
  - cup (suction)
  - closed and drained cavities
  - ring around a pin (trapping)
  - overlapping multi-shell model (nonzero union)
- **CI invariants:** the §7 lints, plus determinism hash and golden metrics. Metrics are: contacts per cm² of downward area at the same angle threshold, contacts on cosmetic regions, shafts, anchors, contact area, resin volume and the greedy/optimal gap.
- **End to end:**
  `fruitslicer support model.stl --rotate … -o supported.3mf --points-out p.json --report r.json`
  then slice in SatelLite, check with UVtools, and print a same-plate A/B on the 16K.

## Open items for the user
- ~~**FruitSlicer licence**~~: decided: AGPL-3.0-or-later.
- **Measure on the printer:** film type, hinge side and peel axis; whether the 16K reads `.ctb` (UNVERIFIED); the GOO version.
- **Physical testing:** the plan assumes the user can print calibration plates on their own 16K with their usual resin.


## Implementation status (updated as milestones land)

| Milestone | State | Notes |
|---|---|---|
| M0 foundation | done | Nonzero slicer chained by mesh edges; enclosed inverted shells kept as cavities; 2M-triangle sphere sliced at 0.05 mm in ~9 s. |
| M0.5 calibration plates | done | `fruitslicer calibrate tips / spacing / islands`, manifests + result CSVs. **Not printed yet.** All seeds below remain unverified until they are. |
| M1 point generator | done | Coverage through material + capacity + island minimums, greedy + prune, cosmetic/edge cost, persistent manual edits. |
| M2 support tree | phase 1 done | Collision-aware heads, pillars with 45° bridges, diagonal ground routes, bracing outside the flex zone, one-piece pad raft, floating-island validation. |
| M3 orientation | done | Two-stage search; −15 % / −50 % points on the armadillo / bunny vs their input orientation (the bunny with its scan holes repaired; −17 % before mesh repair existed). |
| M5 branching (part) | done | Pillars merge into trunks at 45° below the flex zone; armadillo: 82 → 47 trunks, 189 → 35 braces, 4.4 → 3.9 ml. Model anchors with a weight limit and the no-trap rule are still open. |
| Calibration loop | done | `fit` turns result sheets into a resin profile (logistic capacity, coverage radius, critical angle, island sizes); `--profile` everywhere. |
| Safety gaps | done | Hollow analysis (cups / traps, polygon version of UVtools' two passes), no tips on closed-cavity ceilings, no-trap bracing rule, `--hinge` / `--peel-bias`, `--on-plate`. |
| A/B plate | done | `abplate`: shuffled FruitSlicer vs vendor-supported copies on one plate, manifest + results sheet. |
| M6 native `.goo` (part) | done | Exact-coverage rasterizer on 14×19 µm pixels; GOO V3 writer/reader per Elegoo's spec + UVtools; `goo`, `goo-info --decode`, `goo-compare`, `coupon orientation`; macOS in CI. Checked against a SatelLite 1.0.2.43 16K export: same V3.0 layout; its layers decode; with `--settings-from` only the software name differs. Defaults now follow it: AA off, mirror Y, 5 s wait after retract, its transition ramp. Image orientation still to be confirmed with the F coupon. |
| M4 / grey-level tips | open | UVtools as external lint; native `.goo` with grayscale contact weakening (blocked on inspecting a real 16K file). |

**Corrections and findings found during implementation:**

- **Thin features and island linking.** The review claimed a 0.1 mm wire at 20° never overlaps between 0.05 mm layers. That is wrong. A feature of thickness d at angle θ from horizontal stops overlapping only once d < h·cos θ, so this matters for features thinner than about one layer height, or for thick layers. The IslandGraph's parent growth still handles those cases.
- **Greedy vs. first-fit.** On real meshes the weighted greedy cover saves only 1–8 % of points over the PrusaSlicer-style first-fit rule. Island handling, the coverage radius and orientation move the count far more, as the review predicted.
- **Next-layer absorption (an assumption).** Islands up to 0.12 mm² that join the model on the next layer are left unsupported, on the assumption that the flake left on the film is cured into that layer. Islands under 0.03 mm² that merge within 3 layers are also left unsupported. This rule needs checking with the islands plate.
- **Capacity tips on any underside.** Capacity tips may go on self-supporting undersides too. Without this, a steep downward cone hanging from one apex tip had nowhere to add capacity.
- **Stabilization tips are never pruned.** Their purpose, resisting lateral wobble, is not captured by the coverage and capacity checks.
- **CI.** GitHub Actions (`build-test`: configure, build, ctest, CLI smoke test) runs on every push to the PR and has passed.
- **Where traps can form.** A ring in a single horizontal layer cannot form with the current support topology (MST bracing, tree merges). The real trap is a vertical loop: two pillars plus a brace plus the raft, with model material passing through the loop. The rule now checks for that.
- **Standing on the model at any height (2026-11-12).** The 10 mm limit on a
  support standing on the model left 106 of 462 planned tips without a support
  on a lid stood on its edge, and 106 of 318 on a cup: a ceiling whose floor is
  further down than that had no way to be held. The limit is now the printer's
  height. The foot is as wide as the tip it holds and sunk as deep (it was a
  0.35 mm contact whatever the tip), the shaft keeps its gap to the model, it
  never lands on a face steeper than 60°, and tall ones are braced with each
  other. Both parts now come out with every tip supported. Not printed.
  Such a tip ties two places on the print together: it holds a fresh ceiling
  up but adds nothing to what holds the print on the raft. The plan still
  counts it while placing tips; after the tree, `layersShortOfRaftHold` joins
  what each one ties and reports the layers the raft's own tips can't hold.
- **Feet cut into the model (2026-11-12, from a print file of the user's).**
  A pillar's shaft was tested against the model, its foot was not: the foot
  flares to 3 mm at the bottom, so a pillar within 1.65 mm of a wall had its
  foot inside the wall. On two parts laid flat on the plate (rings with
  mounting tabs), every pillar beside a tab was fused to it over the first
  millimetre, by up to 1.0 mm. The shaft test itself used four rays, which a
  wall can pass between: one shaft stood 0.02 mm from a wall. Now the shaft
  is measured along its length, and the foot is as wide as the room allows
  (`Pillar::base_diameter`, `TreeConfig::foot_clearance`). On those parts the
  nearest foot is 0.50 mm from the model and no tip is lost.
  A tip so low that its head stands on the plate itself now follows the same
  rule sideways (`settlePlateHead`): it leans away from a wall beside it or
  narrows its base, and is reported when it still stands closer than 0.5 mm.
  And on the plate nothing in what the bottom layers print of the model gets
  a tip at all (`SupportConfig::plate_hold_height`, which the app sets from
  the job, `modelInBottomLayers`: its bottom layers, less the height
  correction's layers, times its layer height. It was first a fixed 0.4 mm,
  the default bottom plus transition layers, until the user asked what it
  was tied to; then the bottom layers' whole height, which counted the
  correction's copies under the model as if they were the model's own
  layers: with the defaults, four bottom layers of 0.05 mm of which two are
  the correction's, that kept tips out of 0.2 mm when only 0.1 mm of the
  model has the long exposure). Because that makes the tips follow three
  print settings (bottom layers, layer height, height correction), the
  planner reports the counts of held layers its tips are right for
  (`PointGenResult::held_layers_min/max`: a layer counts where a piece
  standing on the plate would get tips or a flat-face warning, where a
  separate piece starts above the first layer, and, only when another piece
  floats, where one that stands has any newly exposed underside, whose spots
  for extra hold can thin that piece's), and supports
  made with no raft go out of date when the job's count leaves that range,
  not before (`SupportState::fresh`, `Document::supportSettings`). A plain
  block is never out of date for it; a shelf 0.15 mm up is once its own layer
  joins or leaves the bottom layers. The transition layers are left out: with the
  user's resin (27.5 s for 6 layers, then a ramp to 1.5 s over 6 more at
  0.03 mm) the first few of them would still cure through to the plate for
  any plausible light depth, the last ones would not. Islands that
  start that low got tips at first, because the mid-air check did not know
  the plate holds them; they stand on the plate now (below). Measured on the same parts: no head on the user's rack
  parts has a wall beside it; on a lid lying on a shallow curved face all 86
  low heads stand in the wedge between the plate and that face, become plain
  columns and are reported, with the advice to use a raft.
- **Faces just past the angle, and tips with no way down (2026-11-13).**
  Measured on the demo bracket tilted so its undersides lean 46° and 47° from
  upright: 6 tips covered them, as at 45°; from 48° 41. Each layer such a face
  grows only a little past the angle allows (0.0036 mm at 47°), which the
  sliver filter took for noise. Now the outside of each layer part is also
  measured against what was there `anchor_window` below (0.07 mm past at 47°,
  over 1 mm), with 0.5° allowed for rounding: 46° gets 39 tips, 47° 41. Holes
  keep the per-layer rule, because the hollowing's ceilings, shaped at 45°,
  close at 45.6° on average (up to 47°) through its outline rounding, and a
  hollow ball must keep no tips inside. Lid 639 tips, eye cup 342 → 371,
  bracket at 30° 110 → 113, the user's rack parts unchanged. And in the tree,
  a tip with no head of its planned size that finds a way down is built
  smaller (0.4, 0.3 mm), then up to 1.5 mm across the same face, before it is
  dropped: the eye cup at How many "Most" and at Tip size 0.6 mm no longer
  has a spot that blocks saving (one tip moved 0.5 mm; nine built at 0.4 mm).
  Not printed. Not done: replaying the real supports to relieve tips: on the
  user's parts it
  found 2 small island tips over their limit (1 to 2 % chance), and its "90 %
  likely" tip on the lid is the foot of a support standing on a 60° slope,
  which the replay counted as a separate tiny tip (it no longer does: the
  model over the side of a support that stands on it is that support's foot
  for as long as the support stays sunk in the face, up to 2 mm, and where
  two such supports fuse the later one keeps its foot; the lid then has no
  flagged tip, and the eye cup at "Most" none). The planner's rule for each
  tip's own load also stopped too early on those parts: 8 rounds left 16
  tips over their limit on the eye cup with 0.3 mm tips (4 of them flagged
  by the replay) and 6 on the lid; with up to 16 rounds, used only while a
  tip is still over, it took 14 and 11, none is left over, and the replay
  flags none of those.
- **A piece that starts inside the bottom layers stands on the plate
  (2026-11-13).** With no raft, a separate piece whose first layer has its
  underside inside `plate_hold_height` (0.05 mm up with the defaults: the
  second layer) was given tips, counted as starting in mid-air, and called
  unheld by the replay, while a face of a standing piece at the same height
  was left to the plate. The reasoning is the same for both: the bottom
  layers' long exposures cure the resin under it through to the plate. Now
  all three take it as standing on the plate: the planner
  (`processIsland`: no tips, and its layer counts for the held range, so
  supports made with no raft go out of date when a change to the bottom
  layers moves such a piece in or out); the mid-air check
  (`validateLayers` / `validateSupported` take the held height, 0 on a raft,
  and report such pieces as `plate_held`); and the replay
  (`StabilityConfig::plate_hold_height`, set by `stabilityConfig` with no
  raft only: such a part is fixed like the first layer, what prints on it
  pulls on it as on a column on the plate, and fresh skin in those layers is
  not judged, which also covers a wide face that starts there). A piece that
  starts in the first layer above that height (0.1 mm with the defaults) is
  in mid-air as before, and with a raft nothing changes. Tested on a block
  with a small piece beside it at 0.05, 0.1 and 0.2 mm, in the planner, the
  check and the replay, and through the app's own path. Reasoning, not
  printed: how well a piece holds through one layer of resin cured from
  above is not measured.
- **Supports that would stand on one spot of the model share a support
  (2026-11-13).** Each tip with no way to the raft got a support of its own
  standing on the model straight below it, and nothing checked whether
  another already stood there. Where tips are close (four within 1 mm inside
  the user's eye cup) their supports stood within 0.3 mm of each other and
  printed as one, so one 0.4 mm foot was pulled by all of them: the replay
  had that foot at 27 mm² against the 11 it is trusted with (38 mm² with
  0.6 mm tips). On the eye cup 36 of 126 supports on the model shared a spot
  with another like that (85 of 205 at "Most"), on the lid 11 of 158. Now a
  tip whose support would stand within 0.75 mm of one already standing there
  joins it (`joinOnModel`: a 45° bridge onto its shaft, or straight across
  where the support is too short to have one), up to a trunk's six tips, and
  the foot gets as much contact as its tips have together
  (`foot_tip` = √Σd²). At 1 mm apart two such supports were found to carry
  each its own tip's load, so they stay as they are. Eye cup: 126 → 108
  supports on the model, no foot or tip flagged at the starting values, at
  "Most", or with 0.3 or 0.6 mm tips; lid 158 → 152, none flagged. Not
  printed. That left one foot on the eye cup at twice its load (a chance of
  4 %, under the app's 1 in 20): two tips whose heads ended 0.13 mm apart.
  The first had walked 3 mm to a pillar on the raft; that pillar was then
  full and the second tip's own walk was 0.05 mm short of its gap to the
  model, so it stood on the model, fused to the first one's junction, and
  being 3 mm tall against 30 it took both tips' pull and part of two more
  that fuse in through the bridge. Now a tip whose junction lies within
  0.25 mm of the junction of a head with a way to the raft shares that
  junction and goes down with it (`shareJunction`, tried last before
  standing on the model, up to a trunk's six tips): the eye cup has 107
  supports on the model and no foot over 1.5 times what it is trusted with.
- **The planner asks how long each support will be (2026-11-13).** The rule
  for each tip's own load took every support as reaching the plate. A support
  that stands on the model a little way below is many times stiffer, and
  the stiffer of two tips beside each other is pulled more: on the lid with
  0.3 mm tips one tip at the top, on a support standing 15 mm down beside
  neighbours whose supports reach the raft 58 mm down, carried 13 mm² against
  the 6 it is trusted with (a chance of 7 %), though the planner had it
  within its limit. A look straight down does not say which it will be: no
  tip with nothing below it stood on the model, but of those with the model
  below about half reached the raft all the same (245 of 403 on the lid).
  Routing the supports does, and takes 2 to 4 s of the 13 to 21 s the
  supports take. So the planner now asks once, before the rule changes any
  tip, how long the support under each would be
  (`PointGenOptions::support_lengths`, answered by `supportLengthsOnModel`:
  the tree for those tips; tip to foot, plus what the narrow foot gives, for
  one that stands on the model; 0 for one that reaches the raft, which is
  as before). A tip the rule adds is taken to stand as the tip it is put
  beside where the model is under it too, else to reach the plate. Lid with
  0.3 mm tips: that tip is no longer flagged (995 tips, were 996); lid 644
  (641), eye cup 371 as it was, 564 at "Most", 362 with 0.6 mm tips, 607
  (601) with 0.3 mm: nothing flagged on any. Making the supports takes 2 to
  6 s longer on those parts. Not printed.
- **Edge tips no smaller than 0.3 mm from Tip size (2026-11-14).** Tip
  size 0.3 mm moved the tips near edges and visible faces down to 0.2 mm.
  How much a 0.2 mm tip holds is not measured (the built-in table scales it
  by area, about a quarter of a 0.4 mm tip, and the planner adds tips on
  that), it is about 14 × 10 pixels on the Saturn 4 Ultra 16K's screen, and
  Phrozen's guide for Aqua Hyperfine, the user's resin, gives 0.3 mm as the
  smallest support that prints in it without bending or breaking (a tall pin
  test, so for a tip only indirect). So until a tips plate is fitted, a
  smaller Tip size keeps edge tips at 0.3 mm (never above the usual tip),
  and the caption says why; the Advanced page, a value saved with the resin
  and a fitted table still set 0.2 mm. Tips where a part starts in mid-air
  still take whatever fits a narrow spot. At Tip size 0.3 mm the user's lid
  has 704 tips instead of 995, the eye cup 594 instead of 607, the demo
  bracket 87 as before; nothing flagged by the replay on any. Not printed.
- **What a review of the replay found, fixed (2026-11-15).** An Opus lead
  and nine Haiku agents looked for flaws in the replay. The lead checked
  their 50 findings against the code and kept 15; these are the ones that
  were confirmed, now fixed:
  - Fresh skin at a free edge was given the reach of skin held all round.
    The half-tear reach was the planner's tip spacing over the root of the
    margin (5.54 mm with the built-in table), so a 0.1 mm ledge 3 mm out
    with nothing under it was not flagged. Bending theory says that harsher
    case tears at 0.7 to 1.5 mm. A point nearer the part's own outline than
    anything that holds it now hangs off one side like a ledge, with its own
    reach: the planner's skin reach (1.6 mm) over the same root, 2.77 mm
    (`StabilityConfig::skin_edge_reach`, `--edge-reach`). Skin held all
    round keeps the spacing's reach. That ledge now shows a spot at a 1 in
    2 chance at its far edge. Neither reach depends yet on the layer height
    or the pull, as the formula says it should: the skin's thickness is the
    cure depth, which the replay does not know. With it, small flaps showed
    up at the lowest layer of gently sloped faces. On the lid one was
    0.5 mm², at the lowest point, where the nearest tips touch two or three
    layers higher. Ordinary models then read "1 place may not print". So a
    skin likelier than not to tear, but over less than 0.1 mm³ of model (2 mm²
    of a 0.05 mm layer), is counted and not listed (`small_skins`). This is
    the same bar as a tip's sliver with nothing going with it. Its chance
    still colours the heatmap.
  - The whole-print chance in the CLI report left out parts that start in
    mid-air and fresh skins. It read 1.0 next to a 950 mm³ part that was
    certain to fall. Now both count: what starts in mid-air in every replay,
    and each skin drawn once a replay from its bands (one strength for the
    whole skin, so each band tears as often as its chance says). The app
    does not show this number.
  - After a tip lets go, whether the piece over it is stiff or thin (the
    2026-11-14 rule) was judged by the height printed since then, not by
    how thick the model over that tip is. A thin frame joined to a post
    10 mm tall was taken as stiff, and its tips were lost in every replay.
    Now it is judged by the model standing over the tip, layer on layer
    (`columnOver`, as `step()` reads the model's thickness under a patch).
  - When the load of a tip that let go was spread over 50 or more tips,
    each share was under 0.02, and none of them followed the load as it
    grew. Now only the smallest shares, under a twentieth of the load
    together, are kept at what they were given.
  - A tip touching with between 0.75 and 1 of its face counted as touching
    with all of it. It now holds as much as touches, with no step. Contact
    is still summed over all of its layers, which is why a sunk tip, two or
    three times its face, still counts as whole. A tip whose size nobody
    gave is still sized by its largest joint, because the sum would count a
    sunk tip's rings several times over. The planner cannot see this
    shortfall, because it plans before the supports exist.
  - A rod's lean was capped at 45°, though the app lets heads lean to 60°:
    a 60° head was taken as 1.4 times as strong and 4 times as stiff as it
    is. The cap is now 60°.
  - A tip that holds nothing (no size) never let go. It now lets go as soon
    as it is pulled.
  - An empty model crashed the replay. It now says there is nothing to
    print.
  - The CLI no longer runs out-of-range values silently as other values:
    `--replays 0`, `--tip-margin 2`, `--steepness 0.5` and the rest are
    refused with their ranges, and 0 now means what each help line says it
    means.
  - The planner's count of tips still over their load came from before its
    last round when it ran out of rounds. It now counts the tips as they
    are left.
  - The CLI does not apply values typed on the app's Resins page: only the
    app knows those settings. It now says so in `profile_notes` and names
    them.
  - The Risk card's note named only the borrowed tip strengths. It now also
    says how far a layer's pull spreads through thin model is a guess.
    Changing that guess from 2 to 4 mm takes the lid's likeliest tip from
    under 1 in 100 to more than 1 in 4. Where the print has a cup, the note
    names the suction as a guess too.
  - Two header comments were wrong: "a fifth at twice the slab" (it is
    1/17), and a default skin reach no shipped path uses.
  - Each fix has a test that fails without it. The skin test now pins its
    half-way point exactly, instead of "at least a half".
  Not done: tests that pin the slab's exponent and a seam's later layers.
  Also not done: a check of an added tip's support length against the
  model under it, which the lead left as plausible only. Not printed.
- **Tips whose faces overlap hold as one (2026-11-20).** On the user's
  CHamber job the Layers chart's "how close" line jumped to 180–200 % at
  three layers near 28 mm. All three were one tip under Chamber_Base that
  touched the model with 0.012 mm², 0.15 mm from a tip that touched with
  0.30 mm² over four layers. Together the two carried about 2.4 mm² of pull
  on every layer there. But where thin model's pull is shared out among the
  tips near a patch, each tip was a spring of its own, and a pull 3.5 mm off
  to one side made the pair a lever 0.15 mm long: +6.8 mm² on the grazing
  tip, −4.4 mm² (a push) on its neighbour. (The existing floor on levers,
  0.05 mm, is tested on all the holds together, so it never caught a pair.)
  Now tips under the model whose faces overlap (centres closer than their
  two radii) are one hold in that share. The set's pull is split by how much
  each tip touches the model, as one contact would split it. Tips that
  overlap no other take the old path unchanged. Only the per-layer share
  changed. The whole-piece solve holds every tip of the piece at its own
  stiffness, so it cannot pivot on a pair like this. The hand-over when a
  joint lets go already drops pushes. Neither was changed.
  Measured, with the fix off and on, same commit:
  - CHamber (supports remade, 493 tips): the line at layers 545 to 570 went
    from up to 197 % to 26–47 %. Its highest point is now 72 %, the
    Chamber_Top neck bending at 6.1 mm. Places: 2 to 1 (the thin support
    under that tip is gone). Nothing of the model was lost in either.
  - A wide cone hanging from its point (`test_points`): the 0.2 mm tip at
    the point and the 0.4 mm tip 0.24 mm beside it carried 6.6 and 3.0
    times their trust; now 1.8 and 1.4. That test's check that the planner
    alone leaves a tip over twice its trust is now "over 1.5".
  - Lid: the planner adds 113 tips beside overloaded ones instead of 111
    (598 tips, were 596). The replay's places are the same, and its
    chances move by less than 0.001. Eye cup: identical.
  A new test pins it: a 1 mm shelf 3.5 mm past such a pair took 110 mm² on
  the grazing tip and 29 on its neighbour; now 4.8 and 30.9, split by
  contact. Not printed.
- **Taking a tip off is quicker (2026-11-21).** Each tip edit rebuilt the whole
  job's supports and checks from nothing: 5 to 6 s a tip on a 1,223-tip job of
  the user's. Where the time went, on copies of two of the user's jobs:
  - The job's slots (the whole model sliced and mapped): 1.25 s of 3.3 s on
    TheDipper_full (1,130 tips, 6 parts), though the model doesn't change.
  - The checks of what dropped and tied tips leave unheld: two planner passes
    over the edited part, 2.1 s of 2.7 s on CHamber. Over half of each pass
    worked out a layer's free rim (`freeRim`) on every layer that needed tips,
    though only layers reaching past the one below use it.
  - Every other part's tree, mesh, gap check and checks, and each raft's
    outline, made again unchanged.
  Now:
  - The planner works out the rim only where a layer reaches past the one
    below. Its tips are the same (an intersection with nothing is nothing),
    and every planning pass is quicker for it: CHamber's two checks 2.1 s to
    0.85 s.
  - The app keeps a `RebuildCache` from one tip edit to the next while the job,
    its settings and the bottom layers are the same. It holds the slots, the
    model's index and edges, and each part's tree with what was worked out
    from it: the gap check, the mesh, the two hold checks, for the settings it
    was made with (a part on a raft it shares takes that raft's). It also holds
    each raft's outline and pieces for the versions of its parts' trees. Only
    the edited part is made again. An edit that stops part way keeps nothing
    of its own.
  - When tips start being edited, the supports are made once more in the
    background, as they are, so the first edit is quick too. It gives way to
    any other work and shows nothing.
  - Still made each time: the edited part's tree and checks, and the check
    that nothing starts in mid-air, over the whole job. One tip taken off
    changed 60 to 85 % of the job's support layers (its support goes down to
    the raft), so cutting only the changed layers would save little. Letting
    a part below be grown only when a part above doesn't rest on it as it is
    saved about 5 % and was left out.
  Six tips taken off one after another, against main built beside it (the
  faster of two runs each, on a busy Mac): CHamber 2.68 s to 0.69 s a tip,
  TheDipper_full 3.27 s to 1.31 s. The 29 screenshots of the tip, supports,
  per-part and risk scripts are the same as main's, byte for byte.
  A test makes a job of three parts, two whose rafts meet (one with a thicker
  raft of its own, then a wider one), takes tips off one part after another
  and puts one back, and compares each result with the cache to one made
  without it: tips, mesh, trees, rafts, checks and problems all the same. It
  fails when a part's old mesh is kept after its tips change.
- **A tip taken off goes at once (2026-11-21).** Still about a second a tip
  on a big job: mostly the check that nothing starts in mid-air, over the
  whole job. But while tips are edited the view shows only the tips, and the
  tips an edit gives are known before anything is made: the planner's, with
  the edits applied, part by part (`editedTips`, what finishWithEdits starts
  from, each as it was built last time). So the app shows those at once and
  picks, selects and edits among them (`App::shownTips`) while the supports
  and checks are made behind. When they are done, the supports' own tips
  take over. The tip selected meanwhile stays selected, found by where it
  is; before, it was dropped when the update finished. Planned again (an
  edit the planner worked around taken back), the tips are not known until
  then, and the view waits as before.
  Before this, every tip edit also made the supports out of date until it was
  done (`SupportState::fresh` wants the result's edits to be the job's): the
  view hid every tip, the panel said "Supports out of date" and offered
  Remake supports, and the resin line lost the supports. Now, while the
  supports for exactly the job's edits are being made from the result's
  plans (`SupportState::making`), the result still counts: the panels keep
  its checks, with "Updating the tips" in the corner, till the new ones come.
  A saved job is right meanwhile too: it keeps the plans and the edits, and
  the edits are applied again when it opens. Planned again, nothing changes:
  the supports are out of date till then, as before. The test above now
  also checks, at every step, that the tips
  shown at once are the rebuild's own: the same tips in the same order, with
  their sizes and parts.
- **The Risk view, easier to read (2026-11-23).** The user found "Where a
  break starts" too thin to see and the heatmap's set-up confusing. Four
  reviews (a cold read of the screenshots, then the paint, the scale and
  legend, and the controls, each from the code) agreed on why:
  - A break starts at one joint, the line between two layers, so the
    paint was one layer tall: 0.03 mm, under a pixel at a normal zoom.
  - The paint was shaded like the model, so on the dark side teal and the
    "fine" blue were the same navy.
  - "Fine" was a blue close to the model's own look, so a model with
    nothing to show looked like a model not painted.
  - The scale ran 1 in 500 to 1 in 1 in 16 rainbow steps whose 1 in 5 was
    orange while the places' was red, with no word for what the chance
    is of, under a bar 8 points tall.
  - Places | Heatmap and What would be lost | Where a break starts read as
    four peer modes. Once on the same bands, Places' paint and the heatmap
    of what would be lost are the same picture (both by `partChance`).
  Now the Risk view is one picture: the numbered places and the tips that may
  let go over the model, painted by one of "What may not print" or "Where it
  may break" (`risk_colour` in scripts). The steps are fine (under 1 in 100:
  the model's own grey, never painted), a small chance (from 1 in 100, a blue
  that stays apart from the grey and the bands with any colour sight), then
  the places' own may, likely and very likely in their colours. Where a
  break may start is painted 0.5 mm above and below each joint
  (`kBreakBand`; the warmest joint within reach wins). Paint is shaded at 0.4
  of the model's light (`kPaintShade`), and in the Risk view the supports are
  darker than the model. The legend is the five steps side by side, the
  chance each starts at over them and its name under them. Over the view, a
  line says what the paint is of, and one line that it is a guide; the full
  note on the strengths stays in the Checks. Layers' "Show risk" uses the
  same steps (fine stays white) and the same two choices. The places' own
  paint (`paintRisk`, `paintRiskLayer`) is gone, and so are the i1_risk
  shots of it; their tests now check the heatmap's same steps. On CHamber,
  the replay with its paint takes as long as before (1.6 to 2.0 s). Not
  printed.
- **Supports for a tall model in minutes, not a quarter of an hour
  (2026-11-23).** The user's TubeRack_v2 (175 × 53 × 94 mm, 65,396
  triangles, about 2,800 tips) was still making its supports after 9
  minutes. Replayed headlessly on main: planning took 121 s, the supports
  167 s, and each round of the loop that strengthens tips (`lowerPeak`, up
  to 6) about 3½ minutes more. Where the time went:
  - Joining pillars into trunks (`mergePillars`): after each merge, every
    pair of pillars was looked at again, and each pair in reach checked with
    walks down the whole 94 mm beside the model. About 110 s of the
    planner's 121 (its trial tree, `supportLengthsOnModel`) and most of the
    167, on one core. Whether two pillars can merge is theirs and the
    model's alone, so the pairs in reach now wait in order of distance, each
    is checked once (the next few dozen side by side), and the first that
    can merge does: the same pairs merge in the same order. The planner's
    trial tree went from about 110 s to 2.7 s, its merging to 0.3 s. A head
    joining a pillar
    (`attachToPillar`) checked its bridge to every pillar in reach before
    taking the nearest that fits; now only as far as the one taken.
  - The replay in each check (`analyzeStability`), 45 s:
    - It worked out the whole print's hollows with all their measures (the
      thinnest wall, the widest circle), one cup after another, though it
      wants only where the cups are (`hollowRegions`): 15 s to 2 s.
    - Its 500 replays ran one after another: 13 s. They now run side by
      side, each on its own. What they sum is kept by replay and added up
      after in replay order, so every number is the same however many
      threads; the thread test now checks the sums too.
    - The model's parts are kept from round to round (`ModelKept`), as its
      layers were.
  - Choosing where the loop adds a tip over a fresh skin looked at every
    point of the skin again for each tip it added: 10 s in the first round
    of the TubeRack's. Each point now keeps its distance to the nearest tip.
  - Cutting layers (`sliceMesh`) gave each thread an equal share of layers,
    but the lower layers, with the supports and the raft, take longer to
    cut, so most threads waited. Runs of layers now go to whichever thread
    is free, each cut as before.
  TubeRack_v2, main built beside this, on a busy Mac: Generating supports
  97 s (main 833 s), the loop's two rounds included; counting the tips for
  the five stops after it 42 s (513 s); the whole script, with the Risk
  replay, the layers and the save, 2.8 minutes (23). CHamber 4.6 s to 4.5 s
  and TheDipper_full 13.1 s to 12.3 s (the faster of two runs each, taking
  turns): no slower where it was quick. Main and this wrote the
  same print files (all but the save time in the header) and the same
  screenshots of the supports, the Risk view and Layers for TubeRack_v2,
  CHamber, TheDipper_full, the lid and the eyecup. Not printed.
- **The tree's join on every core (2026-11-23).** After the trunks, the
  last part of a tree on one core was joining the tips in order (1.1 to
  1.5 s a tree on the TubeRack, four trees a Generate). Nearly all of it
  (over 90 %) was the 300 or so tips whose planned head found no way down,
  each trying a smaller size and then nearby spots on the same face
  (`supportSmallerOrMoved`), with every try's head, drop, route and stand
  worked out one after another; half needed 9 to 32 tries. What a try works
  out depends on the model alone, so a tip that may find no way down
  (`Ahead::sure` false: only the supports made before it could give it one)
  has its tries (`triesFor`) worked out in the first, side-by-side pass,
  each to the end (`workOut(…, true)`, as supportTip goes given nothing),
  as far as the first sure of a way: the join never goes past it. Joining
  stays in order. A try not worked out ahead is worked out when needed, a
  few side by side. TubeRack's join 1.1 to 1.5 s to 0.04 to 0.18 s a tree,
  the first pass 0.1 s longer. Print files and screenshots the same as
  main's for TubeRack_v2, CHamber, TheDipper_full, the lid and the eyecup;
  649 tests pass.
- **The supports card says where it is (2026-11-23).** The user asked for the
  app to keep them up to date while supports are made. The card listed six
  stages read off the bar's fraction as it was shared out before the
  strengthening loop (`lowerPeak`) took the last 40 % of it: on the TubeRack
  it showed "Slice the model" through the trees and "Check for parts in
  mid-air" through the whole loop, and the bar moved once a round. Now:
  - What reports progress can say which part of the work it is: a child
    control labels it (`JobControl::sub(begin, end, label)`, "plan|part
    2/5|islands"). Generating supports labels its plan, its making (its
    checks "make|check|…") and its strengthening; making supports ahead
    labels the same. The card reads its stage from the labels
    (`supportStage`) and goes forward through all of them; how the bar is
    shared out is `kPlanShare` and `kMadeShare`.
  - A seventh stage, "Replay the print, strengthen tips", and a line under
    the stages (`supportsDoing`): the part being planned ("Planning part 2
    of 5"), the first replay, then each round ("Round 2 of up to 6: adding
    77 tips and making 3 bigger", "…: replaying the print with them", "…:
    again, without the tips that made it worse").
  - The loop's first replay is a fifth of its share of the bar, and each
    round as much of the rest as the most rounds share it: its pick, its
    supports and its replay move the bar as they go.
  - The card stands above the model tools, as the banners do: its Cancel
    sat under the tools' buttons.
  Tests: the stages and lines from labelled reports; a real Generate goes
  forward through all seven stages; the loop on the thinned plate says each
  thing it does and moves the bar more than 20 times a round.
- **Thin model hangs only from the tips under its own part (2026-11-23).** On
  the user's TubeRack_v3 the Layers chart's "how close" line jumped to 204 %
  at 54 mm (layers 1071 to 1094) and came back under 100 % by 55 mm. It was
  one 0.2 mm tip at the end of the rack's divider (0.028 mm² of contact,
  half let go at 7.5 mm²) carrying 15.3 mm², 15.2 of it given outright by
  the thin-model share. 14.7 came from the two arms of the back wall's
  lattice, which leave a node at 52 mm and climb outward at about 50°: 3 to
  4 mm from the tip, across a gap, and separate parts of the layer from it.
  Their outer edges have 0.25 mm of model straight under them, so they
  counted as thin; the post they grow from is another part of the layer, so
  no thick model was near; and which tips may hold a patch was judged only by
  how far under the layer each is. With the patch off to one side of the line
  of tips along the divider, the lever put 2.1 times the arms' pull on that
  tip. Now a tip takes thin model's pull outright only where the model goes
  on straight over the tip into the patch's own part of the layer, or ended
  in a part the patch's part stands on (`buildColumns`: each tip's column,
  followed up to 20 mm; `underPart`: a part walked down through what it
  stands on, as far as asked). The rest goes by stiffness, as thick model's
  does. Measured, rule off and on, same commit (saved supports, hinge +y):
  - TubeRack_v3: 204 % at 54 mm to 39 %; 90 % at 80 mm (another arm, the
    same cause) to 54 %; the top plate's 98 % at 98.5 mm, a real thin slab
    over a tip, unchanged. Tips that may let go: 5 to 1; places 1 and 1.
  - CHamber: 8 layers move, by at most 3 points. DishRack_SideBar,
    DishRack_Top, MMPLA_print01, the MMPLA lid, TheDipper_Front and
    TheDipper_full: identical.
  - Lid, planned afresh: 637 tips to 636; places, tips shown and the worst
    layer the same; 35 layers move, by at most 7 points. Eye cup: identical.
  Given up: two parts of one layer on one thin slab, with a tip right under
  one of them; the other no longer hangs from that tip outright. A new test
  pins it: a post with a stem and an arm, the tip under the stem's end; the
  arm put it at 2.33 mm² (worst at layer 74) instead of 1.99 (layer 62).
  Not printed.
- **The top of a hole through a wall holds itself up (2026-11-24).** The
  user: the planner works well on small models, but large ones need other
  logic. On TubeRack_v3 (175 mm tall, 144 round holes through 2 mm walls)
  1,250 of 2,738 tips sat on the tops of the holes, about 7 a hole, within
  32° to 41° of the top. The look-back (`anchor_window`, `self_supported_reach`)
  read the two sides of a gap closing over 1 mm each as a ledge reaching over
  nothing, and coverage then packed tips 0.7 to 0.94 mm apart along the arc.
  Each layer of such a closing reaches out from both sides at once, as a
  bridge is built. Now a gap that closes is judged once, where it closes, and
  its pieces below get no tips when: each piece is within `closing_reach`
  (0.8 mm, half of `skin_reach`) of the layer below; 1 mm under where it
  closes the gap is 4.0 mm across or more (narrower: the apexes of the star
  and diamond windows, whose tips the walls beside them pull on) and no wider
  than a round crown whose last layer reaches 0.8 mm from each side (6.9 mm
  at 0.05 mm layers); both sides are one wall 1.5 to 3.0 mm deep with the gap
  open past both faces (not a cup, a tube, a notch, a blind hole), each side
  at least 1 mm wide, and both came in over that millimetre, neither by less
  than half the other (an arch, not a fin reaching for a wall); it closes
  within 4 mm; and no flat face over 50 mm² appears within 2 mm above it (the
  rack's row of holes 0.65 mm under its lattice plate keeps its 56 tips). A
  top that rises across the wall by 14° or more (a tapered bore) may be up to
  12 mm across, only on a model taller than 80 mm: CHamber's bores, on parts
  tilted about 45°, rise 43° to 45° across their walls with closing gaps of
  7.3 to 9.6 mm, and keep their tips. The spots for extra hold on such a top
  are left out too. Found while measuring it: the lowest pieces of the 30 mm
  holes' tops are about 20 mm across, so the line that looks for the far side
  of a gap reaches 25 mm; a tapered top is followed until it has closed just
  inside both faces; and in a closing that holds itself up a piece keeps tips
  only where it reaches past 0.8 mm (as the look-back does): the 17.5 mm
  bores' last piece is one U (both sides and a 1.606 mm bridge, 3 µm past
  the reach at its middle), which had put 8 tips across it. Measured, app,
  Generate (the strengthening loop ran, 0 rounds, as before):
  - TubeRack_v3: 2,738 tips to 1,503; on the holes' tops 1,250 to 60 (the 56
    under the plate, and 2 on each of two bridges), tips for extra hold 76 to
    36 (those beside the tips on the tops), 5 others; every other kind of
    face the same. Supports' resin, the slices less the model's, 51.4 ml to
    33.6 ml (the shown sum, 115.0 to 51.6); trunks 996 to 482, braces 4,621
    to 1,555; "Tips over the edge of a slot or wall" 42 to 5. The replay, 500
    replays a seed: as it is, 0 of 2,000 lose any model, expected loss
    0.062 mm³ either way, tips that may let go 5 to 0; 1.41 times weaker,
    18 to 17 of 1,500, 5.36 to 0.16 mm³; 3 times weaker, 1,359 to 1,352,
    700 to 153 mm³; the tips alone 3 times weaker, 10 to 3 of 2,500, 81 to
    24 mm³; the tips 8 times weaker (the tips plate's level), all lost
    either way. The replay does not judge whether a bare top itself prints.
  - Lid, eye cup, CHamber, TheDipper_full: identical (the planner's plan, the
    plan after the strengthening loop, the tips as built, the tree, the
    replay); no closing holds itself up on any of them.
  - The rule's own time: 0.38 s of 38 s planning TubeRack, about 1 % on the
    lid and eye cup.
  Tests: `test_closings.cpp` (holes of 6.5 and 11.8 mm lose their tops' tips
  and nothing else changes; 14 mm, walls of 1.2 and 4 mm, a side of 0.6 mm, a
  blind hole, a cup, a tube, a flat-topped window, a 50° apex, a shallow roof,
  a fin into a wall and a plate 0.65 mm over a top keep theirs; a tapered bore
  only past the height; a box of holes with rule 4; the socket, the slab over
  two fingers and the slotted floor the same either way). Not printed.
- **Stacked floors drawn as one column (2026-11-24).** On TubeRack_v3 the
  floors stacked over one another send their supports down one line: of
  1,234 raft pillars before branching, 497 stood within 0.005 mm of a taller
  one (every such column had its two bridges taken, so the head below drew
  its own). In the print the two are one shaft; the tree drew, braced and
  branched them as two (1,703 of 5,060 braces under 0.01 mm sideways, 398 of
  1,028 raft links under 0.01 mm). Now, after the heads have chosen their
  ways down, a pillar whose middle is within 0.01 mm (under a pixel) of a
  longer column's is drawn as that column, which carries its tips too (up to
  6, `max_tips_per_trunk`), where the column drops more than 70 mm to its
  foot (`fold_min_drop`). It is as wide as the wider of the two, as it
  printed; braces and branches keep 2 mm (`flex_zone`) clear below where it
  joins, and two tall columns that close get no brace or branch between
  them. No pillar on a part lower than 70 mm can be that long (a pillar's
  top is below a tip, so the eye cup's longest possible is 61.6 mm), so
  those parts keep their trees bit for bit. Columns near each other but not
  in one line (a median 0.46 mm apart) are left as they are: joining them
  would make the print weaker. Measured, app, Generate, after the closings
  rule above:
  - TubeRack_v3: the same 1,503 tips (the plans identical); trunks 482 to
    422, pillars 1,100 to 998, braces 1,555 to 1,275, branches 250 to 208;
    supports' resin 33.6 ml to 33.2 (the shown sum 51.6 to 45.1). The replay
    the same as it is, 1.41 times weaker and with the tips alone 3 times
    weaker; 3 times weaker, 1,352 to 1,353 of 1,500 lose model, expected loss
    153 to 112 mm³.
  - Lid, eye cup, CHamber, TheDipper_full: identical.
  Tests: `test_tree.cpp` [fold] (shelves 30 mm apart, the top one 85 mm up:
  the column drawn once, heads and bridges as they were, nothing in the band
  below a joint, the same resin when branching is off; the same rack under
  70 mm keeps every pillar, rod and triangle; a column takes no more than 6
  tips). Not printed.
- **The supports' resin, counted once (2026-11-24).** The figure shown ("ml
  of supports") added every piece's own volume up: where shafts, heads,
  balls and bars overlap, and where tips sink into the model, the resin was
  counted again. On TubeRack_v3 it read 115.0 ml for 51.4 ml of resin. Now it
  is what the supports add to the print, raft included: the layers of model
  and supports together (cut for the check after making) less the model's
  own (or the hollowed model's), at 0.05 mm (`layersVolume`). On TubeRack's
  model alone that cut gives 38.922 ml against 38.923 ml from its mesh. A
  part of a job with more parts has its own supports' layers (sunk-in tips
  in), so the parts add up to the job's within 1 %. "at most" is gone from
  the Job panel; Home's recent jobs keep the new figure under a new name, so
  an older entry's shows as not known. The command line reports it as
  `support_resin_ml` (`support_volume_ml` is the old sum, an upper bound).
  Measured, app (the same tips, trees and replays; only the figure): lid
  12.4 to 8.3 ml, eye cup 12.0 to 9.0, CHamber 10.5 to 9.3, TheDipper_full
  23.8 to 21.0, TubeRack_v3 (after the two changes above) 45.1 to 33.2; each
  equal to its sliced union less its model.
- **A coupon for the tops of holes (2026-11-24).** The replay cannot judge
  whether a bare top prints, so `fruitslicer coupon crowns` prints the
  closings rule above. It is a box of four 2 mm walls laid out like a corner
  of TubeRack_v3 (75.6 × 42.5 × 37 mm, 12 ml):
  - front: straight holes of 6.5, 6.5, 11.8 and 11.8 mm, whose tops the rule
    leaves bare, and one of 14 mm that keeps its tips;
  - ends: TubeRack's tapered bores, measured off its mesh. Its 17.5 mm ones
    go from 16.70 to 18.25 mm across the wall, rising 21°; its 30 mm ones
    from 29.25 to 30.53 mm, rising 18°;
  - back: its lattice (windows 9.96 × 9.27 mm, struts 1.2 mm leaning 47°),
    which keeps its tips.

  A tapered top is bare only on a model taller than 80 mm, so `crowns-tall`
  adds a 6 mm post up to 82 mm. Planned in the app (Generate, hinge at the
  back), short and tall:
  - the four tops: no tips either way;
  - the 14 mm hole: 11; the lattice: 86 and 85;
  - the 17.5 mm bore: 15, and none on the tall one;
  - the 30 mm bore: 25, and 15 on the tall one, 14 in a row along the rim of
    its outer face and one at the top of its inner face.

  Found while measuring it: where a tapered top is judged, the wall's faces
  are looked for 1.5 mm into the side beside the gap (`kSideInset`). At the
  30 mm bore's narrow end that spot is inside the wide end's opening. The
  wall reads 1.87 mm deep, and what is held stops 0.03 mm short of the outer
  face; the sliver it leaves in each layer gets the tips. TubeRack's own 30 mm
  bores read 1.93 mm, placed so that they are held right across, by 0.03 mm
  (the command line on TubeRack: no tips on any of their tops). The coupon's
  other closings read the full 2 mm. Not changed here.

  Print time with the user's settings, the 5 mm lift included: about 2¼ h at
  0.05 mm layers (3¾ h at 0.03), the tall one 4½ h (7½ h). What to look for
  and how to score it: `docs/crowns-coupon.md`. Tests: `test_closings.cpp`
  (one closed piece of the stated size; its 6.5 and 11.8 mm tops get no tips,
  while the 14 mm one and every window keep theirs; the bores keep theirs on
  the short one; on the tall one, the 17.5 mm bore none and the 30 mm one at
  most 20). Not printed.
- **The tips plate, on the supports a job gets (2026-11-24).** The plate
  printed on 2026-11-18 held discs of 15-60 mm² on three tips each, 1 mm
  shafts for every size, 5 mm up: 106 of 120 let go, which read as tips 7 to
  9 times weaker than the replay takes them. The user: those tips "did not
  test supports appropriately", and a new plate should test how supports
  would be placed, "as accurate as possible". The planner never puts three
  tips under a face that size: its spacing rule wants one within 3.2 mm of
  every point, and each tip's own load is held to what it is trusted with.
  Now each coupon is a flat tile (6, 8 or 10 mm across, 1 mm thick, 10 mm up)
  held by what a job of it gets: `generateSupportPoints` with the job's rules
  (each tip's own load included) and `buildSupportTree` with FruitSlicer's own
  heads, depths, shafts, trunks and feet, on one raft. One tip size a tile
  (`tileSupportConfig`), planned as if that size held 0.125 to 2.83 times
  FruitSlicer's starting value, in steps of 1.41: the planner puts fewer
  tips under it, each carrying more, until the spacing rule sets the fewest
  (13 tips on a 10 mm tile, 8 on a 6 mm one). The replay of the print
  (`analyzeStability`, the tile and its tree, no extra pull far from the
  hinge) works out each tip's load; the manifest keeps every tip's, the
  busiest (`load_max_mm2`) and the load one tip alone lets go at as often as
  one of the tile's does on the replay's weakest-link curve, (Σ lᵢ⁷)^(1/7)
  (`load_mm2`). Measured (two sets, 96 tiles, 2.2 s on all cores): the
  busiest tip carries 0.2 mm 1.4-7.5 mm², 0.3 mm 1.9-11, 0.4 mm 2.5-15.7,
  0.6 mm 4-15.7 (the 0.6 mm ladder's two lightest steps both get edge tips
  of about 4.3 mm²: 40-50 tips under the tile); per mm² each touches, about
  15 to 260, spanning the disc plate's 45 and the starting values' 270 (the
  replay's half-let-go load). Replayed at strengths round each tile's
  `load_mm2`, the replay's own chance that some tip lets go matches 1 −
  2^(−(l/h)⁷) to three places, and a tile is lost about 7 % sooner than its
  first tip lets go: a tile with a tip off it is scored "partly". The 0.1 vs
  0.2 mm depth comparison and the breakaway necks are gone from this plate
  (jobs use one depth per size and plain heads; the user agreed); the disc
  plate is kept as `discTipsPlate` for its sheets and the fit's tests of
  those. **The fit** reads `load_mm2`, and a disc's load per tip times 3^(1/7)
  (1.17): a disc held only when all three tips did, so until now each tip's
  strength came out about 15 % low. A depth is fitted only where a size was
  tried at more than one (`penetration_table` otherwise left out); a partly
  held tile may name up to all but one of its own tips. **The app** makes a
  plate on a thread of its own ("Making the plate…", a second or two for the
  tips plate, 0.6 s before), counts a tile's tips let go in a field instead of
  a button each, says "coupon" for "disc" on the Fit page, and a kept tips
  plate notes its design: an earlier disc plate's sheet makes Calibrate's
  next step "Print the new tips plate" instead of scoring or fitting it (its
  scores stay, set aside when the new plate is saved). Not printed yet.
- **Quick controls named for what they set (2026-11-13).** The Supports
  panel had three five-stop sliders named after outcomes (Hold, Amount,
  Sturdiness). Measured through the app's own logic on seven placements:
  Hold reached 0 of 10 tips on a part laid flat and 21 of 72 on the demo
  bracket (it set only the usual tip; mid-air and edge tips have their own
  sizes) while its caption named a size no tip had; its end stops also scaled
  what a tip is counted on to hold, which gave 18, 27, 23, 21, 24 tips across
  its five stops on a tilted bracket; Amount's lighter half only moved the
  lean angle and changed nothing on flat parts, and on the bracket tilted 45°
  all five stops gave the same 22 tips, because the short-ledge rule (1 mm
  over 1 mm, itself 45°) kept every newly included face without tips;
  Sturdiness never changed a tip. Now: **How many** (the angle, with the
  ledge rule and the spacing towards More and the fresh-edge reach towards
  Fewer; a lighter stop keeps the fewer of its own plan and the stop
  above's), **Tip size** (the usual tip, the other two sizes moving with it)
  and **Look further** (a 30 mm search for a way down: supports standing on
  the model go from 114 to 8 on an eye cup, for 18% more resin). The panel
  shows this job's tips under each stop (`tipCounts`, counted in the
  background). Tips per stop, Fewest to Most: bracket 56 / 68 / 72 / 91 /
  109; bracket at 45° 22 / 22 / 22 / 71 / 79; and with the rule that no tip
  carries more than it is trusted with (added the same night, on by default)
  59 / 71 / 75 / 91 / 109 and 67 / 67 / 70 / 129 / 126: that rule adds the
  tips for hold that the 45° bracket lacked, so above the middle the count
  can come out a few tips either way, and a smaller Tip size now means more
  tips (bracket at 30°: 231, 110 and 86 tips at 0.3, 0.4 and 0.6 mm), some
  made a size bigger again. Jobs saved with the sliders
  convert to the same planner and tree settings (`convertOldQuick`, checked
  for all 125 combinations). The stop sizes are arbitrary until a plate is
  printed; the lighter stops relax the fresh-edge rule, which is itself
  unmeasured.
- **A column where no head fits (2026-11-12).** F-01's island (inside the
  lid's threaded bore, 0.5 mm above another face) could not take a head in any
  direction, so the lid always had one part starting in mid-air. A straight
  column as wide as the tip, sunk into both faces, now joins a tip to the model
  up to 3 mm below it when nothing is beside it on the way. The lid as it sits
  has no dropped tip and nothing floating. Not printed.
- **Closer tips at fresh edges (2026-11-12).** Reasoning, not measured: see
  `SupportConfig::skin_advance` / `skin_reach`. Where a layer reaches more
  than 0.5 mm past the one before, and its edge there is the part's own
  outline (a free edge: not the side facing what holds it, not a ceiling with
  walls all round), that edge needs a tip within 1.6 mm. The
  first version of this was a zone (the first 2 mm of height of every part);
  on a ball that put nearly every tip under its lowest point and none up its
  sides, where the hold rule used to spread them as the layers grew, so it was
  replaced by this test of what each layer actually does. A vendor's file for
  a 90 mm disc tipped 12.6° (87 contacts: a 10 mm grid plus a row 1.6 mm apart
  along the lowest edge) is where the idea of holding a starting edge closer
  came from; the user reports that vendor's supports sometimes fail, so its
  numbers are not a target. PrusaSlicer's own constants (11.1 mm² per 0.4 mm
  tip; a 3.2 mm radius growing to 4 mm at 3.9 mm) match the spacing used
  everywhere else.
- **A bridge left hanging (2026-11-12).** A pillar made at the end of a
  diagonal route did not record where the route lands on it. When a later head
  raised its top and it then merged into a trunk, the merge could start above
  the landing, leaving the route's end in mid-air. Found by the validator on
  the cap test once the tips moved; `lowest_attach` is now set for it.
- **SatelLite 16K sample (1.0.2.43).**
  - Anti-aliasing off: every pixel is 0 or 255, and AA and grey-scale flags are 0.
  - Mirror Y = 1.
  - 5 s wait after every retract.
  - Transition ramp ends exactly on the normal exposure.
  - Float32 layer heights (0.03 stored as 0.0299999993) used to add one layer to FruitSlicer's slicing. Fixed.
