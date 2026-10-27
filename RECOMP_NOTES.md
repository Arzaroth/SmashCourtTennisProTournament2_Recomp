# Smash Court Tennis Pro Tournament 2 recompilation notes

## The disc

The ISO 9660 file system holds only `SYSTEM.CNF`, `SLUS_209.33` and `IOPRP300.IMG` (1.6 MB); the image is 4.4 GB.
Everything else, IRX modules included (the ELF names no `cdrom0:` IRX path, only `IOPRP300.IMG`), sits in raw
sectors the game reads by LSN. `scripts/extract_iso.sh` links the image as `game/disc.iso` and
`runtime/sct2_overrides.cpp` sets it as the runtime's CD image, which serves any read no file entry covers.

## Mapping the ELF

`SLUS_209.33` is stripped, entry `0x100000`, two loadable segments: `.text` at `0x100000` (0x10C510 bytes) and
`.staytrash` at `0x46EA80` (0x393B7 bytes, writable and executable, holds code). `recomp/sct2.toml` is
`ps2_analyzer` output with the paths made relative to the repository root:

- 1,889 functions extracted; 158 SCE SDK functions bound to runtime handlers (`[general].stubs`,
  `name@0xADDRESS`), 289 more recognised as library code without a runtime handler.
- Two jump tables detected (`0x207790`, `0x2079C0`), no patches.
- `ps2_recomp` emits 1,951 sources. It reports unresolved `JR`/`JALR` targets in 158 places and promotes fallback
  entries for them. Four functions get tens of thousands of entries, a sign the analyzer merged unrelated code into
  them: `0x1A7E34` (48,868), `0x483C94` (32,003), `0x46F470` (20,943), `0x1D790C` (17,552). These are the first
  candidates for a Ghidra map and hint lists, as in the Soulcalibur III port.

## VU1 microcode

`ps2_vu_aot` finds 5 microcode fragments (2,076 instruction pairs, one also used as VU0 microcode) in the ELF and
compiles them into 13 files. Microcode loaded from the disc at run time runs in the VU1 interpreter until it is found
and compiled.

## Status

Not booted yet.
