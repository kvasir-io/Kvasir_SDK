#!/usr/bin/env python3
"""Write the J-Link command file that starts a RAM_ONLY image without a reset.

A reset runs the chip's boot ROM, which boots whatever is in flash (or waits in BOOTSEL), never
the image just loaded into RAM. So the script loads the image and then does what a reset would
do with this image's vector table (Armv6-M ARM DDI0419E B1.5.5 "Reset behavior", the same in
Armv8-M): SP_main = word 0, PC = word 1 with bit 0 cleared, EPSR.T = 1 (xPSR bit 24, B1.4.2),
and VTOR = the table (0xE000ED08: RP2040 datasheet 2.4.8 Table 106, Armv8-M ARM DDI0553B.y
D1.2.272), so interrupts reach this image's handlers and not the boot ROM's.

The words come from the linked ELF: the table starts at the linker symbol
_LINKER_vectors_start_ (common_vectors_body.inc.ld).

usage: ram_image_jlink.py <image.elf> <template> <out.jlink>
The template is the script with {vector_table}, {initial_sp} and {reset_pc} in it.
"""

import struct
import sys

VECTORS_SYMBOL = b"_LINKER_vectors_start_"
SHT_PROGBITS = 1
SHT_SYMTAB = 2


def die(msg: str) -> None:
    sys.exit(f"ram_image_jlink.py: {msg}")


def vector_table(data: bytes) -> tuple[int, int, int]:
    """(address, word 0, word 1) of the image's vector table."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        die("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    sections = [struct.unpack_from(
        "<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]

    table = None
    for sh in sections:
        if sh[1] != SHT_SYMTAB:
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], sh[9]):
            name, value = struct.unpack_from("<II", data, off)
            start = strtab[4] + name
            if data[start:start + len(VECTORS_SYMBOL) + 1] == VECTORS_SYMBOL + b"\0":
                table = value
                break
    if table is None:
        die(f"no symbol {VECTORS_SYMBOL.decode()}")

    for sh in sections:
        addr, offset, size = sh[3], sh[4], sh[5]
        if sh[1] == SHT_PROGBITS and addr <= table and table + 8 <= addr + size:
            sp, reset = struct.unpack_from("<II", data, offset + table - addr)
            return table, sp, reset
    die(f"no loaded section holds the vector table at 0x{table:08X}")


def main() -> None:
    if len(sys.argv) != 4:
        die("usage: ram_image_jlink.py <image.elf> <template> <out.jlink>")
    elf, template, out = sys.argv[1:]
    with open(elf, "rb") as f:
        table, sp, reset = vector_table(f.read())
    if sp & 3 or not reset & 1:
        die(f"implausible vector table at 0x{table:08X}: SP 0x{sp:08X}, reset 0x{reset:08X} "
            "(SP must be word-aligned, the reset vector a Thumb address)")
    with open(template) as f:
        script = f.read().format(vector_table=f"0x{table:08X}", initial_sp=f"0x{sp:08X}",
                                 reset_pc=f"0x{reset & ~1:08X}")
    with open(out, "w", newline="\n") as f:
        f.write(script)


if __name__ == "__main__":
    main()
