# The SDK's libc++ sources (lib/libcxx/libcxx.cmake) compiled as a firmware tree compiles them: the real toolchain file
# for COMPILE_TARGET, a Cortex-M33 with an FPU (core_cortex_m33/cmake/core.cmake), every release and the sanitize flags.
# test_runtime_lib_flags.py configures and builds it once per toolchain and C library and fails on any diagnostic.
cmake_minimum_required(VERSION 3.28)

set(kvasir_cmake_dir ${CMAKE_CURRENT_LIST_DIR}/../../cmake)
set(TARGET_CPU cortex-m33)
set(TARGET_FLOAT_ABI hard)
set(TARGET_FPU fpv5-sp-d16)
set(TARGET_ARCH v8-m.main+fp+dsp)
set(TARGET_ARM_INSTRUCTION_MODE thumb)
set(TARGET_ENDIAN little-endian)
set(TARGET_TRIPLE thumbv8-m.main-none-eabihf)
set(CPPLIB libc++)
set(CMAKE_TOOLCHAIN_FILE ${kvasir_cmake_dir}/${COMPILE_TARGET}.cmake)

project(runtime_lib_flags CXX)

include(${kvasir_cmake_dir}/../lib/libcxx/libcxx.cmake)

# one object library per flag set util.cmake gives the shared runtime archives (target_add_shared_kvasir_lib)
foreach(variant IN ITEMS size speed debug)
    add_library(libcxx_${variant} OBJECT ${LIBCXX_SOURCE_FILES})
    target_compile_options(libcxx_${variant} PRIVATE ${optimize_option_${variant}})
endforeach()
add_library(libcxx_sanitize OBJECT ${LIBCXX_SOURCE_FILES})
target_compile_options(libcxx_sanitize PRIVATE ${optimize_option_size} ${sanitize_option})

# the compiler and the whole command line a libc++ source gets, for the probes test_runtime_lib_flags.py compiles by
# hand
file(WRITE ${CMAKE_BINARY_DIR}/libcxx_flags.txt "${CMAKE_CXX_COMPILER}\n${CMAKE_CXX_FLAGS} ${LIBCXX_FLAGS}\n")
