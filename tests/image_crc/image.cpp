// An image for test_patch_image_crc.py: a .boot2 word, code, constants, initialised data, and (KVASIR_IMAGE_CRC)
// the descriptor; RAM_SEGMENT adds a section that loads into RAM. RAM_IMAGE (with test_ram.ld): vectors, a block in
// .after_vectors, a RAM function; RAM_CTOR adds a static constructor (an .init_array entry, which is writable).
#include "kvasir/Util/ImageDescriptor.hpp"

#include <cstdint>

#ifndef RAM_IMAGE
[[gnu::used, gnu::section(".boot2")]] std::uint32_t const boot2[4] = {0xB0072B00U, 1, 2, 3};
#endif
[[gnu::used]] std::uint32_t const table[7]   = {11, 22, 33, 44, 55, 66, 77};
[[gnu::used]] std::uint32_t volatile counter = 0x1234'5678U;
#ifdef RAM_SEGMENT
[[gnu::used, gnu::section(".ramimage")]] std::uint32_t volatile inRam = 5;
#endif
#ifdef RAM_IMAGE
// test_ram.ld: a vector table, a picobin-like block that a later step rewrites, a RAM function
[[gnu::used, gnu::section(".core_vectors")]] std::uint32_t const vectors[4]
  = {0x20010000U, 0x20000101U, 3, 4};
[[gnu::used, gnu::section(".after_vectors")]] std::uint32_t const imageDef[5]
  = {0xFFFFDED3U, 0x10210142U, 0x1FFU, 0, 0xAB123579U};

extern "C" [[gnu::used,
             gnu::noinline,
             gnu::section(".ramfunc")]] std::uint32_t
twice(std::uint32_t v) {
    return v * 2U;
}
#endif
#ifdef RAM_CTOR
struct Constructed {
    [[gnu::noinline]] Constructed() : value{counter} {}

    std::uint32_t value;
};

[[gnu::used]] Constructed constructed;
#endif
#ifdef RAM_FLASH_SEGMENT
[[gnu::used, gnu::section(".inflash")]] std::uint32_t const inFlash[2] = {1, 2};
#endif

extern "C" [[gnu::used,
             gnu::section(".text.entry"),
             noreturn]] void entry() {
    while(true) {
        counter = counter + table[counter % 7];
#ifdef RAM_IMAGE
        counter = twice(counter);
#endif
    }
}
