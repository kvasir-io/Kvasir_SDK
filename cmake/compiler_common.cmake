# no C++20 modules here, and gcc's dependency scanner breaks the gcc + libc++ tree (it resolves headers through
# libstdc++'s bits/stdc++.h)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# The flags every image and runtime library starts from, either compiler: sections per function and object, the stack
# protector, the language standards without exceptions and RTTI, the static link with its entry ResetISR.

set(compiler_common_flags
    -ffunction-sections
    -fdata-sections
    -fno-common
    -fomit-frame-pointer
    -fmerge-all-constants
    # no math function here sets errno (the libc is built LIBC_MATH_NO_ERRNO): with this the compiler may use the
    # instruction for sqrtf and friends where the core has one
    -fno-math-errno
    -fstack-protector-strong)

set(compiler_common_cxx_flags
    -std=c++26
    -fno-exceptions
    -fno-unwind-tables
    # with UBSan on, clang still emits .ARM.exidx entries under -fno-unwind-tables alone
    -fno-asynchronous-unwind-tables
    -fno-use-cxa-atexit
    -fno-rtti
    -fno-threadsafe-statics
    -fstrict-enums)

set(compiler_common_c_flags -std=c23)

set(compiler_common_asm_flags)

set(linker_common_flags --static --Bstatic --gc-sections --entry=ResetISR)
