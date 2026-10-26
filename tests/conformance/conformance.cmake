# The shared CMake of the chip and core conformance trees (chip_<x>/tests/conformance, core_cortex_<x>/tests/
# conformance). A conformance tree is a firmware-shaped project: the chip package's own chip.cmake and core.cmake run,
# with them every svd_convert() of the chip and the core SVD (and so the write-only guard), and the TUs compile with
# exactly the flags a firmware TU gets, for the target. Building the tree is the test: a missing contract row, a header
# that does not compile, or a write-only field a register write would read back is a compile error.
#
# Use, in a package's tests/conformance/CMakeLists.txt (KVASIR_ROOT and CHIP_ROOT set before):
#
# include(${KVASIR_ROOT}/tests/conformance/conformance.cmake) kvasir_conformance_begin()                 # before
# project(): kvasir.cmake project(chip_x_conformance CXX C ASM) include(${KVASIR_ROOT}/cmake/util.cmake)
# kvasir_conformance_project(NAME chip_x SOURCES conformance.cpp INSTANTIATE instantiate.cpp CONFIG_HEADER
# kvasir_config.hpp HEADERS <dir>/*.hpp ... NOT_ALONE not_alone.txt)
#
# kvasir_conformance_project(): NAME          the object library's name SOURCES       the contract TU(s): the config
# header, the chip, the rows (ChipContract.hpp / CoreContract.hpp) INSTANTIATE   TU(s) that instantiate the dependent
# register writes of the drivers (never-called functions), so every dependent apply() is checked against the write-only
# rules (Register::Semantics, Register/Diagnostic.hpp) CONFIG_HEADER the image config header as a fixture
# (kvasir_config.hpp: log backend, log clock, chip variant): each header TU includes it HEADERS       globs of public
# headers; each one gets a TU of its own: the config header, then that header alone NOT_ALONE     a file listing headers
# (file names, `#` comments) that do not compile alone yet, one reason per line
#
# MAIN_TU       (chip trees, optional) the chip's main-TU fixture: kvasir/StartUp/StartUp.hpp (or SecondaryCore.hpp)
# FIRST, then the chip, a Startup instantiated - the late hooks compiled in the order a main may choose; without it a
# generated TU (the chip, then StartUp.hpp, nothing instantiated) MAIN_TU_DEFINITIONS definitions of the main and the
# second TU (KVASIR_MULTICORE=1: the RP2040's cross-core lock in the atomic shim)
#
# The one-TU rule (chip trees): the main TU and a second TU (chip/chip.hpp + chip/drivers.hpp) are linked into one
# relocatable object in `all` - a strong definition in a chip or SDK header is a duplicate symbol there - and ctest
# <name>_two_main_tus links the main TU with a copy of itself and passes only on the guard's duplicate
# (kvasir_StartUp_hpp_is_included_by_two_TUs_of_one_image, StartUp/Runtime/All.hpp).
#
# kvasir_conformance_must_fail() (after the project): a must-fail set compiled with the same flags, run by ctest in the
# tree (described at the function).
#
# The flags come from a reference executable that is configured like a firmware image (target_configure_kvasir VARIANT
# debug: the log and the check levels of a debug image) and never built: the object library takes its compile options,
# definitions and include directories, and the libraries it links except the runtime archives (libc, libc++, compiler-rt
# are not built).

set(KVASIR_CONFORMANCE_DIR
    ${CMAKE_CURRENT_LIST_DIR}
    CACHE INTERNAL "")

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/kvasir_locate.cmake)

# kvasir_core_conformance(PAIRED_CHIP <repository> [PAIRED_CHIP_SUBMODULES <path>...] [DEVICES]): a core's tree
# (core_cortex_<x>/tests/conformance), before kvasir_conformance_begin(). CORE_ROOT is the core checkout;
# kvasir_locate() finds the SDK's submodules (a fetched SDK), the paired chip (a real chip of this core: its SVD, linker
# script and sources; PAIRED_CHIP_SUBMODULES its submodules but `core` when fetched) and kvasir_devices (DEVICES);
# CHIP_ROOT is <build>/paired_chip, the paired chip with this checkout linked in as its `core`.
function(kvasir_core_conformance)
    cmake_parse_arguments(PARSE_ARGV 0 A "DEVICES" "PAIRED_CHIP" "PAIRED_CHIP_SUBMODULES")
    if(A_UNPARSED_ARGUMENTS OR NOT A_PAIRED_CHIP)
        message(FATAL_ERROR "kvasir_core_conformance: PAIRED_CHIP <repository> [PAIRED_CHIP_SUBMODULES <path>...] "
                            "[DEVICES], got ${ARGN}")
    endif()
    get_filename_component(_core "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set(_more)
    if(DEFINED A_PAIRED_CHIP_SUBMODULES)
        list(APPEND _more PAIRED_CHIP_SUBMODULES ${A_PAIRED_CHIP_SUBMODULES})
    endif()
    if(A_DEVICES)
        list(APPEND _more DEVICES)
    endif()
    kvasir_locate(SDK_SUBMODULES ALL PAIRED_CHIP ${A_PAIRED_CHIP} ${_more})

    set(_chip "${CMAKE_BINARY_DIR}/paired_chip")
    file(MAKE_DIRECTORY ${_chip})
    foreach(_entry cmake linker src chip.svd)
        file(CREATE_LINK ${PAIRED_CHIP_ROOT}/${_entry} ${_chip}/${_entry} SYMBOLIC)
    endforeach()
    file(CREATE_LINK ${_core} ${_chip}/core SYMBOLIC)

    set(CORE_ROOT
        "${_core}"
        PARENT_SCOPE)
    set(PAIRED_CHIP_ROOT
        "${PAIRED_CHIP_ROOT}"
        PARENT_SCOPE)
    set(CHIP_ROOT
        "${_chip}"
        PARENT_SCOPE)
    if(A_DEVICES)
        set(KVASIR_DEVICES_ROOT
            "${KVASIR_DEVICES_ROOT}"
            PARENT_SCOPE)
    endif()
endfunction()

# before project(). The write-only profile (D19: guard error, registers derived, mask on) is svd_convert()'s default,
# and kvasir_conformance_project() requires that no svd_convert() of the tree weakened it (SVD_CONVERT_WEAKENED).
macro(kvasir_conformance_begin)
    # nothing is flashed or logged from here: no host build of the printer (and none of its dependencies fetched), and
    # the reference executable's flash scripts name J-Link Commander without a J-Link installed (jlink.cmake finds it
    # REQUIRED; a found one stays in the cache)
    set(jlinkexe
        JLinkExe
        CACHE FILEPATH "J-Link Commander (never run by a conformance tree)")
    set(UC_LOG_BUILD_PRINTER
        OFF
        CACHE BOOL "build the uc_log_printer host tool" FORCE)
    include(${KVASIR_ROOT}/cmake/kvasir.cmake)
endmacro()

# A macro, so that enable_testing() takes effect (the ctest of the one-TU rule; see kvasir_conformance_must_fail)
macro(kvasir_conformance_project)
    enable_testing()
    _kvasir_conformance_project(${ARGN})
endmacro()

function(_kvasir_conformance_project)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "NAME;CONFIG_HEADER;NOT_ALONE;MAIN_TU"
                          "SOURCES;INSTANTIATE;HEADERS;MAIN_TU_DEFINITIONS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "kvasir_conformance_project: unknown argument ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    # every chip and core SVD converts with the profile: a package that weakens it is a conformance failure
    get_property(_weakened GLOBAL PROPERTY SVD_CONVERT_WEAKENED)
    if(_weakened)
        message(FATAL_ERROR "kvasir_conformance_project: svd_convert() below Kvasir's write semantics: ${_weakened}")
    endif()
    if(NOT ARG_NAME OR NOT ARG_SOURCES)
        message(FATAL_ERROR "kvasir_conformance_project: NAME and SOURCES are required")
    endif()

    set(_ref ${ARG_NAME}_reference)
    add_executable(${_ref} EXCLUDE_FROM_ALL ${KVASIR_CONFORMANCE_DIR}/reference_main.cpp)
    # the config header reaches every TU as a firmware's does: uc_log/uc_log.hpp reads it (UC_LOG_CONFIG_HEADER, copied
    # with the reference's definitions below)
    set(_config_header_arg)
    if(ARG_CONFIG_HEADER)
        get_filename_component(_config_header ${ARG_CONFIG_HEADER} ABSOLUTE)
        set(_config_header_arg CONFIG_HEADER ${_config_header})
    endif()
    # a debug image's log and check levels; size optimisation and no self-tests (nothing here runs them)
    target_configure_kvasir(
        ${_ref}
        VARIANT
        debug
        OPTIMIZATION_STRATEGY
        size
        SELFTEST_VARIANT
        none
        ${_config_header_arg})

    # one TU per public header: the config header, then the header
    set(_header_tus)
    set(_not_alone)
    if(ARG_NOT_ALONE)
        get_filename_component(_na ${ARG_NOT_ALONE} ABSOLUTE)
        file(STRINGS ${_na} _lines)
        foreach(_l IN LISTS _lines)
            string(REGEX REPLACE "#.*" "" _l "${_l}")
            string(STRIP "${_l}" _l)
            if(_l)
                string(REGEX REPLACE "[ \t].*" "" _l "${_l}")
                list(APPEND _not_alone ${_l})
            endif()
        endforeach()
        set_property(
            DIRECTORY
            APPEND
            PROPERTY CMAKE_CONFIGURE_DEPENDS ${_na})
    endif()
    if(ARG_HEADERS)
        if(NOT ARG_CONFIG_HEADER)
            message(FATAL_ERROR "kvasir_conformance_project: HEADERS needs CONFIG_HEADER")
        endif()
        file(GLOB _headers CONFIGURE_DEPENDS ${ARG_HEADERS})
        list(SORT _headers)
        foreach(_h IN LISTS _headers)
            get_filename_component(_hn ${_h} NAME)
            get_filename_component(_hd ${_h} DIRECTORY)
            get_filename_component(_hdn ${_hd} NAME)
            set(_key ${_hdn}/${_hn})
            if(_hn IN_LIST _not_alone OR _key IN_LIST _not_alone)
                continue()
            endif()
            string(MAKE_C_IDENTIFIER "${_key}" _id)
            set(_tu ${CMAKE_CURRENT_BINARY_DIR}/headers/${_id}.cpp)
            file(
                GENERATE
                OUTPUT ${_tu}
                CONTENT
                    "// generated by kvasir_conformance_project(): ${_key} alone, after the config header\n#include \"${_config_header}\"\n#include \"${_h}\"\n"
            )
            list(APPEND _header_tus ${_tu})
        endforeach()
    endif()

    # a chip tree: every SDK header before the chip (any include order works), the exceptions in sdk_first_not.txt
    if(DEFINED CHIP_ROOT AND EXISTS ${CHIP_ROOT}/src/chip/chip.hpp)
        _kvasir_conformance_sdk_first(_sdk_first_tus)
        list(APPEND _header_tus ${_sdk_first_tus})
    endif()

    add_library(${ARG_NAME} OBJECT ${ARG_SOURCES} ${ARG_INSTANTIATE} ${_header_tus})
    _kvasir_conformance_flags(${ARG_NAME} ${_ref})

    if(DEFINED CHIP_ROOT AND EXISTS ${CHIP_ROOT}/src/chip/chip.hpp)
        _kvasir_conformance_one_tu(${ARG_NAME} ${_ref} "${ARG_MAIN_TU}" "${ARG_MAIN_TU_DEFINITIONS}")
    endif()
endfunction()

# The one-TU rule (the head of this file): <name>_one_tu = main TU + second TU, linked relocatable by <name>_one_tu_link
# (in all); <name>_two_main_tus = the main TU twice, linked by the ctest of that name.
function(_kvasir_conformance_one_tu name ref main_tu defs)
    set(_dir ${CMAKE_CURRENT_BINARY_DIR}/one_tu)
    if(main_tu)
        get_filename_component(_main ${main_tu} ABSOLUTE)
    else()
        set(_main ${_dir}/main_tu.cpp)
        file(
            GENERATE
            OUTPUT ${_main}
            CONTENT
                "// generated by kvasir_conformance_project(): the image's main TU (no Startup instantiated)\n#include \"chip/chip.hpp\"\n#include \"kvasir/StartUp/StartUp.hpp\"\n"
        )
    endif()
    set(_second_drivers)
    if(EXISTS ${CHIP_ROOT}/src/chip/drivers.hpp)
        set(_second_drivers "#include \"chip/drivers.hpp\"\n")
    endif()
    file(
        GENERATE
        OUTPUT ${_dir}/second_tu.cpp
        CONTENT
            "// generated by kvasir_conformance_project(): a second TU of the image - every chip header but no Startup\n#include \"chip/chip.hpp\"\n${_second_drivers}"
    )
    file(
        GENERATE
        OUTPUT ${_dir}/main_tu_again.cpp
        CONTENT
            "// generated by kvasir_conformance_project(): the main TU a second time (must not link)\n#include \"${_main}\"\n"
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(_rlink ${CMAKE_LINKER} -r --error-limit=0)
    else()
        separate_arguments(_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
        set(_rlink ${CMAKE_CXX_COMPILER} ${_flags} -flto -nostdlib -r)
    endif()

    foreach(_kind one_tu two_main_tus)
        if(_kind STREQUAL "one_tu")
            set(_srcs ${_main} ${_dir}/second_tu.cpp)
        else()
            set(_srcs ${_main} ${_dir}/main_tu_again.cpp)
        endif()
        set(_lib ${name}_${_kind})
        add_library(${_lib} OBJECT EXCLUDE_FROM_ALL ${_srcs})
        _kvasir_conformance_flags(${_lib} ${ref})
        if(defs)
            target_compile_definitions(${_lib} PRIVATE ${defs})
        endif()
        set(_out ${_dir}/${_kind}.o)
        add_custom_command(
            OUTPUT ${_out}
            COMMAND ${_rlink} $<TARGET_OBJECTS:${_lib}> -o ${_out}
            DEPENDS ${_lib} $<TARGET_OBJECTS:${_lib}>
            COMMAND_EXPAND_LISTS
            COMMENT "one-TU rule: relocatable link of ${_kind}")
        if(_kind STREQUAL "one_tu")
            add_custom_target(${_lib}_link ALL DEPENDS ${_out})
        else()
            add_custom_target(${_lib}_link DEPENDS ${_out})
            add_test(NAME ${_lib} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${_lib}_link)
            set_tests_properties(
                ${_lib}
                PROPERTIES
                    PASS_REGULAR_EXPRESSION
                    "(duplicate symbol|multiple definition of)[^\n]*kvasir_StartUp_hpp_is_included_by_two_TUs_of_one_image"
                    RESOURCE_LOCK
                    conformance_build
                    LABELS
                    must_fail)
        endif()
    endforeach()
endfunction()

# One TU per SDK header (src/kvasir/**.hpp): that header first, then the chip's chip/chip.hpp. Every SDK header names
# the chip's pieces late (Port::LateT, a dependent call), so it may come before the chip in a firmware's TU; the config
# header comes in where the SDK header first includes uc_log, as in a firmware. sdk_first_not.txt lists the headers that
# cannot (path below src/kvasir, a reason per line).
function(_kvasir_conformance_sdk_first out)
    set(_not)
    set(_list ${KVASIR_CONFORMANCE_DIR}/sdk_first_not.txt)
    file(STRINGS ${_list} _lines)
    foreach(_l IN LISTS _lines)
        string(REGEX REPLACE "#.*" "" _l "${_l}")
        string(STRIP "${_l}" _l)
        if(_l)
            string(REGEX REPLACE "[ \t].*" "" _l "${_l}")
            list(APPEND _not ${_l})
        endif()
    endforeach()
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS ${_list})
    set(_sdk ${KVASIR_ROOT}/src/kvasir)
    file(
        GLOB_RECURSE _headers CONFIGURE_DEPENDS
        RELATIVE ${_sdk}
        ${_sdk}/*.hpp)
    list(SORT _headers)
    set(_tus)
    foreach(_key IN LISTS _headers)
        set(_skip FALSE)
        foreach(_n IN LISTS _not)
            # an entry ending in / names a directory
            if(_key STREQUAL _n OR (_n MATCHES "/$" AND _key MATCHES "^${_n}"))
                set(_skip TRUE)
            endif()
        endforeach()
        if(_skip)
            continue()
        endif()
        string(MAKE_C_IDENTIFIER "sdk_first/${_key}" _id)
        set(_tu ${CMAKE_CURRENT_BINARY_DIR}/sdk_first/${_id}.cpp)
        file(
            GENERATE
            OUTPUT ${_tu}
            CONTENT
                "// generated by kvasir_conformance_project(): kvasir/${_key} before the chip\n#include \"kvasir/${_key}\"\n#include \"chip/chip.hpp\"\n"
        )
        list(APPEND _tus ${_tu})
    endforeach()
    set(${out}
        ${_tus}
        PARENT_SCOPE)
endfunction()

# the reference executable's flags onto an object library of the tree
function(_kvasir_conformance_flags target ref)
    foreach(_p COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES)
        get_target_property(_v ${ref} ${_p})
        if(_v)
            set_property(
                TARGET ${target}
                APPEND
                PROPERTY ${_p} ${_v})
        endif()
    endforeach()
    get_target_property(_libs ${ref} LINK_LIBRARIES)
    # the runtime archives (target_add_shared_kvasir_lib: $<LINK_LIBRARY:KVASIR_WHOLE_ARCHIVE,...>) would be built for
    # nothing: no object of this library is linked. Not linked here, and out of `all` (they are created in it).
    foreach(_l IN LISTS _libs)
        if(_l MATCHES "^\\$<LINK_LIBRARY:[A-Z_]+,([^>]+)>$" AND TARGET ${CMAKE_MATCH_1})
            set_target_properties(${CMAKE_MATCH_1} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()
    list(FILTER _libs EXCLUDE REGEX "LINK_LIBRARY:")
    target_link_libraries(${target} PRIVATE ${_libs})
    # the chip's queued bus drivers and bus recovery include kvasir_devices (kvasir::bus, kvasir::devices for the
    # quantities): a firmware that uses them links kvasir::devices, the chip package only registers it (no target in a
    # package without kvasir_devices)
    if(TARGET kvasir::devices)
        target_link_libraries(${target} PRIVATE kvasir::devices)
    endif()
    target_include_directories(${target} PRIVATE ${KVASIR_CONFORMANCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    # a warning is a failure, as in a firmware's build-all (every TU here is a second TU of an image: no main(), no
    # Startup instantiated - what such a TU warns about, a firmware's would too)
    target_compile_options(${target} PRIVATE -Werror)
    add_dependencies(${target} cmake_git_version_check_repository)
endfunction()

# kvasir_conformance_must_fail(NAME <name> PROJECT <kvasir_conformance_project NAME> SOURCE <tu> DEFINE <macro> CASES
# "<n>|<regex>"...): a must-fail set with the tree's flags, for the target. <tu> with -D<macro>=0 is an object library
# in `all` (the control: it must compile); case <n> is an object library <name>_<n> out of `all` with -D<macro>=<n>, and
# ctest <name>_<n> builds it and passes only when the build output matches <regex> - the case's own diagnostic, so a
# case that fails for another reason does not pass. The ctests build in this tree: they hold one RESOURCE_LOCK. A macro,
# because enable_testing() has no effect inside a function (CMake 4.4) - so a macro's argument rules apply to the
# regexes: no backslash (CMake re-reads it as an escape), [(] [.] for a literal character.
macro(kvasir_conformance_must_fail)
    enable_testing()
    _kvasir_conformance_must_fail(${ARGN})
endmacro()

function(_kvasir_conformance_must_fail)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "NAME;PROJECT;SOURCE;DEFINE" "CASES")
    if(ARG_UNPARSED_ARGUMENTS
       OR NOT ARG_NAME
       OR NOT ARG_PROJECT
       OR NOT ARG_SOURCE
       OR NOT ARG_DEFINE
       OR NOT ARG_CASES)
        message(FATAL_ERROR "kvasir_conformance_must_fail: NAME, PROJECT, SOURCE, DEFINE and CASES are required")
    endif()
    if(NOT TARGET ${ARG_PROJECT}_reference)
        message(
            FATAL_ERROR "kvasir_conformance_must_fail: no kvasir_conformance_project(NAME ${ARG_PROJECT}) before it")
    endif()
    add_library(${ARG_NAME}_0 OBJECT ${ARG_SOURCE})
    _kvasir_conformance_flags(${ARG_NAME}_0 ${ARG_PROJECT}_reference)
    target_compile_definitions(${ARG_NAME}_0 PRIVATE ${ARG_DEFINE}=0)
    foreach(_case IN LISTS ARG_CASES)
        string(FIND "${_case}" "|" _bar)
        if(_bar LESS 1)
            message(FATAL_ERROR "kvasir_conformance_must_fail: case '${_case}' is not <n>|<regex>")
        endif()
        string(SUBSTRING "${_case}" 0 ${_bar} _n)
        math(EXPR _from "${_bar} + 1")
        string(SUBSTRING "${_case}" ${_from} -1 _expect)
        set(_t ${ARG_NAME}_${_n})
        add_library(${_t} OBJECT EXCLUDE_FROM_ALL ${ARG_SOURCE})
        _kvasir_conformance_flags(${_t} ${ARG_PROJECT}_reference)
        target_compile_definitions(${_t} PRIVATE ${ARG_DEFINE}=${_n})
        add_test(NAME ${_t} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${_t})
        set_tests_properties(${_t} PROPERTIES PASS_REGULAR_EXPRESSION "${_expect}" RESOURCE_LOCK conformance_build
                                              LABELS must_fail)
    endforeach()
endfunction()
