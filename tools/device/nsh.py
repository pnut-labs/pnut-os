#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
"""Run NuttShell commands on the T-Deck Max over its USB serial console.

    python3 tools/device/nsh.py "uname -a" "free" "ls /dev"
    python3 tools/device/nsh.py --boot-wait 5   # just show what it prints

Each command is sent, then the output is read until the next "nsh> "
prompt.  The exit status is 1 when the board cannot be reached, or a
command's prompt does not come back within --timeout; a command that
fails on the device does not change it.

Needs pyserial: `apt install python3-serial` on Debian or Ubuntu, or
`pip install pyserial` in a virtual environment; ESP-IDF's Python has it.
"""
import argparse
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    serial = None

PROMPT = b"nsh> "
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
NEEDS = ("needs pyserial: apt install python3-serial (Debian, Ubuntu), "
         "or pip install pyserial in a virtual environment")
GONE = "the port went away (board reset? cable pulled?)"
PORT_HELP = "serial port (default: $ESPTOOL_PORT, else /dev/ttyACM0)"
PORT_WAIT_HELP = ("seconds to wait for the port to (re)appear "
                  "(default: %(default)s)")


class PortGone(Exception):
    """The port failed while reading; args[0] is what was read before."""


def read_until(ser, marker, timeout):
    """Read until the output (ANSI escapes removed) ends with `marker`.

    Returns the output and whether the marker came; raises PortGone, with
    the output read so far, when the port fails."""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        try:
            chunk = ser.read(4096)
        except (serial.SerialException, OSError) as e:
            raise PortGone(ANSI.sub(b"", buf)) from e
        if chunk:
            buf += chunk
            if marker and ANSI.sub(b"", buf).endswith(marker):
                return ANSI.sub(b"", buf), True
    return ANSI.sub(b"", buf), False


def send_line(ser, line, pace=0):
    """Type `line` and Enter; with `pace`, that many bytes at a time, 50 ms
    apart.  Exits when the board does not take it."""
    data = line.encode() + b"\n"
    step = pace if pace > 0 else len(data)
    try:
        for i in range(0, len(data), step):
            ser.write(data[i:i + step])
            if pace > 0:
                ser.flush()
                time.sleep(0.05)
    except serial.SerialTimeoutException:
        sys.exit("board is not accepting input (hung? reset it)")
    except (serial.SerialException, OSError):
        sys.exit(GONE)


def open_port(port, baud, wait):
    """Open `port`, waiting up to `wait` seconds for it to appear and
    open.  Exits when it does not."""
    if serial is None:
        sys.exit(NEEDS)
    end = time.time() + wait
    while True:
        why = f"{port} did not appear"
        if os.path.exists(port):
            try:
                return serial.Serial(port, baud, timeout=0.2,
                                     write_timeout=3)
            except (serial.SerialException, OSError) as e:
                why = str(e)
        if time.time() > end:
            sys.exit(why)
        time.sleep(0.2)


def show(text):
    """Print `text`, ending it with a newline."""
    if text:
        sys.stdout.write(text if text.endswith("\n") else text + "\n")


def tidy(out):
    """Output as text, without carriage returns and the closing prompt."""
    text = out.decode("utf-8", "replace").replace("\r", "")
    if text.endswith("nsh> "):
        text = text[: -len("nsh> ")]
    return text


def body(cmd, out):
    """A command's output without its echoed line and the next prompt."""
    lines = tidy(out).split("\n")
    if lines and cmd in lines[0]:
        lines = lines[1:]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands", nargs="*", help="NSH commands to run in order")
    ap.add_argument("--port", help=PORT_HELP,
                    default=os.environ.get("ESPTOOL_PORT", "/dev/ttyACM0"))
    ap.add_argument("--baud", type=int, default=115200,
                    help="baud rate (default: %(default)s)")
    ap.add_argument("--boot-wait", type=float, default=2.0,
                    help="seconds to capture output before the first command "
                         "(default: %(default)s)")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="seconds to wait for each command to finish "
                         "(default: %(default)s)")
    ap.add_argument("--port-wait", type=float, default=15.0,
                    help=PORT_WAIT_HELP)
    ap.add_argument("--pace", type=int, default=0,
                    help="type long lines N bytes at a time, 50 ms apart "
                         "(the console drops characters of a fast long line)")
    args = ap.parse_args()

    ser = open_port(args.port, args.baud, args.port_wait)
    cmd = None
    try:
        boot, _ = read_until(ser, None, args.boot_wait)
        show(boot.decode("utf-8", "replace"))

        send_line(ser, "")
        out, ok = read_until(ser, PROMPT, 5.0)
        show(tidy(out).strip("\n"))
        if not ok:
            sys.exit("no 'nsh> ' prompt (is the board running NuttX?)")

        rc = 0
        for cmd in args.commands:
            print(f"nsh> {cmd}")
            send_line(ser, cmd, args.pace)
            out, ok = read_until(ser, PROMPT, args.timeout)
            show(body(cmd, out))
            if not ok:
                print(f"[timed out after {args.timeout}s]")
                rc = 1
        sys.exit(rc)
    except PortGone as e:
        out = e.args[0]
        show(tidy(out) if cmd is None else body(cmd, out))
        sys.exit(GONE)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
