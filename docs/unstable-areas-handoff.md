# Handoff: the unstable-areas model (the replay of the print)

For any Claude session that looks into the model of what may not print, or
changes it. Written 2026-11-16 by the session that built most of it, when main
was at `0228445`. Line numbers drift, so look things up by function name.

The model answers two questions for a print on the user's ELEGOO Saturn 4 Ultra
16K (a tilting vat), in their resin, Phrozen Aqua Hyperfine:
- Which places are likely not to print, and how likely is that?
- Does any support tip carry more than it is trusted with?

It has three users:
- the replay engine itself (`src/stability/`);
- the support planner's "each tip's own load" rule (`Generator::relieve`);
- the app's Risk view and checks (`src/gui/Risk.cpp`, `src/gui/screens/RiskParts.cpp`).

## 0. Rules that come first

These are the user's standing rules for every session on this project.

- **Never print.** Never send anything to the printer, and never use Wi-Fi
  sending (`build/src/net/fruitslicer-printer` and the app's Send). Print files
  may be written to a temp folder in tests.
- **Files you must not touch:**
  - `testing/findings.md`, `out/` and `assembly2.goo`;
  - other sessions' branches and worktrees;
  - `build-app/`: the user runs the app from there and rebuilds it themselves.
    Never build in it.
- **Work in a worktree of your own.** Configure it offline with
  `bash "/Users/anyosag/Documents/Claude Sandbox/FruitSlicer/.worktrees/_deps/wt-configure.sh" <your worktree>`.
  This makes `<worktree>/build`. Then run `cmake --build build` and
  `ctest --test-dir build -j8 --output-on-failure`. The first build takes
  minutes: run it in the background.
- **Never skip or disable a test.**
- **No pull requests.** Finished, tested work is merged into `main` and pushed.
  End commit messages with the `Co-Authored-By` line the harness gives you.
- **Screenshot goldens** (`tests/gui/golden`) come only from CI's Linux renders.
  Push; `app-linux` lists changed pictures as a warning and in the run's
  summary without failing (only a screenshot the app didn't write fails it).
  Download its `app-screens-linux` artifact, look at every changed picture,
  then copy those over the goldens. Never copy Mac
  screenshots. CI's macOS jobs run on a self-hosted runner on this Mac, which
  sleeps after a minute idle: run `caffeinate -i -t 3000 &` while you wait.
- **App wording:** plain words that say what has been checked and what hasn't.
  No jargon, no marketing.
- **The written model stays untracked.** `docs/unstable-areas-model.md` lives in
  the main checkout (`/Users/anyosag/Documents/Claude Sandbox/FruitSlicer`), on
  purpose not in git. Update it when the model changes; never `git add` it
  unless the user says so.
- **Builds without `-DFRUIT_OPENS_JOB_FILES=ON`** are the bundle
  `io.github.anyosag.fruitslicer.dev` and declare no file types. If you run a
  build of older code, unregister it afterwards:
  `/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -u <that FruitSlicer.app>`.
- **The user's private models** `~/lidv12.STL` and `~/eyecupv3.STL` are the
  real test cases. Never upload them anywhere.

## 1. Where to read first

| What | Where |
|---|---|
| The written model: physics, sources, every decision and what it changed (sections 1–15) | `docs/unstable-areas-model.md` in the **main checkout** (untracked) |
| The dated log of changes, with numbers before and after | `docs/resin-support-plan.md`, "Implementation status" (entries from 2026-11-12 on) |
| The planner's rules in user-facing words (rule 5 is each tip's own load) | `README.md`, the list of support rules |
| The engine | `src/stability/Stability.h` (config, inputs, results; read every comment), `Stability.cpp` |
| The report the CLI writes | `src/stability/StabilityReport.cpp` |
| The planner's rule | `src/support/PointGenerator.cpp`, `Generator::relieve`; `SupportConfig.h` (`tip_load_limit`, `relief_rounds`, `kMeasuredTipLoadLimit`) |
| How long each support will be, for the planner | `src/support/SupportTree.cpp`, `supportLengthsOnModel` |
| The app | `src/gui/Risk.cpp` (`checkRisk`, `findRiskPlaces`, `riskStrengthsNote`, heatmap); `src/gui/screens/RiskParts.cpp` (the Checks card) |
| The CLI | `src/app/main.cpp`, `cmdStability` and `StabilityCli` |
| Tests | `tests/test_stability.cpp` (53 cases), `tests/test_points.cpp` (`[relief]`), `tests/test_tree.cpp`, `tests/gui/test_risk_logic.cpp`, `tests/gui/test_quick_support_logic.cpp` |
| Notes shared between this group of sessions | `/Users/anyosag/Library/Application Support/Claude/side-session-notes/local_a54bf105-293c-4fa0-8a3b-6dbfc777bd22/` (`model-flaw-hunt-2026-11-15.md`, `planner-each-tips-own-load.md`) |

## 2. How the replay works

Units:
- **Load** is in mm² of layer: the pull on that much resin.
- **holds** is the load at which half of such joints break.
- **closeness** `u = load / holds`.
- **Chance of breaking** is `1 − 2^(−u^m)`, a Weibull curve with steepness
  m = 7 (published 5 to 11).

`Analysis::run()` runs these steps in order.

1. **Parts and links.** `buildParts` and `buildLinks`:
   - Every connected part of every layer is a part, of the model or of the
     supports.
   - Parts in neighbouring layers that overlap are linked. A link's kind is
     Model, Rod, Tip (support under the model), Foot (support standing on the
     model) or Dead.
2. **Strengths.** `sizeJoints`:
   - A model joint holds `bulk_factor × area × per_mm2`.
   - A tip holds from the tip table. The table gives the *trusted* load, for
     example 11.1 mm² for a 0.4 mm tip, borrowed from PrusaSlicer. Half let go
     at `capacity / usable`: usable is 1/3 for the borrowed table
     (`kBorrowedMargin`) and 0.7 for a fitted one (`kFittedMargin`).
   - The joints of one tip, layer on layer, are one group that holds and
     lets go together.
   - A tip whose size is given holds `tipHolds(d) × min(1, touching / face)`.
     Touching is summed over its layers, so a tip sunk into the model counts
     as whole.
   - A tip nobody gave a size for is sized by its largest joint.
   - A support lying along the model for more than 0.6 mm is a seam: each of
     its joints holds by itself.
   - A tip that holds nothing gets `1e-9`, so it lets go at once.
3. **Rods.** `buildRods`: a leaning rod gives more. `eff = A / √(1+t²)`,
   `k = A / (1+t²)²`, with t capped at tan 60° (the steepest head the app
   allows).
4. **Bodies, depths, sheets, pulls.** `buildBodies`, `buildDepths`
   (`workOutDepths`: the lattice of how many layers of model stand under each
   point), `buildSheets`, `buildPulls`.
   - Pulls include the peel bias toward the side away from the hinge.
   - Cups (`HollowReport::cup_regions`) pull `cup_pull × area`. Only layers with
     a cup open to the vat pull this way.
5. **Each layer's pull.** `step(k)`, the core.
   - **Thin model:** the part of a patch's pull given outright to the tips near
     it is `1/(1 + (solid/slab)^4) × 1/(1 + far²)`, where solid is the model's
     depth under the patch. With slab 2 mm that is half at 2 mm and 1/17 at
     4 mm.
   - "Near" reaches `len = max(0.3, share_reach × solid^0.75)`.
   - **Tips whose faces overlap** (centres closer than their two radii) are
     one hold in that share (`group_one_`, set where tips get their place).
     The set's pull is split by how much each tip touches the model. Before
     2026-11-20 a pair 0.2 mm apart was a lever: a tip that barely touched
     came out far past where half let go (the CHamber job's spikes at 28 mm).
   - **Only tips under the same part:** a tip takes thin model's pull outright
     only when the model goes on straight over the tip into that same part of
     the layer, or ended in a part this one stands on (`buildColumns`,
     `underPart`). Before 2026-11-23 any tip within reach did. On the TubeRack
     job, two arms of a wall's lattice leaving a node handed their pull to a
     0.2 mm tip at the end of the divider, 3 to 4 mm away across a gap, at
     2.1 times the arms' own pull: the chart's 204 % at 54 mm.
   - **Stiff model:** the rest goes to the whole piece by stiffness, as springs
     (`settle`: force and both moments balanced).
   - The worst load of every joint is recorded. Joints are walked in runs
     (`findRuns`), so a stem breaks once, not once per layer.
6. **Fresh skins.** `findSkins`: a layer's part beyond what holds it, its
   parents in the layer below (model or support tips).
   - **Between holds** (held all round, like a plate): the half-tear reach is
     `skin_half_reach`, the planner's spacing radius 3.2 mm / √margin, so 5.54 mm
     built-in.
   - **At a free edge** (a point nearer the part's own outline than any hold,
     so it hangs like a ledge): `skin_edge_reach`, the planner's `skin_reach`
     1.6 mm / √margin, so 2.77 mm built-in.
   - Results are bands at chances of 1, 5, 20, 50 and 90 %, nested by reach.
   - Neither reach depends on layer height or on the pull. The formula
     (doc section 3) says both should, but the cure depth is unknown.
   - Skins in the plate-held layers are not judged.
7. **Replays.** `runReplays`, 500 of them, seeded and deterministic.
   - **What can break:** every tip or foot. Any other joint only if its worst
     closeness reaches a 1-in-10,000 chance (`floor_u_`), or once tips there
     can go.
   - **Each replay** draws every joint's strength, `xi = (−log2(1−U))^(1/m)`,
     then runs an event loop. The earliest joint to pass its strength lets go;
     pieces are rejoined at layer k (union-find over `pieces`); whatever no
     longer reaches the plate falls, with everything that rests on it.
   - **A joint that let go while the piece above is still held** passes its
     load to the joints that take over:
     - **Who takes it:** the joints leading down from that piece, weighted by
       what each carries and by a Gaussian in distance, with length
       `len(over)`.
     - **How a stiff piece shares it:** by the lever rule (`settle`), mixed
       with a plain share in proportion as far as the piece is thin:
       `thin = 1/(1+(over/slab)^4)`.
     - **`over`:** the model standing over that joint, layer on layer
       (`columnOver`), not the height printed since.
     - **Negative lever shares** are dropped on purpose: the far side is not
       let off.
     - **Later loads:** a taker carries its share of the load the lost joint
       *would* have carried later (`Given`, `carriesAt`, `firstOverAll`). Only
       the smallest shares, together under 5 % of the load, are frozen at the
       load when given (`kTracked`). Each joint keeps at most 8 sources
       (`kMostTracked`).
   - **Whole print:** a replay counts as whole when less than 1 mm³ of model
     (`kWorthLosing`) is lost. Lost here means lost through joints, plus parts
     that start in mid-air (lost every time), plus fresh skins. Each skin is
     drawn once per replay from its bands, with one strength for the whole
     skin.
   - **When nothing can break** (`nc == 0`), it returns early. It still counts
     mid-air parts and skins in the whole-print chance.
8. **Listing.** `findSpots`, `listTips`, `summarise`.
   - Weak places are listed from a chance of 1 in 20.
   - A tip contact under 0.02 mm² (`kSliverContact`) with nothing going with
     it is not listed.
   - A skin whose likely tear is under 0.1 mm³ (`kCountsAsLost`) is counted in
     `small_skins`, not listed. Its chance still colours the heatmap.

**Standalone mode** (`standaloneTipLoads`, `StabilityInput::standalone_tips`)
is the planner's question: what would each tip carry if it stood on a rod of
its own? Rods are `rod_diameter` 1.0 mm, ending in a `head_length` 1.2 mm cone,
and as long as `TipContact::rod` says. No replays run in this mode.

## 3. The planner's rule (each tip's own load)

`Generator::relieve`, rule 5 in the README.
- Up to `relief_rounds` (16) rounds. Each round works out every tip's standalone
  load. A tip over `kReliefSlack × tip_load_limit × capacity(d)`, that is
  1.1 × 1 × capacity (or 0.7 × capacity with a measured table), gets one size
  bigger if that carries it. Otherwise it gets tips beside it (`reliefSpot`).
  A tip chosen small on purpose (at an edge or on a visible face) gets tips
  beside it instead, and a size bigger only where there's no room for one.
- Support lengths are asked once, before any change
  (`PointGenOptions::support_lengths`, answered by `supportLengthsOnModel`):
  - a support that stands on the model gets its length, plus the extra give
    of its narrow foot;
  - a support that reaches the raft gets 0.
- An added tip borrows its neighbour's length when the model is under it.
- `relief_left` (shown as "still over") is counted after the last round, even
  when the rounds run out.
- The planner cannot see partial contacts: it plans before the supports exist.
  The replay does see them.

## 4. The app

`checkRisk` (Risk.cpp):
- slices the model and the supports;
- builds `stabilityConfig(planner)` with the Hold slider at its default (Hold
  changes what the planner asks of a tip, not how strong a tip is), with the
  supports' `plate_hold_height`;
- replays, then `findRiskPlaces`.

What is shown, and how:
- **Bands:** places show from 1 in 20, "likely" from 1 in 5, "very likely" from
  1 in 2.
- **Tips** are one place together. A tip shows at 1 in 20 or more *and* when it
  carries more than 1.1 times its trust. Exception: when no tip passes that but
  the model goes with tips (1 in 20 or more), all tips at 1 in 20 or more show.
- **The note** under the Risk check (`riskStrengthsNote`) names the borrowed or
  measured strengths and the sharing guess, and the suction guess where
  `stats.cup_layers > 0`.
- **The heatmap** (`paintHeat`, `HeatOf::Lost` and `HeatOf::Breaks`) was built
  by the UI session in `0c3e7ab`.

## 5. The CLI

```bash
./build/src/fruitslicer stability ~/lidv12.STL --threads 4 --report /tmp/x/lid.json --all-tips
```

**Options:**
- **For the replay's own guesses:** `--thin-slab`, `--share-reach`,
  `--skin-reach` (between holds), `--edge-reach` (free edge), `--tip-margin`,
  `--steepness`, `--resin`, `--cup-pull`, `--shape-effect`, `--replays`,
  `--seed`.
  - Values out of range are refused.
  - `-1` (the default) means not given.
  - For the reaches, 0 means not judged.
- **For the planner:** `--tip-limit`, `--hinge`, `--peel-bias`, `--on-plate`,
  `--raft`, `--profile`.
- **To replay the same supports with other values:** `--save-points` writes
  the supports' points; `--points` reads them back.

**The report** (`stability` in the JSON):
- `rests_on`: every assumed value used.
- `spots` and `tips`.
- `every_tip`, with `--all-tips`.
- `whole_print_chance`, `expected_loss_mm3`, `volume_at_risk_mm3`.
- `small_skins_not_listed`, `layers_with_cup_suction`.
- `profile_notes`: says when the profile has values typed on the Resins page.
  The CLI does **not** apply those; only the app does (section 8).

**To run the app's own path headless**, give it a JSON script, as the GUI
tests do:

```bash
./build/src/gui/FruitSlicer.app/Contents/MacOS/FruitSlicer --script <script.json> --shots <dir>
```

The log has lines like `supports: 627 tips ...` and
`risk: 0 places, 0 tips that may let go`. A minimal script for a real model:

```json
{"size": [1440, 1400], "scale": 1, "steps": [{"open": "/Users/anyosag/lidv12.STL"}, {"wait": "jobs"},
 {"health": "place"}, {"step": "supports"}, {"supports": "generate"}, {"wait": "jobs"},
 {"view": "angled"}, {"risk": "wait"}, {"shot": "pc_lid"}]}
```

Steps to vary it: `{"setting": ["tip_load_limit", 0]}`, `{"setting": ["capacity", 9]}`
(demo strengths), `{"advanced": "reset"}`. See `tests/gui/i1_risk.json` and
`src/gui/Script.cpp`.

## 6. What is assumed, not measured

The user has printed none of the calibration plates yet. All strengths are
borrowed.

| Value | Default | Where | How much it matters |
|---|---|---|---|
| Tip table | PrusaSlicer's, 11.1 mm² for 0.4 mm, trusted at 1/3 of where half fail | `CapacityModel`, `kBorrowedMargin` | Sets every tip's chance. The tips plate measures it. |
| Steepness m | 7 | `StabilityConfig::steepness` | Published 5–11 |
| Model's own resin across layers | the same per mm² as a tip contact | `bulk_factor` 1 | Necks; unmeasured |
| Thin slab | 2 mm | `slab` | **Large:** 2 → 4 mm took the lid's likeliest tip from 0.8 % to 28.5 % (0 → 5 tips shown); 1 mm gives 0.5 % |
| Share reach | 1.8 mm | `share_reach` | 0.9 / 1.8 / 3.6 mm: lid worst tip 2.3 / 0.8 / 2.1 % |
| Cup suction | 4 × the pull on resin | `cup_pull` | Test pocket closeness 0.11 / 0.37 / 0.71 at 0 / 4 / 8 |
| Skin reaches | 5.54 mm between holds, 2.77 mm at a free edge | from the planner, ÷ √margin | No layer-height or pull term |
| Peel bias | 0.25 with a hinge side; the user hasn't said which side drops | `peel_bias` | Its ramp pulls a part at the plate's centre 12.5 % harder than it should (not fixed) |
| Plate hold | the model's own bottom layers | `plate_hold_height` | Reasoning |
| Foot run | 2 mm | `kFootRun` | Reasoning |

What would measure these, in the agent's order of value: the **tips plate**
first, then **slab coupons** (for the thin slab), then **which side of the vat
drops** (for the hinge).

## 7. Numbers to compare against

All measured on 2026-11-15 with borrowed strengths, through the app's own path:

| Model | Setting | Tips | Places | Tips that may let go |
|---|---|---|---|---|
| Lid (`~/lidv12.STL`) | starting values | 627 | 0 | 0 |
| Lid | 0.3 mm tips | 636 | 0 | 0 |
| Eye cup (`~/eyecupv3.STL`) | starting values | 373 | 0 | 0 |
| Eye cup | Most | 562 | 0 | 0 |
| Eye cup | 0.6 mm tips | 361 | 0 | 0 |
| Eye cup | 0.3 mm tips | 568 | 0 | 0 |
| Demo bracket | — | 87 | 0 | 0 |
| Demo shelf (`i1_risk.json`) | — | — | 2 | — |

- The demo shelf's two places are a thin place at 11.0 mm (343 mm³ would be
  lost) and a skin at 15.0 mm (128 mm²).
- Replay time is about 2 s on the lid and eye cup.
- After the free-edge reach went in, the lid has one 0.5 mm² flap at its lowest
  layer. It is counted, not listed.
- Run your change against these. A new place on the lid or eye cup needs an
  explanation before it ships.

## 8. Open items

Not done, and why:
- **The CLI and the Resins page:** the CLI doesn't apply the values typed on the
  Resins page (the `yours` key in a profile). The table of those settings lives
  in `src/gui/Supports.cpp`, which is the supports session's code. It would
  need moving into the core library. The CLI says so in `profile_notes`.
- **Skin reach** has no layer-height or pull term. The skin's thickness is the
  cure depth, and the exposure isn't known.
- **Peel-bias ramp centring** (it pulls a centred part 12.5 % harder). Wait
  until the user says which side of the vat drops.
- **More replays near the band edges:** 500 replays can't tell 1 % from 5 %
  well there.
- **Left as "plausible" by the review:**
  - an added tip borrows its neighbour's support length without checking how
    far the model is below it;
  - no test pins the slab exponent;
  - no test checks a seam's later layers.
- **Not looked at well by the review:**
  - a weak neck that loads up after tips elsewhere let go;
  - a piece falling in the same event a tip lets go;
  - force balance in thick parts;
  - the raft;
  - the model's own strength between layers;
  - nested cups.
- **Old worktree:** `.worktrees/physics-accuracy` (the accuracy agent's) is
  still on disk. All its work is merged. The user was offered its deletion and
  didn't answer: ask before removing it.

Still open with the user:
- which failures they have actually had on this printer;
- which side of the vat drops when it tilts.

Suggested physical check: print the demo shelf with a few tips removed.

## 9. Decisions already taken, and why

Don't undo these without asking the user.

- **The lever rule is limited to stiff parts.** The user chose this, after
  seeing thin shelves cascade under a rigid lever. A thin piece shares a lost
  tip's load in proportion to what each neighbour carries (`94d7fa7`). The
  2026-11-15 review fixed how "thin" is judged (`a939f67`).
- **Edge tips stay at 0.3 mm or more.** Tip size doesn't take edge tips below
  0.3 mm until a capacity table is fitted (`ecb40d5`). Phrozen's guide gives
  0.3 mm as the smallest support that prints in Aqua Hyperfine.
- **A piece that starts inside the bottom layers stands on the plate**
  (`58525c4`), in the planner, the mid-air check and the replay alike.
- **Supports that would stand on one spot share it** (`cd2bc9a`, `joinOnModel`,
  `shareJunction`). The planner learns support lengths (`fcc7bcf`) and gets up
  to 16 rounds (`3ee20c9`).
- **The accuracy agent's seven changes were merged:**
  - lever rule (`103145f`);
  - later loads (`5761767`);
  - small-load tips (`c514172`);
  - skin margin (`9a2816d`);
  - finer sampling (`f945153`);
  - layer-height test (`5a5d91b`);
  - CLI values (`2501b69`).
- **The flaw-hunt fixes** (`a939f67`, `36f0519`, goldens `ed665c2`). An Opus
  lead and nine Haiku agents found 50 candidate flaws; 15 survived. The full
  list, with what was refuted, is in the notes folder and in the written
  model's section 15.

## 10. Working on it: what has worked

- **Check that a test really tests the fix.** Revert just the fixed line,
  rebuild `fruit_tests`, run the test, see it fail, restore. Twice this showed a
  plausible test that passed under the old code as well (a straight line of tips
  gives the same share under both rules; a square of tips does not).
- **Catch2 stops at the first crash.** A segfault in one test case hides every
  case after it. Look for "fatal error condition".
- **Fine margins:** with steepness 1000, a margin under 1 % still flips about
  1 in 4 replays. Use `steepness = 1e5`, as the sixty-tip test does.
- **zsh:** `$a` with spaces is not split into arguments; use `${=a}`. Quote
  `"======"` (a bare `=` word is expanded).
- **`lldb --batch` hangs here** (it waits on a permission prompt). Find a crash
  by guarding the input instead.
- **A test's numbers from the engine:** read `TipRisk.load` and `.holds` from a
  first run, then set strengths (for example `usable_fraction`) from them.
- **Fan-out reviews:** give each agent a narrow brief with function names and
  concrete probes. Haiku misreads code often, so verify every finding against
  the code before acting.

**Golden reviews.** Download the artifact into an empty folder, run
`tools/compare_screens.py golden shots diffs` for the list, then group the
changes with the script in the appendix. Look at every group: the panel the
picture shows is often not the one that changed. On 2026-11-15, 59 pictures
moved because one note got longer. Then copy over exactly the ones CI listed
as `DIFFERS`.

## Appendix: group changed screenshots by what changed

Save as, say, `/tmp/review2.py`. Run it with
`python3 -I review2.py tests/gui/golden <shots> <out>`. It writes
`group_NN.png` sheets (golden on the left, new on the right) and `index.txt`.

```python
#!/usr/bin/env python3
import hashlib, sys
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw

golden, shots, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
out.mkdir(parents=True, exist_ok=True)
groups, new = {}, []
for p in sorted(shots.glob("*.png")):
    g = golden / p.name
    if not g.exists():
        new.append(p.name)
        continue
    gi, si = Image.open(g).convert("RGB"), Image.open(p).convert("RGB")
    if gi.size != si.size:
        groups.setdefault(("size", p.name), []).append((p.name, gi, si, (0, 0) + si.size))
        continue
    r, gch, b = ImageChops.difference(gi, si).split()
    mask = ImageChops.lighter(ImageChops.lighter(r, gch), b).point(lambda v: 255 if v > 24 else 0)
    if mask.histogram()[255] == 0:
        continue
    box = mask.getbbox()
    gc, sc = gi.crop(box), si.crop(box)
    key = (box[2] - box[0], box[3] - box[1], hashlib.sha1(gc.tobytes()).hexdigest(), hashlib.sha1(sc.tobytes()).hexdigest())
    groups.setdefault(key, []).append((p.name, gi, si, box))
order = sorted(groups.items(), key=lambda kv: -len(kv[1]))
sheet_w, side = 1880, 920
sheet, y, n, index = None, 0, 0, []
def flush():
    global sheet, y, n
    if sheet is not None:
        sheet.crop((0, 0, sheet_w, max(1, y))).save(out / f"group_{n:02d}.png")
        n += 1
    sheet, y = None, 0
for gi_, (key, members) in enumerate(order):
    name, gi, si, box = members[0]
    pad = 16
    b = (max(0, box[0] - pad), max(0, box[1] - pad), min(gi.size[0], box[2] + pad), min(gi.size[1], box[3] + pad))
    gc, sc = gi.crop(b), si.crop(b)
    k = min(1.0, side / gc.size[0], 560 / gc.size[1])
    size = (max(1, int(gc.size[0] * k)), max(1, int(gc.size[1] * k)))
    gc, sc = gc.resize(size), sc.resize(size)
    index.append(f"G{gi_:02d} x{len(members)} box {box}: " + ", ".join(m[0][:-4] for m in members))
    row_h = size[1] + 30
    if sheet is None or y + row_h > 1700:
        flush()
        sheet = Image.new("RGB", (sheet_w, 1750), (40, 40, 48))
    ImageDraw.Draw(sheet).text((6, y + 4), f"G{gi_:02d}  x{len(members)}  first: {name}  box {box}  (left golden, right new)", fill=(255, 220, 120))
    sheet.paste(gc, (6, y + 22))
    sheet.paste(sc, (6 + side + 20, y + 22))
    y += row_h + 8
flush()
(out / "index.txt").write_text("\n".join(index) + "\n" + "\n".join("NEW " + x for x in new) + "\n")
print(f"groups {len(groups)}, new {len(new)}, sheets {n}")
```
