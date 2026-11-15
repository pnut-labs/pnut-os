#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
"""Draw the system UI's icons as 1-bit LVGL fonts, one for each size.

    mkicons.py OUTDIR ICON.svg...

The icons are the design's 24x24 SVGs (square caps, mitred joins), drawn
without anti-aliasing by PyMuPDF at each size the UI uses, with the
design's stroke for that size unless an SVG gives its own.  As fonts they
take the colour of the text around them, so an icon on an inverted row
inverts with it.  The icons get code points from U+E000 in the order of
their file names; OUTDIR/icons.h names them, UI_ICON_<NAME> as a UTF-8
string for a label.  OUTDIR gets icons_<size>.c, defining g_ui_icons_<size>.
"""
import os
import re
import sys

import pymupdf

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfont import box, write  # noqa: E402

# Size and the design's stroke at that size

SIZES = [(11, 2.4), (12, 2.4), (14, 2.4), (16, 2.2), (18, 2.0), (20, 2.0)]
BASE = 0xE000


def render(svg, size, stroke):
    """The icon's pixels at size, rows of 0 and 1."""
    root = re.search(r"<svg\b[^>]*>", svg).group(0)
    inner = svg[svg.index(root) + len(root):svg.rindex("</svg>")]
    own = re.search(r'stroke-width="([\d.]+)"', root)
    doc = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
           'viewBox="0 0 24 24" fill="none" stroke="#000" '
           'stroke-width="%s" stroke-linecap="square" '
           'stroke-linejoin="miter">%s</svg>'
           % (size, size, own.group(1) if own else stroke, inner))
    page = pymupdf.open(stream=doc.encode(), filetype="svg")[0]
    pix = page.get_pixmap(colorspace=pymupdf.csGRAY, alpha=False)
    return [[1 if pix.pixel(x, y)[0] < 128 else 0 for x in range(pix.width)]
            for y in range(pix.height)]


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: mkicons.py OUTDIR ICON.svg...")
    out = sys.argv[1]
    svgs = sorted(sys.argv[2:], key=os.path.basename)
    names = [os.path.basename(p)[:-4].upper().replace("-", "_")
             for p in svgs]
    texts = [open(p).read() for p in svgs]

    pymupdf.TOOLS.set_aa_level(0)
    for size, stroke in SIZES:
        glyphs = {}
        for i, svg in enumerate(texts):
            glyphs[BASE + i] = box(render(svg, size, stroke), size * 16,
                                   0, size)
        name = "g_ui_icons_%d" % size
        n = write(os.path.join(out, "icons_%d.c" % size), name,
                  "%s: the system UI's icons at %d px, 1 bpp; made by "
                  "tools/mkicons.py." % (name, size), glyphs, size, 0)
        print("%s: %d icons, %d bytes" % (name, len(glyphs), n))

    lines = ["/* The system UI's icons, as UTF-8 strings for labels; made by",
             " * tools/mkicons.py.",
             " */", "",
             "#ifndef __PNUT_OS_UI_ICONS_H",
             "#define __PNUT_OS_UI_ICONS_H", ""]
    for i, name in enumerate(names):
        utf8 = chr(BASE + i).encode()
        lines.append("#define UI_ICON_%-14s \"%s\""
                     % (name, "".join("\\x%02x" % b for b in utf8)))
    lines += ["", "#endif /* __PNUT_OS_UI_ICONS_H */", ""]
    with open(os.path.join(out, "icons.h"), "w") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
