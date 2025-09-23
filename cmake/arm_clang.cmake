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
        COMMAND arm-none-eabi-gcc -print-sysroot
        OUTPUT_VARIABLE GCC_ARM_NONE_EABI_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    execute_process(
        COMMAND arm-none-eabi-gcc -print-search-dirs
        OUTPUT_VARIABLE GCC_ARM_NONE_EABI_LIB_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "^install: ([^\n\r ]*)" GCC_ARM_NONE_EABI_LIB_DIR ${GCC_ARM_NONE_EABI_LIB_DIR})
    string(REGEX REPLACE "^install: " "" GCC_ARM_NONE_EABI_LIB_DIR ${GCC_ARM_NONE_EABI_LIB_DIR})

    get_filename_component(GCC_ARM_NONE_EABI_ROOT "${GCC_ARM_NONE_EABI_ROOT}" REALPATH)

    file(GLOB_RECURSE GCC_ARM_NONE_EABI_INCLUDE "${GCC_ARM_NONE_EABI_ROOT}/include/c++/*/cstddef")

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

set(CMAKE_C_LINK_EXECUTABLE "${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/two_stage_link.py ${CMAKE_SIZE} \
<CMAKE_LINKER> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_CXX_LINK_EXECUTABLE "${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/two_stage_link.py ${CMAKE_SIZE} \
<CMAKE_LINKER> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

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

set(optimize_option_common -ggdb3 -flto -fwhole-program-vtables -fforce-emit-vtables)

set(optimize_option_speed ${optimize_option_common} -O3 -mllvm -arm-promote-constant=true)
set(optimize_option_size ${optimize_option_common} -Oz)
set(optimize_option_debug ${optimize_option_common} -Og)

set(optimize_specs_speed ${SPEC_REPLACEMENT_EMPTY_MARKER})
set(optimize_specs_size "_nano")
set(optimize_specs_debug "_nano")

set(sanitize_option
    -fsanitize=alignment
    -fsanitize=bool
    -fsanitize=builtin
    -fsanitize=bounds
    -fsanitize=enum
    -fsanitize=float-cast-overflow
    -fsanitize=float-divide-by-zero
    -fsanitize=implicit-unsigned-integer-truncation
    -fsanitize=implicit-signed-integer-truncation
    -fsanitize=implicit-unsigned-integer-truncation
    -fsanitize=implicit-signed-integer-truncation
    -fsanitize=implicit-integer-sign-change
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
    -fsanitize=unsigned-shift-base
    -fsanitize=signed-integer-overflow
    -fsanitize=unreachable
    -fsanitize=unsigned-integer-overflow
    -fsanitize=vla-bound
    # groups
    -fsanitize=undefined
    -fsanitize=implicit-integer-truncation
    -fsanitize=implicit-integer-arithmetic-value-change
    # -fsanitize=implicit-conversion -fsanitize=integer
    -fsanitize=nullability
    # runtime
    -fsanitize-minimal-runtime
    -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)

set(system_includes)

if("${CPPLIB}" STREQUAL "libstdc++")
    list(APPEND system_includes ${GCC_ARM_NONE_EABI_INCLUDE} ${GCC_ARM_NONE_EABI_INCLUDE}/arm-none-eabi
         ${GCC_ARM_NONE_EABI_INCLUDE}/backward ${GCC_ARM_NONE_EABI_ROOT}/include)
else()
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libcxx/include)
    list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libcxx/src)

    if("${CLIB}" STREQUAL "newlib")
        list(APPEND system_includes ${GCC_ARM_NONE_EABI_ROOT}/include)
    else()
        list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libc/include)
        list(APPEND system_includes ${kvasir_cmake_dir}/../lib/libc)
    endif()
endif()

list(TRANSFORM system_includes PREPEND "-isystem")

set(linker_search_path)

if("${CPPLIB}" STREQUAL "libstdc++"
   OR "${COMPILER_RT}" STREQUAL "gcc"
   OR "${CLIB}" STREQUAL "newlib")
    if(TARGET_FLOAT_ABI MATCHES soft)
        set(fp_folder nofp)
        set(fp_postfix)
    else()
        set(fp_folder hard)
        if(TARGET_FPU MATCHES sp)
            set(fp_postfix +fp)
        else()
            set(fp_postfix +dp)
        endif()
    endif()
    list(APPEND linker_search_path
         ${GCC_ARM_NONE_EABI_LIB_DIR}${TARGET_ARM_INSTRUCTION_MODE}/${TARGET_ARCH}${fp_postfix}/${fp_folder}/
         ${GCC_ARM_NONE_EABI_ROOT}/lib/${TARGET_ARM_INSTRUCTION_MODE}/${TARGET_ARCH}${fp_postfix}/${fp_folder}/)
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
    -Wno-unsafe-buffer-usage)

set(profile_flags)

if("${CPPLIB}" STREQUAL "libc++")
    list(APPEND profile_flags -D_LIBCPP_ABI_VERSION=2 -D_LIBCPP_HARDENING_MODE_DEFAULT=_LIBCPP_HARDENING_MODE_FAST)
endif()

if("${CLIB}" STREQUAL "llvm")
    list(APPEND profile_flags -DLIBC_NAMESPACE=__llvm_libc)
endif()

set(common_flags ${target_flags} ${common_warning_flags} ${profile_flags} ${system_includes} ${compiler_common_flags}
                 ${arm_compiler_common_flags})

set(cxx_flags ${common_flags} ${compiler_common_cxx_flags} -nostdinc++)

set(c_flags ${common_flags} ${compiler_common_c_flags} -nostdinc)

set(asm_flags ${common_flags} ${compiler_common_asm_flags})

set(linker_flags
    ${linker_common_flags}
    -nostdlib
    --lto-O3
    --ignore-data-address-equality
    --ignore-function-address-equality
    --lto-whole-program-visibility
    --icf=all
    --no-allow-multiple-definition
    ${linker_search_path}
    ${system_libs})

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common_end.cmake)
