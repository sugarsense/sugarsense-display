#!/usr/bin/env python3
"""Compose docs/screens.png, the picture in the README and on the install page.

    python3 tools/host_test.py --render out/
    python3 tools/make_screens.py out/ docs/screens.png

Nine screens exactly as the firmware draws them (the host renders), on the
page's dark background, two of them the split screen of several people. Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw

# The Color scene leads: it is what a display shows by day out of the box.
TILES = [
    "L_21_color_inrange", "L_22_color_high_mmol", "L_09_night_inrange",
    "L_01_dark_inrange", "L_24_dark_wide_swing", "L_41_split2_color",
    "L_35_nograph_color", "L_47_split4_dark", "L_16_pairing",
]
COLS, W, H, GAP, RADIUS = 3, 480, 320, 24, 16
BG, EDGE = (10, 14, 17), (35, 44, 54)


def main():
    src, out = sys.argv[1], sys.argv[2]
    rows = (len(TILES) + COLS - 1) // COLS
    sheet = Image.new("RGB", (COLS * W + (COLS + 1) * GAP, rows * H + (rows + 1) * GAP), BG)
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), RADIUS, fill=255)
    draw = ImageDraw.Draw(sheet)
    for i, name in enumerate(TILES):
        tile = Image.open(os.path.join(src, name + ".png")).convert("RGB")
        x = GAP + (i % COLS) * (W + GAP)
        y = GAP + (i // COLS) * (H + GAP)
        sheet.paste(tile, (x, y), mask)
        draw.rounded_rectangle((x - 1, y - 1, x + W, y + H), RADIUS + 1, outline=EDGE, width=1)
    sheet.save(out, optimize=True)
    print("wrote", out, sheet.size)


if __name__ == "__main__":
    main()
