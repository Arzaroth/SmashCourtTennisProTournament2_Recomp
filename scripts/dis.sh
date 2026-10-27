#!/bin/bash
# usage: dis.sh <start> <stop>  - disassemble the game ELF (SCT2_ELF) between two addresses
llvm-objdump -d --no-show-raw-insn --triple=mipsel --mcpu=mips3 --start-address=$1 --stop-address=$2 "${SCT2_ELF:-$(dirname "$0")/../game/SLUS_209.33}" | grep -E '^ +[0-9a-f]+:' | sed -E 's/<\.text\+0x[0-9a-f]+>//'
