#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
"""Drive the pnut-os system UI with keys and save what the screen shows.

    python3 tools/device/uiscreen.py -o shot.png ["keys w w e" ...]

Each argument is an NSH command run first (normally `keys ...`); then the
`screen` command prints the framebuffer as a base64 PBM, which is saved as
a PNG at twice the size.  Needs pyserial, as nsh.py.  Close any serial
monitor first.  Screenshots stay on this computer: they can show
neighbours' network names.
"""
import argparse
import base64
import os
import struct
import sys
import time
import zlib

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nsh import PROMPT, read_until  # noqa: E402


def run(ser, cmd, timeout=10.0):
    ser.write(cmd.encode() + b"\n")
    out, ok = read_until(ser, PROMPT, timeout)
    if not ok:
        sys.exit(f"'{cmd}': no prompt back")
    return out.decode("utf-8", "replace").replace("\r", "")


def png(pbm, out, scale=2):
    head, _, rest = pbm.partition(b"\n")
    size, _, bits = rest.partition(b"\n")
    w, h = (int(v) for v in size.split())
    row = (w + 7) // 8
    rows = []
    for y in range(h):
        line = bytearray()
        for x in range(w):
            black = bits[y * row + x // 8] & (0x80 >> (x % 8))
            line += bytes([0 if black else 255]) * scale
        rows += [b"\0" + bytes(line)] * scale

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    with open(out, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale,
                                           8, 0, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(b"".join(rows))))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="*")
    ap.add_argument("-o", "--out", default="screen.png")
    ap.add_argument("--port", default=os.environ.get("ESPTOOL_PORT",
                                                     "/dev/ttyACM0"))
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds between the last command and the capture")
    a = ap.parse_args()

    ser = serial.Serial(a.port, 115200, timeout=0.2, write_timeout=3)
    try:
        ser.write(b"\n")
        _, ok = read_until(ser, PROMPT, 5.0)
        if not ok:
            sys.exit("no 'nsh> ' prompt")
        for cmd in a.commands:
            sys.stdout.write(run(ser, cmd).split("\n", 1)[-1].replace(
                "nsh> ", ""))
        time.sleep(a.settle)
        text = run(ser, "screen", 20.0)
    finally:
        ser.close()

    try:
        body = text.split("-----BEGIN SCREEN-----\n", 1)[1]
        body = body.split("-----END SCREEN-----", 1)[0]
    except IndexError:
        sys.exit("no screen in the output:\n" + text)
    png(base64.b64decode("".join(body.split())), a.out)
    print(a.out)


if __name__ == "__main__":
    main()
