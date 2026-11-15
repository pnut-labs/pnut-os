#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
"""Compare the system UI's screens, as its tests drew them, with the images
kept in the source, or write those images again.

    screens.py check  DRAWN KEPT    each DRAWN/<name>.pbm against
                                    KEPT/<name>.png, pixel for pixel
    screens.py update DRAWN KEPT    write KEPT/<name>.png from each PBM,
                                    and remove images no test draws

The tests (tests/unit/test_ui_screens.c) write 1-bit PBM pictures (P4); the
images kept are PNGs, 8-bit grey, black and white, one pixel a pixel, so
that a pull request shows a screen's change as an image.  Only Python's
standard library: zlib for the PNGs.
"""
import os
import struct
import sys
import zlib


def read_pbm(path):
    """(width, height, rows of 0/1, 1 black) from a P4 PBM."""
    with open(path, "rb") as f:
        data = f.read()
    magic, size, bits = data.split(b"\n", 2)
    if magic != b"P4":
        raise ValueError("%s: not a P4 PBM" % path)
    w, h = (int(v) for v in size.split())
    stride = (w + 7) // 8
    return w, h, [[(bits[y * stride + x // 8] >> (7 - x % 8)) & 1
                   for x in range(w)] for y in range(h)]


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(0 if v else 255 for v in row)
                   for row in rows)

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def read_png(path):
    """(width, height, rows of 0/1) from a PNG this tool wrote: 8-bit grey,
    no interlace, every row filter 0."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    i = 8
    idat = b""
    w = h = 0
    while i < len(data):
        n = struct.unpack(">I", data[i:i + 4])[0]
        tag = data[i + 4:i + 8]
        body = data[i + 8:i + 8 + n]
        if tag == b"IHDR":
            w, h, depth, colour, _, _, interlace = struct.unpack(
                ">IIBBBBB", body)
            if depth != 8 or colour != 0 or interlace != 0:
                raise ValueError("%s: not 8-bit grey" % path)
        elif tag == b"IDAT":
            idat += body
        i += 12 + n
    raw = zlib.decompress(idat)
    rows = []
    for y in range(h):
        line = raw[y * (w + 1):(y + 1) * (w + 1)]
        if line[0] != 0:
            raise ValueError("%s: a row filtered, not written here" % path)
        rows.append([0 if v >= 128 else 1 for v in line[1:]])
    return w, h, rows


def names(directory, ext):
    return sorted(f[:-len(ext)] for f in os.listdir(directory)
                  if f.endswith(ext)) if os.path.isdir(directory) else []


def check(drawn, kept):
    bad = []
    for name in names(drawn, ".pbm"):
        png = os.path.join(kept, name + ".png")
        if not os.path.exists(png):
            bad.append("%s: no image kept" % name)
            continue
        a = read_pbm(os.path.join(drawn, name + ".pbm"))
        b = read_png(png)
        if a[:2] != b[:2]:
            bad.append("%s: %dx%d, kept %dx%d" % ((name,) + a[:2] + b[:2]))
            continue
        diff = sum(1 for ra, rb in zip(a[2], b[2])
                   for va, vb in zip(ra, rb) if va != vb)
        if diff:
            bad.append("%s: %d pixels differ" % (name, diff))
    for name in names(kept, ".png"):
        if not os.path.exists(os.path.join(drawn, name + ".pbm")):
            bad.append("%s: kept, but no test draws it" % name)
    for line in bad:
        print("screens: " + line, file=sys.stderr)
    if bad:
        print("screens: look at the screens; if they are right, "
              "`make screens` keeps them", file=sys.stderr)
        return 1
    print("screens: %d match" % len(names(drawn, ".pbm")))
    return 0


def update(drawn, kept):
    os.makedirs(kept, exist_ok=True)
    for name in names(drawn, ".pbm"):
        w, h, rows = read_pbm(os.path.join(drawn, name + ".pbm"))
        write_png(os.path.join(kept, name + ".png"), w, h, rows)
    for name in names(kept, ".png"):
        if not os.path.exists(os.path.join(drawn, name + ".pbm")):
            os.remove(os.path.join(kept, name + ".png"))
    print("screens: %d kept" % len(names(drawn, ".pbm")))
    return 0


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("check", "update"):
        sys.exit("usage: screens.py check|update DRAWN KEPT")
    run = check if sys.argv[1] == "check" else update
    sys.exit(run(sys.argv[2], sys.argv[3]))


if __name__ == "__main__":
    main()
