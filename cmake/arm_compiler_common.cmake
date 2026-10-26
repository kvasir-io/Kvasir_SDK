# What both Arm toolchain files share: a bare-metal system (Generic, arm), try_compile as a static library, no find in
# the host's roots, and the flags of a freestanding target and its runtime libraries.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
set(CMAKE_CROSSCOMPILING True)

set(arm_compiler_common_flags -ffreestanding)

# Each toolchain file sets per optimisation set <s> (size, speed, debug): optimize_option_<s> (compile),
# optimize_link_option_<s> (backend options of an LTO link) and linker_flags_<s> (the whole link: libraries, specs);
# util.cmake takes the three of the image's set.

# kvasir_gcc_sysroot(<gcc driver> <out root> <out c++ include>): arm-none-eabi-gcc's sysroot and its one
# include/c++/<version> directory (libstdc++'s headers), both real paths
function(kvasir_gcc_sysroot gcc out_root out_cxx_include)
    execute_process(
        COMMAND ${gcc} -print-sysroot
        OUTPUT_VARIABLE _root
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(_root "${_root}" REALPATH)
    file(GLOB_RECURSE _cstddef "${_root}/include/c++/*/cstddef")
    list(LENGTH _cstddef _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "expected exactly one include/c++/<version>/cstddef under ${_root}, found ${_count}: "
                            "${_cstddef}")
    endif()
    get_filename_component(_include "${_cstddef}" DIRECTORY)
    set(${out_root}
        ${_root}
        PARENT_SCOPE)
    set(${out_cxx_include}
        ${_include}
        PARENT_SCOPE)
endfunction()

# kvasir_gcc_multilib_dirs(<gcc driver> <out>): the directories of libc_nano.a and libgcc.a for the core (TARGET_*), as
# the gcc driver picks its multilib - for a link that is not done by the gcc driver (clang + newlib/libgcc)
function(kvasir_gcc_multilib_dirs gcc out)
    set(_flags -mcpu=${TARGET_CPU} -mfloat-abi=${TARGET_FLOAT_ABI} -m${TARGET_ARM_INSTRUCTION_MODE})
    if(NOT TARGET_FPU MATCHES none)
        list(APPEND _flags -mfpu=${TARGET_FPU})
    endif()
    set(_dirs)
    foreach(_lib libc_nano.a libgcc.a)
        execute_process(
            COMMAND ${gcc} ${_flags} -print-file-name=${_lib}
            OUTPUT_VARIABLE _path
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT IS_ABSOLUTE "${_path}")
            message(FATAL_ERROR "arm-none-eabi-gcc has no ${_lib} for ${_flags}")
        endif()
        get_filename_component(_dir "${_path}" DIRECTORY)
        get_filename_component(_dir "${_dir}" REALPATH)
        list(APPEND _dirs "${_dir}/")
    endforeach()
    list(REMOVE_DUPLICATES _dirs)
    set(${out}
        ${_dirs}
        PARENT_SCOPE)
endfunction()

# kvasir_whole_archive_feature(<linker prefix>): the link feature KVASIR_WHOLE_ARCHIVE, --whole-archive bracketed around
# one archive (util.cmake's shared runtime archives); a macro: the variables belong to the toolchain file's scope
macro(kvasir_whole_archive_feature prefix)
    set(CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE_SUPPORTED TRUE)
    set(CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE "${prefix}--whole-archive" "<LINK_ITEM>"
                                                        "${prefix}--no-whole-archive")
    set(CMAKE_CXX_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE_SUPPORTED TRUE)
    set(CMAKE_CXX_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE ${CMAKE_C_LINK_LIBRARY_USING_KVASIR_WHOLE_ARCHIVE})
endmacro()

# Which C library libc++ sits on: llvm-libc's own types (mbstate_t from llvm-libc-types) and, once localization or wide
# characters are switched on, its locale and wchar support headers. Upstream's libc++ build sets the same macro from
# RUNTIMES_USE_LIBC. 0 in a try-compile, where CLIB is not passed on.
set(libcxx_on_llvm_libc 0)
if("${CLIB}" STREQUAL "llvm")
    set(libcxx_on_llvm_libc 1)
endif()

# the vendored libc++'s configuration, shared by arm_clang.cmake and arm_gcc.cmake so the two cannot drift
set(libcxx_profile_flags
    -D_LIBCPP_ABI_VERSION=2
    -D_LIBCPP_ABI_NAMESPACE=__2
    -D_LIBCPP_HAS_MONOTONIC_CLOCK=1
    -D_LIBCPP_HAS_THREADS=0
    -D_LIBCPP_HAS_FILESYSTEM=0
    -D_LIBCPP_HAS_RANDOM_DEVICE=0
    -D_LIBCPP_HAS_LOCALIZATION=0
    -D_LIBCPP_HAS_UNICODE=0
    -D_LIBCPP_HAS_WIDE_CHARACTERS=0
    -D_LIBCPP_HARDENING_MODE_DEFAULT=_LIBCPP_HARDENING_MODE_FAST
    -D_LIBCPP_ASSERTION_SEMANTIC_DEFAULT=_LIBCPP_ASSERTION_SEMANTIC_HARDENING_DEPENDENT
    -D_LIBCPP_HAS_TIME_ZONE_DATABASE=0
    # upstream's __config_site always names a PSTL backend; the library build (and -fexperimental-library) needs one
    -D_LIBCPP_PSTL_BACKEND_SERIAL
    -D_LIBCPP_LIBC_NEWLIB=0
    -D_LIBCPP_LIBC_PICOLIBC=0
    -D_LIBCPP_LIBC_LLVM_LIBC=${libcxx_on_llvm_libc})
