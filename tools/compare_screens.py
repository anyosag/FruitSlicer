#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 FruitSlicer contributors
"""Compares app screenshots with stored goldens.

usage: compare_screens.py <golden_dir> <actual_dir> <diff_dir>

A pixel counts as different when any channel differs by more than
--channel (default 24); a screenshot fails when more than --fraction
(default 0: any pixel) of its pixels differ, or when its size changed. CI's
Linux renders match the goldens with no pixel past the channel tolerance,
so any allowance only hides real changes (a 0.05 % one let a new rail item
through). Diff images
(differences in red over a dimmed copy) are written for failures. Exits 1
when a golden is missing, a screenshot is missing, or any comparison fails.
With --report-only (CI) a changed, resized or new screenshot is reported,
not failed: as a warning, and in the run's summary when GITHUB_STEP_SUMMARY
is set; a missing screenshot (a page that didn't open) still fails.
Update goldens by copying the actual screenshots over them after checking
them by eye. Needs Pillow.
"""
import argparse
import os
import sys
from pathlib import Path

from PIL import Image, ImageChops


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("actual")
    ap.add_argument("diff")
    ap.add_argument("--channel", type=int, default=24)
    ap.add_argument("--fraction", type=float, default=0.0)
    ap.add_argument("--report-only", action="store_true",
                    help="changed and new screenshots are listed, not failed; a missing one still fails")
    a = ap.parse_args()
    golden, actual, diff = Path(a.golden), Path(a.actual), Path(a.diff)
    diff.mkdir(parents=True, exist_ok=True)
    names = sorted({p.name for p in golden.glob("*.png")} | {p.name for p in actual.glob("*.png")})
    failed = 0
    missing = []
    changed = []  # (name, what)
    for name in names:
        g, s = golden / name, actual / name
        if not g.exists():
            print(f"NEW      {name}: no golden (copy it into {golden} after checking it)")
            changed.append((name, "new, no reference picture yet"))
            failed += 1
            continue
        if not s.exists():
            print(f"MISSING  {name}: the script did not write it")
            missing.append(name)
            failed += 1
            continue
        gi, si = Image.open(g).convert("RGB"), Image.open(s).convert("RGB")
        if gi.size != si.size:
            print(f"SIZE     {name}: {gi.size} -> {si.size}")
            changed.append((name, f"size {gi.size} -> {si.size}"))
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
            changed.append((name, f"{bad} pixels ({frac:.3%})"))
            failed += 1
        else:
            print(f"ok       {name}: {bad} pixels differ ({frac:.3%})")
    print(f"{len(names) - failed} of {len(names)} screenshots match")
    if not a.report_only:
        return 1 if failed else 0
    for name in missing:
        print(f"::error title=Screenshot missing::{name}: the script did not write it")
    if changed:
        print(f"::warning title=Screenshots changed::{len(changed)} differ from the reference pictures; "
              "check them in the app-screens-linux download")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(f"### Screenshots: {len(names) - failed} of {len(names)} match\n\n")
            if missing:
                f.write("Missing (a page didn't open):\n\n" + "".join(f"- {n}\n" for n in missing) + "\n")
            if changed:
                f.write("Changed, to check and copy into tests/gui/golden:\n\n"
                        + "".join(f"- {n}: {w}\n" for n, w in changed) + "\n")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
