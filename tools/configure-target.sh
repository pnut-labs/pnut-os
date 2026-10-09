#!/usr/bin/env sh
############################################################################
# pnut-os/tools/configure-target.sh
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# Configures NuttX for a pnut-os target: NuttX's configure.sh with the
# board's configuration, then the target's fragment merged over it with
# NuttX's tools/merge_config.py.  Run by the top-level Makefile:
#
#   tools/configure-target.sh <nuttx-dir> <board>:<config> <fragment>
#
# PYTHON names a Python with kconfiglib (python3 by default).

set -e

# NuttX's scripts run make themselves; they do not share a parent make's
# jobs

unset MAKEFLAGS

if [ $# -ne 3 ]; then
  echo "usage: $0 <nuttx-dir> <board>:<config> <fragment>" >&2
  exit 2
fi

nuttx=$1
board=$2
fragment=$(cd "$(dirname "$3")" && pwd)
fragment=$fragment/$(basename "$3")
python=${PYTHON:-python3}

if ! command -v kconfig-tweak > /dev/null; then
  echo "error: kconfig-tweak not found; NuttX's configure.sh needs" \
       "kconfig-frontends" >&2
  exit 1
fi

# merge_config.py exits with success when kconfiglib is missing, so this
# check is what stops the build.  ESP-IDF's Python environment carries a
# kconfiglib of its own (it has no VERSION), which cannot read NuttX's
# Kconfig.

if ! "$python" -c 'import kconfiglib; kconfiglib.VERSION' 2> /dev/null; then
  echo "error: $python has no kconfiglib, or has ESP-IDF's, which cannot" \
       "read NuttX's Kconfig; name another Python with PYTHON=" >&2
  exit 1
fi

cd "$nuttx"

trap 'rm -f .config.board .config.merged .config.arch' EXIT

./tools/configure.sh -E -a ../apps "$board"

# NuttX's Makefile sets EXTERNALDIR for its Kconfig; merge_config.py does
# not, so set it the same way

if [ -r external/Kconfig ]; then
  EXTERNALDIR=external
else
  EXTERNALDIR=dummy
fi

export EXTERNALDIR

# Merge into a file of its own, so that .config stays one NuttX can read
# (and distclean) if a step fails

cp .config .config.board
"$python" tools/merge_config.py -o .config.merged .config.board "$fragment"

# merge_config.py writes a minimal config, which leaves out the symbols
# NuttX's Makefile reads before olddefconfig: the architecture's, the
# chip's and the board's names and directories.  Add back those the
# merged config does not name, from the board's.

awk '
  FILENAME == ARGV[1] {
    if (match($0, /^CONFIG_[A-Za-z0-9_]+=/))
      named[substr($0, 1, RLENGTH - 1)] = 1
    else if ($0 ~ /^# CONFIG_[A-Za-z0-9_]+ is not set$/)
      named[$2] = 1
    next
  }
  /^CONFIG_ARCH(_CHIP|_BOARD|_BOARD_COMMON)?=/ ||
  /^CONFIG_(ARCH|ARCH_CHIP|ARCH_BOARD)_CUSTOM/ {
    name = $0
    sub(/=.*/, "", name)
    if (!(name in named))
      print
  }
' .config.merged .config.board > .config.arch

cat .config.arch >> .config.merged
mv .config.merged .config

make olddefconfig

# Every line of the fragment must be in the final configuration as
# written, CONFIG_X=n as "# CONFIG_X is not set": an option that is
# misspelt, or whose dependencies are not met, is dropped by Kconfig
# without an error

awk '
  FILENAME == ARGV[1] {
    final[$0] = 1
    next
  }
  /^CONFIG_/ || /^# CONFIG_[A-Za-z0-9_]+ is not set/ {
    line = $0
    if (line ~ /^CONFIG_[A-Za-z0-9_]+=n$/) {
      sub(/=n$/, "", line)
      line = "# " line " is not set"
    }
    if (!(line in final))
      missing = missing "  " $0 "\n"
  }
  END {
    if (missing != "") {
      printf "error: not in the final configuration as written (misspelt," \
             " a dependency not met, or a value Kconfig writes" \
             " differently):\n%s", missing > "/dev/stderr"
      exit 1
    }
  }
' .config "$fragment"
