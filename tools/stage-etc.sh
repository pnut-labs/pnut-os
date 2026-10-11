#!/bin/sh
############################################################################
# pnut-os/tools/stage-etc.sh
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# pnut-os's files for the image's /etc (RFC 0010), staged for NuttX's
# ETC_OVERLAY: etc/, and the target's settings' defaults, compiled from
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

defaults="$root/configs/$target/defaults.toml"
if [ -f "$defaults" ]; then
  mkdir -p "$new/pnut"
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
