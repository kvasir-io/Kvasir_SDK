#pragma once
// The image CRC descriptor (IMAGE_CRC): which flash bytes make up the image and their CRC-32, written into the ELF
// after the link by cmake/tools/patch_image_crc.py. Kvasir::ImageCheck (ImageCheck.hpp) walks it at run time.
// In a RAM image (RAM_ONLY) the segments are its read-only part in SRAM instead - .data left out - and on the
// RP2350 the picobin block behind the vectors is left out of either (the patcher's docstring has both rules).
//
// The segments are exactly the data records of <name>_flash.hex - what J-Link programs and the log printer checks
// (uc_log detail/HexImage.hpp): contiguous records form one segment, gaps are never read. The CRC is
// CRC-32/ISO-HDLC chained over the segments in address order (zlib crc32(previous, data), the printer's imageCrc),
// with the descriptor's own 80 bytes left out (the segment holding it is split in two).
#include <cstddef>
#include <cstdint>

namespace Kvasir::ImageCheck {
struct Segment {
    std::uint32_t
      address;   // load address (the XIP window on RP, 0-based NVM on SAM, SRAM in a RAM image)
    std::uint32_t length;   // bytes
};

struct Descriptor {
    static constexpr std::uint32_t Magic       = 0x4B49'4352U;   // "RCIK": patched
    static constexpr std::uint32_t Unpatched   = 0xFFFF'FFFFU;   // as linked
    static constexpr std::uint32_t Version     = 1;
    static constexpr std::size_t   MaxSegments = 8;
    std::uint32_t                  magic;
    std::uint32_t                  version;
    std::uint32_t                  crc;   // CRC-32/ISO-HDLC over segments[0..count)
    std::uint32_t                  count;
    Segment                        segments[MaxSegments];
};

static_assert(sizeof(Descriptor) == 80,
              "patch_image_crc.py writes 80 bytes");

#if defined(KVASIR_IMAGE_CRC) && KVASIR_IMAGE_CRC
// One object, whichever TUs include this (inline, a fixed symbol name for the patcher). volatile: the value is
// written after the link, so the compiler must never fold the initialiser into the code. Placed by
// linker/common_text_body.inc.ld (KEEP(*(.kvasir_image_crc))), after .rodata.
[[gnu::used, gnu::section(".kvasir_image_crc")]] inline Descriptor const volatile linkedDescriptor asm(
  "kvasir_image_crc_descriptor"){.magic    = Descriptor::Unpatched,
                                 .version  = Descriptor::Version,
                                 .crc      = 0,
                                 .count    = 0,
                                 .segments = {}};
#endif
}   // namespace Kvasir::ImageCheck
