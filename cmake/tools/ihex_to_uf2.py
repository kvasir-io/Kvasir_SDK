import struct
import sys


class IntelHexParser:
    """Minimal Intel HEX file parser."""

    def __init__(self, filename):
        self.data = {}  # address -> byte
        self._parse(filename)

    def _parse(self, filename):
        extended_addr = 0
        with open(filename, 'r') as f:
            for line in f:
                line = line.strip()
                if not line.startswith(':'):
                    continue
                # Parse fields: :LLAAAATT[DD...]CC
                byte_count = int(line[1:3], 16)
                address = int(line[3:7], 16)
                record_type = int(line[7:9], 16)

                if record_type == 0x00:  # Data record
                    full_addr = extended_addr + address
                    for i in range(byte_count):
                        byte = int(line[9 + i * 2: 11 + i * 2], 16)
                        self.data[full_addr + i] = byte
                elif record_type == 0x01:  # End of File
                    break
                elif record_type == 0x04:  # Extended Linear Address
                    extended_addr = int(line[9:13], 16) << 16


PAGE_SIZE = 256


def pages_of(data):
    """Group address -> byte onto a 256-byte page grid.

    Returns a sorted list of (page_address, 256 bytes). Bytes the input does not
    cover are 0x00, so a gap between two sections (the linker's alignment padding
    between .vectors and .text, for instance) is filled rather than left to start a
    second, unaligned block run. Both picotool and the RP2 bootrom require every
    block's target address to be page aligned and reject or drop the rest.
    """
    pages = {}
    for addr, byte in data.items():
        page = addr & ~(PAGE_SIZE - 1)
        buf = pages.get(page)
        if buf is None:
            buf = pages[page] = bytearray(PAGE_SIZE)
        buf[addr - page] = byte
    return sorted(pages.items())


def convert_to_uf2(pages, familyid):
    UF2_MAGIC_START0 = 0x0A324655
    UF2_MAGIC_START1 = 0x9E5D5157
    UF2_MAGIC_END = 0x0AB16F30
    FLAG_FAMILY_ID_PRESENT = 0x2000

    datapadding = b"\x00" * (512 - PAGE_SIZE - 32 - 4)
    numblocks = len(pages)
    outp = []
    for blockno, (page, chunk) in enumerate(pages):
        hd = struct.pack(b"<IIIIIIII",
                         UF2_MAGIC_START0, UF2_MAGIC_START1,
                         FLAG_FAMILY_ID_PRESENT, page, PAGE_SIZE, blockno, numblocks, familyid)
        block = hd + bytes(chunk) + datapadding + \
            struct.pack(b"<I", UF2_MAGIC_END)
        assert len(block) == 512
        outp.append(block)
    return b"".join(outp)


in_filename = sys.argv[1]
out_filename = sys.argv[2]
familyid = int(sys.argv[3], base=0)

ih = IntelHexParser(in_filename)

with open(out_filename, "wb") as f:
    f.write(convert_to_uf2(pages_of(ih.data), familyid))
