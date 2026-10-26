# Finds what a tree needs besides the SDK: the chip package, kvasir_devices, a core's paired chip, and the SDK's own
# submodules when the SDK was fetched. Included right after the SDK is found; every tree that is not part of the SDK
# finds the SDK with the same short block (a copy: nothing of the SDK is reachable before it is found):
#
# ~~~
#   # Kvasir_SDK: KVASIR_ROOT, else the checkout next to this repository, else fetched (its submodules and the other
#   # packages: kvasir_locate(), cmake/kvasir_locate.cmake)
#   if(NOT KVASIR_ROOT AND EXISTS "<repository root>/../Kvasir_SDK/cmake/kvasir_locate.cmake")
#       get_filename_component(KVASIR_ROOT "<repository root>/../Kvasir_SDK" ABSOLUTE)
#   elseif(NOT KVASIR_ROOT)
#       include(FetchContent)
#       FetchContent_Declare(kvasir_sdk GIT_REPOSITORY https://github.com/kvasir-io/Kvasir_SDK.git GIT_TAG master
#                            GIT_SHALLOW TRUE GIT_SUBMODULES "" SOURCE_SUBDIR fetch-only)
#       FetchContent_MakeAvailable(kvasir_sdk)
#       set(KVASIR_ROOT "${kvasir_sdk_SOURCE_DIR}")
#   endif()
#   include("${KVASIR_ROOT}/cmake/kvasir_locate.cmake")
# ~~~
#
# A firmware reads the environment first (the chip images set KVASIR_ROOT, CHIP_ROOT and KVASIR_DEVICES_ROOT): the block
# starts with `if(NOT KVASIR_ROOT AND DEFINED ENV{KVASIR_ROOT}) set(KVASIR_ROOT "$ENV{KVASIR_ROOT}") endif()` and calls
# kvasir_locate(USE_ENVIRONMENT ...). A test tree does not: a shell's KVASIR_ROOT must not decide what a lone copy
# tests.
#
# ~~~
# kvasir_locate([USE_ENVIRONMENT] [SDK_SUBMODULES ALL|<path>...] [CHIP <repository>] [DEVICES]
#               [PAIRED_CHIP <repository> [PAIRED_CHIP_SUBMODULES <path>...]])
# ~~~
#
# * SDK_SUBMODULES: what the tree needs of the SDK's submodules. Only a FETCHED SDK (KVASIR_ROOT is the FetchContent
#   clone, kvasir_sdk_SOURCE_DIR, without FETCHCONTENT_SOURCE_DIR_KVASIR_SDK) is touched: each path is initialised level
#   by level at the commit its parent records (`git submodule update --init --depth 1`), its last level with everything
#   below it (`uc_log/remote_fmt` = remote_fmt and string_constant, not uc_log's rtt or jlink); ALL = every submodule. A
#   checkout or a local copy is used as it is.
# * CHIP <repository> -> CHIP_ROOT, DEVICES -> KVASIR_DEVICES_ROOT (kvasir_devices), PAIRED_CHIP <repository> ->
#   PAIRED_CHIP_ROOT (a core's conformance tree): the variable if set, else the environment (USE_ENVIRONMENT only), else
#   the checkout next to the SDK, else fetched from github.com/kvasir-io/<repository> master, shallow, populated only
#   (SOURCE_SUBDIR fetch-only: util.cmake adds the packages after project(), with the target's toolchain).
#   FETCHCONTENT_SOURCE_DIR_<REPOSITORY> points a fetch at a local copy. A fetched chip has all its submodules, a paired
#   chip only PAIRED_CHIP_SUBMODULES (none without it: its core is the checkout under test).
#
# The roots are set in the caller's scope; each found root is printed.

include_guard(GLOBAL)
cmake_policy(SET CMP0174 NEW)

function(kvasir_locate)
    cmake_parse_arguments(PARSE_ARGV 0 A "USE_ENVIRONMENT;DEVICES" "CHIP;PAIRED_CHIP"
                          "SDK_SUBMODULES;PAIRED_CHIP_SUBMODULES")
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "kvasir_locate: unknown argument(s) ${A_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_k CHIP PAIRED_CHIP SDK_SUBMODULES)
        if(_k IN_LIST A_KEYWORDS_MISSING_VALUES)
            message(FATAL_ERROR "kvasir_locate: no value given for ${_k}")
        endif()
    endforeach()
    if(DEFINED A_PAIRED_CHIP_SUBMODULES AND NOT A_PAIRED_CHIP)
        message(FATAL_ERROR "kvasir_locate: PAIRED_CHIP_SUBMODULES without PAIRED_CHIP")
    endif()
    if(NOT KVASIR_ROOT OR NOT EXISTS "${KVASIR_ROOT}/cmake/kvasir_locate.cmake")
        message(FATAL_ERROR "kvasir_locate: KVASIR_ROOT '${KVASIR_ROOT}' is no Kvasir SDK")
    endif()

    if(DEFINED A_SDK_SUBMODULES)
        _kvasir_locate_sdk_submodules("${A_SDK_SUBMODULES}")
    endif()
    if(A_CHIP)
        _kvasir_locate_package(CHIP_ROOT ${A_CHIP} cmake/chip.cmake "${A_USE_ENVIRONMENT}" ALL)
        set(CHIP_ROOT
            "${CHIP_ROOT}"
            PARENT_SCOPE)
    endif()
    if(A_DEVICES)
        _kvasir_locate_package(KVASIR_DEVICES_ROOT kvasir_devices CMakeLists.txt "${A_USE_ENVIRONMENT}" ALL)
        set(KVASIR_DEVICES_ROOT
            "${KVASIR_DEVICES_ROOT}"
            PARENT_SCOPE)
    endif()
    if(A_PAIRED_CHIP)
        _kvasir_locate_package(PAIRED_CHIP_ROOT ${A_PAIRED_CHIP} cmake/chip.cmake "${A_USE_ENVIRONMENT}"
                               "${A_PAIRED_CHIP_SUBMODULES}")
        set(PAIRED_CHIP_ROOT
            "${PAIRED_CHIP_ROOT}"
            PARENT_SCOPE)
    endif()
endfunction()

# _kvasir_locate_package(<var> <repository> <marker file> <use environment> <submodules: ALL or a list, "" = none>)
function(_kvasir_locate_package var repo marker use_env submodules)
    get_filename_component(_next "${KVASIR_ROOT}/../${repo}" ABSOLUTE)
    if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
        set(_dir "${${var}}")
        set(_from "CMake variable")
    elseif(use_env AND DEFINED ENV{${var}})
        set(_dir "$ENV{${var}}")
        set(_from "environment")
    elseif(EXISTS "${_next}/${marker}")
        set(_dir "${_next}")
        set(_from "next to the SDK")
    else()
        include(FetchContent)
        set(_url https://github.com/kvasir-io/${repo}.git)
        if(submodules STREQUAL "ALL")
            FetchContent_Declare(
                ${repo}
                GIT_REPOSITORY ${_url}
                GIT_TAG master
                GIT_SHALLOW TRUE
                SOURCE_SUBDIR fetch-only)
        else()
            # quoted: "" (no submodule) must reach FetchContent as a value
            FetchContent_Declare(
                ${repo}
                GIT_REPOSITORY ${_url}
                GIT_TAG master
                GIT_SHALLOW TRUE
                GIT_SUBMODULES "${submodules}" SOURCE_SUBDIR fetch-only)
        endif()
        FetchContent_MakeAvailable(${repo})
        string(TOLOWER "${repo}" _lower)
        set(_dir "${${_lower}_SOURCE_DIR}")
        set(_from "fetched")
    endif()
    get_filename_component(_dir "${_dir}" ABSOLUTE)
    if(NOT EXISTS "${_dir}/${marker}")
        message(FATAL_ERROR "kvasir_locate: ${var} '${_dir}' (${_from}) has no ${marker}: not ${repo}")
    endif()
    message(STATUS "Kvasir: ${var}=${_dir} (${_from})")
    set(${var}
        "${_dir}"
        PARENT_SCOPE)
endfunction()

# The SDK's submodules a tree needs, initialised in a fetched SDK only (see the top)
function(_kvasir_locate_sdk_submodules paths)
    if(NOT DEFINED kvasir_sdk_SOURCE_DIR
       OR NOT KVASIR_ROOT STREQUAL kvasir_sdk_SOURCE_DIR
       OR FETCHCONTENT_SOURCE_DIR_KVASIR_SDK)
        return()
    endif()
    find_package(Git REQUIRED)
    set(_update submodule update --init --depth 1)
    if(paths STREQUAL "ALL")
        _kvasir_locate_git("${KVASIR_ROOT}" ${_update} --recursive)
        return()
    endif()
    foreach(_path IN LISTS paths)
        string(REPLACE "/" ";" _levels "${_path}")
        list(LENGTH _levels _n)
        set(_parent "${KVASIR_ROOT}")
        set(_i 0)
        foreach(_level IN LISTS _levels)
            math(EXPR _i "${_i} + 1")
            file(GLOB _populated "${_parent}/${_level}/*")
            if(_i EQUAL _n)
                # the last level with everything below it: also when it is there but a submodule of it is not
                _kvasir_locate_git("${_parent}" ${_update} --recursive -- ${_level})
            elseif(NOT _populated)
                _kvasir_locate_git("${_parent}" ${_update} -- ${_level})
            endif()
            set(_parent "${_parent}/${_level}")
        endforeach()
    endforeach()
endfunction()

function(_kvasir_locate_git dir)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${dir}" ${ARGN}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _out)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "kvasir_locate: git -C ${dir} ${ARGN} failed (${_rc}):\n${_out}")
    endif()
endfunction()
