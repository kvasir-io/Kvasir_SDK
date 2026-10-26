#pragma once
// The runtime-library glue of StartUp.hpp, included once at its end (the TU with main). The order of the includes is
// fixed: under LTO the definition order can move .text. Never include this (or a fragment) from another TU: every
// fragment defines strong symbols. A new library stand-in goes into the fragment of its library; LibAtomic.hpp (the
// libatomic stand-in) is the last fragment.
//
// The one-TU rule's guard, defined before every fragment so that it is among the first duplicates a linker names when
// a second TU of the same image includes StartUp.hpp: lld "duplicate symbol: kvasir_StartUp_hpp_is_included_by_two_TUs_
// of_one_image", GNU ld "multiple definition of ...". Nothing calls it: the link resolves it, LTO drops it (no image
// byte). A compile-time #error cannot see another TU.
extern "C" void kvasir_StartUp_hpp_is_included_by_two_TUs_of_one_image();

extern "C" void kvasir_StartUp_hpp_is_included_by_two_TUs_of_one_image() {}

// clang-format off: the order is the point
#include "kvasir/StartUp/Runtime/PanicEntry.hpp"
#include "kvasir/StartUp/Runtime/Libc.hpp"
#include "kvasir/StartUp/Runtime/Aeabi.hpp"
#include "kvasir/StartUp/Runtime/FatalPaths.hpp"
#include "kvasir/StartUp/Runtime/Cxx.hpp"
#include "kvasir/StartUp/Runtime/LibAtomic.hpp"
// clang-format on
