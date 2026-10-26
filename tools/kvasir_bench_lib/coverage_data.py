"""A coverage image's .profraw from Kvasir::Coverage's manifest: the pieces from the image where the board holds the
image's bytes, the counters read live."""
import struct


COVERAGE_MAGIC = 0x4B434F56   # "KCOV", Kvasir::Coverage::Magic
COVERAGE_PIECES = 24          # Kvasir::Coverage::Manifest::pieces
COVERAGE_KINDS = ("memory", "zeros", "inlined")


def assemble_profraw(manifest: bytes, flash_bytes, read_live) -> bytes:
    """The .profraw Kvasir::Coverage's manifest describes: inlined pieces from the manifest's own header copy, zero
    padding, and the pieces by address - flash_bytes(address, length) for what lies in the image (records, names; None
    when not), read_live(address, length) for the rest (the counters, live RAM)."""
    magic, status, count, _used = struct.unpack_from("<4I", manifest, 0)
    if magic != COVERAGE_MAGIC:
        raise ValueError(
            "the manifest is empty: the board has not run Kvasir::Coverage's Startup entry yet")
    if status != 0:
        raise ValueError(
            f"the profile runtime reported {status} laying out the file (manifest too small?)")
    header_at = (16 + 12 * COVERAGE_PIECES + 7) & ~7
    out = bytearray()
    for i in range(count):
        where, length, kind = struct.unpack_from("<3I", manifest, 16 + 12 * i)
        if COVERAGE_KINDS[kind] == "zeros":
            out += bytes(length)
        elif COVERAGE_KINDS[kind] == "inlined":
            out += manifest[header_at + where:header_at + where + length]
        else:
            data = flash_bytes(where, length)
            out += data if data is not None else read_live(where, length)
    return bytes(out)


def image_reader(resident):
    """-> read(where, length): the bytes from the ELF when [where, where + length) lies inside one section of
    `resident` (elf_image.Elf.resident(): runs where it is loaded, not writable - the records and names of a coverage
    image), else None: the caller reads it from the board (the counters, in .data)."""
    def read(where: int, length: int) -> bytes | None:
        for (_name, start, data) in resident:
            if start <= where and where + length <= start + len(data):
                return data[where - start:where - start + length]
        return None
    return read
