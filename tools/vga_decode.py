#!/usr/bin/env python3
"""vga_decode.py - decode a QEMU screendump PPM back to terminal text.

OpSys term renders 8x16 glyphs on a 9x20 px cell grid (1px right +
4px bottom spacing, matching kernel FB_COL/FB_ROW): 1024x768 fb =>
113 x 38 cells.  Glyph pixels are white (0xFFFFFF) on dark blue
(0x082860); the cursor cell is inverted (fg/bg swapped).

Usage:
    python3 tools/vga_decode.py <screen.ppm> [--font font.h]
        --font   path to term/font.h (default: user/services/term/font.h)

Output: 113x38 decoded text grid, one line per cell row.
"""
import os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FONT = os.path.join(REPO, "user/services/term/font.h")

CELL_W, CELL_H = 9, 20
GLYPH_W, GLYPH_H = 8, 16
LUM_THRESH = 400     # white(765) vs dark blue(144)
MAX_DIST = 8         # allowed bit errors per glyph (antialias/shimmer)


def parse_font(path):
    """Extract {code: [16 bytes]} from term/font.h (s_font[95][16]).

    Parsed by BLOCK ORDER (index i -> char 0x20+i), not by the comment
    char: glyph 0x27 is written `'\''` in C and a comment-driven regex
    would mis-parse the escaped quote."""
    txt = open(path, "r", encoding="utf-8").read()
    glyphs = {}
    m = re.search(r"s_font\[95\]\[16\]\s*=\s*\{", txt)
    if not m:
        return glyphs
    blocks = re.findall(r"\{([^}]*)\}", txt[m.end():])[:95]
    for i, b in enumerate(blocks):
        bs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", b)]
        if len(bs) == 16:
            glyphs[0x20 + i] = bytes(bs)
    return glyphs


def parse_ppm(path):
    """Return (width, height, pixel_rgb_bytes).  P6, 24bpp, no extra rows."""
    data = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s*", data)
    if not m:
        raise ValueError("not a P6 PPM: %r" % data[:16])
    w, h, maxv = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if maxv != 255:
        raise ValueError("unsupported maxval %d" % maxv)
    px = data[m.end():]
    if len(px) < w * h * 3:
        raise ValueError("short pixel data: %d < %d" % (len(px), w * h * 3))
    return w, h, px


def cell_bitmap(w, px, cx, cy):
    """Extract 16x8 bitmask for cell (cx,cy): bit=1 where pixel is bright."""
    bits = []
    for row in range(GLYPH_H):
        byte = 0
        for col in range(GLYPH_W):
            x = cx * CELL_W + col
            y = cy * CELL_H + row
            i = (y * w + x) * 3
            lum = px[i] + px[i + 1] + px[i + 2]
            byte = (byte << 1) | (1 if lum > LUM_THRESH else 0)
        bits.append(byte)
    return bytes(bits)


def match_glyph(bits, glyphs):
    """Nearest glyph, trying both normal (bright=fg) and inverted (cursor)
    interpretations.  Returns (char, hamming_distance)."""
    best_ch, best_d = " ", MAX_DIST
    for code, g in glyphs.items():
        d1 = sum(a ^ b for a, b in zip(bits, g))          # normal
        d2 = sum((255 - a) ^ b for a, b in zip(bits, g))  # cursor-inverted
        d = d1 if d1 < d2 else d2
        if d < best_d:
            best_d, best_ch = d, chr(code)
    return best_ch, best_d


def decode(w, h, px, glyphs):
    """Decode full screen to list of text rows (one per cell row)."""
    cols, rows = w // CELL_W, h // CELL_H
    lines = []
    for cy in range(rows):
        line = []
        for cx in range(cols):
            bits = cell_bitmap(w, px, cx, cy)
            ch, _ = match_glyph(bits, glyphs)
            line.append(ch)
        lines.append("".join(line))
    return lines


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        sys.exit(2)
    ppm_path = args[0]
    font_path = DEFAULT_FONT
    if "--font" in sys.argv:
        font_path = sys.argv[sys.argv.index("--font") + 1]

    glyphs = parse_font(font_path)
    w, h, px = parse_ppm(ppm_path)
    for line in decode(w, h, px, glyphs):
        print(line.rstrip())


if __name__ == "__main__":
    main()
