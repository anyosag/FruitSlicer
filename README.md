# FruitSlicer

A resin (MSLA) slicer, written from scratch in Rust as a learning project. The first target printer is the
ELEGOO Saturn 4 Ultra 16K (15120 × 6230 px, 14 × 19 µm pixels, 211.68 × 118.37 × 220 mm).

## Status

- `crates/goo`: reads and writes ELEGOO `.goo` files, version V3, from ELEGOO's *Goo Format Spec V1.2*.
  It streams one layer at a time, since a 16K layer is about 94 MB raw. GOO V5 is not supported yet.
- `crates/slicer`: loads binary and ASCII STLs and places the model on the plate. It cuts each layer at
  its mid-height, fills the cross-section on the LCD's non-square pixels with the non-zero winding rule
  (so holes and overlapping shells work), and encodes layers in parallel. It also renders top-down
  preview thumbnails.
- `crates/fruitslicer-cli`: the `fruitslicer` command, which can `slice`, `test-pattern` and `info`.
- `tools/goo_check.py`: a second, separately written checker (Python standard library only) that walks a
  `.goo` file byte by byte from the spec.

Not built yet: supports, hollowing, anti-aliasing, island detection, a GUI and WiFi upload.

## Usage

```sh
cargo build --release

# Slice an STL. The model is centred on the plate and dropped to Z = 0.
./target/release/fruitslicer slice model.stl --out model.goo \
    --layer-height 0.05 --exposure 2.5 --bottom-exposure 30 --bottom-layers 4

# Optional placement: --scale F, --rotate-x/y/z DEG, --x MM --y MM (offset from the plate centre).

# Write a 3 mm tall calibration print: a 20 x 20 mm square in the centre and
# four 10 x 10 mm pads at (+-60, +-35) mm.
./target/release/fruitslicer test-pattern pattern.goo

# Print every header field, and save layer 0 as a 1/16-scale PGM image.
./target/release/fruitslicer info pattern.goo --layer 0 --pgm layer0.pgm --scale 16

# Check the file independently and report each lit region's size in mm.
python3 tools/goo_check.py pattern.goo 0
```

On a 4-core machine, a 3DBenchy (225,706 triangles, 960 layers) slices in about 0.2 s. Its sliced volume
is within 0.02% of the mesh volume.

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

## License

MIT
