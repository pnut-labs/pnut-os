#!/bin/sh
############################################################################
# pnut-os/tools/generate.sh
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# Generate the code of every interface (RFC 0023): nanopb's message
# structs, in C style (pnut_test_say_request_t, not pnut_test_SayRequest,
# as NuttX's style wants), and tools/protoc-gen-pnut's clients, servers and
# references.
#
#   tools/generate.sh <root> <out> <python> <nanopb>
#
# root is pnut-os; out is where the code goes, made afresh; python has
# Python's protobuf module; nanopb is nanopb's source release, unpacked.

set -e

root=$1
out=$2
python=$3
nanopb=$4

if [ $# -ne 4 ]; then
  echo "usage: $0 <root> <out> <python> <nanopb>" >&2
  exit 2
fi

rm -rf "$out"
mkdir -p "$out/py" "$out/bin"

# The plugins run under the Python named, not whichever comes first on the
# PATH, which in CI is esptool's and has no protobuf

cat > "$out/bin/protoc-gen-nanopb" <<END
#!/bin/sh
exec "$python" "$nanopb/generator/protoc-gen-nanopb" "\$@"
END

cat > "$out/bin/protoc-gen-pnut" <<END
#!/bin/sh
PYTHONPATH="$out/py\${PYTHONPATH:+:\$PYTHONPATH}" \\
  exec "$python" "$root/tools/protoc-gen-pnut" "\$@"
END

chmod +x "$out/bin/protoc-gen-nanopb" "$out/bin/protoc-gen-pnut"

# protoc-gen-pnut reads its options through a module made from them

protoc -I "$root/proto" --python_out="$out/py" "$root/proto/pnut/options.proto"
touch "$out/py/pnut/__init__.py"

# Every interface in one run, so that protoc-gen-pnut sees them all and
# refuses an interface number used twice.  pnut's options are read by the
# generator, not compiled: nanopb leaves their import out (-x).

cd "$root"
protos=$(find proto tests/proto -name '*.proto' \
         ! -path proto/pnut/options.proto | sort)

protoc -I proto -I tests/proto -I "$nanopb/generator/proto" \
  --plugin=protoc-gen-nanopb="$out/bin/protoc-gen-nanopb" \
  --nanopb_opt=--c-style --nanopb_opt=-x --nanopb_opt=pnut/options.proto \
  --nanopb_out="$out" \
  --plugin=protoc-gen-pnut="$out/bin/protoc-gen-pnut" \
  --pnut_out="$out" \
  $protos
