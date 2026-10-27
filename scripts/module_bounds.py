#!/usr/bin/env python3
"""Writes a function map for one overlay module, with its functions cut before the data that follows their code.

usage: module_bounds.py <image.bin> <load address> <generated-dir> <ps2_recomp log> > map.csv

<generated-dir> and the log come from a run of the module without a map. A function the recompiler could not decode
ends at its last "jr $ra" (and delay slot) before the first undecodable word; one with no return before it starts in
data and is left out.
"""
import os
import re
import struct
import sys

JR_RA = 0x03E00008


def main():
    image = open(sys.argv[1], "rb").read()
    load = int(sys.argv[2], 0)
    generated = sys.argv[3]
    end_of_image = load + len(image) - len(image) % 4

    names = {}
    for file in os.listdir(generated):
        match = re.match(r"(.+)_0x([0-9a-f]+)\.cpp$", file)
        if match and "_entry_" not in file and not match.group(1).endswith("register_functions"):
            names[int(match.group(2), 16)] = re.sub(r"^.*?_(sub_|ptr_|boot_)", r"\1", match.group(1))

    first_error = {}
    for line in open(sys.argv[4], errors="replace"):
        match = re.search(r"\[error\].*function=\S*?(?:sub|ptr|boot)_([0-9A-Fa-f]+) addr=0x([0-9a-f]+)", line)
        if match:
            start, address = int(match.group(1), 16), int(match.group(2), 16)
            first_error[start] = min(first_error.get(start, address), address)

    returns = [load + offset for offset in range(0, len(image) - 3, 4)
               if struct.unpack_from("<I", image, offset)[0] == JR_RA]

    starts = sorted(names)
    print("Name,Start,End,Size")
    for index, start in enumerate(starts):
        end = starts[index + 1] if index + 1 < len(starts) else end_of_image
        if start in first_error:
            before = [ret for ret in returns if start <= ret < first_error[start]]
            if not before:
                continue
            end = min(end, before[-1] + 8)
        print(f"{names[start]},0x{start:08X},0x{end:08X},{end - start}")


if __name__ == "__main__":
    main()
