#!/usr/bin/env python3
"""Writes recomp/function_bounds.csv: every function ps2_recomp found without a map, ending at the next one.

usage: function_bounds.py <config.toml> <generated-dir> [--end 0xSTART=0xEND ...] [--exclude 0xSTART-0xEND ...]
       [--callers overlay.bin@0xLOAD ...] > recomp/function_bounds.csv

<generated-dir> is ps2_recomp output from <config.toml> with ghidra_output = "": a map turns the recompiler's own
function discovery off, so the map has to carry every function. [general].skip entries end the function before them
and are left out; --end cuts a function short where data follows its last instruction; --exclude leaves a range
to overlay modules; --callers adds the JAL targets an overlay image has in the ELF's segments, functions only
overlays call.
"""
import argparse
import os
import re
import struct
import tomllib

SEGMENTS = [(0x100000, 0x20C510), (0x46EA80, 0x4A7E38)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("config")
    parser.add_argument("generated")
    parser.add_argument("--end", action="append", default=[])
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--callers", action="append", default=[])
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
