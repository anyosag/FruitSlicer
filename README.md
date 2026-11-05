# FruitSlicer

Resin (MSLA) support generator, targeting the **Elegoo Saturn 4 Ultra 16K**
(15120×6230 px, 14×19 µm pixels, tilt release).

Goals: supports that leave small, well-placed marks, come off easily, and use
as few contact points as the print physically allows. The design, the research
behind it, and the milestone plan are in
[`docs/resin-support-plan.md`](docs/resin-support-plan.md).

## Status

| Milestone | State |
|---|---|
| M0 — foundation: STL I/O, nonzero mesh slicer, mesh minima/thickness, island graph, printer profile, overlap coupon | done |
| M0.5 — calibration plates (tips, spacing, islands) with manifests and result sheets | done |
| M1 — support-point generator (coverage through material + capacity + islands), points JSON with persistent manual edits, preview renderer | done (calibration still needed) |
| M2 — heads (collision-aware), pillars with 45° bridges and diagonal ground routes, bracing outside the flex zone, one-piece pad raft, model+supports STL, floating-island validation | phase 1 done |
| M3 — orientation search | next |

## Build

Requires CMake ≥ 3.24 and a C++20 compiler (GCC 12+, Clang 15+). Dependencies
(Clipper2, CLI11, nlohmann/json, nanoflann, Catch2) are fetched by CMake.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Usage

```sh
# Mesh statistics: shells, manifoldness, volume, downward-facing minima
./build/src/fruitslicer info model.stl

# Place (rotate, centre, lift 5 mm), slice at 0.05 mm, report islands; optional debug PNGs
./build/src/fruitslicer slice model.stl --rotate 30 0 0 --elevate 5 --layer 0.05 \
    --png-dir out/layers --every 20 --json out/report.json

# Week-1 compatibility test: overlapping shells (see below)
./build/src/fruitslicer coupon overlap -o overlap_coupon.stl --png-dir out/overlap_ref
```

### Calibration plates

The support generator's limits (how much area a tip of each size holds, how
far apart tips can be, how many tips an island needs, the self-supporting
angle) depend on your resin and printer, and no vendor publishes them. Three
plates measure them:

```sh
./build/src/fruitslicer calibrate tips    -o tips.stl     # tip diameter × penetration × load
./build/src/fruitslicer calibrate spacing -o spacing.stl  # tilted plates × tip pitch, plus wedges
./build/src/fruitslicer calibrate islands -o islands.stl  # island size × number of tips
```

Each writes `<name>.stl.json` (which cell holds which parameters) and
`<name>.stl.results.csv` (one row per coupon to fill in). Every coupon sits on
its own small raft in a grid; the L-shaped marker in the corner cell shows
which way the rows and columns run. Parameter combinations are shuffled over
the plate with a fixed seed, so position on the plate does not line up with
any one parameter.

### Support points

```sh
./build/src/fruitslicer points model.stl --rotate 30 0 0 -o points.json --report report.json
./build/src/fruitslicer render model.stl --rotate 30 0 0 --points points.json -o preview.png
```

![Support points on the Stanford armadillo, seen from below](docs/armadillo_points_preview.png)

*Armadillo seen from below: island tips orange, overhang tips yellow.*

Points are chosen bottom-up so that

1. **coverage**: every surface that leans more than 45° from vertical has a tip
   within a radius that grows with the cured height above the tip (seeded with
   PrusaSlicer's curve, 3.2 mm → 6 mm), measured *through material*, so a tip
   never "covers" across a slot or onto a neighbouring finger;
2. **capacity**: for every connected piece at every layer, its tips together
   can hold its cross-section under peel (seed: one 0.4 mm tip per 11.1 mm²,
   scaled by tip area);
3. **islands**: every island gets 1 tip (≤ 1.7 mm), 2 tips (≤ 6.5 mm) or at
   least 3, using the mesh minimum as the contact. Islands too small to hold a
   tip that join the model almost at once are left alone and reported.

Placement is a cost-weighted greedy set cover (cosmetic keep-out regions and
sharp edges cost more), followed by pruning that re-checks coverage and
capacity. On real meshes the greedy cover saves only 1–8 % of points over the
simpler "every uncovered sample becomes a tip" rule. The settings that move
the count much more are the coverage radius, the critical angle and the
orientation, which is why calibration and the orientation search come next.

Manual edits persist across regeneration:

```sh
# edits.json: {"add": [{"pos": [x,y,z], "tip_diameter": 0.3}], "delete": [<id>], "move": [...], "resize": [...]}
./build/src/fruitslicer points model.stl --points-in points.json --edits edits.json -o points2.json
```

Cosmetic regions (`--cosmetic cosmetic.json`) make tips avoid boxes or faces
visible from given directions:
`{"keep_out": [{"min": [x,y,z], "max": [x,y,z]}], "visible_from": [[0,-1,0]]}`.

### Supports and printable file

```sh
./build/src/fruitslicer support model.stl --rotate 30 0 0 -o supported.stl \
    --preview preview.png --report report.json
```

This writes the model and its supports as one STL to load into Elegoo
SatelLite or CHITUBOX for slicing. The points used are saved next to it
(`supported.stl.points.json`); edit them and pass them back with `--points`.
Each point gets:

- a pinhead pointing along the surface normal, tilted at most 45° from
  vertical. Its direction is searched when the default one would hit the
  model; the check uses rays along the cone, so walls are seen but the
  surface under the tip is not;
- a vertical pillar to the raft. Up to three heads share one pillar through
  45° bridges. A head with no clear vertical drop walks diagonally outwards
  until it finds one, and only as a last resort stands on the model; that
  case is reported because it marks an upper surface;
- bracing between tall pillars, kept out of the 2 mm flex zone below the
  heads so the tips bend and snap off at the part;
- a pad under each pillar, the pads joined by flat bars, so the raft and
  supports come off as one piece.

The report re-slices model plus supports and lists anything that would still
start printing in mid-air (`validation.floating_islands`, which should be 0).

![Supports generated for the armadillo](docs/armadillo_supported_preview.png)

### Overlap coupon (please run this first)

FruitSlicer exports the model and its supports as one STL made of overlapping
closed shells, which the printer's slicer must merge. `coupon overlap` builds a
small test piece: a raft, nine supports whose tips sink 0.2 mm into a plate, a
box buried inside the plate, and two crossing bars on top. Load
`overlap_coupon.stl` in **Elegoo SatelLite** and **CHITUBOX**, then scrub the
slice preview to the heights listed in the command's output. A correct slicer
shows solid material at each height (compare with the reference PNGs). Rings
around the tips, a square hole in the plate, or a hollow cross mean that the
slicer fills shells even-odd, and FruitSlicer will need to export a merged mesh
instead.

## Licence

Not chosen yet. Until it is, no code is copied from AGPL projects (PrusaSlicer,
UVtools); their algorithms are re-implemented from their descriptions.
