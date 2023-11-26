import intelhex
import struct
import sys

def convert_to_uf2(start, data, familyid):
    UF2_MAGIC_START0 = 0x0A324655
    UF2_MAGIC_START1 = 0x9E5D5157
    UF2_MAGIC_END    = 0x0AB16F30

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

ih = intelhex.IntelHex(in_filename)

with open(out_filename, "wb") as f:
    for s in ih.segments():
        f.write(convert_to_uf2(s[0], bytearray(ih.tobinarray(start=s[0], end=s[1])), familyid))
