# The hole-tops coupon

The planner now leaves bare the top of a round hole through a wall 1.5 to
3 mm thick: each layer of it reaches out from both sides at once, as a
bridge is built ([plan](resin-support-plan.md), "The top of a hole through a
wall holds itself up"). On TubeRack_v3 that took 1,250 tips off the tops of
its holes. The replay of the print cannot judge whether a bare top prints.
This coupon is the test.

```sh
./build/src/fruitslicer coupon crowns -o crowns_coupon.stl            # 37 mm tall
./build/src/fruitslicer coupon crowns-tall -o crowns-tall_coupon.stl  # 82 mm tall
```

## What it is

A box of four 2 mm walls, 75.6 × 42.5 × 37 mm, open at the top and bottom.
It is laid out like a corner of TubeRack, which also prints upright with 2 mm
walls:

| Wall | Piece | On TubeRack | Tips on its top, as planned (crowns / crowns-tall) |
|---|---|---|---|
| Front, left to right | two 6.5 mm holes, 9 mm apart | 96 on the front wall | none / none |
| Front | two 11.8 mm holes | 32 on the back wall | none / none |
| Front, right | a 14 mm hole: **control** | – | 11 / 11 |
| Right end | a 17.5 mm bore, 16.70 mm on the inner face to 18.25 on the outer (its top rises 21° across the wall) | 12 on the right end wall | 15 / none |
| Left end | a 30 mm bore, 29.25 to 30.53 mm (rises 18°) | 4 on the left end wall | 25 / 15: a row along the rim of its outer face, and one tip |
| Back | TubeRack's diamond lattice, struts 1.2 mm wide leaning 47° from upright: **control** | the back wall | 86 / 85 |

The bores widen outward, as on TubeRack. The tips are what the app's Generate
gave on main of 2026-11-24 (default choices, hinge at the back).

## Which one to print

- **crowns** (37 mm tall) tests the 6.5 and 11.8 mm tops bare. Its two bores
  keep their tips: the rule leaves a tapered top bare only on a model taller
  than 80 mm (`closing_ramp_min_height`). So it does not test them.
- **crowns-tall** is the same box with a 6 mm post in the back right corner up
  to 82 mm. Past 80 mm the bores are planned as on TubeRack: the 17.5 mm top is
  bare, and so is the 30 mm one except for 15 small tips (0.2 to 0.4 mm).
  Fourteen are in a row along the rim of its outer face, and one is at the
  very top of its inner face.

  Those 15 tips are the planner's doing, not TubeRack's. Where it judges a
  tapered top, it looks for the wall's two faces 1.5 mm into the wall beside
  the gap. At the 30 mm bore's narrow end that spot is still inside the wide
  end's opening. So the wall reads 1.87 mm deep instead of 2, and what is held
  stops 0.03 mm short of the outer face. The sliver left in each layer gets
  the tips. TubeRack's own 30 mm bores read 1.93 mm deep but are held right
  across, by 0.03 mm. To print that top bare, as TubeRack's are planned,
  delete those 15 tips in the app's tip editor before you save.

With your print settings (5 s wait after each retract), including the 5 mm
lift:

| | 0.05 mm layers | 0.03 mm layers |
|---|---|---|
| crowns | about 2¼ h | about 3¾ h |
| crowns-tall | about 4½ h | about 7½ h |
| TubeRack_v3, for comparison | about 9½ h | about 15¾ h |

The model is 12 ml (14 ml tall), plus supports.

## Printing it

- Open the STL in FruitSlicer and keep it as it opens: upright, on supports.
  Don't tilt it, even though Generate warns that the bottom is a flat face in
  one layer.
- Generate supports. Check that the tips are as in the table.
- Print at the layer height you will print TubeRack at. The planner judges a
  top at 0.05 mm layers, whatever the print's layer height. At 0.03 mm each
  layer reaches less far over a top: the 11.8 mm hole's last layer comes
  0.60 mm from each side instead of 0.77 mm. So a pass at 0.03 mm says less
  about 0.05 mm than the other way round.

## Scoring

Wash and cure it, and take the supports off. Then check each bare top:

- **crowns:** the two 6.5 mm and the two 11.8 mm holes;
- **crowns-tall:** those, plus the 17.5 mm and 30 mm bores, from both faces.

1. **Printed.** The top is closed: no gap, no torn or missing layers, and no
   flakes of it left on the film.
2. **Sag and dips.** Hold the coupon up to a light and look through the hole
   along its axis, with a 10× loupe if you have one. The top of the circle
   should be as smooth and round as its bottom. A flat or a notch at the top
   is a dip. At 10×, a dip of 0.05 mm (one layer) looks 0.5 mm deep, and one
   of 0.1 mm (two layers) looks 1 mm deep.
3. **Seam.** With light from the side, look up into the hole. Draw a
   fingernail along the top from one face to the other. A ridge, groove or
   line along the top, where the two sides met, is a seam.
4. **Round.** With the inside jaws of calipers, measure each hole on both
   faces: up (bottom to top) and across (side to side). Note up minus across.
   Measure the 14 mm control the same way, once its tip marks are sanded
   flush. Or use drill shanks: the largest one that passes should touch all
   round. If it moves side to side but not up and down, the top came down.

Score each top:

| | What you see |
|---|---|
| **Pass** | A smooth arc from both faces, no seam you can feel. Up within 0.05 mm of across, or of the control's up minus across. |
| **Borderline** | A dip or a line you can see at 10× but can't feel. Up 0.05 to 0.1 mm short. |
| **Fail** | A step, a droop, strings or a gap at the top. Up more than 0.1 mm short. Or pieces of the top on the film. |

The controls:

- **The 14 mm hole** has tips on its top. It shows what a supported top looks
  like with this resin. If it sags too, the sag is not from missing tips.
- **The lattice** keeps its tips. Every strut should be whole and straight,
  every crossing joined, and nothing should sag into the window below.

If the lattice or the 14 mm top failed, something else went wrong, such as
exposure or peel. Reprint before you judge the bare tops.

## What the results mean

| Result | Meaning |
|---|---|
| 6.5 and 11.8 mm pass | Flat tops through 2 mm walls hold themselves up to 11.8 mm. TubeRack's 128 front and back holes can print bare. |
| 6.5 mm passes; 11.8 mm is borderline or fails | The reach is too long. A `closing_reach` of 0.6 mm (now 0.8) holds holes up to about 7.2 mm and gives the 11.8 mm tops their tips back. |
| 6.5 mm fails | Bare tops don't print here. Turn the rule off (`closing_reach` 0). |
| crowns-tall: the bores pass | TubeRack's end walls can print bare. |
| crowns-tall: a bore fails | Tapered tops need their tips. Raise `closing_ramp_min_height` past TubeRack's 175 mm, or narrow `closing_gap_ramp` (now 12 mm). |
