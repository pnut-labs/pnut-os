#!/bin/sh
############################################################################
# pnut-os/tools/stage-usr.sh
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# pnut-os's /usr (RFC 0010): the firmware's read-only files, a ROMFS image
# the system program mounts at start.  So far share/sounds, the built-in
# sounds (RFC 0032), synthesized by gen-sounds for a target whose
# description has a speaker; share/ alone for the others.
#
#   tools/stage-usr.sh <root> <out> <target> <python>
#
# root is pnut-os; out the build's directory for it: out/tree is the
# staged tree, out/usr_romfs.c its image as a C array, which the system
# program compiles.  The C file is replaced only when the image changed,
# so that an unchanged /usr does not build the system program again.

set -e

root=$1
out=$2
target=$3
python=$4

if [ $# -ne 4 ]; then
  echo "usage: $0 <root> <out> <target> <python>" >&2
  exit 2
fi

board="$root/configs/$target/board.toml"
tree="$out/tree"

rm -rf "$tree"
mkdir -p "$tree/share"

if [ -f "$board" ] && grep -q '^kind = "speaker"' "$board"; then
  "$python" "$root/tools/gen-sounds" "$tree/share/sounds"
fi

genromfs -f "$out/usr.img" -d "$tree" -V "pnut-usr"

"$python" - "$out/usr.img" "$out/usr_romfs.c.new" <<'EOF'
import sys

# Padded to whole sectors of 64 bytes, as the system program registers it

data = open(sys.argv[1], "rb").read()
data += bytes(-len(data) % 64)
rows = []
for i in range(0, len(data), 12):
    rows.append("  " + ", ".join("0x%02x" % b for b in data[i:i + 12]) +
                ",")

with open(sys.argv[2], "w") as f:
    f.write("/* pnut-os's /usr, a ROMFS image: made by tools/stage-usr.sh, "
            "not to edit */\n\n")
    f.write("#include <nuttx/compiler.h>\n\n")
    f.write("const unsigned char aligned_data(4) g_pnut_usr_romfs[] =\n{\n")
    f.write("\n".join(rows) + "\n};\n\n")
    f.write("const unsigned int g_pnut_usr_romfs_len = %d;\n" % len(data))
EOF

if cmp -s "$out/usr_romfs.c.new" "$out/usr_romfs.c"; then
  rm -f "$out/usr_romfs.c.new"
else
  mv "$out/usr_romfs.c.new" "$out/usr_romfs.c"
fi
