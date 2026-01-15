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

    def segments(self):
        """Return list of (start, end) tuples for contiguous memory regions (end is exclusive)."""
        if not self.data:
            return []
        addresses = sorted(self.data.keys())
        segments = []
        seg_start = addresses[0]
        seg_end = seg_start
        for addr in addresses[1:]:
            if addr == seg_end + 1:
                seg_end = addr
            else:
                segments.append((seg_start, seg_end + 1))
                seg_start = addr
                seg_end = addr
        segments.append((seg_start, seg_end + 1))
        return segments

    def tobinarray(self, start, end):
        """Return bytes for address range [start, end) (end exclusive)."""
        return bytes(self.data[a] for a in range(start, end))


def convert_to_uf2(start, data, familyid):
    UF2_MAGIC_START0 = 0x0A324655
    UF2_MAGIC_START1 = 0x9E5D5157
    UF2_MAGIC_END = 0x0AB16F30

    datapadding = b""
    while len(datapadding) < 512 - 256 - 32 - 4:
        datapadding += b"\x00\x00\x00\x00"
    numblocks = (len(data) + 255) // 256
    outp = []
    for blockno in range(numblocks):
        ptr = 256 * blockno
        chunk = data[ptr:ptr + 256]
        flags = 0x2000
        hd = struct.pack(b"<IIIIIIII",
                         UF2_MAGIC_START0, UF2_MAGIC_START1,
                         flags, ptr + start, 256, blockno, numblocks, familyid)
        while len(chunk) < 256:
            chunk += b"\x00"
        block = hd + chunk + datapadding + struct.pack(b"<I", UF2_MAGIC_END)
        assert len(block) == 512
        outp.append(block)
    return b"".join(outp)


in_filename = sys.argv[1]
out_filename = sys.argv[2]
familyid = int(sys.argv[3], base=0)

ih = IntelHexParser(in_filename)

with open(out_filename, "wb") as f:
    for s in ih.segments():
        f.write(convert_to_uf2(s[0], bytearray(
            ih.tobinarray(start=s[0], end=s[1])), familyid))
