#!/usr/bin/env bash
# Extracts the files of your own Smash Court Tennis Pro Tournament 2 (NTSC-U, SLUS-20933) disc image into game/
# and links the image itself as game/disc.iso: the game reads its data from sectors outside the file system.
# Usage: scripts/extract_iso.sh <image.iso>
source "$(dirname "$0")/common.sh"
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 <image.iso>"; exit 1; }
command -v bsdtar >/dev/null || { echo "bsdtar not found (libarchive)"; exit 1; }
mkdir -p "$GAME_DIR"
bsdtar -xf "$1" -C "$GAME_DIR"
chmod -R u+w "$GAME_DIR"
[ -f "$ELF" ] || { echo "no SLUS_209.33 in $1: not the NTSC-U disc?"; exit 1; }
expected=34da8370f6aca2c4c51e249cc97162aa03999fe5
actual=$(sha1sum "$ELF" | cut -d' ' -f1)
[ "$actual" = "$expected" ] || echo "warning: SLUS_209.33 SHA-1 is $actual, the port is built against $expected"
ln -sfn "$(realpath "$1")" "$GAME_DIR/disc.iso"
echo "Disc files and disc.iso in $GAME_DIR"
