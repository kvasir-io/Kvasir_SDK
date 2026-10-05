// An image for test_patch_image_crc.py: a .boot2 word, code, constants, initialised data, and (KVASIR_IMAGE_CRC)
// the descriptor; RAM_SEGMENT adds a section that loads into RAM.
#include "kvasir/Util/ImageDescriptor.hpp"

#include <cstdint>

[[gnu::used, gnu::section(".boot2")]] std::uint32_t const boot2[4] = {0xB0072B00U, 1, 2, 3};
[[gnu::used]] std::uint32_t const                         table[7] = {11, 22, 33, 44, 55, 66, 77};
[[gnu::used]] std::uint32_t volatile counter                       = 0x1234'5678U;
#ifdef RAM_SEGMENT
[[gnu::used, gnu::section(".ramimage")]] std::uint32_t volatile inRam = 5;
#endif

extern "C" [[gnu::used,
             gnu::section(".text.entry"),
             noreturn]] void entry() {
    while(true) { counter = counter + table[counter % 7]; }
}
