# Rear shell (Quad Lock mount)

- `rear-shell.step` - CAD (single solid, mm). Imports into Fusion 360 as a body you can
  edit directly (press/pull, move face, add features); it has no feature history.
- `rear-shell.stl` - for printing.
- `shell.py` - the parametric source: edit the values at the top and run
  `python enclosure/shell.py` (needs `pip install cadquery`) to regenerate both files.

The cover glass is the front face: the shell is exactly the glass outline (110.76 x 192.96 mm,
R7.88) and the glass rests on its 2 mm rim. Depth 20.9 mm. The centre of the back is flat and
clear (60 mm circle) for a **Quad Lock adhesive pad**. Mounted landscape, USB-C end on the left.

## Fitting

1. A strip of thin foam/VHB tape (0.5 mm) on the rim.
2. Plug in the cable - a **right-angle USB-C plug** fits best (~35 mm between the board edge and
   the shell wall) - and feed it out through the slot in the back.
3. Lay the display in, glass up, and fix it from the back with **4 x M3 x 16 mm countersunk**
   screws into the module's M3 tabs. The bosses stop 1 mm short, so the screws pull the glass
   down onto the rim.
4. Stick the Quad Lock pad in the centre of the back.

## Printing

- **Orientation: back on the bed, no supports.** Everything is vertical or self-supporting: the
  vents are vertical with 45-degree pointed tops, the countersinks are 45-degree cones.
- PETG or ASA, not PLA - a closed caravan in summer will soften PLA.
- 0.2 mm layers, 3+ walls; solid infill under the Quad Lock pad area helps (or 4+ bottom layers).
- The BOOT/RESET buttons and SD card are inaccessible when assembled; firmware updates go over
  WiFi.

## Check before printing

From Waveshare's drawing (ESP32-S3-Touch-LCD-7B-20251025.pdf), but these are estimates -
measure your unit with calipers, edit the top of `shell.py` and regenerate:

| Parameter | Value | What it is |
|---|---|---|
| `GLASS_T` | 1.1 mm | cover glass thickness |
| `FRONT_TO_TAB` | 7.5 mm | glass front -> seat of the M3 tabs on the LCD back plate |
| `FRONT_TO_DEEPEST` | 17.0 mm | glass front -> deepest connector |

The shell depth and boss heights are derived from these.
