#!/usr/bin/env python3
"""Writes recomp/function_bounds.csv: every function ps2_recomp found without a map, ending at the next one.

usage: function_bounds.py <config.toml> <generated-dir> [--end 0xSTART=0xEND ...] [--exclude 0xSTART-0xEND ...]
       [--callers overlay.bin@0xLOAD ...] [--pointers elf-or-image@0xLOAD ...] > recomp/function_bounds.csv

<generated-dir> is ps2_recomp output from <config.toml> with ghidra_output = "": a map turns the recompiler's own
function discovery off, so the map has to carry every function. [general].skip entries end the function before them
and are left out; --end cuts a function short where data follows its last instruction; --exclude leaves a range
out of the map (overlay code, data); --callers adds the JAL targets an overlay image has in the ELF's segments,
functions only overlays call; --pointers adds every function a word of the image points at, when the word there is an
"addiu $sp, $sp, -N" prologue or right after a "jr $ra" and its delay slot, or code builds with a lui/addiu pair
(functions reached only through function pointers). An image given as ELF@0 is read through its program headers.
"""
import argparse
import os
import re
import struct
import tomllib

SEGMENTS = [(0x100000, 0x20C510), (0x46EA80, 0x4A7E38)]
JR_RA = 0x03E00008


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config")
    parser.add_argument("generated")
    parser.add_argument("--end", action="append", default=[])
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--callers", action="append", default=[])
    parser.add_argument("--pointers", action="append", default=[])
    args = parser.parse_args()

    general = tomllib.load(open(args.config, "rb"))["general"]
    names = {}
    for file in os.listdir(args.generated):
        match = re.match(r"(.+)_0x([0-9a-f]+)\.cpp$", file)
        if match and not file.startswith("entry_"):
            names[int(match.group(2), 16)] = match.group(1)
    for key in ("stubs", "untracked_stubs"):
        for selector in general.get(key, []):
            name, address = selector.split("@")
            names.setdefault(int(address, 16), name)
    skips = {int(selector.split("@")[1], 16) for selector in general.get("skip", [])}
    ends = {int(start, 16): int(end, 16) for start, end in (cut.split("=") for cut in args.end)}
    excluded = [(int(start, 16), int(end, 16)) for start, end in (span.split("-") for span in args.exclude)]

    for caller in args.callers:
        path, load = caller.rsplit("@", 1)
        load = int(load, 16)
        image = open(path, "rb").read()
        for offset in range(0, len(image) - 3, 4):
            word = struct.unpack_from("<I", image, offset)[0]
            if word >> 26 != 3:
                continue
            target = ((load + offset + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
            inside = any(begin <= target < end for begin, end in SEGMENTS)
            if inside and not any(begin <= target < end for begin, end in excluded):
                names.setdefault(target, f"sub_{target:08X}")

    elf_chunks = []
    for spec in args.pointers:
        data = open(spec.rsplit("@", 1)[0], "rb").read()
        if data[:4] == b"\x7fELF":
            phoff, phentsize, phnum = struct.unpack_from("<I", data, 28)[0], *struct.unpack_from("<HH", data, 42)
            for index in range(phnum):
                kind, offset, vaddr, _, filesz = struct.unpack_from("<IIIII", data, phoff + index * phentsize)
                if kind == 1:
                    elf_chunks.append((vaddr, data[offset:offset + filesz]))
    def consider(target):
        if target % 4 or not any(begin <= target < end for begin, end in SEGMENTS[:1]):
            return
        if any(begin <= target < end for begin, end in excluded):
            return
        for vaddr, code in elf_chunks:
            if vaddr + 12 <= target < vaddr + len(code) - 3:
                word = struct.unpack_from("<I", code, target - vaddr)[0]
                after_return = JR_RA in (struct.unpack_from("<I", code, target - vaddr - back)[0] for back in (8, 12))
                if (word >> 16 == 0x27BD and word & 0x8000) or after_return:
                    names.setdefault(target, f"sub_{target:08X}")

    for spec in args.pointers:
        path, load = spec.rsplit("@", 1)
        data = open(path, "rb").read()
        if data[:4] == b"\x7fELF":
            chunks = elf_chunks
        else:
            chunks = [(int(load, 16), data)]
        for _, chunk in chunks:
            high = {}
            for offset in range(0, len(chunk) - 3, 4):
                word = struct.unpack_from("<I", chunk, offset)[0]
                consider(word)
                opcode, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
                if opcode == 0x0F:
                    high[rt] = ((word & 0xFFFF) << 16, offset)
                elif opcode == 0x09 and rs in high and offset - high[rs][1] <= 64:
                    consider((high[rs][0] + ((word & 0xFFFF) ^ 0x8000) - 0x8000) & 0xFFFFFFFF)

    starts = sorted(set(names) | skips)
    print("Name,Start,End,Size")
    for index, start in enumerate(starts):
        if start in skips or any(begin <= start < end for begin, end in excluded):
            continue
        segment_end = next(end for begin, end in SEGMENTS if begin <= start < end)
        end = segment_end
        if index + 1 < len(starts) and starts[index + 1] < segment_end:
            end = starts[index + 1]
        end = ends.get(start, end)
        print(f"{names[start]},0x{start:08X},0x{end:08X},{end - start}")


if __name__ == "__main__":
    main()
