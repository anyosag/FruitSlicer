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
| Mesh repair on load: holes, cracks, unjoined seams, wrong-way and doubled faces (`repair`) | done |
| Desktop app for macOS: open and check, orient, supports, layers, save a checked `.goo` | early build, downloadable from CI; calibration and hollowing next |
| Grey-level tips (weaker contacts, smaller marks) | next |

## Build

Requires CMake ≥ 3.24 and a C++20 compiler (GCC 12+, Clang 15+, or Apple
Clang from current Xcode command line tools; CI builds on Ubuntu and on
macOS/Apple Silicon). Dependencies
(Clipper2, Manifold, CLI11, nlohmann/json, Catch2) are fetched by CMake.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Desktop app (early build)

A Mac app is being built in stages on top of the same engine. Today it
opens STL, OBJ and 3MF files and checks them: closed shells, inside-out
parts, hollows, low points, and whether the model fits the Saturn 4 Ultra
16K's plate. A file that is not watertight is **repaired as it opens** (see
[Mesh repair](#mesh-repair)): the check says what was done, in amber when a
patch is big enough to be worth a look or when the file could not be closed;
the patches are tinted on the model in the Model step, and **Show the
patch** turns the view to the biggest. A saved job keeps the repaired mesh
and what was repaired. You then choose its size (full, the largest that fits, a custom
percentage, or ×25.4 for models drawn in inches), rotation, and whether it
stands raised on supports or flat on the plate. Edits undo with ⌘Z, and ⌘/
lists the shortcuts. **⌘S saves the job** (a `.fruitjob` file): the model's
own copy, how it is placed and stands, the support settings and tips, and
the print settings (with the SatelLite file's header, if one gave them),
and a small picture of how it sits. Opening a job brings its supports back
as they were. Home shows the open job with its next step (**Continue**),
recent jobs as cards with that picture, their state, parts, tips and
support volume, and **Before you print**: what FruitSlicer has checked and
what it hasn't (the printer, where the print settings come from, the image
orientation check, and whether the support values are measured on your
resin or still the starting ones). With nothing open it offers the demo
bracket. If a job's model file has moved, the job still opens from its copy
(or you point it at the file). Closing or opening something else asks
first when there are unsaved changes. The Orient step scores how the model sits (tips, island
tips, biggest layer, height, layers short of hold), lets you pick the sides
people will see, and searches 65 directions for better ways to stand it,
compared side by side with how it sits now; using one is undoable. The
Supports step generates tips and the support tree from three quick
controls, named for what they set rather than for an outcome (the planner
has rules that place tips, and the part decides which rule applies, so a
"hold" or "amount" dial is dead wherever another rule decides). **How many**
has five stops; the middle is the resin's own values ("Starting values"
until a resin measures them), and the others move the lean angle together
with what would otherwise block it: towards More the spacing and the
short-ledge rule (a 1 mm ledge over 1 mm is itself 45°, so a lower angle
alone adds almost nothing), towards Fewer how close a tip sits to a free
fresh edge. A lighter stop never keeps more tips than the stop above it.
Under the stops the panel shows the tips this job would get at each,
counted in the background, and says when a stop changes nothing here.
**Tip size** (0.3, 0.4, 0.6 mm) is the usual tip; the sizes for spots in
mid-air and near edges move with it, so it reaches every tip. **Look
further for a way down** lets a blocked support walk 30 mm instead of 15
before it stands on the model (more resin; routes that long are not printed
yet). Trunks and braces, once a slider, is a row under Advanced settings,
next to a Tip spacing row that can also set tips wider apart than the
borrowed reach. Jobs saved with the three sliders these replaced (Hold,
Amount, Sturdiness) open with the same settings, written as the tip size
and values of their own. Faces that need tips are tinted
first; the run shows its stages with Cancel, and then the tree in 3D with
three views (Supports, Tips only, Why), why any tip you click is there, the
checks (nothing starts in mid-air, the raft comes off in one piece, no
support stands on the model) and a drawer of things to review. Faces that
only tips the tree could not reach would have held block saving ("Spots no
support reaches", with Show). Changing
the model afterwards marks the supports out of date. So does, with no raft,
a change to the resin's bottom layers, layer height or height correction that
would move a tip: the planner notes which counts of the model's layers in the
bottom layers its tips are right for, the job keeps that, and other changes
to those settings leave the supports as they are. **Edit tips by hand**
adds tips where you click the model, moves (drag), deletes, resizes and pins
them, and draws no-tip zones. Every edit is listed and can be undone on its
own (changing a tip of your own changes the edit that made it), is kept in
the model's own coordinates and is saved with the job. An edit applies on top
of the planner's tips straight away; when the tips are planned again
(regenerating, or after turning the model) the planner works around your
tips and keeps its own out of the zones, including the 1.8 mm one a deleted
tip leaves. **Advanced settings**
lists every value the tip planner and the tree builder use (29 rows, from
tip sizes and how much a tip holds to braces and the raft) with where each
comes from: built in, borrowed from PrusaSlicer until you calibrate, or an
assumption not measured yet. You can change them; your values are marked,
stay with you for the next model, and **Reset all** brings back
FruitSlicer's own. The Layers step cuts
the model and its supports into the printer's layers and shows any of them
at the printer's own pixel shape (14 × 19 µm), with the layer below, the
area of every layer by height, and checks on the whole print (nothing in
mid-air, suction cups and resin traps); ↑ ↓, Page Up/Down and Home/End move
through the layers. The Print step checks the job before saving (nothing in
mid-air, model and supports inside the build volume, a raft at least as
thick as the bottom and transition layers) and takes exposure and motion
settings from a `.goo` that SatelLite saved for your printer, or exposure
times you type. It shows the print time, resin and layer count. **Save
.goo** writes to a USB drive it finds, or a folder you choose; if your
SatelLite file uses another layer height, the layers are cut again at that
height so the exposures fit. Then it reads the whole file back: every
layer's checksum, no grey pixels, and the decoded volume against what was
written. The drive can be ejected from the app, and **Export STL** writes
the model with its supports for another slicer. Until the image-orientation
check has confirmed it, the app asks once per launch before saving, because
a wrong guess prints mirrored.

**Calibrate** holds the printer checks. The **image orientation check** takes
five minutes and no resin: save the F test piece (`f.stl`), slice it in
SatelLite as it is, and choose SatelLite's `.goo`. FruitSlicer slices the same
F with that file's settings, lines up the two images, shows them side by side
with how well each flip overlaps, and **Use this for my printer** applies the
fix to every print file from then on (the warnings then go away). The
**Printer** page shows the printer, the orientation in use and the side the
tilting vat hinges on; once the orientation is confirmed, supports get up to
+25 % hold on the side away from the hinge. Both are kept between launches in
`settings.json` in the app's data folder.

The first launch runs **setup**: the printer, the print settings of the resin
new jobs start with (from a SatelLite `.goo`, whose header FruitSlicer keeps a
copy of, or exposures typed in), the image orientation check, and calibration
now or later. A `.goo` made for another printer is refused, with the reason,
here and on the Print step. **Resins** keeps a library: each resin's print
settings and the support values measured for it, imported from a profile that
`fruitslicer fit` wrote, each shown as measured, borrowed or FruitSlicer's own.
A job is printed in one resin (the chip at the top of the job switches it) and
keeps its own copy of that resin's values, so changing the resin later never
changes a saved job; switching makes the supports out of date. Tip sizes are
matched by diameter, so a profile that measured other sizes still means what
you chose.

The **resin test plates** (tips, spacing, islands) are on Calibrate, for the
resin new jobs use. Each opens a preview: the build plate from above as the
plate is made, with its L marker, and any coupon's settings and what its
result would mean. **Save .goo to print** writes it with the resin's own print
settings and the orientation fix, read back like any print file (**Save STL**
is for slicing in SatelLite). FruitSlicer then keeps the plate's manifest and
score sheet with the resin in its own folder, so they stay put when the `.goo`
went onto a USB drive. **Enter results** scores the printed plate coupon by
coupon (held, partly or fell off; the mark left, 1 to 5; notes) without showing
any settings, and keeps each score as you go. A sheet filled in by hand can be
imported instead (`x.results.csv` with `x.json` beside it, as `fruitslicer
calibrate` writes them). Saving another plate over one with scores sets those
scores aside as `<kind>.earlier.results.csv`, never over them.

**Fit the scores** (on the score page, or the hub's *Resin profile* card)
shows what the resin can do: how much one tip of each size holds against
FruitSlicer's starting values (as a chart or a table), and every value the fit
sets with how it was found. A value the plates only bound (every coupon held,
so the real figure is higher) says so, and so does a guess (none held, even at
the lightest load). **Use for** puts the values into the resin, replacing only
the plates that were fitted; jobs saved before keep theirs. **Save profile
as…** writes them as a profile for `fruitslicer fit`'s users or another Mac.

The **A/B test** (Calibrate → A/B test) prints copies of the current job's
model on one plate: half with FruitSlicer's supports (A), half bare for
SatelLite's auto-supports (B), their places shuffled so the plate position
favours neither. It offers as many copies as fit, shows where each goes,
and saves two STL files (`<name>_A.stl` with the supports and the L marker,
`<name>_B.stl` bare) to load in SatelLite, which supports the B copies only.
FruitSlicer keeps which copy is where. After printing, **A/B results** scores
each copy (did it print whole, seconds to clean off its supports, the mark
left) with its side hidden, then **Reveal and compare** puts the two sides
next to each other. The verdict names a winner only where chance is an
unlikely explanation (exact tests: Fisher's for staying on, permutation
tests for marks and clean-up time, p < 0.05), and says how much the number
of copies can show: with 3 or fewer a side even a clean sweep could be
chance.

**Tools** (on the rail) has two. **Inspect a .goo** opens any print file and
shows its header in plain words (printer, image, exposures, waits, movement,
layers), the pictures the printer shows, and, on request, a read-back of
every layer: checksums, pixel values, the volume they add up to against the
header's, and the end of the file. **Compare two files** lists the header
settings that differ, says which of them change how the print cures, and
overlays a layer from each to see whether one is the other flipped; a shape
that looks the same flipped is said to be unable to show the orientation,
rather than called a match.

**Settings** (at the bottom of the rail) has the 3D view's
smoothing, how many processor cores slicing and saving may use, whether `.goo`
files go to a plugged-in USB drive, the resin for new jobs, the log file, and
**Run setup again**; FruitSlicer sends no usage data. **About** says
what has been checked and what not yet, and has the full licence text of
FruitSlicer and of every library and the typeface built into the app. It is
one universal app for Apple silicon and Intel Macs, macOS 14 or later.

**Download.** Every CI run builds it. On GitHub, open the run (a pull
request's *Checks* tab, or *Actions* → **CI**), scroll to *Artifacts*, and
download **FruitSlicer-macOS.zip**. Unzip it and move `FruitSlicer.app` to
Applications.

**First launch.** The app is signed ad hoc, not with an Apple Developer ID,
so macOS blocks it the first time:

1. Double-click FruitSlicer. macOS says it could not verify the app; click
   **Done**.
2. Within about an hour, open **System Settings → Privacy & Security**,
   scroll to **Security**, and click **Open Anyway** next to the FruitSlicer
   message.
3. Confirm with your login password. From then on it opens normally.

Since macOS 15, Control-click → Open no longer skips these steps. Each newly
downloaded build is checked again, so expect to repeat them after updating.
(For builds from this repository's CI only, `xattr -dr com.apple.quarantine
/Applications/FruitSlicer.app` in Terminal skips the check instead.)

**Building it yourself** (Xcode command-line tools, CMake and Ninja):

```sh
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DFRUIT_BUILD_APP=ON
cmake --build build-app --target fruitslicer_app
open build-app/src/gui/FruitSlicer.app
```

On Linux the same commands build `build-app/src/gui/fruitslicer_app` (X11),
which CI uses for screenshot tests.

**If something looks wrong:** *About* lists the graphics driver and display
scale, and the log is `~/Library/Application Support/FruitSlicer/fruitslicer.log`.
`/Applications/FruitSlicer.app/Contents/MacOS/FruitSlicer --self-test-gl`, run
in Terminal, checks the engine, the fonts and OpenGL without the interface.

**Screenshot tests.** The scripts in `tests/gui/*.json` drive the app
through the same commands as its buttons (`--script s.json --shots dir`; the
steps are listed in `src/gui/Script.h`), and CI compares the pictures with
`tests/gui/golden` using `tools/compare_screens.py`. A run also fails when
Dear ImGui reports a misuse. After an intended visual change, check the new
pictures (CI uploads them as *app-screens-linux*) and copy them over the
goldens. The app's document logic has its own tests (`fruit_gui_tests`).

## Usage

Every command reads STL (binary or text), OBJ and 3MF models.

```sh
# Mesh statistics: shells, manifoldness, volume, downward-facing minima
./build/src/fruitslicer info model.stl

# Place (rotate, centre, lift 5 mm), slice at 0.05 mm, report islands; optional debug PNGs
./build/src/fruitslicer slice model.stl --rotate 30 0 0 --elevate 5 --layer 0.05 \
    --png-dir out/layers --every 20 --json out/report.json

# Week-1 compatibility test: overlapping shells (see below)
./build/src/fruitslicer coupon overlap -o overlap_coupon.stl --png-dir out/overlap_ref
```

### Mesh repair

A mesh slices into layers with a clear inside and outside when every edge is
used as often one way round as the other. Edges where that fails are *open*:
at holes, along seams that were never joined, and where a face is turned the
wrong way. Every command, and the app, repairs a model as it loads; a mesh
with no open edges is left exactly as it is. The repair never moves a point
of the model. It only

- turns faces so that neighbours run the same way round, and turns a piece
  it had to repair to face outward as a whole;
- drops faces that are there twice, where that closes edges;
- adds faces over each hole, and across each gap between open edges that run
  within 2 ‰ of the model's size of each other (0.1 mm at most): the patch
  with the least area, which is the flat cap for a flat hole and the strip
  that zips a crack shut;
- drops loose surfaces that have no thickness.

```sh
# What loading repairs in a file; -o writes the repaired mesh. Exit status 1 if it is still not watertight.
./build/src/fruitslicer repair model.stl -o repaired.stl
```

`info` reports `"watertight"` and the same `"repair"` block. The other
commands say on stderr when a file needed repair and put the block in their
JSON report, with sizes at the scale the model is placed at.

What it does not do:

- **It guesses nothing about shape.** A hole gets the flattest patch that
  fits: a ball with its top cut off stays cut off, with a flat lid.
- **Surfaces further apart than the seam gap are not joined.** Each is
  closed on its own, and a flat one, which then holds no volume, is dropped.
  A file that is all loose surfaces is left as it is and reported as not
  watertight: that one needs fixing in the modelling program.
- **A broken hollow prints solid.** A hollow is a shell that faces inward,
  and a shell the repair had to close can't be told from one that is simply
  inside out, so it is turned outward. Hollows whose shell needed no repair
  are kept.
- **Parts that pass through each other are left alone**: the slicer's
  non-zero union already prints them as one.

The Stanford bunny scan is an example: it has five holes in its underside
(223 open edges; the largest is 13.2 mm across with the bunny 46 mm tall).
As loaded, 53 of its 927 layers at 0.05 mm came out more than 1 % off in
area, one of them with 294 mm² in place of 641 mm². Repaired, the volume of
the layers equals the volume of the mesh (20.38 ml).

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
   never "covers" across a slot or onto a neighbouring finger. A face just past
   the angle grows only a little past it each layer (0.0036 mm at 47° on
   0.05 mm layers), so on the part's outside that growth is also measured
   against what was there 1 mm below; before, faces up to about 47.5° got no
   tips. Within 0.5° of the angle is taken for the outlines' rounding. Ceilings
   over holes (hollows, cups, bores) keep the layer-by-layer rule: a hollow's
   ceiling is shaped at the angle and its rounding closes it at 45.6° on average;
2. **capacity**: for every connected piece at every layer, its tips together
   can hold its cross-section under peel (seed: one 0.4 mm tip per 11.1 mm²,
   scaled by tip area);
3. **islands**: every island gets 1 tip (≤ 1.7 mm), 2 tips (≤ 6.5 mm) or at
   least 3, using the mesh minimum as the contact. Islands too small to hold a
   tip that join the model almost at once are left alone and reported;
   Standing on the plate (no raft), a part gets no tip in what the bottom
   layers print of it: their count, less the height correction's layers
   (copies of the first layer that go under the model and are bottom layers
   themselves), times the print's layer height, all from the job's resin
   ((6 − 3) × 0.03 mm = 0.09 mm, say; (4 − 2) × 0.05 mm = 0.1 mm without
   one). Those layers are cured many times longer than the rest, which
   hardens the resin between them and the plate too, so the plate holds them
   and a tip there would only print as part of the model (reasoning, not
   measured). The transition layers after them are not counted, because
   their exposure falls to the normal one. A separate piece that starts that
   low is held the same way, so it counts as standing on the plate: it gets
   no tips, the check for parts in mid-air passes it, and the replay of the
   print takes it as held. One that starts just above those layers is in
   mid-air and gets its tips;
4. **fresh edges**: where a layer reaches more than 0.5 mm past the layer
   before it, that part is one cured skin with nothing behind it: the first
   layer of a flat face, the ends of the sliver a tilted face starts as, the
   rim of a shelf. Where its edge is free (the part's own outline, not the
   side that faces what holds it, nor a ceiling with walls all round) it has
   a tip within 1.6 mm, closer than the coverage radius asks. Slow growth (a
   dome widening, a steady slope) is not touched. This is reasoning, not a
   measured value ("Edges with nothing behind them" under Advanced settings;
   0 turns it off). It adds little on most models (a lid 462 to 494 tips, a
   cup 318 to 320, a ball none) and most where a wide flat face prints
   level, which is the hardest thing to hold.

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
| Stanford bunny | 198 → 99 (−50 %) | 842 → 697 mm² | 12 → 5 |

Each search takes about 30 s at 0.1 mm evaluation layers. The bunny's row is
for the repaired mesh. Before mesh repair existed it read 214 → 177 points
and 62 → 61 island tips: the holes in the scan's underside broke the layers
they pass through, and the pieces counted as islands. In the input
orientation that accounts for 50 of the 62 island tips (the same run with
the repair switched off gives 214 points and 62, with it 198 and 12).

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
  surface under the tip is not. Where the model is so close under the tip
  that no head fits at all (inside a thread, between ribs), a straight column
  as wide as the tip joins the tip to the face up to 3 mm below it instead;
- a vertical pillar to the raft. Up to three heads share one pillar through
  45° bridges. A head with no clear vertical drop walks diagonally outwards
  until it finds one. Where none is in reach, its support stands on the model
  below instead, however far down, on a foot as wide as its tip; it steps
  aside when the straight drop would brush a wall or land on a face steeper
  than 60°. A tip whose support would stand within 0.75 mm of one that
  already stands there joins that support, and their shared foot is as wide
  as their tips together (two feet on one spot would print as one, carrying
  both tips on a contact made for one). And a tip whose head ends in the
  very place another head does (their junctions within 0.25 mm), when that
  one has a way to the raft, goes down with it instead of standing on the
  model. Standing on the model is reported because it marks an upper surface
  (`anchor_drop` limits how far down the model may be; 0 turns it off);
- a gap to the model everywhere but at the tip: shafts, bridges, braces and
  feet stay 0.15 mm clear of it. A pillar's foot flares to 3 mm where it
  stands, but only as far as the room beside the model allows, so a pillar
  next to a wall gets a narrower foot instead of one that cuts into the wall.
  A foot that stands on the plate itself (no raft) keeps 0.5 mm, because the
  first layers are cured much longer and come out wider than drawn (an
  assumption, not measured); one with no room for that anywhere is built at
  the shaft's gap and reported ("Feet close to the model"). A tip so low that
  its head stands on the plate itself follows the same rule: the head leans
  away from a wall beside it, or its base narrows down to the tip's own
  width, and it is reported when even that leaves it closer than 0.5 mm;
- bracing between tall pillars, those on the raft with each other and those
  on the model with each other, kept out of the 2 mm flex zone below the
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

A tip with no head of its planned size that finds a way down is first built
a size or two smaller (0.4, then 0.3 mm: a narrower head and shaft), then, at
the planned size and smaller, half a millimetre to 1.5 mm across the same
face; the report counts them (`smaller_heads`, `moved_heads`) and the tip's
card in the app says so. A tip the tree still cannot build is dropped, and
`tree_warnings` counts why. The planner's own coverage
and hold tests then run again with only the tips that were built:
`validation.unsupported` counts the dropped tips that were the only cover of
some face (`tips`, with where the first 20 were), the surface samples left with
no tip in reach (`samples`), and the layers short of hold without them
(`short_layers`). A support that stands on the model ties two places on the
print together: it holds a ceiling up but adds nothing to what holds the print
on the raft, so `short_layers_from_raft` counts the layers that are short once
those tips are left out of the hold. All four should be 0. They are not the same as
`point_stats.uncovered_samples`, which is the plan before the tree. With the
default settings a tip is dropped only where no head fits and nothing is close
enough below for a column, or no spot to stand on is found; with `anchor_drop` lowered, turning the model or a longer `route`
brings them down.

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
  no elevation and no raft. The base is held by the plate, and so is what
  starts inside the bottom layers (a face or a separate piece), so only
  overhangs above them get tips.

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
measurement. A tip size that never let go gets 70 % of the heaviest load
tried, a floor (half would let go somewhere above it); one that never held
gets half the lightest load, a guess. `--profile` works with `points`, `support` and `orient`, and
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

**Still to confirm: image orientation.** In the app: Calibrate → Image
orientation (it runs the steps below). From the command line:

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

In the app, choose **Save to: Printer over Wi-Fi** on the Print step. The
app writes the file, reads it back to check it, then lists the printers that
answer on the network; you can also add one by its address, which is kept
for next time. Sending shows its progress and can be cancelled, and the
printer is told to drop the partial file. **Start printing when it arrives**
is off by default; turning it on says what to check at the printer first.

The first search makes macOS ask whether FruitSlicer may find devices on
your local network (macOS 15 and later). If that was declined, allow it in
System Settings → Privacy & Security → Local Network and search again.
Because the app is signed ad hoc rather than with an Apple Developer ID,
macOS may ask again after an update ([TN3179](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)).

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
