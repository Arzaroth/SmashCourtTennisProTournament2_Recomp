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

`.staytrash` is boot code the game overwrites with code it decompresses from disc (thread 2, entry `0x17F9B8`, is
an LZ decoder fed by the request block at `0x464640`; the images are not stored raw on disc). The per-frame task
list (`0x176760`, tasks of 0x80 bytes with the function at `+0` and the next task at `+8`) calls into them.

- `.staytrash` is left out of the main function table (`scripts/function_bounds.py --exclude`): the main table wins
  over overlay modules, so the boot routine kept running where the overlay was.
- `scripts/recomp_overlays.sh` recompiles `0x46EA80`-`0x46F424` of the ELF (the boot code; IRX images and data
  follow it) as module `boot`, with the 7 functions `.text` calls as entry points, and every captured image in
  `game/overlays/` as module `ovl_<hash>`. Functions an overlay reaches only through its own data (task tables) are
  added as entry points: words pointing at an `addiu $sp, $sp, -N` inside the image.
- Capture: `PS2X_SCT2_OVERLAY_DUMP=game/overlays` writes the bytes of `.staytrash` that differ from the ELF before
  each run of the task list, named by the hash of the first 64 bytes, the guard the runtime tells modules apart by
  (the overlay's own data changes from frame to frame).
- `scripts/update_bounds.sh` adds the `.text` functions only the captured overlays call (`0x1332F0` and 16 more
  for the first overlay); rerun it, `scripts/recomp.sh` and `scripts/recomp_overlays.sh` after each new capture.

First overlay: `0x46EA80`-`0x4776D8`, 82 sources (68 decode errors, all in data at its end).

## The black screen: StartThread's return value

Frames were presented but black: the scene task (`0x126A08`, state byte at task `+0x14`, jump table `0x207C70`)
sat in state 2 waiting for the stream library's status byte (`0x463C00`) to reach 2. That library's worker thread
(entry `0x17CA90`) had run 16 cycles in total. Its init (`0x17CC78`) does `tid = CreateThread(...)`, then
`StartThread(tid, 0)` and stores StartThread's result over the id (`bltz v0; sw v0, 0x514(s1)` at `0x17CD4C`). The
EE kernel returns the thread id there; the runtime returned `KE_OK` (0), so every request woke thread 0 instead of
the worker. Fixed in the runtime on boubou666/PS2Recomp branch `sctpt2/startthread-tid` (with a test), which the
submodule tracks until it is merged. Found by classifying the 8 writers of the overlay task's wait flag (`+0x12`)
and walking back from the ones that never ran, as the room's coordinator suggested.

## Title screen and functions reached through pointers

Past the memory card prompt the title screen called `.text` functions nothing else calls: `0x138FF8` through a
pointer in data, `0x139528` through a table of leaf functions copied to the stack. `scripts/function_bounds.py
--pointers` adds every function a data word, or a lui/addiu pair in code, points at when the target is an
`addiu $sp, $sp, -N` prologue or follows a `jr $ra` and its delay slot (2,212 functions), with the data blocks of
`.text` left out of the map (`--exclude`), since byte patterns there look like pointers.

## The menu movie: DMA channel 4

The main menu plays a movie behind its panels. After a first `sceMpegAddBs` the game feeds the IPU itself:
before each `sceMpegGetPicture` (`0x1780F4`) it writes `D4_TADR`, `D4_QWC = 0` and `D4_CHCR = 0x105`
(`0x178008`-`0x178030`). The chain is `next` (`0x1241090`) to `ref` (`0x7D00` quadwords at `0x11C4080`) and back: the
whole 500 KB movie held in RAM, looped by a chain that never ends. The runtime had no channel 4, so `GetPicture`
waited for ever on `EeWaitReason::Mpeg`; reading the chain at once took 256 MiB of repeated stream instead. On
boubou666/PS2Recomp branch `sctpt2/ipu-in-dma` channel 4 stays busy after a start and `sceMpegGetPicture` pulls
64 KiB at a time from where the chain stopped until a picture decodes.

## Into a match

- Overlays are filled by the decompressor thread over several frames: the capture waits for the request's status
  byte (`0x464640 + 0x1E`) and takes the overlay's size from the request (`+0x8`), since the match overlay runs to
  `0x576928`, past `.staytrash`. Overlay modules recompile in two passes (`scripts/module_bounds.py`) so the data in
  an image does not become code: every module now has 0 decode errors.
- `function_bounds.py --pointers` took a switch's case labels (they follow a `jr $ra`) for functions and cut
  `0x1065F8` in two; switch tables are now found from the `jr` that dispatches through them and left out.

Exhibition reaches the match intro (Rod Laver Arena, Roddick against Henin-Hardenne, 3D photographers through VU1).
Then the main thread spins in `0x1000B4`, a walk over VIF packets that patches each `UNPACK` (`0x6C`) until the
`MSCNT`+IRQ code (`0x97`): the packet address comes from a DMA tag table (`0x1B81A0`) indexed by halfwords the code
reads from VU memory (`0x110082BC`), and those are garbage, so the walk reads random memory for ever.

## Status

Boots, menus, Exhibition setup and the match intro work; the match itself stalls on the VU-produced index list above.
