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
- `.text` mixes code and data, and without symbols a function runs to the next known start. The analyzer also
  takes data words that decode as `JAL` for calls, so it found 21 "functions" that start in data (most of
  `0x1D4800`-`0x20C500`, plus `0x483804` and `0x4A2C0C` in `.staytrash`), and four real functions swallowed the data
  after them: `0x1A7D30`, `0x205DD8`, `0x206D70` and `0x46F0C8`, up to 13 MB of generated C++ each and 27,692
  undecodable words in total. The 21 are in `[general].skip`; `recomp/function_bounds.csv` (`ghidra_output`) carries
  every function with those four ends cut after their last `jr $ra`. A map turns the recompiler's own discovery
  off, so the map must list every function: `scripts/function_bounds.py` writes it from a run with
  `ghidra_output = ""` (`--end 0xSTART=0xEND` for the cuts). Result: 1,939 sources, 0 decode errors.
- A headless Ghidra run (stock `MIPS:LE:32`, no Emotion Engine extension) is not usable as the map: it does not
  decode R5900 instructions, cuts functions after 4 or 8 bytes (`_InitSys` at `0x19F860` became 4 bytes, the main
  thread died on its first call) and labels the 273 KB of data at `0x1C3410` as one function.

## VU1 microcode

`ps2_vu_aot` finds 5 microcode fragments (2,076 instruction pairs, one also used as VU0 microcode) in the ELF and
compiles them into 13 files. Microcode loaded from the disc at run time runs in the VU1 interpreter until it is found
and compiled.

## Boot

- `.staytrash` (`0x46EA80`) holds the IOP side of the boot: the code at `0x46F0C8` reboots the IOP and loads 12 IRX
  modules embedded in the ELF from `0x4A7EC0` on through `sceSifLoadModuleBuffer`. Before that it calls
  `sceSifSearchModuleByName("cdvd_ee_driver")` to stop an old copy. The runtime has no handler for it and no loadfile
  RPC server, so libkernel's `_lf_bind` retried sid `0x80000006` for ever; the override answers
  `KE_UNKNOWN_MODULE` (-202), which is what a freshly rebooted IOP says.
- The IOP's first raw read works (`lsn=0x213932`, 6 sectors, from `game/disc.iso`), the game sets PMODE and the
  display buffers and draws through PATH2/PATH3.

- crt0 calls `SetupThread` with the main stack at `0x1FF8000`-`0x2000000`, where the runtime keeps its interrupt and
  callback stacks by default: the main thread's saved `$ra` was overwritten and it returned to 0 after five frames.
  The override moves the runtime's reserved range to kernel RAM (`configureReservedMemory(0x80000, 0x100000,
  0x1FF8000)`), as in the Fighting for One Piece and Soulcalibur III ports.
- The main loop (`0x100270`) then runs at 60 Hz: the VSync callback `0x178D98` (registered with
  `sceGsSyncVCallback`) wakes the main thread, which sleeps in `0x178EC8` every frame.

## Code overlays in .staytrash

`.staytrash` is boot code the game overwrites with code loaded from disc: during a frame, 36 KB at
`0x46EA80`-`0x4776D8` differ from the ELF. The per-frame task list (`0x176760`, tasks of 0x80 bytes with the
function at `+0` and the next task at `+8`) calls `0x46EA80`, so the runtime keeps running the recompiled boot
routine (IOP reboot, fileio reset) instead of the overlay. The overlays have to be captured and recompiled as
overlay modules (`ps2_overlay_modules.h`).

## Status

Boots, loads its IOP modules and runs the main loop at 60 Hz, but nothing is presented yet (`gs frames=0`): the
overlay code at `0x46EA80` is the next step.
