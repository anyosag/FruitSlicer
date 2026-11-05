# FruitSlicer

A resin (MSLA) slicer, written from scratch in Rust as a learning project. The first target printer is the
ELEGOO Saturn 4 Ultra 16K (15120 × 6230 px, 14 × 19 µm pixels, 211.68 × 118.37 × 220 mm).

## The app

`FruitSlicer` (`crates/fruitslicer-app`) is a desktop app built on the same core as the command line tool.
It is one Rust binary using egui and wgpu, so there is no runtime to install and the slicer runs in the same
process as the UI. Download `FruitSlicer.exe` from the latest CI run (Actions > CI > *FruitSlicer-windows-x64*),
or build it with `cargo build --release -p fruitslicer-app`.

What it does:

- **Load** STL (binary or ASCII), OBJ and 3MF files, by File > Open or by dropping them on the window.
  Several models can share the plate. Move them by dragging, or set scale, rotation and position in the
  left panel. *Lay flat*, *Arrange* and undo/redo (Ctrl+Z / Ctrl+Y) are there too.
- **Automatic supports**: straight columns with tapered tips standing on a raft. Overhangs flatter than
  the set angle get a grid of supports, and the lowest points of each downward part get one as well.
  Tick *Edit by hand* to add a support with a click or remove one with a right-click. There are no tree
  supports.
- **Print checks**, run during slicing on a 0.25 mm grid:
  - *Island*: a region that starts in mid-air with nothing under it.
  - *Unsupported overhang*: a region more than 2.5 mm from anything in the layer below.
  - *Suction cup*: a hollow that is open only towards the vat floor, so it pulls on the film.
  - *Trapped resin*: a fully enclosed hollow.
  Click an issue in *Results* to jump to it in the layer view. *Add supports under islands* does what it
  says and asks you to slice again.
- **Hollowing** with a set wall thickness, and drain holes cut through the floor of each hollow.
- **Anti-aliasing** (2×, 4× or 8×), written as grey edge pixels.
- **Layer viewer** with zoom, pan, issue markers and a cut-away of the 3D view at the current layer.
- **Open an existing `.goo`** to view its layers, run the same checks on it and see every header field.
- **Save** the `.goo`, export the plate with its supports as one STL, and save the whole plate as a
  project (`.fsproj`: a zip with the settings, placements, support edits and each model's mesh).
- **Resin profiles**: save the exposure settings under a name.

If the graphics driver doesn't work, the app falls back to OpenGL and then to software rendering, and
says so in the status bar.

## Crates

- `crates/goo`: reads and writes ELEGOO `.goo` files, version V3, from ELEGOO's *Goo Format Spec V1.2*.
  It streams one layer at a time, since a 16K layer is about 94 MB raw. GOO V5 is not supported yet.
- `crates/slicer`: cuts each layer at its mid-height and fills the cross-section on the LCD's non-square
  pixels with the non-zero winding rule (so holes and overlapping shells work). It also holds supports,
  print checks, hollowing, anti-aliasing and preview thumbnails.
- `crates/fruitslicer-cli`: the `fruitslicer` command, which can `slice`, `test-pattern` and `info`.
- `crates/fruitslicer-app`: the desktop app.
- `tools/goo_check.py`: a second, separately written checker (Python standard library only) that walks a
  `.goo` file byte by byte from the spec.

Not built: WiFi upload to the printer, tree supports and GOO V5. The print time shown is a rough
guess (exposure plus 5 s per layer for the tilt), not measured on the printer.

## Command line

```sh
cargo build --release

# Slice an STL with supports and checks. The model is centred on the plate and dropped to Z = 0.
./target/release/fruitslicer slice model.stl --out model.goo --supports on \
    --layer-height 0.05 --exposure 2.5 --bottom-exposure 30 --bottom-layers 4

# Placement: --scale F, --rotate-x/y/z DEG, --x MM --y MM (offset from the plate centre).
# Supports: --lift MM, --overhang-angle DEG, --support-spacing MM.
# Hollowing: --hollow WALL_MM, --drain-holes on|off, --hole-diameter MM.
# Anti-aliasing: --aa 2|4|8.  Checks: --checks on|off.

# Write a 3 mm tall calibration print: a 20 x 20 mm square in the centre and
# four 10 x 10 mm pads at (+-60, +-35) mm.
./target/release/fruitslicer test-pattern pattern.goo

# Print every header field, and save layer 0 as a 1/16-scale PGM image.
./target/release/fruitslicer info pattern.goo --layer 0 --pgm layer0.pgm --scale 16

# Check the file independently and report each lit region's size in mm.
python3 tools/goo_check.py pattern.goo 0
```

On a 4-core machine, a 3DBenchy (225,706 triangles, 960 layers) slices in about 0.2 s without supports,
and in about 1.6 s with supports and checks. Its sliced volume is within 0.02% of the mesh volume.

## Unverified assumptions

These come from UVtools' printer profile, not from a file made by ELEGOO's own slicer. Compare against a
SatelLite or Chitubox `.goo` before trusting a print.

- **Tilting-vat movement fields** (lift and retract distance and speed) are set to `0.05`, which look like
  placeholders that the firmware ignores in favour of its tilt modes.
- **`mirror_x` is `true`, and the slicer mirrors layer images in X to match.** If prints come out
  mirrored, slice with `--mirror-x off`. The test pattern is symmetric, so this doesn't change its output,
  but it does matter for text and other asymmetric models.
- **`printer_type` is `"DLP"`**, UVtools' default.
- **Exposure defaults** (2.5 s, and 30 s for 4 bottom layers) are placeholders. Set them for your resin.
- **Support sizes, the overhang angle and the check thresholds** are reasonable starting points, not
  values tested on this printer.
- **Anti-aliasing** writes grey pixels as the spec describes; whether the printer's firmware shows them
  as grey, and how that changes the cure, is untested.

## License

MIT
