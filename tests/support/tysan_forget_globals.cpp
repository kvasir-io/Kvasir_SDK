// Linked into every executable of a TypeSanitizer tree (KVASIR_TEST_SANITIZER=type).
//
// TypeSanitizer records the type of every global when the program starts, as the type of the whole object, and its
// check does not look into an array member: each read of an element of a constant table (a std::array of structs, of
// pointers, of string views) is then reported as an access of another type - thousands of reports per test, none of
// them a violation. The recorded types of the image are dropped here, after the sanitizer's own start-up and before
// the first initialiser of the program. A store at run time records its type as before, so an access through a
// pointer of another type to memory the program wrote is still found; memory that is only ever read is not checked.
#include <cstddef>
#include <cstdint>
#include <sanitizer/tysan_interface.h>

extern "C" {
// the linker's: the first byte of the image and the byte after its last (.bss)
extern char const __ehdr_start[];
extern char const _end[];
}

namespace {
// 101: the first priority a program may use; the sanitizer's constructors run at 0, the program's initialisers after
[[gnu::constructor(101)]] void forgetGlobalTypes() {
    auto const first = reinterpret_cast<std::uintptr_t>(&__ehdr_start[0]);
    auto const last  = reinterpret_cast<std::uintptr_t>(&_end[0]);
    __tysan_set_type_unknown(&__ehdr_start[0], last - first);
}
}   // namespace
