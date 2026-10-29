#!/bin/sh
############################################################################
# pnut-os/tools/stage-etc.sh
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# pnut-os's files for the image's /etc (RFC 0010), staged for NuttX's
# ETC_OVERLAY: etc/; the target's description, compiled from
# configs/<target>/board.toml into pnut/board.pb (RFC 0024), which every
# target has; and its settings' defaults, compiled from
# configs/<target>/defaults.toml into pnut/defaults.pb (RFC 0025).
#
#   tools/stage-etc.sh <root> <out> <target> <python> <gen>
#
# root is pnut-os; out the staged directory; gen the interfaces' code,
# with its descriptor set and Python modules (tools/generate.sh).  out is
# replaced only when something in it changed, so that an unchanged /etc
# does not make NuttX build its ROMFS again.

set -e

root=$1
out=$2
target=$3
python=$4
gen=$5

if [ $# -ne 5 ]; then
  echo "usage: $0 <root> <out> <target> <python> <gen>" >&2
  exit 2
fi

new="$out.new"
rm -rf "$new"
mkdir -p "$new"
cp -R "$root/etc/." "$new/"

board="$root/configs/$target/board.toml"
if [ ! -f "$board" ]; then
  echo "$0: $board: every target has a description (RFC 0024)" >&2
  rm -rf "$new"
  exit 1
fi

mkdir -p "$new/pnut"
PYTHONPATH="$gen/py${PYTHONPATH:+:$PYTHONPATH}" \
  "$python" "$root/tools/board-description" "$gen/descriptors.pb" \
  "$board" "$target" "$new/pnut/board.pb"

defaults="$root/configs/$target/defaults.toml"
if [ -f "$defaults" ]; then
  PYTHONPATH="$gen/py${PYTHONPATH:+:$PYTHONPATH}" \
    "$python" "$root/tools/settings-defaults" "$gen/descriptors.pb" \
    "$defaults" "$new/pnut/defaults.pb"
fi

if [ -d "$out" ] && diff -r -q "$new" "$out" > /dev/null 2>&1; then
  rm -rf "$new"
else
  rm -rf "$out"
  mv "$new" "$out"
fi
