# Kvasir's entry point, included by a firmware before project(): the roots of the SDK, the chip package and the packages
# next to them, the chip package (kvasir_chip()), the toolchain file (COMPILE_TARGET) and the build options. util.cmake,
# included after project(), makes the images.

# cmake_parse_arguments(PARSE_ARGV) defines a one-value keyword given "" (as empty; OLD leaves it undefined, and warns).
# Set at the top of the SDK's two entry files, this one and util.cmake: an include() has a policy scope of its own, and
# a function runs with the policies of the file that defined it.
cmake_policy(SET CMP0174 NEW)

set(kvasir_cmake_dir
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

# the package contract: kvasir_resolve_root, kvasir_add_package, kvasir_core, kvasir_chip
include(${CMAKE_CURRENT_LIST_DIR}/kvasir_chip.cmake)

# KVASIR_ROOT is the SDK this file belongs to; CHIP_ROOT, the chip package, has no default
get_filename_component(KVASIR_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(KVASIR_ROOT
    "${KVASIR_ROOT}"
    CACHE INTERNAL "" FORCE)
message(STATUS "Kvasir: KVASIR_ROOT=${KVASIR_ROOT}")
kvasir_resolve_root(CHIP_ROOT)

# A tree of sibling checkouts can hand FetchContent dependencies (aglio, which uc_log fetches) its own checkouts: a
# kvasir_fetch_links.cmake next to the SDK sets FETCHCONTENT_SOURCE_DIR_<NAME> for them (a workspace's link step writes
# it, its unlink empties it). Found with a glob so that a file that appears or goes re-runs cmake.
get_filename_component(_kvasir_tree_dir "${KVASIR_ROOT}" DIRECTORY)
file(GLOB _kvasir_fetch_links CONFIGURE_DEPENDS "${_kvasir_tree_dir}/kvasir_fetch_links.cmake")
if(_kvasir_fetch_links)
    include("${_kvasir_fetch_links}")
endif()

# Validate that CHIP_ROOT exists
if(NOT IS_DIRECTORY "${CHIP_ROOT}")
    message(FATAL_ERROR "CHIP_ROOT does not exist or is not a directory: ${CHIP_ROOT}")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cppcheck.cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CROSSCOMPILING True)

add_subdirectory(${KVASIR_ROOT}/svd_converter ${CMAKE_BINARY_DIR}/kvasir_svd_converter)
# the chip package calls kvasir_chip() (and its core.cmake kvasir_core()): cmake/kvasir_chip.cmake, included above; its
# SVD, part and generated part SVD go through cmake/chip_package.cmake
include(${CMAKE_CURRENT_LIST_DIR}/chip_package.cmake)
include(${CHIP_ROOT}/cmake/chip.cmake)
if(NOT TARGET kvasir_chip)
    message(FATAL_ERROR "Kvasir: ${CHIP_ROOT}/cmake/chip.cmake did not call kvasir_chip() (cmake/kvasir_chip.cmake)")
endif()

set(COMPILE_TARGET
    arm_clang
    CACHE STRING "Choose the compile target, options are: arm_clang arm_gcc")
set_property(CACHE COMPILE_TARGET PROPERTY STRINGS arm_clang arm_gcc)

set(SWD_SPEED
    ""
    CACHE STRING "Choose the SWD speed for jlink empty for maxspeed")

set(JLINK_IP
    ""
    CACHE STRING "Choose the ip address of jlink if not using USB")

set(JLINK_PROBE
    ""
    CACHE
        STRING
        "Which J-Link on USB: serial number or nickname (the USB line of the JLinkExe command files, uc_log_printer --probe). Empty takes the only one connected; with two connected the printer refuses to start and JLinkExe cannot connect"
)

set(DUPLEX_BASE_PORT
    ""
    CACHE STRING "Choose the first tcp port for uc_log duplex channels, empty for default")

set(UC_LOG_TRANSPORT
    ""
    CACHE STRING "uc_log_printer control and duplex transport: unix (default) or tcp")

if(COMPILE_TARGET STREQUAL arm_clang)
    set(CPPLIB
        libc++
        CACHE STRING "Choose the stdlib to use, options are: libc++ libstdc++")
    set_property(CACHE CPPLIB PROPERTY STRINGS libc++ libstdc++)

    set(CLIB
        llvm
        CACHE STRING "Choose libc to use, options are: llvm newlib")
    set_property(CACHE CLIB PROPERTY STRINGS llvm newlib)

    set(COMPILER_RT
        clang
        CACHE STRING "Choose compiler runtime to use, options are: clang gcc")
    set_property(CACHE COMPILER_RT PROPERTY STRINGS clang gcc)
else()
    # arm_gcc: same two library switches, defaulting to gcc's own; the runtime stays libgcc
    set(CPPLIB
        libstdc++
        CACHE STRING "Choose the stdlib to use, options are: libc++ libstdc++")
    set_property(CACHE CPPLIB PROPERTY STRINGS libc++ libstdc++)

    set(CLIB
        newlib
        CACHE STRING "Choose libc to use, options are: llvm newlib")
    set_property(CACHE CLIB PROPERTY STRINGS llvm newlib)

    set(COMPILER_RT gcc)
endif()

if(COMPILE_TARGET STREQUAL arm_clang)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/arm_clang.cmake)
elseif(COMPILE_TARGET STREQUAL arm_gcc)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/arm_gcc.cmake)
else()
    message(FATAL_ERROR "no COMPILE_TARGET specified!!!!")
endif()

mark_as_advanced(FORCE FETCHCONTENT_BASE_DIR)
mark_as_advanced(FORCE FETCHCONTENT_FULLY_DISCONNECTED)
mark_as_advanced(FORCE FETCHCONTENT_QUIET)
mark_as_advanced(FORCE FETCHCONTENT_UPDATES_DISCONNECTED)
