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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nsh import (GONE, PORT_HELP, PORT_WAIT_HELP, PROMPT,  # noqa: E402
                 PortGone, body, open_port, read_until, send_line, show)


def run(ser, cmd, timeout=10.0):
    send_line(ser, cmd)
    try:
        out, ok = read_until(ser, PROMPT, timeout)
    except PortGone as e:
        if cmd != "screen":
            show(body(cmd, e.args[0]))
        sys.exit(GONE)
    if not ok:
        sys.exit(f"'{cmd}': no prompt back")
    return out.decode("utf-8", "replace").replace("\r", "")


def png(pbm, out, scale=2):
    """Write the P4 PBM `pbm` to `out` as a PNG, `scale` times its size;
    ValueError when it is not a whole P4 picture."""
    head, _, rest = pbm.partition(b"\n")
    size, _, bits = rest.partition(b"\n")
    if head != b"P4":
        raise ValueError("not a P4 picture")
    try:
        w, h = (int(v) for v in size.split())
    except ValueError:
        raise ValueError(f"no size in {size[:20]!r}") from None
    row = (w + 7) // 8
    if w <= 0 or h <= 0 or len(bits) != row * h:
        raise ValueError(f"{len(bits)} of {row * h} bytes")
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
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands", nargs="*",
                    help="NSH commands run before the capture, normally "
                         "`keys ...`")
    ap.add_argument("-o", "--out", default="screen.png",
                    help="PNG to write (default: %(default)s)")
    ap.add_argument("--port", help=PORT_HELP,
                    default=os.environ.get("ESPTOOL_PORT", "/dev/ttyACM0"))
    ap.add_argument("--port-wait", type=float, default=15.0,
                    help=PORT_WAIT_HELP)
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds between the last command and the capture "
                         "(default: %(default)s)")
    a = ap.parse_args()

    ser = open_port(a.port, 115200, a.port_wait)
    try:
        send_line(ser, "")
        try:
            _, ok = read_until(ser, PROMPT, 5.0)
        except PortGone:
            sys.exit(GONE)
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
        dump = text.split("-----BEGIN SCREEN-----\n", 1)[1]
        dump = dump.split("-----END SCREEN-----", 1)[0]
    except IndexError:
        sys.exit("no screen in the output:\n" + text)
    try:
        pbm = base64.b64decode("".join(dump.split()), validate=True)
        png(pbm, a.out)
    except ValueError as e:  # binascii.Error is one
        sys.exit(f"the screen arrived damaged ({e}); run it again")
    print(a.out)


if __name__ == "__main__":
    main()
