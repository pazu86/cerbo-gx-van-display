"""Rear shell for the Waveshare ESP32-S3-Touch-LCD-7B, mounted with a Quad Lock pad.

The cover glass is the front face: it sits on the shell's rim, the shell has
exactly the glass outline. The display is held by 4 x M3 countersunk screws
inserted from the back (heads flush) into the module's M3 tabs. The centre of
the back is left flat and clear for a Quad Lock adhesive pad.

Printed back-down with no supports: every feature is either vertical or
self-supporting (45-degree countersinks, pointed-top vents).

Mounted landscape with the USB-C end on the LEFT.

Dimensions from Waveshare's drawing ESP32-S3-Touch-LCD-7B-20251025.pdf.
Coordinates below are as seen from the FRONT, glass top-left = (0, 0),
portrait as drawn (USB-C end at the bottom). The drawing's back view is
mirrored left-right relative to this.

    python enclosure/shell.py      (needs: pip install cadquery)

writes rear-shell.step and .stl next to this file.
"""
import pathlib

import cadquery as cq

# ---- Display (Waveshare drawing) ------------------------------------------
GLASS_W, GLASS_H, GLASS_R = 110.76, 192.96, 7.88
GLASS_T = 1.1            # cover glass thickness (estimate - check with calipers)
FRONT_TO_TAB = 7.5       # glass front face -> M3 tab seat on the LCD back plate
FRONT_TO_DEEPEST = 17.0  # glass front face -> deepest part (connectors)
# M3 tabs: 65.65 x 126.20 pattern; x mirrored from the drawing's back view
MOUNT_HOLES = [(21.58, 31.85), (87.23, 31.85), (21.58, 158.05), (87.23, 158.05)]
USB_END_Y = 153.0        # approx. y of the board edge with the USB-C ports

# ---- Shell ------------------------------------------------------------------
WALL = 2.0               # side wall = width of the rim the glass rests on
FLOOR = 2.5              # back wall thickness
TAPE = 0.5               # foam/VHB tape between rim and glass
CLEARANCE = 3.0          # gap under the deepest component
BOSS_D = 7.0
BOSS_SHORT = 1.0         # bosses stop short so screws clamp the glass onto the rim
M3_CLEAR, M3_CSK = 3.4, 6.6
# Kept clear on the back for the Quad Lock pad (diameter, centred)
PAD_ZONE_D = 60.0
# Cable slot: from just past the USB-C end of the board to near the end wall
CABLE_SLOT_W = 44.0
CABLE_SLOT_START_Y = USB_END_Y + 8.0           # clear of the board edge
CABLE_SLOT_END_GAP = 2.0                       # floor left between slot and end wall
CABLE_SLOT_END_Y = GLASS_H - WALL - CABLE_SLOT_END_GAP
CABLE_SLOT = (CABLE_SLOT_W, CABLE_SLOT_END_Y - CABLE_SLOT_START_Y)
CABLE_SLOT_Y = (CABLE_SLOT_START_Y + CABLE_SLOT_END_Y) / 2
# Printed back-down with no supports, so vents are vertical with a 45-degree
# pointed top (self-supporting) rather than horizontal slots (bridged ceilings).
VENTS = True
VENT_COUNT, VENT_W = 10, 2.2
VENT_Z0, VENT_Z1 = FLOOR + 3.0, 17.0     # bottom and tip height above the wall

# Height from the wall to the rim, sized for the deepest component
SHELL_H = round(FRONT_TO_DEEPEST + CLEARANCE + FLOOR - TAPE - GLASS_T, 1)
TAB_Z = SHELL_H + TAPE + GLASS_T - FRONT_TO_TAB    # M3 tab seat height above the wall
BOSS_TOP = TAB_Z - BOSS_SHORT


def xy(x, y):
    """Drawing coords (from glass top-left) -> centred CadQuery coords."""
    return (x - GLASS_W / 2, GLASS_H / 2 - y)


def rounded_rect(w, h, r):
    return cq.Sketch().rect(w, h).vertices().fillet(r)


# Outer body and cavity
shell = cq.Workplane("XY").placeSketch(rounded_rect(GLASS_W, GLASS_H, GLASS_R)).extrude(SHELL_H)
shell = shell.faces("<Z").edges().chamfer(0.8)   # soften the wall-side edge (before any holes)
cavity = (cq.Workplane("XY").workplane(offset=FLOOR)
          .placeSketch(rounded_rect(GLASS_W - 2 * WALL, GLASS_H - 2 * WALL, GLASS_R - WALL))
          .extrude(SHELL_H))
shell = shell.cut(cavity)

def csk_hole(x, y, clear_d, head_d, z_face, upward):
    """Through hole + 90-degree countersink, cut at absolute coordinates.
    upward=True: countersink opens at z_face facing -Z (screw from the wall
    side); False: opens at z_face facing +Z (screw from inside)."""
    cx, cy = xy(x, y)
    hole = cq.Solid.makeCylinder(clear_d / 2, 100, cq.Vector(cx, cy, -50))
    depth = (head_d - clear_d) / 2   # 90 degrees: depth = radius difference
    if upward:
        cone = cq.Solid.makeCone(head_d / 2, clear_d / 2, depth, cq.Vector(cx, cy, z_face))
    else:
        cone = cq.Solid.makeCone(clear_d / 2, head_d / 2, depth, cq.Vector(cx, cy, z_face - depth))
    return hole.fuse(cone)


# Display bosses with countersunk M3 holes from the wall side
for x, y in MOUNT_HOLES:
    boss = cq.Workplane("XY").center(*xy(x, y)).circle(BOSS_D / 2).extrude(BOSS_TOP)
    shell = shell.union(boss)
for x, y in MOUNT_HOLES:
    shell = shell.cut(csk_hole(x, y, M3_CLEAR, M3_CSK, 0.0, upward=True))

# Cable entry through the back
slot = (cq.Workplane("XY").center(*xy(GLASS_W / 2, CABLE_SLOT_Y))
        .placeSketch(rounded_rect(*CABLE_SLOT, 4.0)).extrude(FLOOR + 0.2, both=True))
shell = shell.cut(slot)

# Vent slots along both long edges (top/bottom when mounted landscape):
# vertical, pointed tops, so they print without support
if VENTS:
    pitch = (GLASS_H - 60) / (VENT_COUNT - 1)
    h = VENT_W / 2
    for side in (-1, 1):
        for i in range(VENT_COUNT):
            v = GLASS_H / 2 - (30 + i * pitch)        # position along the wall
            outline = [(v - h, VENT_Z0), (v + h, VENT_Z0), (v + h, VENT_Z1 - h), (v, VENT_Z1), (v - h, VENT_Z1 - h)]
            vent = (cq.Workplane("YZ").workplane(offset=side * (GLASS_W / 2 - WALL / 2))
                    .polyline(outline).close().extrude(WALL * 2, both=True))
            shell = shell.cut(vent)

out = pathlib.Path(__file__).parent
cq.exporters.export(shell, str(out / "rear-shell.step"))
cq.exporters.export(shell, str(out / "rear-shell.stl"), tolerance=0.05, angularTolerance=0.1)

screw_len = FLOOR + BOSS_TOP - FLOOR + 3.0   # through floor + boss, ~3 mm into the tab
print(f"shell {GLASS_W} x {GLASS_H} x {SHELL_H} mm (glass front ~{SHELL_H + TAPE + GLASS_T:.1f} mm from the back)")
print(f"M3 tab seat at {TAB_Z:.1f} mm, bosses to {BOSS_TOP:.1f} mm -> M3 countersunk x {round(screw_len)} mm")
