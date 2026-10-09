#!/usr/bin/env python3
############################################################################
# pnut-os/tools/sim-test.py
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

"""Run test programs inside NuttX's simulator, through its console.

    tools/sim-test.py <nuttx-binary> <program>...

Each program runs from NSH in turn; it passes when its output says
"[  PASSED  ]" and nothing says "[  FAILED  ]" (cmocka's summary lines).
The simulator is powered off at the end.  The exit status is 0 when every
program passed.
"""

import os
import select
import subprocess
import sys
import time

PROMPT = b"nsh> "
BOOT_TIMEOUT = 30
TEST_TIMEOUT = 120


def read_until(proc, marker, timeout):
    """Read the console until marker appears; return what was read."""
    out = b""
    end = time.monotonic() + timeout
    fd = proc.stdout.fileno()
    while marker not in out:
        left = end - time.monotonic()
        if left <= 0:
            raise TimeoutError(out)
        ready, _, _ = select.select([fd], [], [], left)
        if not ready:
            continue
        chunk = os.read(fd, 4096)
        if not chunk:
            raise EOFError(out)
        out += chunk
    return out


def send(proc, text):
    """Type a line on the console; False if the simulator has gone."""
    try:
        proc.stdin.write(text.encode() + b"\n")
        proc.stdin.flush()
        return True
    except OSError:
        return False


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)

    binary, programs = sys.argv[1], sys.argv[2:]
    try:
        proc = subprocess.Popen([binary], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
    except OSError as e:
        print(f"cannot start {binary}: {e.strerror}")
        return 1

    failed = []
    healthy = True

    try:
        read_until(proc, PROMPT, BOOT_TIMEOUT)
    except (TimeoutError, EOFError) as e:
        print(e.args[0].decode(errors="replace"))
        print("the simulator did not reach NSH")
        failed.append("boot")
        healthy = False

    for program in programs if healthy else []:
        if not send(proc, program):
            failed.append(program)
            print(f"{program}: the simulator has gone")
            healthy = False
            break

        try:
            out = read_until(proc, PROMPT, TEST_TIMEOUT)
        except (TimeoutError, EOFError) as e:
            print(e.args[0].decode(errors="replace"))
            if isinstance(e, TimeoutError):
                print(f"{program}: hung, no prompt back")
            else:
                proc.wait()
                print(f"{program}: crashed, no prompt back"
                      f" (the simulator exited with {proc.returncode})")
            failed.append(program)
            healthy = False
            break

        text = out.decode(errors="replace")
        print(text.replace("\r", ""), end="")
        if "[  PASSED  ]" not in text or "[  FAILED  ]" in text:
            failed.append(program)

    # A hung program holds the console, so only a healthy simulator is
    # powered off; any other is killed.

    if healthy and send(proc, "poweroff"):
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass

    if proc.poll() is None:
        proc.kill()
        proc.wait()

    print()
    if failed:
        print("failed: " + " ".join(failed))
        return 1

    print(f"passed: {len(programs)} programs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
