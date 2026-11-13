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
  printed. Still there, under the app's 1 in 20: a short support whose head
  fuses with a neighbour's can carry part of that neighbour's tip (one foot
  on the eye cup at twice its load, a chance of 4 %).
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
