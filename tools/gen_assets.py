"""Generate firmware/sugarsense_display/assets.h: the anti-aliased fonts, the
trend arrows, the Sugar Sense mark and the app icon for the display.

Every glyph is a 4-bit alpha bitmap (two pixels per byte, row major), so the
firmware blends it against whatever is already on the screen and text reads
smooth on the 320x480 panel.

Everything comes from this project's own assets/ folder:
  assets/fonts/   Inter (SIL Open Font License 1.1, see OFL.txt)
  assets/brand/   the Sugar Sense app icon

    python3 -m venv .venv && .venv/bin/pip install pillow
    .venv/bin/python tools/gen_assets.py

The header is committed; regenerate it only when a size or a charset changes.
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
FONT_DIR = os.path.join(ROOT, "assets", "fonts")
ICON_PNG = os.path.join(ROOT, "assets", "brand", "icon-512.png")
OUT = os.path.join(ROOT, "firmware", "sugarsense_display", "assets.h")

ASCII = "".join(chr(c) for c in range(32, 127))
# Three glyphs outside ASCII live in private slots so a font stays one
# contiguous 8-bit range: 0x80 the real minus, 0x81 the middle dot, 0x82 the
# ellipsis. The firmware writes them as CH_MINUS, CH_DOT, CH_ELL.
SPECIALS = {0x80: "−", 0x81: "·", 0x82: "…"}
DIGITS = "0123456789"

# The glucose number comes in five sizes: the layout picks the largest one
# whose widest value ("888" or "88.8") still fits beside the arrow. The 184 px
# size is for the screen without a graph.
FONTS = [
    # name, file, px, charset, tabular digits
    ("F_NUM_S", "inter_bold.ttf", 100, DIGITS + ".", False),
    ("F_NUM_M", "inter_bold.ttf", 116, DIGITS + ".", False),
    ("F_NUM_X", "inter_bold.ttf", 132, DIGITS + ".", False),
    ("F_NUM_L", "inter_bold.ttf", 150, DIGITS + ".", False),
    ("F_NUM_XL", "inter_bold.ttf", 184, DIGITS + ".", False),
    ("F_CLOCK", "inter_semibold.ttf", 58, DIGITS + ":", True),
    ("F_HEAD", "inter_bold.ttf", 62, "No Data", False),
    ("F_CODE", "inter_bold.ttf", 46, DIGITS + "ABCDEFGHJKMNPQRSTVWXYZ", False),
    ("F_TITLE", "inter_bold.ttf", 28, ASCII, False),
    ("F_MED", "inter_semibold.ttf", 21, ASCII, False),
    ("F_SMALL", "inter_medium.ttf", 16, ASCII, False),
    ("F_TINY", "inter_semibold.ttf", 12, ASCII, False),
]

# One arrow set per number size, about half the number's height.
ARROWS = [("F_ARROW_XS", 34), ("F_ARROW_S", 52), ("F_ARROW_M", 60), ("F_ARROW_L", 78)]
MARK_PX = 26           # the drop mark beside the wordmark
ICON_PX = 88           # the app icon on the splash and pairing screens
PAGE_BG = (0x0A, 0x0E, 0x11)   # C_BG in the firmware; the icon is pre-blended on it


def pack4(img):
    """8-bit alpha image -> 4-bit nibbles, two per byte, row major."""
    w, h = img.size
    px = img.load()
    nib = [(px[x, y] + 8) // 17 for y in range(h) for x in range(w)]
    if len(nib) % 2:
        nib.append(0)
    return bytearray((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))


def build_font(path, px, charset, tabular):
    font = ImageFont.truetype(path, px)
    ascent, descent = font.getmetrics()
    codes = {ord(c): c for c in charset}
    if charset is ASCII:
        codes.update(SPECIALS)
    first, last = min(codes), max(codes)
    cell = max(round(font.getlength(d)) for d in DIGITS) if tabular else 0
    bitmap = bytearray()
    glyphs = []
    for code in range(first, last + 1):
        ch = codes.get(code)
        if ch is None:
            glyphs.append((len(bitmap), 0, 0, 0, 0, 0))
            continue
        adv = max(1, round(font.getlength(ch)))
        bbox = font.getbbox(ch)
        if bbox is None or bbox[2] - bbox[0] == 0 or bbox[3] - bbox[1] == 0:
            glyphs.append((len(bitmap), 0, 0, adv, 0, 0))
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, fill=255, font=font)
        xo = x0
        if tabular and ch in DIGITS:
            # every digit sits centred in one cell, so a clock never shifts
            xo = (cell - w) // 2
            adv = cell
        off = len(bitmap)
        bitmap += pack4(img)
        glyphs.append((off, w, h, adv, xo, y0 - ascent))
    return first, last, bitmap, glyphs, ascent + descent


def build_arrows(px):
    """Trend codes 1..5 (down, down-right, flat, up-right, up) as glyphs '1'..'5':
    the app's solid arrow, a thick shaft into a wide triangular head."""
    ss = 8
    s = px * ss
    base = Image.new("L", (s, s), 0)
    d = ImageDraw.Draw(base)
    mid = s / 2
    tail, neck, tip = s * 0.10, s * 0.52, s * 0.92
    shaft, head = s * 0.105, s * 0.30
    d.polygon([(tail, mid - shaft), (neck, mid - shaft), (neck, mid - head), (tip, mid),
               (neck, mid + head), (neck, mid + shaft), (tail, mid + shaft)], fill=255)
    angles = {1: -90, 2: -45, 3: 0, 4: 45, 5: 90}   # PIL rotates counter-clockwise
    bitmap = bytearray()
    glyphs = []
    for code in range(1, 6):
        img = base.rotate(angles[code], resample=Image.BICUBIC).resize((px, px), Image.LANCZOS)
        off = len(bitmap)
        bitmap += pack4(img)
        glyphs.append((off, px, px, px, 0, -px))
    return ord("1"), ord("5"), bitmap, glyphs, px


def build_mark(px):
    """The drop from the app icon as one alpha glyph ('M'), so it can be drawn in
    any ink: the white of the icon is the shape, the green trace cuts through."""
    src = Image.open(ICON_PNG).convert("RGB")
    w, h = src.size
    sp = src.load()
    mask = Image.new("L", (w, h), 0)
    mp = mask.load()
    for y in range(h):
        for x in range(w):
            m = min(sp[x, y])
            mp[x, y] = max(0, min(255, int((m - 118) * 255 / 60)))
    mask = mask.crop(mask.getbbox())
    scale = px / mask.height
    mask = mask.resize((max(1, round(mask.width * scale)), px), Image.LANCZOS)
    return ord("M"), ord("M"), pack4(mask), [(0, mask.width, px, mask.width, 0, -px)], px


def build_icon(px):
    src = Image.open(ICON_PNG).convert("RGBA").resize((px, px), Image.LANCZOS)
    # the launcher icon's rounded square, cut here because the source is a full bleed tile
    ss = 4
    corner = Image.new("L", (px * ss, px * ss), 0)
    ImageDraw.Draw(corner).rounded_rectangle([0, 0, px * ss - 1, px * ss - 1], radius=int(px * ss * 0.2237), fill=255)
    src.putalpha(corner.resize((px, px), Image.LANCZOS))
    bg = Image.new("RGBA", (px, px), PAGE_BG + (255,))
    flat = Image.alpha_composite(bg, src).convert("RGB")
    out = []
    for y in range(px):
        for x in range(px):
            r, g, b = flat.getpixel((x, y))
            out.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return out


def emit_font(f, name, first, last, bitmap, glyphs, line):
    f.write(f"static const uint8_t {name}_bmp[] PROGMEM = {{\n")
    for i in range(0, len(bitmap), 20):
        f.write("  " + ",".join(f"0x{b:02X}" for b in bitmap[i:i + 20]) + ",\n")
    f.write("};\n")
    f.write(f"static const AAGlyph {name}_gl[] PROGMEM = {{\n")
    for g in glyphs:
        f.write("  {%d,%d,%d,%d,%d,%d},\n" % g)
    f.write("};\n")
    f.write(f"static const AAFont {name} = {{ {name}_bmp, {name}_gl, {first}, {last}, {line} }};\n\n")
    print(f"{name}: {len(bitmap)} B, {len(glyphs)} glyphs, line {line}")


def main():
    with open(OUT, "w") as f:
        f.write("// GENERATED by tools/gen_assets.py, do not edit by hand.\n")
        f.write("// Fonts: Inter (SIL Open Font License 1.1), 4-bit alpha glyphs.\n")
        f.write("#ifndef SS_ASSETS_H\n#define SS_ASSETS_H\n")
        f.write("#ifdef ARDUINO\n#include <Arduino.h>\n#else\n#include <stdint.h>\n#define PROGMEM\n#endif\n\n")
        f.write("struct AAGlyph { uint32_t off; uint8_t w, h, adv; int8_t xo; int16_t yo; };\n")
        f.write("struct AAFont { const uint8_t *bmp; const AAGlyph *gl; uint8_t first, last, line; };\n\n")
        for name, file, px, charset, tabular in FONTS:
            emit_font(f, name, *build_font(os.path.join(FONT_DIR, file), px, charset, tabular))
        for name, px in ARROWS:
            emit_font(f, name, *build_arrows(px))
        emit_font(f, "F_MARK", *build_mark(MARK_PX))
        icon = build_icon(ICON_PX)
        f.write(f"#define MARK_PX {MARK_PX}\n#define ICON_PX {ICON_PX}\n")
        f.write("static const uint16_t ICON_RGB565[] PROGMEM = {\n")
        for i in range(0, len(icon), 16):
            f.write("  " + ",".join(f"0x{v:04X}" for v in icon[i:i + 16]) + ",\n")
        f.write("};\n\n#endif\n")
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
