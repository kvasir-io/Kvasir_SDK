# Kvasir Main Configuration Main entry point for Kvasir embedded development framework Configures toolchain selection,
# git submodules, and build options

set(kvasir_cmake_dir
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

# Detect paths from multiple sources - check each variable independently Priority: 1. CMake variable, 2. Environment
# variable, 3. Default/computed

# Determine KVASIR_ROOT
if(DEFINED KVASIR_ROOT)
    set(KVASIR_ROOT_DIR ${KVASIR_ROOT})
    set(KVASIR_ROOT_SOURCE "CMake variable")
elseif(DEFINED ENV{KVASIR_ROOT})
    set(KVASIR_ROOT_DIR $ENV{KVASIR_ROOT})
    set(KVASIR_ROOT_SOURCE "environment")
else()
    set(KVASIR_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
    set(KVASIR_ROOT_SOURCE "submodule mode")
endif()
get_filename_component(KVASIR_ROOT_DIR ${KVASIR_ROOT_DIR} ABSOLUTE)

# Determine CHIP_ROOT
if(DEFINED CHIP_ROOT)
    set(CHIP_ROOT_DIR ${CHIP_ROOT})
    set(CHIP_ROOT_SOURCE "CMake variable")
elseif(DEFINED ENV{CHIP_ROOT})
    set(CHIP_ROOT_DIR $ENV{CHIP_ROOT})
    set(CHIP_ROOT_SOURCE "environment")
else()
    # Compute from sibling relationship
    get_filename_component(KVASIR_PARENT_DIR ${KVASIR_ROOT_DIR} DIRECTORY)
    set(CHIP_ROOT_DIR "${KVASIR_PARENT_DIR}/chip")
    set(CHIP_ROOT_SOURCE "computed as sibling")
endif()
get_filename_component(CHIP_ROOT_DIR ${CHIP_ROOT_DIR} ABSOLUTE)

# Determine KVASIR_DEVICES_ROOT
if(DEFINED KVASIR_DEVICES_ROOT)
    set(KVASIR_DEVICES_ROOT_DIR ${KVASIR_DEVICES_ROOT})
    set(KVASIR_DEVICES_ROOT_SOURCE "CMake variable")
elseif(DEFINED ENV{KVASIR_DEVICES_ROOT})
    set(KVASIR_DEVICES_ROOT_DIR $ENV{KVASIR_DEVICES_ROOT})
    set(KVASIR_DEVICES_ROOT_SOURCE "environment")
else()
    # Compute from sibling relationship
    get_filename_component(KVASIR_PARENT_DIR ${KVASIR_ROOT_DIR} DIRECTORY)
    set(KVASIR_DEVICES_ROOT_DIR "${KVASIR_PARENT_DIR}/kvasir_devices")
    set(KVASIR_DEVICES_ROOT_SOURCE "computed as sibling")
endif()
get_filename_component(KVASIR_DEVICES_ROOT_DIR ${KVASIR_DEVICES_ROOT_DIR} ABSOLUTE)

# Report configuration
message(STATUS "Kvasir: KVASIR_ROOT=${KVASIR_ROOT_DIR} (from ${KVASIR_ROOT_SOURCE})")
message(STATUS "Kvasir: CHIP_ROOT=${CHIP_ROOT_DIR} (from ${CHIP_ROOT_SOURCE})")
message(STATUS "Kvasir: KVASIR_DEVICES_ROOT=${KVASIR_DEVICES_ROOT_DIR} (from ${KVASIR_DEVICES_ROOT_SOURCE})")

# Make these available globally
set(KVASIR_ROOT
    ${KVASIR_ROOT_DIR}
    CACHE INTERNAL "")
set(CHIP_ROOT
    ${CHIP_ROOT_DIR}
    CACHE INTERNAL "")
set(KVASIR_DEVICES_ROOT
    ${KVASIR_DEVICES_ROOT_DIR}
    CACHE INTERNAL "")

# Validate that CHIP_ROOT exists
if(NOT IS_DIRECTORY "${CHIP_ROOT}")
    message(FATAL_ERROR "CHIP_ROOT does not exist or is not a directory: ${CHIP_ROOT}")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/tidy.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cppcheck.cmake)

find_package(Git QUIET REQUIRED)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    option(GIT_SUBMODULE "Check submodules during build" ON)
    if(GIT_SUBMODULE)
        message(STATUS "Submodule update")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            RESULT_VARIABLE GIT_SUBMOD_RESULT)
        if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(
                FATAL_ERROR "git submodule update --init failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
        endif()
    endif()
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CROSSCOMPILING True)
add_subdirectory(${KVASIR_ROOT}/svd_converter ${CMAKE_BINARY_DIR}/kvasir_svd_converter)
add_subdirectory(${KVASIR_ROOT}/uc_log ${CMAKE_BINARY_DIR}/kvasir_uc_log)

include(${CHIP_ROOT}/cmake/chip.cmake)

set(COMPILE_TARGET
    arm_clang
    CACHE STRING "Choose the compile target, options are: arm_clang arm_gcc clang gcc")
set_property(CACHE COMPILE_TARGET PROPERTY STRINGS arm_clang arm_gcc)

set(SWD_SPEED
    ""
    CACHE STRING "Choose the SWD speed for jlink empty for maxspeed")

set(JLINK_IP
    ""
    CACHE STRING "Choose the ip address of jlink if not using USB")

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
    set(CPPLIB libstdc++)
    set(CLIB newlib)
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
