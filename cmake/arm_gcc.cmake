# ARM GCC Compiler Configuration Configures the build environment for ARM targets using GCC toolchain Uses
# arm-none-eabi-gcc cross-compilation tools

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
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP arm-none-eabi-objdump)

find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)

set(CMAKE_C_LINK_EXECUTABLE
    "${Python3_EXECUTABLE} -X pycache_prefix=<CMAKE_BINARY_DIR>/__pycache__ ${kvasir_cmake_dir}/tools/two_stage_link.py ${CMAKE_SIZE} \
<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_CXX_LINK_EXECUTABLE
    "${Python3_EXECUTABLE} -X pycache_prefix=<CMAKE_BINARY_DIR>/__pycache__ ${kvasir_cmake_dir}/tools/two_stage_link.py ${CMAKE_SIZE} \
<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

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

set(optimize_option_speed ${optimize_option_common} -Ofast)
set(optimize_option_size ${optimize_option_common} -Os)
set(optimize_option_debug ${optimize_option_common} -Og)

set(optimize_specs_speed "nosys")
set(optimize_specs_size "nano")
set(optimize_specs_debug "nano")

set(common_warning_flags
    -Wall
    -Wextra
    # errors, matching arm_clang.cmake: the ISO-conformance issues this catches (the extern "C" main declaration, the
    # __STDC_HOSTED__ redefine this flag was once disabled for) must fail the build, not scroll past as warnings
    -pedantic-errors
    -Wdouble-promotion
    -Wcast-align
    -Wcast-qual
    -Wdisabled-optimization
    -Wformat=2
    -Winit-self
    -Wlogical-op
    -Wredundant-decls
    -Wshadow
    # level 5 is too noisy: it fires inside libc++'s to_chars and on the KVASIR_START vector read
    -Wstrict-overflow=2
    -Wundef
    -Wno-unused
    # gcc, unlike clang, fires this for the unnameable base-class subobject of designated-initialized aggregates
    -Wno-missing-field-initializers
    -Wstrict-aliasing=1)

# gcc has no minimal UBSan runtime; kvasir/Util/ubsan.hpp defines the full-ABI __ubsan_handle_* functions. Compile-only:
# never on the link line, so gcc does not add its libubsan. _GLIBCXX_ASSERTIONS is libstdc++'s counterpart of libc++'s
# debug hardening mode (reported through std::__glibcxx_assert_fail, defined in StartUp.hpp).
set(sanitize_option -fsanitize=undefined -fsanitize=bounds-strict -D_GLIBCXX_ASSERTIONS)
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
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-sysroot
            OUTPUT_VARIABLE _gcc_sysroot
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        get_filename_component(_gcc_sysroot "${_gcc_sysroot}" REALPATH)
        file(GLOB_RECURSE _gcc_cxx_cstddef "${_gcc_sysroot}/include/c++/*/cstddef")
        list(LENGTH _gcc_cxx_cstddef _gcc_cxx_count)
        if(NOT _gcc_cxx_count EQUAL 1)
            message(FATAL_ERROR "expected exactly one include/c++/<version>/cstddef under "
                                "${_gcc_sysroot}, found ${_gcc_cxx_count}: ${_gcc_cxx_cstddef}")
        endif()
        get_filename_component(_gcc_cxx_include "${_gcc_cxx_cstddef}" DIRECTORY)
        list(APPEND system_includes ${_gcc_cxx_include}/arm-none-eabi ${_gcc_cxx_include} ${_gcc_cxx_include}/backward)
    endif()
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libc/include ${kvasir_cmake_dir}/../lib/libc)
    list(APPEND profile_flags -DLIBC_NAMESPACE=__llvm_libc)
    list(APPEND nostd_c_flags -nostdinc)
    list(APPEND nostd_cxx_flags -nostdinc)
endif()
list(TRANSFORM system_includes PREPEND "-isystem")

set(common_flags ${target_flags} ${common_warning_flags} ${profile_flags} ${system_includes} ${compiler_common_flags}
                 ${arm_compiler_common_flags})

set(cxx_flags ${common_flags} ${compiler_common_cxx_flags} ${nostd_cxx_flags} -Woverloaded-virtual -Wsign-promo
              -Wstrict-null-sentinel)

set(c_flags ${common_flags} ${compiler_common_c_flags} ${nostd_c_flags})

set(LINKER_PREFIX "-Wl,")

# --whole-archive bracketed around one archive, for util.cmake's shared runtime archives
set(CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE_SUPPORTED TRUE)
set(CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE "${LINKER_PREFIX}--whole-archive" "<LINK_ITEM>"
                                                    "${LINKER_PREFIX}--no-whole-archive")
set(CMAKE_CXX_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE_SUPPORTED TRUE)
set(CMAKE_CXX_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE ${CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE})

list(TRANSFORM linker_common_flags PREPEND ${LINKER_PREFIX})

if("${CLIB}" STREQUAL "llvm")
    # no specs file: the driver must not add newlib; libgcc is still wanted
    set(linker_flags ${linker_common_flags} -nostdlib)
    # standard libraries land after the objects, where GNU ld needs -lgcc to be
    set(CMAKE_C_STANDARD_LIBRARIES "-lgcc")
    set(CMAKE_CXX_STANDARD_LIBRARIES "-lgcc")
    set(optimize_specs_speed ${SPEC_REPLACEMENT_EMPTY_MARKER})
    set(optimize_specs_size ${SPEC_REPLACEMENT_EMPTY_MARKER})
    set(optimize_specs_debug ${SPEC_REPLACEMENT_EMPTY_MARKER})
else()
    set(linker_flags ${linker_common_flags} -nostartfiles --specs=${SPEC_REPLACEMENT_STRING}.specs -Wl,--start-group
                     -Wl,--end-group)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common_end.cmake)
