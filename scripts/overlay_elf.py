#!/usr/bin/env python3
"""Wraps a raw overlay image in a MIPS ELF with one executable .text section at its load address, for ps2_recomp.

usage: overlay_elf.py <image.bin> <load address> <out.elf>
"""
import struct
import sys

EHDR = struct.Struct("<16sHHIIIIIHHHHHH")
PHDR = struct.Struct("<IIIIIIII")
SHDR = struct.Struct("<IIIIIIIIII")
EM_MIPS = 8
SHSTRTAB = b"\0.text\0.shstrtab\0"


def main():
    image = open(sys.argv[1], "rb").read()
    load = int(sys.argv[2], 0)
    data_offset = 0x1000
    shstr_offset = data_offset + len(image)
    sh_offset = (shstr_offset + len(SHSTRTAB) + 3) & ~3

    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    ehdr = EHDR.pack(ident, 2, EM_MIPS, 1, load, EHDR.size, sh_offset, 0x20924001,
                     EHDR.size, PHDR.size, 1, SHDR.size, 3, 2)
    phdr = PHDR.pack(1, data_offset, load, load, len(image), len(image), 7, 0x1000)
    sections = [
        SHDR.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        SHDR.pack(1, 1, 0x6, load, data_offset, len(image), 0, 0, 16, 0),
        SHDR.pack(7, 3, 0, 0, shstr_offset, len(SHSTRTAB), 0, 0, 1, 0),
    ]

    out = bytearray(sh_offset + SHDR.size * len(sections))
    out[0:EHDR.size] = ehdr
    out[EHDR.size:EHDR.size + PHDR.size] = phdr
    out[data_offset:data_offset + len(image)] = image
    out[shstr_offset:shstr_offset + len(SHSTRTAB)] = SHSTRTAB
    out[sh_offset:] = b"".join(sections)
    open(sys.argv[3], "wb").write(out)


if __name__ == "__main__":
    main()
