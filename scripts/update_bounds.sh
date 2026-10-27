#!/usr/bin/env bash
# Rewrites recomp/function_bounds.csv: ps2_recomp's own function discovery (a run without the map), the four ends
# cut before data, .staytrash left to the overlay modules, and the .text functions only the captured overlays call.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp" ] || "$ROOT/scripts/build_tools.sh"
OVERLAYS="${OVERLAYS:-$GAME_DIR/overlays}"
cd "$ROOT"

rm -rf build/nomap
mkdir -p build/nomap
sed -e 's|^output = .*|output = "build/nomap/"|' -e 's|^ghidra_output = .*|ghidra_output = ""|' "$CONFIG" > build/nomap.toml
"$TOOLS_BUILD/ps2xRecomp/ps2_recomp" build/nomap.toml > build/nomap.log 2>&1 || { echo "see build/nomap.log"; exit 1; }

callers=()
shopt -s nullglob
for image in "$OVERLAYS"/ovl_*.bin; do
    callers+=(--callers "$image@0x46EA80")
done
python3 -I scripts/function_bounds.py "$CONFIG" build/nomap \
    --end 0x1A7D30=0x1A7D74 --end 0x205DD8=0x206C88 --end 0x206D70=0x207048 \
    --exclude 0x46EA80-0x4A7E38 "${callers[@]}" > recomp/function_bounds.csv
echo "$(($(wc -l < recomp/function_bounds.csv) - 1)) functions in recomp/function_bounds.csv (${#callers[@]} overlay args)"
