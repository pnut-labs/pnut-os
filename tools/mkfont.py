#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
"""Turn a TrueType font into a 1-bit LVGL 9 font, as C (lv_font_fmt_txt).

    mkfont.py FONT.ttf SIZE NAME text|digits OUT.c

The glyphs are drawn by FreeType (freetype-py) for a monochrome target with
its auto-hinter, which fits the outlines and the advances to whole pixels:
LVGL places glyphs on whole pixels, so fractional advances would be rounded
one glyph at a time and the letters would drift apart.  What the e-paper
shows is what FreeType drew.

"text" is Latin (ASCII, Latin-1, Latin Extended-A) and common punctuation;
"digits" what a clock or a PIN needs.  The output defines
`const lv_font_t NAME;`.  The system UI's fonts are made in build/ui at
build time, never committed.
"""
import sys

import freetype

# The text fonts' characters; src/ui/describe.c (describe_drawn) shows a
# network's name with these alone, the others as \xNN: keep them alike
TEXT = [cp for lo, hi in ((0x20, 0x7E), (0xA0, 0x17F))
        for cp in range(lo, hi + 1)]
TEXT += [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
         0x2190, 0x2192]
DIGITS = [ord(c) for c in " 0123456789:"] + [0x2022, 0x25CF]

LOAD = (freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO |
        freetype.FT_LOAD_FORCE_AUTOHINT)


def glyph(face, cp):
    """(advance in 1/16 px, box w, box h, x offset, y offset, bits) for cp,
    or None where the font has none.  The offsets are from the pen on the
    baseline; y is the box's bottom above the baseline."""
    if cp != 0x20 and face.get_char_index(cp) == 0:
        return None
    face.load_char(chr(cp), LOAD)
    g = face.glyph
    bm = g.bitmap
    adv = (g.advance.x + 32) >> 6
    rows = [[(bm.buffer[y * bm.pitch + x // 8] >> (7 - x % 8)) & 1
             for x in range(bm.width)] for y in range(bm.rows)]
    return box(rows, adv * 16, g.bitmap_left, g.bitmap_top)


def box(rows, adv, left, top):
    """The ink's box of a bitmap whose top left is at (left, top) above the
    baseline, as glyph() gives it."""
    ink = [(x, y) for y, r in enumerate(rows) for x, v in enumerate(r) if v]
    if not ink:
        return adv, 0, 0, 0, 0, []
    x0 = min(x for x, _ in ink)
    x1 = max(x for x, _ in ink) + 1
    y0 = min(y for _, y in ink)
    y1 = max(y for _, y in ink) + 1
    bits = [rows[y][x] for y in range(y0, y1) for x in range(x0, x1)]
    return adv, x1 - x0, y1 - y0, left + x0, top - y1, bits


def pack(bits):
    """Bits as one MSB-first stream, rows not padded (LVGL, 1 bpp)."""
    out = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            out[i // 8] |= 0x80 >> (i % 8)
    return out


def runs(cps):
    """Sorted code points as runs of consecutive ones."""
    out = []
    for cp in cps:
        if out and cp == out[-1][1] + 1:
            out[-1][1] = cp
        else:
            out.append([cp, cp])
    return out


def write(out, name, comment, glyphs, line_height, base_line,
          ul_pos=-1, ul_thick=1):
    """Write glyphs ({code point: glyph()}) as C defining
    `const lv_font_t name;`.  Returns the bitmaps' size."""
    bitmap = bytearray()
    dscs = ["  {0}  /* id 0: none */"]
    for cp in sorted(glyphs):
        adv, bw, bh, ox, oy, bits = glyphs[cp]
        dscs.append("  {.bitmap_index = %d, .adv_w = %d, .box_w = %d, "
                    ".box_h = %d, .ofs_x = %d, .ofs_y = %d}"
                    % (len(bitmap), adv, bw, bh, ox, oy))
        bitmap += pack(bits)

    cmaps = []
    gid = 1
    for lo, hi in runs(sorted(glyphs)):
        cmaps.append("  {.range_start = %d, .range_length = %d, "
                     ".glyph_id_start = %d, .list_length = 0, "
                     ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}"
                     % (lo, hi - lo + 1, gid))
        gid += hi - lo + 1

    lines = ["/* %s */" % comment, "",
             "#include <lvgl/lvgl.h>", "",
             "static const uint8_t g_bitmap[] =", "{"]
    for i in range(0, len(bitmap), 12):
        lines.append("  " + ", ".join("0x%02x" % b
                                      for b in bitmap[i:i + 12]) + ",")
    if not bitmap:
        lines.append("  0x00")
    lines += ["};", "",
              "static const lv_font_fmt_txt_glyph_dsc_t g_dsc[] =", "{",
              ",\n".join(dscs), "};", "",
              "static const lv_font_fmt_txt_cmap_t g_cmaps[] =", "{",
              ",\n".join(cmaps), "};", "",
              "static const lv_font_fmt_txt_dsc_t g_font =", "{",
              "  .glyph_bitmap = g_bitmap,",
              "  .glyph_dsc = g_dsc,",
              "  .cmaps = g_cmaps,",
              "  .cmap_num = %d," % len(cmaps),
              "  .bpp = 1,", "};", "",
              "const lv_font_t %s =" % name, "{",
              "  .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
              "  .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
              "  .line_height = %d," % line_height,
              "  .base_line = %d," % base_line,
              "  .subpx = LV_FONT_SUBPX_NONE,",
              "  .underline_position = %d," % ul_pos,
              "  .underline_thickness = %d," % ul_thick,
              "  .dsc = &g_font,", "};", ""]
    with open(out, "w") as f:
        f.write("\n".join(lines))
    return len(bitmap)


def main():
    if len(sys.argv) != 6 or sys.argv[4] not in ("text", "digits"):
        sys.exit("usage: mkfont.py FONT.ttf SIZE NAME text|digits OUT.c")
    path, size, name, chars, out = sys.argv[1:]
    size = int(size)

    face = freetype.Face(path)
    face.set_pixel_sizes(0, size)
    ascent = (face.size.ascender + 63) >> 6
    descent = (-face.size.descender + 63) >> 6

    glyphs = {}
    for cp in sorted(set(TEXT if chars == "text" else DIGITS)):
        g = glyph(face, cp)
        if g is not None:
            glyphs[cp] = g

    # The line holds every glyph, accents over capitals included: a label
    # clips to its line

    for _, _, bh, _, oy, _ in glyphs.values():
        if bh > 0:
            ascent = max(ascent, oy + bh)
            descent = max(descent, -oy)

    comment = ("%s: %s at %d px, 1 bpp, %d glyphs; made by tools/mkfont.py."
               "\n * The font is under the SIL Open Font License "
               "(src/ui/theme/fonts/OFL.txt)."
               % (name, path.rsplit("/", 1)[-1], size, len(glyphs)))
    n = write(out, name, comment, glyphs, ascent + descent, descent,
              -max(1, size // 10), max(1, size // 14))
    print("%s: %d glyphs, %d bytes, line %d px"
          % (name, len(glyphs), n, ascent + descent))


if __name__ == "__main__":
    main()
