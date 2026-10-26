# The toolchain file of COMPILE_TARGET arm_gcc: arm-none-eabi-gcc and its binutils for the chip package's core
# (TARGET_*, kvasir_core()), libstdc++ and newlib by default (CPPLIB, CLIB), libgcc.

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/arm_compiler_common.cmake)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_SIZE arm-none-eabi-size)
set(CMAKE_STRIP arm-none-eabi-strip)
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_NM arm-none-eabi-nm)
set(CMAKE_RANLIB arm-none-eabi-ranlib)
# llvm-objcopy when it is installed, as in the clang trees: GNU objcopy warns "empty loadable segment detected" for
# every <target>_flash.elf it cuts out of an image with an .eeprom segment (the segments cmake/tools/elf_image.py
# removes right after), and a warning fails a build.
find_program(KVASIR_LLVM_OBJCOPY llvm-objcopy)
if(KVASIR_LLVM_OBJCOPY)
    set(CMAKE_OBJCOPY llvm-objcopy)
else()
    set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
endif()
set(CMAKE_OBJDUMP arm-none-eabi-objdump)

find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)

set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

if(TARGET_FPU MATCHES none)
    set(target_fpu auto)
else()
    set(target_fpu ${TARGET_FPU})
endif()

# NOT -fno-short-enums: newlib, libgcc and libstdc++ are built for AAPCS variable-size enums; pin every enum that is
# serialised or shared instead
set(target_flags -mfloat-abi=${TARGET_FLOAT_ABI} -mfpu=${target_fpu} -mcpu=${TARGET_CPU} -march=arm${TARGET_ARCH}
                 -m${TARGET_ARM_INSTRUCTION_MODE} -m${TARGET_ENDIAN})

# =auto: plain -flto warns "using serial compilation of N LTRANS jobs" on every link
set(optimize_option_common -ggdb3 -flto=auto)

# plain -fstack-protector in the release sets, as arm_clang.cmake. speed is -O3 as in arm_clang.cmake, not -Ofast: gcc's
# -Ofast is -O3 plus the fast-math set (-ffinite-math-only folds an isnan/isfinite check to a constant, reassociation,
# no signed zeros, excess precision fast) and -fallow-store-data-races (a store the source does not make, seen by the
# other core or an ISR) - neither is wanted from a speed set, and a gcc image must compute what the clang one computes
set(optimize_option_speed ${optimize_option_common} -O3 -fstack-protector)
set(optimize_option_size ${optimize_option_common} -Os -fstack-protector)
set(optimize_option_debug ${optimize_option_common} -Og)

# The warning list, shared with host test trees: kvasir::warnings carries it into the image's own code (util.cmake),
# with three C++-only ones; the runtime libraries compile with the compiler's defaults
include(${CMAKE_CURRENT_LIST_DIR}/kvasir_warnings.cmake)
set(kvasir_image_warning_flags
    ${kvasir_gcc_warning_flags} -Wno-unused-macros $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsign-promo> $<$<COMPILE_LANGUAGE:CXX>:-Wstrict-null-sentinel>)
# gcc's LTO link runs the middle end again, and its warning options (-Wextra, -Wstrict-overflow=2, -Wshadow, ...) reach
# lto1 from the link line only (lto-wrapper takes no -W option from the objects): the image's link gets the list too
set(kvasir_image_link_warning_flags ${kvasir_gcc_warning_flags} -Wno-unused-macros -Woverloaded-virtual -Wsign-promo
                                    -Wstrict-null-sentinel)

# gcc has no minimal UBSan runtime; kvasir/Runtime/ubsan.hpp defines the full-ABI __ubsan_handle_* functions.
# Compile-only: never on the link line, so gcc does not add its libubsan. _GLIBCXX_ASSERTIONS is libstdc++'s counterpart
# of libc++'s debug hardening mode (reported through std::__glibcxx_assert_fail, defined in StartUp/Runtime/Libc.hpp).
#
# gcc's null-pointer checks stay on: address 0 is readable on these chips (the boot ROM on the RP chips - ROM_BASE,
# RP2040 datasheet md l.1238, RP2350 md l.1439 -, the flash on SAM D21/C21 - "Internal Flash 0x00000000", SAM D21
# datasheet md l.1507), so nothing else catches a null dereference. Their price is in constant evaluation: with them gcc
# cannot evaluate "a pointer into a namespace-scope object or a template argument != nullptr" ("... != 0 is not a
# constant expression", in the sanitize variant only). Code that is evaluated at compile time therefore looks things up
# by INDEX, not by pointer, and does not call std::string_view::find / contains on namespace-scope text
# (std::ranges::search instead).
set(sanitize_option -fsanitize=undefined -fsanitize=bounds-strict -fstack-protector-strong -D_GLIBCXX_ASSERTIONS)
if("${CPPLIB}" STREQUAL "libc++")
    # gcc's null-check wrappers inside the arguments of uc_log's constexpr log() make the call immediate-escalating with
    # libc++'s tuple/time_point in the signature; a gcc front-end limitation, scoped to exactly this combination
    list(APPEND sanitize_option -fno-sanitize=null,nonnull-attribute,returns-nonnull-attribute)
endif()

# libstdc++ needs __STDC_HOSTED__=1; drop -ffreestanding instead of redefining the built-in macro, which gcc diagnoses
# with an un-silenceable warning (fatal under -Werror)
list(REMOVE_ITEM arm_compiler_common_flags -ffreestanding)

set(profile_flags)

# vendored libc++ / llvm-libc on gcc: same configuration as arm_clang.cmake, the toolchain's own headers hidden with
# -nostdinc++/-nostdinc; only libgcc still comes from the toolchain
set(system_includes)
set(nostd_cxx_flags)
set(nostd_c_flags)
if("${CPPLIB}" STREQUAL "libc++")
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libcxx/include ${kvasir_cmake_dir}/../lib/libcxx/src)
    list(APPEND profile_flags ${libcxx_profile_flags})
    list(APPEND nostd_cxx_flags -nostdinc++)
endif()
if("${CLIB}" STREQUAL "llvm")
    if("${CPPLIB}" STREQUAL "libstdc++")
        # -nostdinc also drops gcc's own C++ directories; put libstdc++'s back by hand, in driver order
        kvasir_gcc_sysroot(${CMAKE_CXX_COMPILER} _gcc_sysroot _gcc_cxx_include)
        list(APPEND system_includes ${_gcc_cxx_include}/arm-none-eabi ${_gcc_cxx_include} ${_gcc_cxx_include}/backward)
    endif()
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libc/include ${kvasir_cmake_dir}/../lib/libc)
    list(APPEND profile_flags -DLIBC_NAMESPACE=__llvm_libc)
    list(APPEND nostd_c_flags -nostdinc)
    list(APPEND nostd_cxx_flags -nostdinc)
endif()
list(TRANSFORM system_includes PREPEND "-isystem")

# C++26 gives an uninitialised local an erroneous value: gcc initialises it (.DEFERRED_INIT), and at -Og an 8-byte one
# is a memset call - in a RAM function a call into flash (an apply() reading two registers). Locals stay uninitialised
# as in C++23 and as clang leaves them; reading one is still an error the sanitizers and warnings look for.
set(auto_var_init_flags -ftrivial-auto-var-init=uninitialized)

set(common_flags ${target_flags} ${profile_flags} ${system_includes} ${compiler_common_flags}
                 ${arm_compiler_common_flags} ${auto_var_init_flags})

set(cxx_flags ${common_flags} ${compiler_common_cxx_flags} ${nostd_cxx_flags})

set(c_flags ${common_flags} ${compiler_common_c_flags} ${nostd_c_flags})

# as arm_clang.cmake: without it every .S was assembled for the driver's default target (no -mcpu/-mfloat-abi), so
# __ARM_PCS_VFP and the core's feature macros were missing in hard-float trees
set(asm_flags ${common_flags} ${compiler_common_asm_flags})

set(LINKER_PREFIX "-Wl,")

kvasir_whole_archive_feature("${LINKER_PREFIX}")

# GNU ld warns "LOAD segment with RWX permissions": RAM functions next to the data are wanted
list(APPEND linker_common_flags --no-warn-rwx-segments)

list(TRANSFORM linker_common_flags PREPEND ${LINKER_PREFIX})

# the link per optimisation set (cmake/arm_compiler_common.cmake)
set(optimize_link_option_speed)
set(optimize_link_option_size)
set(optimize_link_option_debug)
if("${CLIB}" STREQUAL "llvm")
    # no specs file: the driver must not add newlib; libgcc is still wanted
    foreach(_set speed size debug)
        set(linker_flags_${_set} ${linker_common_flags} -nostdlib)
    endforeach()
    # standard libraries land after the objects, where GNU ld needs -lgcc to be
    set(CMAKE_C_STANDARD_LIBRARIES "-lgcc")
    set(CMAKE_CXX_STANDARD_LIBRARIES "-lgcc")
else()
    # newlib through gcc's specs: nano (newlib-nano, libstdc++ nano) for size and debug, nosys for speed
    set(linker_flags_speed ${linker_common_flags} -nostartfiles --specs=nosys.specs)
    set(linker_flags_size ${linker_common_flags} -nostartfiles --specs=nano.specs)
    set(linker_flags_debug ${linker_common_flags} -nostartfiles --specs=nano.specs)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common_end.cmake)
