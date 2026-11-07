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
| M3 — orientation search (two-stage: mesh proxies over 64 directions, full generator on the 6 best) | done |
| M5 (part) — branching: nearby pillars merge into trunks below the flex zone | done |
| Calibration loop: `fit` result sheets → resin profile → `--profile` | done |
| Safety: suction-cup / resin-trap warnings, no supports in closed cavities, no-trap bracing, hinge bias, on-plate mode | done |
| Same-plate A/B helper for the go/no-go (`abplate`) | done |
| Physical validation: overlap coupon, calibration plates, A/B plate | **needs your printer** |
| Native `.goo` writer/reader (`goo`, `goo-info`, `goo-compare`, orientation coupon) | done; header verified against SatelLite 1.0.2.43's 16K file; image orientation pending the F comparison |
| macOS (Apple Silicon) build in CI | done |
| Grey-level tips (weaker contacts, smaller marks) | next |

## Build

Requires CMake ≥ 3.24 and a C++20 compiler (GCC 12+, Clang 15+, or Apple
Clang from current Xcode command line tools; CI builds on Ubuntu and on
macOS/Apple Silicon). Dependencies
(Clipper2, CLI11, nlohmann/json, Catch2) are fetched by CMake.

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

### Orientation

```sh
./build/src/fruitslicer orient model.stl --visible-from 0 -1 0 --preview-dir out/orient
```

The search tries the input orientation and 64 build directions spread over a
sphere. Each is first scored with cheap mesh measures: area that needs
support, number of downward minima (islands), peak layer area, height, and
area needing support on faces visible from the `--visible-from` directions
(given in the model's own frame). The six best, plus the input orientation,
then go through the real point generator. The command prints the best
`--rotate` values next to the input orientation's numbers.

| Model | Points (input → best) | Peak layer area | Island tips |
|---|---|---|---|
| Armadillo | 239 → 204 (−15 %) | 750 → 551 mm² | 33 → 10 |
| Stanford bunny | 214 → 177 (−17 %) | 842 → 690 mm² | 62 → 61 |

Each search takes about 30 s at 0.1 mm evaluation layers.

### Supports and printable file

```sh
./build/src/fruitslicer support model.stl --rotate 30 0 0 -o supported.stl \
    --preview preview.png --report report.json
```

This writes the model and its supports as one STL to load into Elegoo
SatelLite or CHITUBOX for slicing. The points used are saved next to it
(`supported.stl.points.json`); edit them and pass them back with `--points`.
A points file records the placement it was made for: without `--rotate`,
`--elevate` or `--scale`, `support --points` uses that placement; a different
one is refused, since the points would no longer touch the model.
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
- branching: pillars within 8 mm merge where 45° branches from each meet,
  below the flex zone and below any bridge already on them. The trunk's
  cross-section is the sum of the two, and at most 6 tips share one trunk.
  On the armadillo this cut trunks reaching the raft from 82 to 47 and braces
  from 189 to 35 (`--no-merge` turns it off);
- a pad under each trunk, the pads joined by flat bars, so the raft and
  supports come off as one piece.

The report re-slices model plus supports and lists anything that would still
start printing in mid-air (`validation.floating_islands`, which should be 0).

Safety checks and options:

- **Suction cups and resin traps.** Hollows closed towards the plate but open
  towards the film are cups, and each peel pulls a vacuum through them.
  Fully closed hollows are traps. Both are reported with their height range
  and volume. Tips are never placed on the ceiling of a closed cavity, since
  they could not be removed.
- **No-trap bracing.** Two trunks are never braced when model material passes
  between them below the braces. The pillars, brace and raft would form a
  loop around it, and the supports could not come off (PrusaSlicer #14594).
- **`--hinge ±x|±y`.** Tells FruitSlicer which side of the plate the tilting
  vat's hinge is on. Peel stress is highest on the far side (Wu 2016), so tips
  there carry extra demand (`--peel-bias`, default 0.25; the right value is
  unmeasured).
- **`--on-plate`.** Prints flat-backed parts standing on the build plate, with
  no elevation and no raft. The base is held by the plate, so only overhangs
  get tips.

![Supports generated for the armadillo in its best orientation](docs/armadillo_supported_preview.png)

### From printed plates to a resin profile

After printing the plates, fill in the `result` column (`ok`, `failed` or
`partial`) and, for the tips plate, the `scar` column (1 = invisible …
5 = pit). Leave `scar` empty or 0 when not scored. Then:

```sh
./build/src/fruitslicer fit tips.stl.results.csv spacing.stl.results.csv islands.stl.results.csv \
    --resin "My resin" --layer 0.05 --exposure 2.5 --temp 30 -o my_resin.json
./build/src/fruitslicer support model.stl --profile my_resin.json -o supported.stl
```

`fit` estimates the following:

- **from the tips plate:** how much layer area each tip size holds (70 % of
  the load at which half the tips fail), the best penetration for each size,
  and a default tip (the lowest scar score among tips that hold at least
  4 mm²);
- **from the spacing plate:** the coverage radius (the widest tip pitch that
  held at every angle, divided by √2) and the critical overhang angle;
- **from the islands plate:** how large an island one or two tips can hold.

Estimates resting on incomplete data (nothing failed, or nothing held) are
listed under `notes` in the profile, so a bound is never mistaken for a
measurement. `--profile` works with `points`, `support` and `orient`, and
explicit command-line options still override it.

### Go/no-go: same-plate comparison with SatelLite

```sh
./build/src/fruitslicer abplate model.stl --copies 8 --profile my_resin.json -o ab
```

This writes `ab_A.stl` (copies with FruitSlicer supports, plus an L marker in
the corner cell) and `ab_B.stl` (bare copies). Import both into SatelLite,
add its auto-supports to the B copies only, and print them together. Which
grid cell gets which arm is shuffled with a fixed seed (`ab.json`). Record
each copy in `ab.results.csv`:

- result: ok / failed / partial;
- scar, 1–5, judged blind under raking light;
- seconds to remove the supports.

Count each arm's support contacts in the slicer. The plan's bar is: at the
same failure rate, at least 25 % fewer contacts or visibly smaller marks.

### Printing with FruitSlicer's own `.goo`

FruitSlicer writes Elegoo's `.goo` print files directly, so SatelLite is no
longer needed in the loop. The easiest way to get your exposure settings
right is to copy them from any file SatelLite exported for your printer:

```sh
./build/src/fruitslicer support model.stl --rotate 30 0 0 -o supported.stl
./build/src/fruitslicer goo supported.stl -o model.goo --settings-from my_satellite_export.goo
./build/src/fruitslicer goo-info model.goo --decode     # header, checksums, volume
```

Without a sample, pass `--layer`, `--exposure` and `--bottom-exposure`
yourself. Bottom and transition layers then default to the printer's 4 + 4;
`--bottom-layers` and `--transition-layers` change them.

**Checked against a 16K file exported by ELEGOO SatelLite 1.0.2.43:**

- **Format.** Same `V3.0` layout, header size and footer. FruitSlicer's
  decoder reads all of SatelLite's layers with valid checksums.
- **Header.** With `--settings-from`, FruitSlicer's header matches
  SatelLite's in every field except the software name (`goo-compare` checks
  this). That includes:
  - printer name and type,
  - anti-aliasing off (pure black/white pixels),
  - the Y-mirror flag,
  - the 5 s wait after each retract,
  - lift/retract 0.05,
  - SatelLite's transition exposure ramp.
- **Print time.** The estimate matches SatelLite's.

**Still to confirm: image orientation.**

1. Run `fruitslicer coupon orientation -o f.stl`.
2. Slice `f.stl` in SatelLite.
3. Run `fruitslicer goo f.stl -o f_fruit.goo --settings-from <SatelLite's f.goo>`.
4. Run `fruitslicer goo-compare f_fruit.goo <SatelLite's f.goo>`.

This compares an asymmetric "F" under every flip. "orientation matches" means
FruitSlicer's files print the right way round; otherwise it tells you which
`--flip-x` / `--flip-y` to use.

`--aa N` writes anti-aliased grey pixels, but grey on the 16K is untested,
so the default is off, as in SatelLite.

### Sending over Wi-Fi

`fruitslicer-printer` finds the printer on the local network, shows its
status, uploads a `.goo` and, only when asked, starts it (SDCP V3; notes and
sources in [`docs/wifi-sdcp.md`](docs/wifi-sdcp.md)). Not yet tried on a real
printer.

```sh
./build/src/net/fruitslicer-printer discover
./build/src/net/fruitslicer-printer status 192.168.1.50
./build/src/net/fruitslicer-printer send 192.168.1.50 model.goo            # upload only
./build/src/net/fruitslicer-printer send 192.168.1.50 model.goo --start    # asks before printing
```

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

FruitSlicer is free software, released under the **GNU Affero General Public
License, version 3 or (at your option) any later version**
(`AGPL-3.0-or-later`); see [`LICENSE`](LICENSE). Every source file carries an
SPDX identifier.

This licence is compatible with PrusaSlicer ("AGPLv3 or higher") and UVtools
(AGPL-3.0), so their code may be ported into FruitSlicer as long as their
copyright notices are kept and the source file says where the code came from.
