# clang-tidy while compiling (USE_TIDY=ON): kvasir_add_tidy_flags(<target> <variant>) sets CXX_CLANG_TIDY on the images
# of one variant (USE_TIDY_VARIANT, default debug) to the SDK's configuration file plus the firmware-only checks of
# tools/tidy_firmware_checks.txt - the file and list tools/tidy.py uses. Findings are warnings in the build output of
# the objects that are rebuilt. This is a convenience for the target being worked on, in a tree configured for it; the
# gate is tools/tidy.py (`just tidy`): it sees every unit, drops findings of excluded headers by their primary location,
# deduplicates and attributes them. cmake_helpers' target_add_tidy_flags(<target> <scope>) is another function: the host
# builds of the same configure (uc_log, svd_converter) load it, so the names stay apart.

set(USE_TIDY
    OFF
    CACHE BOOL "run clang-tidy on the images of USE_TIDY_VARIANT while compiling them")
set(USE_TIDY_VARIANT
    "debug"
    CACHE STRING "the image variant USE_TIDY tidies")
get_filename_component(_kvasir_tidy_config "${CMAKE_CURRENT_LIST_DIR}/../.clang-tidy" ABSOLUTE)
set(TIDY_CONFIG_FILE
    "${_kvasir_tidy_config}"
    CACHE FILEPATH "the clang-tidy configuration USE_TIDY passes with --config-file")

if(USE_TIDY)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "USE_TIDY needs a clang tree: clang-tidy cannot read a ${CMAKE_CXX_COMPILER_ID} "
                            "command line (it would take the host's standard library)")
    endif()
    find_program(
        CLANG_TIDY_EXE
        NAMES "clang-tidy"
        DOC "Path to clang-tidy executable")
    mark_as_advanced(FORCE CLANG_TIDY_EXE)
    if(NOT CLANG_TIDY_EXE)
        message(FATAL_ERROR "USE_TIDY: clang-tidy not found")
    endif()
    if(NOT EXISTS "${TIDY_CONFIG_FILE}")
        message(FATAL_ERROR "USE_TIDY: no clang-tidy configuration at ${TIDY_CONFIG_FILE}")
    endif()
    set(_kvasir_tidy_firmware_list "${CMAKE_CURRENT_LIST_DIR}/../tools/tidy_firmware_checks.txt")
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${_kvasir_tidy_firmware_list}")
    file(STRINGS "${_kvasir_tidy_firmware_list}" _kvasir_tidy_lines)
    set(_kvasir_tidy_firmware_checks)
    foreach(_line IN LISTS _kvasir_tidy_lines)
        string(REGEX REPLACE "#.*$" "" _line "${_line}")
        string(STRIP "${_line}" _line)
        if(_line)
            list(APPEND _kvasir_tidy_firmware_checks "${_line}")
        endif()
    endforeach()
    list(JOIN _kvasir_tidy_firmware_checks "," _kvasir_tidy_firmware_checks)
    set(KVASIR_CLANG_TIDY_CXX "${CLANG_TIDY_EXE}" "--config-file=${TIDY_CONFIG_FILE}" "--quiet"
                              "--checks=${_kvasir_tidy_firmware_checks}")
endif()

function(kvasir_add_tidy_flags target variant)
    if(USE_TIDY AND variant STREQUAL USE_TIDY_VARIANT)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${KVASIR_CLANG_TIDY_CXX}")
    endif()
endfunction()
