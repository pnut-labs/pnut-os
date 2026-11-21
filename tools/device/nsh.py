#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
"""Run NuttShell commands on the T-Deck Max over its USB serial console.

    python3 tools/device/nsh.py "uname -a" "free" "ls /dev"
    python3 tools/device/nsh.py --boot-wait 5   # just show what it prints

Needs pyserial (`pip install pyserial`; ESP-IDF's Python has it).  Each
command is sent, then the output is read until the next "nsh> " prompt.
"""
import argparse
import os
import re
import sys
import time

import serial

PROMPT = b"nsh> "
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


def read_until(ser, marker, timeout):
    """Read until the output (ANSI escapes removed) ends with `marker`."""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if marker and ANSI.sub(b"", buf).endswith(marker):
                return ANSI.sub(b"", buf), True
    return ANSI.sub(b"", buf), False


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands", nargs="*", help="NSH commands to run in order")
    ap.add_argument("--port",
                    default=os.environ.get("ESPTOOL_PORT", "/dev/ttyACM0"))
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--boot-wait", type=float, default=2.0,
                    help="seconds to capture output before the first command")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="seconds to wait for each command to finish")
    ap.add_argument("--port-wait", type=float, default=15.0,
                    help="seconds to wait for the port to (re)appear")
    ap.add_argument("--pace", type=int, default=0,
                    help="type long lines N bytes at a time, 50 ms apart "
                         "(the console drops characters of a fast long line)")
    args = ap.parse_args()

    end = time.time() + args.port_wait
    while not os.path.exists(args.port):
        if time.time() > end:
            sys.exit(f"{args.port} did not appear")
        time.sleep(0.2)

    ser = serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=3)
    try:
        boot, _ = read_until(ser, None, args.boot_wait)
        if boot:
            sys.stdout.write(boot.decode("utf-8", "replace"))
            if not boot.endswith(b"\n"):
                print()

        try:
            ser.write(b"\n")
        except serial.SerialTimeoutException:
            sys.exit("board is not accepting input (hung? reset it)")
        _, ok = read_until(ser, PROMPT, 5.0)
        if not ok:
            sys.exit("no 'nsh> ' prompt (is the board running NuttX?)")

        rc = 0
        for cmd in args.commands:
            print(f"nsh> {cmd}")
            try:
                line = cmd.encode() + b"\n"
                step = args.pace if args.pace > 0 else len(line)
                for i in range(0, len(line), step):
                    ser.write(line[i:i + step])
                    if args.pace > 0:
                        ser.flush()
                        time.sleep(0.05)
            except serial.SerialTimeoutException:
                sys.exit("board stopped accepting input (hung? reset it)")
            out, ok = read_until(ser, PROMPT, args.timeout)
            text = out.decode("utf-8", "replace").replace("\r", "")
            # Drop the echoed command line and the trailing prompt.
            lines = text.split("\n")
            if lines and cmd in lines[0]:
                lines = lines[1:]
            body = "\n".join(lines)
            if body.endswith("nsh> "):
                body = body[: -len("nsh> ")]
            if body and not body.endswith("\n"):
                body += "\n"
            sys.stdout.write(body)
            if not ok:
                print(f"[timed out after {args.timeout}s]")
                rc = 1
        sys.exit(rc)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
