#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 FruitSlicer contributors
"""Compares app screenshots with stored goldens.

usage: compare_screens.py <golden_dir> <actual_dir> <diff_dir>

A pixel counts as different when any channel differs by more than
--channel (default 24); a screenshot fails when more than --fraction
(default 0.05 %) of its pixels differ, or when its size changed: enough to
catch a changed line of text at 1440 × 900. Diff images
(differences in red over a dimmed copy) are written for failures. Exits 1
when a golden is missing, a screenshot is missing, or any comparison fails.
Update goldens by copying the actual screenshots over them after checking
them by eye. Needs Pillow.
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageChops


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("actual")
    ap.add_argument("diff")
    ap.add_argument("--channel", type=int, default=24)
    ap.add_argument("--fraction", type=float, default=0.0005)
    a = ap.parse_args()
    golden, actual, diff = Path(a.golden), Path(a.actual), Path(a.diff)
    diff.mkdir(parents=True, exist_ok=True)
    names = sorted({p.name for p in golden.glob("*.png")} | {p.name for p in actual.glob("*.png")})
    failed = 0
    for name in names:
        g, s = golden / name, actual / name
        if not g.exists():
            print(f"NEW      {name}: no golden (copy it into {golden} after checking it)")
            failed += 1
            continue
        if not s.exists():
            print(f"MISSING  {name}: the script did not write it")
            failed += 1
            continue
        gi, si = Image.open(g).convert("RGB"), Image.open(s).convert("RGB")
        if gi.size != si.size:
            print(f"SIZE     {name}: {gi.size} -> {si.size}")
            failed += 1
            continue
        d = ImageChops.difference(gi, si)
        # A pixel differs when any channel does.
        r, gch, b = d.split()
        any_ch = ImageChops.lighter(ImageChops.lighter(r, gch), b).point(lambda v: 255 if v > a.channel else 0)
        bad = any_ch.histogram()[255]
        total = gi.size[0] * gi.size[1]
        frac = bad / total
        if frac > a.fraction:
            print(f"DIFFERS  {name}: {bad} pixels ({frac:.3%})")
            dim = si.point(lambda v: v // 3)
            red = Image.new("RGB", si.size, (255, 40, 40))
            dim.paste(red, (0, 0), any_ch)
            dim.save(diff / name)
            failed += 1
        else:
            print(f"ok       {name}: {bad} pixels differ ({frac:.3%})")
    print(f"{len(names) - failed} of {len(names)} screenshots match")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
