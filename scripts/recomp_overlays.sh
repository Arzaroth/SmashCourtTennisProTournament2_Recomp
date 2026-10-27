#!/usr/bin/env bash
# Recompiles the code overlays of .staytrash (0x46EA80) into generated/overlays/<name>/: the ELF's own boot code as
# module "boot", and every image a run with PS2X_SCT2_OVERLAY_DUMP=game/overlays captured, as ovl_<hash>.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp" ] || "$ROOT/scripts/build_tools.sh"
OVERLAYS="${OVERLAYS:-$GAME_DIR/overlays}"
WORK="$ROOT/build/overlays"
rm -rf "$WORK"
mkdir -p "$WORK" "$GENERATED/overlays"

# Boot code: 0x46EA80-0x46F424 of the ELF's .staytrash (file offset 0x10D510); data and IRX images follow it. Its
# functions are called from .text, which the module's own discovery cannot see.
BOOT_ENTRIES='"boot_46EB38@0x0046EB38", "boot_46EBD0@0x0046EBD0", "boot_46EDA0@0x0046EDA0", "boot_46EE50@0x0046EE50",
  "boot_46EE90@0x0046EE90", "boot_46EED8@0x0046EED8", "boot_46F0C8@0x0046F0C8"'

python3 -I -c 'import sys; d = open(sys.argv[1], "rb").read(); sys.stdout.buffer.write(d[0x10D510:0x10D510 + 0x9A4])' \
    "$ELF" > "$WORK/boot.bin"
images=("$WORK/boot.bin")
shopt -s nullglob
images+=("$OVERLAYS"/ovl_*.bin)

modules=()
for image in "${images[@]}"; do
    name=$(basename "$image" .bin)
    python3 -I "$ROOT/scripts/overlay_elf.py" "$image" 0x46EA80 "$WORK/$name.elf"
    mkdir -p "$WORK/$name"
    # Functions reached only through pointers in the overlay's own data (task tables): words pointing into the image
    # at an "addiu $sp, $sp, -N" prologue.
    entries=$(python3 -I -c 'import struct, sys
image = open(sys.argv[1], "rb").read()
load = 0x46EA80
found = set()
for offset in range(0, len(image) - 3, 4):
    target = struct.unpack_from("<I", image, offset)[0]
    if load <= target < load + len(image) - 3 and target % 4 == 0:
        word = struct.unpack_from("<I", image, target - load)[0]
        if word >> 16 == 0x27BD and word & 0x8000:
            found.add(target)
print(", ".join(f"\"ptr_{t:08X}@0x{t:08X}\"" for t in sorted(found)))' "$image")
    if [ "$name" = boot ]; then
        entries="$BOOT_ENTRIES${entries:+, $entries}"
    fi
    python3 -I -c 'import sys; t = open(sys.argv[1]).read()
for key, value in zip(("@INPUT@", "@OUTPUT@", "@NAME@", "@ENTRIES@"), sys.argv[2:]):
    t = t.replace(key, value)
sys.stdout.write(t)' "$ROOT/recomp/overlay.toml.in" "$WORK/$name.elf" "$WORK/$name/" "$name" "$entries" > "$WORK/$name.toml"
    "$TOOLS_BUILD/ps2xRecomp/ps2_recomp" "$WORK/$name.toml" > "$WORK/$name.log" 2>&1 \
        || { echo "ps2_recomp failed on $name, see $WORK/$name.log"; exit 1; }
    mkdir -p "$GENERATED/overlays/$name"
    rsync -rc --delete "$WORK/$name/" "$GENERATED/overlays/$name/"
    modules+=("$name")
    echo "$name: $(find "$WORK/$name" -name '*.cpp' | wc -l) sources, $(grep -c '\[error\]' "$WORK/$name.log") decode errors"
done

for dir in "$GENERATED"/overlays/*/; do
    name=$(basename "$dir")
    [[ " ${modules[*]} " == *" $name "* ]] || rm -rf "$dir"
done
