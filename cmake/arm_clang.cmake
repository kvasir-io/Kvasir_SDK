# ARM Clang Compiler Configuration Configures the build environment for ARM targets using Clang/LLVM toolchain Supports
# both custom libc++ and GCC libstdc++ standard libraries

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/arm_compiler_common.cmake)

if("${CPPLIB}" STREQUAL "libstdc++"
   OR "${COMPILER_RT}" STREQUAL "gcc"
   OR "${CLIB}" STREQUAL "newlib")
    find_program(arm-none-eabi-gcc arm-none-eabi-gcc REQUIRED)

    mark_as_advanced(FORCE arm-none-eabi-gcc)
    # ugly way to get arm-none-eabi-gcc include and lib path
    execute_process(
        COMMAND ${arm-none-eabi-gcc} -print-sysroot
        OUTPUT_VARIABLE GCC_ARM_NONE_EABI_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    execute_process(
        COMMAND ${arm-none-eabi-gcc} -print-search-dirs
        OUTPUT_VARIABLE GCC_ARM_NONE_EABI_LIB_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "^install: ([^\n\r ]*)" GCC_ARM_NONE_EABI_LIB_DIR ${GCC_ARM_NONE_EABI_LIB_DIR})
    string(REGEX REPLACE "^install: " "" GCC_ARM_NONE_EABI_LIB_DIR ${GCC_ARM_NONE_EABI_LIB_DIR})

    get_filename_component(GCC_ARM_NONE_EABI_ROOT "${GCC_ARM_NONE_EABI_ROOT}" REALPATH)

    file(GLOB_RECURSE GCC_ARM_NONE_EABI_INCLUDE "${GCC_ARM_NONE_EABI_ROOT}/include/c++/*/cstddef")
    list(LENGTH GCC_ARM_NONE_EABI_INCLUDE _gcc_cxx_count)
    if(NOT _gcc_cxx_count EQUAL 1)
        message(FATAL_ERROR "expected exactly one include/c++/<version>/cstddef under "
                            "${GCC_ARM_NONE_EABI_ROOT}, found ${_gcc_cxx_count}: ${GCC_ARM_NONE_EABI_INCLUDE}")
    endif()

    get_filename_component(GCC_ARM_NONE_EABI_INCLUDE "${GCC_ARM_NONE_EABI_INCLUDE}" DIRECTORY)
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
# and call __sync_* for the read-modify-writes only, which kvasir/Atomic/detail/arm_Common_atomic.hpp provides on the
# same lock as before. Measured 2026-10-05 (kvasir_work plans/binary_quality/RESULTS.md): M0+ release images 2.2 %
# smaller in sum, an atomic load 14 -> 0 cycles and Clock::now() 91 -> 58 on the RP2040. Part of target_flags, which
# becomes the CMAKE_<LANG>_FLAGS string: as a target option list CMake would drop the second -Xclang.
if(TARGET_ARCH STREQUAL "v6-m")
    list(APPEND target_flags -Xclang -target-feature -Xclang +atomics-32)
endif()

set(optimize_option_common -ggdb3 -flto -fwhole-program-vtables -fforce-emit-vtables)

# A/B switches for flag experiments (kvasir_work plans/binary_quality), given with -D when a tree is configured; unset
# or empty leaves every command line as it is. KVASIR_EXTRA_COMPILE_FLAGS: a ;-list added at the end of each
# optimisation set, so the runtime libraries get it too and a later -O wins. KVASIR_EXTRA_LINK_FLAGS: a ;-list added to
# the link. KVASIR_LTO_LEVEL: the digit of lld's --lto-O.
set(kvasir_lto_level 3)
if(NOT "${KVASIR_LTO_LEVEL}" STREQUAL "")
    set(kvasir_lto_level ${KVASIR_LTO_LEVEL})
endif()

# The stack protector per variant (dominic, 2026-10-06, plans/binary_quality F4): compiler_common.cmake's
# -fstack-protector-strong guards every function that takes a local's address, about 24 bytes a function (550 B in
# usb_playground's bulk, 860 B in water_mix, 1.3 KB in i2c_testing). The release sets take plain -fstack-protector (char
# arrays only; the later flag wins), debug keeps strong, sanitize puts strong back below. The libc and compiler-rt have
# no protector at all, their own flag lists say so.
set(optimize_option_speed ${optimize_option_common} -O3 -fstack-protector ${KVASIR_EXTRA_COMPILE_FLAGS})
set(optimize_option_size ${optimize_option_common} -Oz -fstack-protector ${KVASIR_EXTRA_COMPILE_FLAGS})
set(optimize_option_debug ${optimize_option_common} -Og ${KVASIR_EXTRA_COMPILE_FLAGS})

# Backend options of an optimisation set go to the LINKER: with -flto the code is generated inside lld, and an -mllvm
# given at compile time never reaches it (the speed set's -arm-promote-constant did nothing until 2026-10-05).
set(optimize_link_option_speed --mllvm=-arm-promote-constant=true)
set(optimize_link_option_size)
set(optimize_link_option_debug)

set(optimize_specs_speed ${SPEC_REPLACEMENT_EMPTY_MARKER})
set(optimize_specs_size "_nano")
set(optimize_specs_debug "_nano")

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
    set(_multilib_flags -mcpu=${TARGET_CPU} -mfloat-abi=${TARGET_FLOAT_ABI} -m${TARGET_ARM_INSTRUCTION_MODE})
    if(NOT TARGET_FPU MATCHES none)
        list(APPEND _multilib_flags -mfpu=${TARGET_FPU})
    endif()
    foreach(_lib libc_nano.a libgcc.a)
        execute_process(
            COMMAND ${arm-none-eabi-gcc} ${_multilib_flags} -print-file-name=${_lib}
            OUTPUT_VARIABLE _lib_path
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT IS_ABSOLUTE "${_lib_path}")
            message(FATAL_ERROR "arm-none-eabi-gcc has no ${_lib} for ${_multilib_flags}")
        endif()
        get_filename_component(_lib_dir "${_lib_path}" DIRECTORY)
        get_filename_component(_lib_dir "${_lib_dir}" REALPATH)
        list(APPEND linker_search_path "${_lib_dir}/")
    endforeach()
    list(REMOVE_DUPLICATES linker_search_path)
endif()

list(TRANSFORM linker_search_path PREPEND "--library-path=")

set(system_libs)

if("${CPPLIB}" STREQUAL "libstdc++")
    list(APPEND system_libs stdc++${SPEC_REPLACEMENT_STRING})
endif()

if("${CLIB}" STREQUAL "newlib")
    list(APPEND system_libs c${SPEC_REPLACEMENT_STRING})
    list(APPEND system_libs m)
endif()

if("${COMPILER_RT}" STREQUAL "gcc")
    list(APPEND system_libs gcc)
endif()

list(TRANSFORM system_libs PREPEND "--library=")

set(common_warning_flags
    -Weverything
    -Wno-switch-default
    -Wnan-infinity-disabled
    -Wno-c++98-compat
    -Wno-c++98-compat-pedantic
    -Wno-c++20-compat
    -Wno-pre-c2x-compat
    -Wno-pre-c11-compat
    -pedantic-errors
    -Wno-padded
    -Wno-covered-switch-default
    -Wno-switch-enum
    -Wno-class-varargs
    -Wno-gnu-zero-variadic-macro-arguments
    -Wno-float-equal
    -Wno-unknown-warning-option
    -Wno-reserved-identifier
    -ftemplate-backtrace-limit=0
    -fmacro-backtrace-limit=0
    -Wno-zero-as-null-pointer-constant # TODO remove
    -Wno-documentation-unknown-command
    -Wno-documentation
    -Wno-declaration-after-statement
    -Wno-braced-scalar-init
    -Wno-unsafe-buffer-usage
    -Wno-nrvo
    -Wno-unknown-warning-option)

# clang 23 brought -Wlifetime-safety-* into -Weverything; the two suggestion groups only propose
# [[clang::lifetimebound]] marks (hundreds, mostly in fetched fmt and llvm-libc). The checks that find dangling
# references stay on. Guarded: clang 22 does not know the names. Asked from the compiler: a toolchain file runs before
# CMAKE_CXX_COMPILER_VERSION is set.
execute_process(
    COMMAND ${clang++} -dumpversion
    OUTPUT_VARIABLE kvasir_clang_version
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(kvasir_clang_version VERSION_GREATER_EQUAL 23)
    list(APPEND common_warning_flags -Wno-lifetime-safety-intra-tu-suggestions
         -Wno-lifetime-safety-intra-tu-constructor-suggestions)
endif()

set(profile_flags)

# libstdc++ hides hosted headers under __STDC_HOSTED__=0 and clang, unlike gcc, refuses to redefine the macro, so
# -ffreestanding has to go; -Wno-main because StartUp.hpp declares main() extern "C" and takes its address on purpose
if("${CPPLIB}" STREQUAL "libstdc++")
    list(REMOVE_ITEM arm_compiler_common_flags -ffreestanding)
    list(APPEND common_warning_flags -Wno-main)
endif()
# Hosted application code (2026-10-06, plans/binary_quality): clang then copies a small struct with ldm/stm instead of
# calling memcpy, calls __aeabi_memcpy4 / __aeabi_memclr8 / __aeabi_memset8 for copies and fills of known alignment
# (direct entries in Kvasir's assembly memory functions, lib/libc/kvasir/arm/memory_v6m.S and memory_v7m.S - with
# compiler-rt's forwarders instead they cost 2 to 5 cycles more a copy), and uses what it knows of the C library
# (-fno-math-errno is already set). The runtime libraries keep -ffreestanding (and -fno-builtin) in their own flag
# lists, so no memcpy is built out of memcpy. -Wno-main: StartUp.hpp declares main() extern "C" and takes its address.
# __STDC_HOSTED__ stays 0: the C library has no FILE, and third-party headers read the macro as "stdio exists" (emio
# compiles its std::fseek file buffer under it, ambient_light_control 2026-10-06). Clang has no flag for "builtins yes,
# hosted no" - -fbuiltin after -ffreestanding changes nothing - so the macro is set by hand, which clang calls
# redefining a builtin macro.
list(REMOVE_ITEM arm_compiler_common_flags -ffreestanding)
list(APPEND arm_compiler_common_flags -U__STDC_HOSTED__ -D__STDC_HOSTED__=0)
list(APPEND common_warning_flags -Wno-main -Wno-builtin-macro-redefined)

if("${CPPLIB}" STREQUAL "libc++")
    list(APPEND profile_flags ${libcxx_profile_flags})
endif()

if("${CLIB}" STREQUAL "llvm")
    list(APPEND profile_flags -DLIBC_NAMESPACE=__llvm_libc)
endif()

if("${COMPILER_RT}" STREQUAL "gcc")
    # tells StartUp.hpp to provide the AEABI memory helpers, which libgcc does not have
    list(APPEND profile_flags -DKVASIR_COMPILER_RT_LIBGCC=1)
endif()

set(common_flags ${target_flags} ${common_warning_flags} ${profile_flags} ${system_includes} ${compiler_common_flags}
                 ${arm_compiler_common_flags})

set(cxx_flags ${common_flags} ${compiler_common_cxx_flags} -nostdinc++)

set(c_flags ${common_flags} ${compiler_common_c_flags} -nostdinc)

set(asm_flags ${common_flags} ${compiler_common_asm_flags})

# --whole-archive bracketed around one archive, for util.cmake's shared runtime archives
set(CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE_SUPPORTED TRUE)
set(CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE "${LINKER_PREFIX}--whole-archive" "<LINK_ITEM>"
                                                    "${LINKER_PREFIX}--no-whole-archive")
set(CMAKE_CXX_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE_SUPPORTED TRUE)
set(CMAKE_CXX_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE ${CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE})

set(linker_flags
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
    ${linker_search_path}
    ${system_libs})

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common_end.cmake)
