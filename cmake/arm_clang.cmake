# The toolchain file of COMPILE_TARGET arm_clang: clang and lld for the chip package's core (TARGET_*, kvasir_core()),
# with the SDK's libc++, llvm-libc and compiler-rt (lib/) or arm-none-eabi-gcc's libstdc++, newlib and libgcc (CPPLIB,
# CLIB, COMPILER_RT).

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/arm_compiler_common.cmake)

if("${CPPLIB}" STREQUAL "libstdc++"
   OR "${COMPILER_RT}" STREQUAL "gcc"
   OR "${CLIB}" STREQUAL "newlib")
    find_program(arm-none-eabi-gcc arm-none-eabi-gcc REQUIRED)

    mark_as_advanced(FORCE arm-none-eabi-gcc)
    kvasir_gcc_sysroot(${arm-none-eabi-gcc} GCC_ARM_NONE_EABI_ROOT GCC_ARM_NONE_EABI_INCLUDE)
endif()

find_program(clang clang PATHS REQUIRED)
find_program(clang++ clang++ PATHS REQUIRED)
find_program(llvm-size llvm-size PATHS REQUIRED)
find_program(llvm-ar llvm-ar PATHS REQUIRED)
find_program(llvm-nm llvm-nm PATHS REQUIRED)
find_program(llvm-strip llvm-strip PATHS REQUIRED)
find_program(llvm-ranlib llvm-ranlib PATHS REQUIRED)
find_program(ld.lld ld.lld PATHS REQUIRED)
find_program(llvm-objcopy llvm-objcopy PATHS REQUIRED)
find_program(llvm-objdump llvm-objdump PATHS REQUIRED)

mark_as_advanced(
    FORCE
    clang
    clang++
    llvm-size
    llvm-ar
    llvm-nm
    llvm-strip
    llvm-ranlib
    ld.lld
    llvm-objcopy
    llvm-objdump)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_SIZE llvm-size)
set(CMAKE_STRIP llvm-strip)
set(CMAKE_AR llvm-ar)
set(CMAKE_NM llvm-nm)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_LINKER ld.lld)
set(CMAKE_OBJCOPY llvm-objcopy)
set(CMAKE_OBJDUMP llvm-objdump)

find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)

set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_LINKER> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_LINKER> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(target_flags
    -mfloat-abi=${TARGET_FLOAT_ABI}
    -mfpu=${TARGET_FPU}
    -mcpu=${TARGET_CPU}
    -mtune=${TARGET_CPU}
    -march=arm${TARGET_ARCH}
    -m${TARGET_ARM_INSTRUCTION_MODE}
    -m${TARGET_ENDIAN}
    -target
    ${TARGET_TRIPLE})

# The Cortex-M0+ has no exclusive accesses, so clang calls a library function for EVERY atomic access there, a plain
# load or store included. +atomics-32 lets it do loads and stores of up to 32 bits inline (they are single instructions)
# and call __sync_* for the read-modify-writes only, which kvasir/StartUp/Runtime/LibAtomic.hpp provides on the same
# lock as before. Measured: M0+ release images 2.2 % smaller in sum, an atomic load 14 -> 0 cycles and Clock::now() 91
# -> 58 on the RP2040. Part of target_flags, which becomes the CMAKE_<LANG>_FLAGS string: as a target option list CMake
# would drop the second -Xclang.
if(TARGET_ARCH STREQUAL "v6-m")
    list(APPEND target_flags -Xclang -target-feature -Xclang +atomics-32)
endif()

set(optimize_option_common -ggdb3 -flto -fwhole-program-vtables -fforce-emit-vtables)

# A/B switches for flag experiments, given with -D when a tree is configured; unset or empty leaves every command line
# as it is. KVASIR_EXTRA_COMPILE_FLAGS: a ;-list added at the end of each optimisation set, so the runtime libraries get
# it too and a later -O wins. KVASIR_EXTRA_LINK_FLAGS: a ;-list added to the link. KVASIR_LTO_LEVEL: the digit of lld's
# --lto-O.
set(kvasir_lto_level 3)
if(NOT "${KVASIR_LTO_LEVEL}" STREQUAL "")
    set(kvasir_lto_level ${KVASIR_LTO_LEVEL})
endif()

# -fvirtual-function-elimination with default visibility and lld's --lto-whole-program-visibility nulls EVERY vtable
# slot, called ones too (clang/lld 23.1.1): under default visibility a virtual call is a plain load, which VFE does not
# count as a use of the slot. With -fvisibility=hidden the calls are type.checked.load and VFE is exact.
if("-fvirtual-function-elimination" IN_LIST KVASIR_EXTRA_COMPILE_FLAGS AND NOT "-fvisibility=hidden" IN_LIST
                                                                           KVASIR_EXTRA_COMPILE_FLAGS)
    message(FATAL_ERROR "-fvirtual-function-elimination needs -fvisibility=hidden here (see cmake/arm_clang.cmake)")
endif()

# Source coverage (the `coverage` variant of kvasir_executable_variants): the instrumentation the bare-metal profile
# runtime reads. `bytes`: one byte a region, a plain store, covered or not; `counts`: 64-bit counters (non-atomic adds),
# for PGO. Compile-time only: under LTO the counters are lowered before the bitcode is written.
set(coverage_option_bytes -fprofile-instr-generate -fcoverage-mapping "SHELL:-mllvm -enable-single-byte-coverage=true")
set(coverage_option_counts -fprofile-instr-generate -fcoverage-mapping)

# The stack protector per variant: compiler_common.cmake's -fstack-protector-strong guards every function that takes a
# local's address, about 24 bytes a function (0.5 to 1.3 KB in a firmware). The release sets take plain
# -fstack-protector (char arrays only; the later flag wins), debug keeps strong, sanitize puts strong back below. The
# libc and compiler-rt have no protector at all, their own flag lists say so.
set(optimize_option_speed ${optimize_option_common} -O3 -fstack-protector ${KVASIR_EXTRA_COMPILE_FLAGS})
set(optimize_option_size ${optimize_option_common} -Oz -fstack-protector ${KVASIR_EXTRA_COMPILE_FLAGS})
set(optimize_option_debug ${optimize_option_common} -Og ${KVASIR_EXTRA_COMPILE_FLAGS})

# Backend options of an optimisation set go to the LINKER: with -flto the code is generated inside lld, and an -mllvm
# given at compile time never reaches it (the speed set's -arm-promote-constant did nothing there).
set(optimize_link_option_speed --mllvm=-arm-promote-constant=true)
set(optimize_link_option_size)
set(optimize_link_option_debug)

# Real Undefined Behavior (Standard C/C++ UB)
set(sanitize_option_ub
    -fsanitize=alignment
    -fsanitize=bool
    -fsanitize=builtin
    -fsanitize=bounds
    -fsanitize=enum
    -fsanitize=float-cast-overflow
    -fsanitize=float-divide-by-zero
    -fsanitize=integer-divide-by-zero
    -fsanitize=nonnull-attribute
    -fsanitize=null
    -fsanitize=nullability-arg
    -fsanitize=nullability-assign
    -fsanitize=nullability-return
    -fsanitize=object-size
    -fsanitize=pointer-overflow
    -fsanitize=return
    -fsanitize=returns-nonnull-attribute
    -fsanitize=shift
    -fsanitize=shift-base
    -fsanitize=shift-exponent
    -fsanitize=signed-integer-overflow
    -fsanitize=unreachable
    -fsanitize=vla-bound
    -fsanitize=function
    # groups
    -fsanitize=undefined
    -fsanitize=nullability)

# Extensions/Implementation-Defined Behavior
set(sanitize_option_extension
    -fsanitize=unsigned-shift-base
    -fsanitize=implicit-unsigned-integer-truncation
    -fsanitize=implicit-signed-integer-truncation
    -fsanitize=implicit-integer-sign-change
    -fsanitize=implicit-bitfield-conversion
    -fsanitize=unsigned-integer-overflow
    # groups
    -fsanitize=implicit-integer-truncation
    -fsanitize=implicit-integer-arithmetic-value-change
    -fsanitize=implicit-conversion
    -fsanitize=integer)

set(sanitize_option ${sanitize_option_ub} -fsanitize-minimal-runtime -fstack-protector-strong
                    -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)

if(ENABLE_SANITIZE_EXTENSIONS)
    list(APPEND sanitize_option ${sanitize_option_extension})
endif()

set(system_includes)

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-resource-dir
    OUTPUT_VARIABLE _clang_resource_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# libc++ must precede whichever libc among the -isystem entries
if("${CPPLIB}" STREQUAL "libstdc++")
    list(APPEND system_includes ${GCC_ARM_NONE_EABI_INCLUDE} ${GCC_ARM_NONE_EABI_INCLUDE}/arm-none-eabi
         ${GCC_ARM_NONE_EABI_INCLUDE}/backward)
else()
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libcxx/include)
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libcxx/src)
endif()

if("${CLIB}" STREQUAL "newlib")
    # newlib, unlike llvm-libc, does not ship the compiler headers (stddef.h, ...) that -nostdinc hides
    list(APPEND system_includes ${GCC_ARM_NONE_EABI_ROOT}/include ${_clang_resource_dir}/include)
else()
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libc/include)
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libc)
endif()
# last, so a libc that ships its own stddef.h/stdint.h (llvm-libc does) wins
list(APPEND system_includes ${_clang_resource_dir}/include)

list(TRANSFORM system_includes PREPEND "-isystem")

set(linker_search_path)

if("${CPPLIB}" STREQUAL "libstdc++"
   OR "${COMPILER_RT}" STREQUAL "gcc"
   OR "${CLIB}" STREQUAL "newlib")
    # ask the gcc driver where its multilib lives instead of assembling the directory name by hand
    kvasir_gcc_multilib_dirs(${arm-none-eabi-gcc} linker_search_path)
endif()

list(TRANSFORM linker_search_path PREPEND "--library-path=")

# The warning list (kvasir_warnings.cmake, shared with host test trees): kvasir::warnings carries it into the image's
# own code (util.cmake); the runtime libraries, built from upstream code, compile with the compiler's defaults
include(${CMAKE_CURRENT_LIST_DIR}/kvasir_warnings.cmake)
set(kvasir_image_warning_flags ${kvasir_clang_warning_flags} -Wno-unused-macros)

set(profile_flags)

# Hosted application code: clang then copies a small struct with ldm/stm instead of calling memcpy, calls
# __aeabi_memcpy4 / __aeabi_memclr8 / __aeabi_memset8 for copies and fills of known alignment (direct entries in
# Kvasir's assembly memory functions, lib/libc/kvasir/arm/memory_v6m.S and memory_v7m.S - with compiler-rt's forwarders
# instead they cost 2 to 5 cycles more a copy), and uses what it knows of the C library (-fno-math-errno is already
# set). The runtime libraries keep -ffreestanding (and -fno-builtin) in their own flag lists, so no memcpy is built out
# of memcpy. -Wno-main: StartUp.hpp declares main() extern "C" and takes its address. __STDC_HOSTED__ stays 0: the C
# library has no FILE, and third-party headers read the macro as "stdio exists" (emio compiles its std::fseek file
# buffer under it). Clang has no flag for "builtins yes, hosted no" - -fbuiltin after -ffreestanding changes nothing -
# so the macro is set by hand, which clang calls redefining a builtin macro (a default warning: its -Wno stays on every
# command line, the runtime libraries' too). Not with libstdc++: it hides <string> and the rest of its hosted headers
# under __STDC_HOSTED__=0 (requires_hosted.h), and newlib, its C library here, has FILE anyway.
list(REMOVE_ITEM arm_compiler_common_flags -ffreestanding)
list(APPEND kvasir_image_warning_flags -Wno-main)
if(NOT "${CPPLIB}" STREQUAL "libstdc++")
    list(APPEND arm_compiler_common_flags -U__STDC_HOSTED__ -D__STDC_HOSTED__=0 -Wno-builtin-macro-redefined)
endif()

if("${CPPLIB}" STREQUAL "libc++")
    list(APPEND profile_flags ${libcxx_profile_flags})
endif()

if("${CLIB}" STREQUAL "llvm")
    list(APPEND profile_flags -DLIBC_NAMESPACE=__llvm_libc)
endif()

if("${COMPILER_RT}" STREQUAL "gcc")
    # tells StartUp/Runtime/Aeabi.hpp to provide the AEABI memory helpers, which libgcc does not have
    list(APPEND profile_flags -DKVASIR_COMPILER_RT_LIBGCC=1)
endif()

set(common_flags ${target_flags} ${profile_flags} ${system_includes} ${compiler_common_flags}
                 ${arm_compiler_common_flags})

set(cxx_flags ${common_flags} ${compiler_common_cxx_flags} -nostdinc++)

set(c_flags ${common_flags} ${compiler_common_c_flags} -nostdinc)

set(asm_flags ${common_flags} ${compiler_common_asm_flags})

kvasir_whole_archive_feature("${LINKER_PREFIX}")

set(link_flags_common
    ${linker_common_flags}
    -nostdlib
    --lto-O${kvasir_lto_level}
    --ignore-data-address-equality
    --ignore-function-address-equality
    --lto-whole-program-visibility
    --icf=all
    --no-allow-multiple-definition
    # lld's own level: 2 also merges string tails
    -O2
    ${KVASIR_EXTRA_LINK_FLAGS}
    ${linker_search_path})

# the system libraries per optimisation set: size and debug link newlib's and libstdc++'s nano builds
set(_nano_speed "")
set(_nano_size _nano)
set(_nano_debug _nano)
foreach(_set speed size debug)
    set(_libs)
    if("${CPPLIB}" STREQUAL "libstdc++")
        list(APPEND _libs stdc++${_nano_${_set}})
    endif()
    if("${CLIB}" STREQUAL "newlib")
        list(APPEND _libs c${_nano_${_set}} m)
    endif()
    if("${COMPILER_RT}" STREQUAL "gcc")
        list(APPEND _libs gcc)
    endif()
    list(TRANSFORM _libs PREPEND "--library=")
    set(linker_flags_${_set} ${link_flags_common} ${_libs})
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common_end.cmake)
