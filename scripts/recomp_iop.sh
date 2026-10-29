#!/usr/bin/env bash
# usage: recomp_iop.sh <dump-dir>
# Recompiles the IOP modules a run dumped with PS2X_IOP_DUMP_MODULES=<dump-dir> into generated/iop/.
source "$(dirname "$0")/common.sh"
dump=${1:?usage: recomp_iop.sh <dump-dir>}
staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT
rm -rf "$GENERATED/iop"
mkdir -p "$GENERATED/iop"
# The game loads every module from one EE buffer, so they all share its name: each takes its dump's file name.
for json in "$dump"/*.json; do
  stem=$(basename "$json" .json)
  name=$(echo "$stem" | tr -c 'A-Za-z0-9_\n' '_')
  python3 -I -c 'import json, sys
info = json.load(open(sys.argv[1]))
info["name"] = sys.argv[2]
json.dump(info, open(sys.argv[3], "w"))' "$json" "$name" "$staging/$name.json"
  cp "${json%.json}.bin" "$staging/$name.bin"
  python3 -I "$ROOT/PS2Recomp/tools/iop_recomp.py" "$staging/$name.json" --out "$GENERATED/iop" | grep -E "left to the interpreter"
done
echo "Recompiled $(find "$GENERATED/iop" -name '*_iop.cpp' | wc -l) IOP module(s) into $GENERATED/iop"
