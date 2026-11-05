# FruitSlicer

A resin (MSLA) slicer, written from scratch in Rust as a learning project. The first target printer is the
ELEGOO Saturn 4 Ultra 16K (15120 × 6230 px, 14 × 19 µm pixels, 211.68 × 118.37 × 220 mm).

## Status

- `crates/goo`: reads and writes ELEGOO `.goo` files, version V3, from ELEGOO's *Goo Format Spec V1.2*.
  It streams one layer at a time, since a 16K layer is about 94 MB raw. GOO V5 is not supported yet.
- `crates/fruitslicer-cli`: the `fruitslicer` command, which inspects files and writes a calibration test pattern.
- `tools/goo_check.py`: a second, separately written checker (Python standard library only) that walks a
  `.goo` file byte by byte from the spec.

There is no STL slicing yet.

## Usage

```sh
cargo build --release

# Write a 3 mm tall calibration print: a 20 x 20 mm square in the centre and
# four 10 x 10 mm pads at (+-60, +-35) mm.
./target/release/fruitslicer test-pattern pattern.goo --exposure 2.5 --bottom-exposure 30

# Print every header field, and save layer 0 as a 1/16-scale PGM image.
./target/release/fruitslicer info pattern.goo --layer 0 --pgm layer0.pgm --scale 16

# Check the file independently and report each lit region's size in mm.
python3 tools/goo_check.py pattern.goo 0
```

## Unverified assumptions

These come from UVtools' printer profile, not from a file made by ELEGOO's own slicer. Compare against a
SatelLite or Chitubox `.goo` before trusting a print.

- **Tilting-vat movement fields** (lift and retract distance and speed) are set to `0.05`, which look like
  placeholders that the firmware ignores in favour of its tilt modes.
- **`mirror_x` is `true`.** The test pattern is symmetric, so this doesn't change its output.
- **`printer_type` is `"DLP"`**, UVtools' default.
- **Exposure defaults** (2.5 s, and 30 s for 4 bottom layers) are placeholders. Set them for your resin.

## License

MIT
