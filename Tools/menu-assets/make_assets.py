#!/usr/bin/env python3
"""Generate Application/SwitchMenuApp/Assets.h for the touch menu.

- Fonts: Noto Sans JP (SIL Open Font License 1.1), rasterised anti-aliased to
  4-bit alpha glyphs. Only bitmaps go into the firmware, no font file.
  Large size: only the characters of the fixed labels; small size: ASCII.
- Icons: drawn here (4x supersampled), 4-bit alpha masks.
- Background: 160 x 90 RGB, a dark blue gradient with soft waves; the menu
  scales it up bilinearly to 1280 x 720.

Usage: python make_assets.py <NotoSansJP-Medium.otf> <NotoSansJP-Regular.otf> <out Assets.h>
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

BIG_TEXT = [
    "Switch Boot Manager", "Boot Windows", "Return to hekate", "UEFI settings", "Power off",
]
BIG_SIZE = 40
MID_SIZE = 30
SMALL_SIZE = 22
SS = 4  # icon supersampling


def font_glyphs(path, size, chars):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    glyphs = {}
    for ch in chars:
        bbox = font.getbbox(ch)
        adv = int(round(font.getlength(ch)))
        if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            glyphs[ch] = (0, 0, 0, 0, adv, b"")
            continue
        w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-bbox[0], -bbox[1]), ch, font=font, fill=255)
        glyphs[ch] = (w, h, bbox[0], bbox[1], adv, pack4(img))
    return ascent, descent, glyphs


def pack4(img):
    """4-bit alpha, two pixels per byte (low nibble first), rows padded to a byte."""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            a = (px[x, y] + 8) // 17
            b = (px[x + 1, y] + 8) // 17 if x + 1 < w else 0
            out.append(min(a, 15) | (min(b, 15) << 4))
    return bytes(out)


def icon(size, draw_fn):
    big = Image.new("L", (size * SS, size * SS), 0)
    draw_fn(ImageDraw.Draw(big), size * SS)
    return big.resize((size, size), Image.LANCZOS)


def ic_windows(d, s):
    g = s * 0.06
    m = s * 0.08
    half = (s - 2 * m - g) / 2
    for i in range(2):
        for j in range(2):
            x0 = m + i * (half + g)
            y0 = m + j * (half + g)
            d.rectangle([x0, y0, x0 + half, y0 + half], fill=255)


def ic_peak(d, s):
    w = s * 0.07
    base = s * 0.86
    pts = [(s * 0.08, base), (s * 0.5, s * 0.12), (s * 0.92, base)]
    d.line(pts + [pts[0]], fill=255, width=int(w), joint="curve")
    # snow cap
    d.polygon([(s * 0.5, s * 0.12), (s * 0.36, s * 0.40), (s * 0.44, s * 0.35), (s * 0.5, s * 0.44),
               (s * 0.56, s * 0.35), (s * 0.64, s * 0.40)], fill=255)
    # foot
    d.polygon([(s * 0.30, base), (s * 0.5, s * 0.62), (s * 0.70, base)], fill=255)


def ic_chip(d, s):
    w = int(s * 0.06)
    a, b = s * 0.24, s * 0.76
    d.rounded_rectangle([a, a, b, b], radius=s * 0.06, outline=255, width=w)
    for k in range(4):
        p = a + (b - a) * (k + 1) / 5
        L = s * 0.12
        d.line([(p, a - L), (p, a - s * 0.02)], fill=255, width=w)
        d.line([(p, b + s * 0.02), (p, b + L)], fill=255, width=w)
        d.line([(a - L, p), (a - s * 0.02, p)], fill=255, width=w)
        d.line([(b + s * 0.02, p), (b + L, p)], fill=255, width=w)


def ic_power(d, s):
    w = int(s * 0.085)
    m = s * 0.14
    d.arc([m, m, s - m, s - m], start=-60, end=240, fill=255, width=w)
    d.line([(s / 2, s * 0.06), (s / 2, s * 0.48)], fill=255, width=w)
    r = w / 2
    for y in (s * 0.06, s * 0.48):
        d.ellipse([s / 2 - r, y - r, s / 2 + r, y + r], fill=255)


def ic_rotate(d, s):
    w = int(s * 0.09)
    m = s * 0.16
    d.arc([m, m, s - m, s - m], start=-40, end=250, fill=255, width=w)
    # arrow head at the -40 degree end
    ang = math.radians(-40)
    cx = s / 2 + (s / 2 - m - w / 2) * math.cos(ang)
    cy = s / 2 + (s / 2 - m - w / 2) * math.sin(ang)
    h = s * 0.22
    d.polygon([(cx - h * 0.9, cy - h * 0.05), (cx + h * 0.55, cy - h * 0.55), (cx + h * 0.25, cy + h * 0.75)], fill=255)


def ic_chevron(d, s):
    w = int(s * 0.12)
    d.line([(s * 0.36, s * 0.18), (s * 0.66, s * 0.5), (s * 0.36, s * 0.82)], fill=255, width=w, joint="curve")


def ic_tap(d, s):
    w = int(s * 0.08)
    d.ellipse([s * 0.12, s * 0.12, s * 0.88, s * 0.88], outline=255, width=w)
    d.ellipse([s * 0.34, s * 0.34, s * 0.66, s * 0.66], fill=255)


def ic_volume(d, s):
    d.polygon([(s * 0.08, s * 0.36), (s * 0.28, s * 0.36), (s * 0.50, s * 0.14), (s * 0.50, s * 0.86),
               (s * 0.28, s * 0.64), (s * 0.08, s * 0.64)], fill=255)
    w = int(s * 0.07)
    d.arc([s * 0.40, s * 0.30, s * 0.76, s * 0.70], start=-50, end=50, fill=255, width=w)
    d.arc([s * 0.36, s * 0.14, s * 0.96, s * 0.86], start=-50, end=50, fill=255, width=w)


def background(w, h):
    """Soft base: deep navy, lighter and bluer toward the right, a glow at the right."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            u, v = x / (w - 1), y / (h - 1)
            r, g, b = 3 + 5 * u, 8 + 12 * u, 26 + 34 * u
            gl = math.exp(-(((u - 0.92) / 0.35) ** 2 + ((v - 0.55) / 0.55) ** 2))
            gl2 = math.exp(-(((u - 0.15) / 0.30) ** 2 + ((v - 1.0) / 0.35) ** 2))
            r += 10 * gl + 4 * gl2
            g += 40 * gl + 18 * gl2
            b += 110 * gl + 60 * gl2
            px[x, y] = (min(255, int(r)), min(255, int(g)), min(255, int(b)))
    return img


# Ribbons: per landscape column x (0..1279): centre y, half width, intensity.
RIBBONS = [
    # (colour, centre(u), half width(u), intensity(u))
    (0x2F6BFF, lambda u: 120 + 560 * u - 260 * math.sin(math.pi * u) , lambda u: 70 + 120 * math.sin(math.pi * u * 0.9),
     lambda u: 0.20 + 0.80 * min(1.0, max(0.0, (u - 0.30) / 0.45))),
    (0x1A4FE0, lambda u: 760 - 420 * u + 120 * math.sin(2.2 * math.pi * u), lambda u: 60 + 50 * u,
     lambda u: 0.55 * (1 - u) + 0.15),
    (0x5AA0FF, lambda u: 40 + 300 * u + 90 * math.sin(1.6 * math.pi * u + 0.8), lambda u: 26 + 30 * u,
     lambda u: 0.65 * max(0.0, (u - 0.45) / 0.55)),
]


def ribbon_profile():
    """Brightness (0..255) across a ribbon: t = -2..2 in 128 steps (|t| = 1 is the edge)."""
    out = []
    for i in range(128):
        t = (i - 64) / 32.0
        body = 0.30 * max(0.0, 1 - t * t)
        rim = math.exp(-((t + 0.9) / 0.10) ** 2)                 # bright upper edge
        rim2 = 0.35 * math.exp(-((t - 0.95) / 0.18) ** 2)        # dimmer lower edge
        halo = 0.18 * math.exp(-max(0.0, abs(t) - 1) * 3.0) if abs(t) > 1 else 0.18
        out.append(min(255, int(255 * (body + rim + rim2 + halo * 0.6))))
    return out


def c_bytes(data, indent="  "):
    lines = []
    for i in range(0, len(data), 24):
        lines.append(indent + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",")
    return "\n".join(lines)


def emit_font(out, name, ascent, descent, glyphs):
    data = bytearray()
    table = []
    for code in range(0x20, 0x7F):
        ch = chr(code)
        if ch in glyphs:
            w, h, xo, yo, adv, bits = glyphs[ch]
            table.append((w, h, xo, yo, adv, len(data)))
            data += bits
        else:
            table.append((0, 0, 0, 0, 0, 0))
    out.append("STATIC CONST UINT8 m%sBits[%d] = {\n%s\n};" % (name, max(1, len(data)), c_bytes(data or b"\0")))
    out.append("STATIC CONST MENU_GLYPH m%sGlyphs[0x5F] = {" % name)
    for code, (w, h, xo, yo, adv, off) in zip(range(0x20, 0x7F), table):
        out.append("  { %d, %d, %d, %d, %d, %d },  // %r" % (w, h, xo, yo, adv, off, chr(code)))
    out.append("};")
    out.append("STATIC CONST MENU_FONT m%s = { %d, %d, m%sGlyphs, m%sBits };\n" % (name, ascent, ascent + descent, name, name))
    return len(data)


def main():
    medium, regular, out_path = sys.argv[1:4]
    out = ["/** @file",
           "  Generated by Tools/menu-assets/make_assets.py - do not edit.",
           "",
           "  Glyph bitmaps rasterised from Noto Sans JP (Copyright 2014-2021 Adobe,",
           "  SIL Open Font License 1.1, https://openfontlicense.org). Icons and the",
           "  background are drawn by the script.",
           "**/",
           "",
           "#ifndef SWITCH_MENU_ASSETS_H_",
           "#define SWITCH_MENU_ASSETS_H_",
           "",
           "typedef struct { UINT8 W, H; INT8 XOff, YOff; UINT8 Advance; UINT32 Offset; } MENU_GLYPH;",
           "typedef struct { UINT8 Ascent, LineHeight; CONST MENU_GLYPH *Glyphs; CONST UINT8 *Bits; } MENU_FONT;",
           "typedef struct { UINT16 Size; CONST UINT8 *Bits; } MENU_ICON;",
           ""]
    total = 0
    big_chars = sorted(set("".join(BIG_TEXT)))
    ascii_chars = [chr(c) for c in range(0x20, 0x7F)]
    total += emit_font(out, "FontBig", *font_glyphs(medium, BIG_SIZE, big_chars))
    total += emit_font(out, "FontMid", *font_glyphs(regular, MID_SIZE, ascii_chars))
    total += emit_font(out, "FontSmall", *font_glyphs(regular, SMALL_SIZE, ascii_chars))

    icons = [("IconWindows", 84, ic_windows), ("IconPeak", 92, ic_peak), ("IconChip", 92, ic_chip),
             ("IconPower", 88, ic_power), ("IconRotate", 32, ic_rotate), ("IconChevron", 32, ic_chevron),
             ("IconTap", 26, ic_tap), ("IconVolume", 26, ic_volume), ("IconPowerSmall", 26, ic_power)]
    for name, size, fn in icons:
        bits = pack4(icon(size, fn))
        total += len(bits)
        out.append("STATIC CONST UINT8 m%sBits[%d] = {\n%s\n};" % (name, len(bits), c_bytes(bits)))
        out.append("STATIC CONST MENU_ICON m%s = { %d, m%sBits };\n" % (name, size, name))

    prof = ribbon_profile()
    out.append("STATIC CONST UINT8 mRibbonProfile[128] = {\n%s\n};" % c_bytes(bytes(prof)))
    out.append("typedef struct { UINT32 Color; INT16 Cy[1280]; UINT8 Hw[1280]; UINT8 Level[1280]; } MENU_RIBBON;")
    out.append("STATIC CONST MENU_RIBBON mRibbons[%d] = {" % len(RIBBONS))
    for color, cy, hw, lvl in RIBBONS:
        us = [x / 1279 for x in range(1280)]
        out.append("  { 0x%06X," % color)
        out.append("    {" + ",".join(str(int(round(cy(u)))) for u in us) + "},")
        out.append("    {" + ",".join(str(max(4, min(255, int(round(hw(u)))))) for u in us) + "},")
        out.append("    {" + ",".join(str(max(0, min(255, int(round(255 * lvl(u)))))) for u in us) + "} },")
    out.append("};\n")
    total += len(RIBBONS) * 1280 * 4

    bw, bh = 160, 90
    bg = background(bw, bh).tobytes()
    total += len(bg)
    out.append("#define MENU_BG_W %d\n#define MENU_BG_H %d" % (bw, bh))
    out.append("STATIC CONST UINT8 mBackground[%d] = {  // RGB\n%s\n};\n" % (len(bg), c_bytes(bg)))
    out.append("#endif")
    with open(out_path, "w", newline="\r\n") as f:
        f.write("\n".join(out) + "\n")
    print("asset bytes:", total)


if __name__ == "__main__":
    main()
