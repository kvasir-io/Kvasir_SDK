# What a firmware includes after project(): the image targets (target_configure_kvasir, kvasir_executable_variants and
# the variant table), their link and post-link steps (tools/kvasir_postlink.py), the J-Link and printer targets.

# as at the top of kvasir.cmake, for the functions defined from here on
cmake_policy(SET CMP0174 NEW)

include(${KVASIR_ROOT}/cmake_git_version/CMakeLists.txt)

include(${kvasir_cmake_dir}/kvasir_interface.cmake)

add_subdirectory(${KVASIR_ROOT}/uc_log ${CMAKE_BINARY_DIR}/kvasir_uc_log)

# glaze (uc_log -> aglio -> glaze) puts -Wno-missing-braces on its INTERFACE compile options, and an INTERFACE option
# reaches every consumer: the image's own code was compiled with -Wmissing-braces off. glaze's headers are SYSTEM
# includes already, so its own code needs no option.
kvasir_quarantine_interface(glaze_glaze STRIP_OPTIONS "-Wno-missing-braces")

find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)

include(${kvasir_cmake_dir}/jlink.cmake)

# kvasir::base, kvasir::sdk (with uc_log, added above), kvasir::test_support
include(${kvasir_cmake_dir}/kvasir_sdk_targets.cmake)

# kvasir::warnings: the warning list of Kvasir code (kvasir_warnings.cmake, as the toolchain file completes it:
# kvasir_image_warning_flags). Not in CMAKE_<LANG>_FLAGS: the runtime libraries and fetched dependencies compile with
# the compiler's defaults. target_configure_kvasir puts its options into the image's own COMPILE_OPTIONS, first, so a
# firmware's later -W<name> still overrides a -Wno-<name> of the list and uc_log's catalog TU (it compiles with the
# image's COMPILE_OPTIONS) gets them too; other code links it. gcc also takes them on the image's link
# (kvasir_image_link_warning_flags: its LTO-time warnings), clang has none there.
add_library(kvasir_warnings INTERFACE)
add_library(kvasir::warnings ALIAS kvasir_warnings)
target_compile_options(kvasir_warnings INTERFACE ${kvasir_image_warning_flags})
if(kvasir_image_link_warning_flags)
    target_link_options(kvasir_warnings INTERFACE ${kvasir_image_link_warning_flags})
endif()

# kvasir_chip()'s LINKER_OPTIONS are bare (`--wrap=...`): the gcc driver needs them behind -Wl, (LINKER_PREFIX, which
# the toolchain file sets inside project(); clang links with ld.lld directly, the prefix is empty there)
kvasir_chip_get(_kvasir_chip_linker_options LINKER_OPTIONS)
if(_kvasir_chip_linker_options)
    list(TRANSFORM _kvasir_chip_linker_options PREPEND "${LINKER_PREFIX}")
    target_link_options(kvasir_chip INTERFACE ${_kvasir_chip_linker_options})
endif()

# packages registered by the chip package (kvasir_add_package)
get_property(_kvasir_packages GLOBAL PROPERTY KVASIR_PACKAGES)
foreach(_package IN LISTS _kvasir_packages)
    string(REPLACE "|" ";" _package "${_package}")
    list(GET _package 0 _source)
    list(GET _package 1 _binary)
    list(GET _package 2 _guard)
    if(TARGET ${_guard})
        continue()
    endif()
    if(NOT EXISTS "${_source}/CMakeLists.txt")
        message(FATAL_ERROR "Kvasir: the chip package needs ${_binary}, but ${_source} has no CMakeLists.txt")
    endif()
    add_subdirectory(${_source} ${CMAKE_BINARY_DIR}/${_binary})
    message(STATUS "Kvasir: added ${_binary} from ${_source}")
endforeach()

include(${kvasir_cmake_dir}/kvasir_variants.cmake)

# kvasir_constexpr_budget(<steps> [<target>...]): how much compile-time evaluation the compiler may do - for
# static_asserts that simulate a plant or measure text (gfx's checked literals). <steps> is clang's count
# (-fconstexpr-steps, default 1 048 576); gcc counts operations (-fconstexpr-ops-limit, default 33 554 432) and needs
# more of them for the same work - one firmware: 30 M clang steps, between 34 M and 268 M gcc operations - so it gets 8
# x <steps>, never less than its default. With targets: on those; without: add_compile_options for the directory.
function(kvasir_constexpr_budget steps)
    math(EXPR _ops "${steps} * 8")
    if(_ops LESS 33554432)
        set(_ops 33554432)
    endif()
    set(_flags $<$<CXX_COMPILER_ID:Clang>:-fconstexpr-steps=${steps}>
               $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=${_ops}>)
    if(ARGN)
        foreach(_target IN LISTS ARGN)
            target_compile_options(${_target} PRIVATE ${_flags})
        endforeach()
    else()
        add_compile_options(${_flags})
    endif()
endfunction()

enable_language(ASM)

include(${kvasir_cmake_dir}/../lib/compiler-rt/compiler-rt.cmake)
include(${kvasir_cmake_dir}/../lib/compiler-rt/profile.cmake)
include(${kvasir_cmake_dir}/../lib/libcxx/libcxx.cmake)
include(${kvasir_cmake_dir}/../lib/libc/libc.cmake)

if("${CPPLIB}" STREQUAL "libstdc++")
    if("${CLIB}" STREQUAL "llvm")
        # libstdc++'s headers are configured against newlib and reach for its internals (ctype table, wide-character
        # API, errno values llvm-libc does not define)
        message(FATAL_ERROR "libstdc++ does not work with llvm libc")
    endif()
endif()

include(${kvasir_cmake_dir}/kvasir_runtime_archives.cmake)

# target_add_shared_kvasir_lib(<image> <lib> <sources> <strategy> <sanitize flags>): links the runtime archive
# kvasir_<lib>_<strategy>[_san] (cmake/kvasir_runtime_archives.cmake), made by the first image that needs it with the
# optimisation set's flags (optimize_option_<strategy>) and the sanitize flags
function(target_add_shared_kvasir_lib target name sources strategy san_flags)
    kvasir_runtime_archive_name(_shared ${name} ${strategy} "${san_flags}")
    set(_flags ${optimize_option_${strategy}} ${san_flags})
    kvasir_runtime_archive_claim(${_shared} "${_flags}" ${target})
    if(${_shared}_NEW)
        add_library("${_shared}" ${sources})
        target_compile_options("${_shared}" PUBLIC ${_flags})
    endif()

    # --whole-archive bracketed around this archive only: a bare --whole-archive link option stays in force for every
    # archive after it (KVASIR_WHOLE_ARCHIVE is defined in the toolchain files)
    target_link_libraries(${target} "$<LINK_LIBRARY:KVASIR_WHOLE_ARCHIVE,${_shared}>")
endfunction()

# Every file the link or a post-link step reads: an edit to one relinks the image (POST_BUILD commands have no
# dependencies of their own). The linker file of the image is added per target.
set(KVASIR_LINK_INPUTS
    ${kvasir_cmake_dir}/../linker/common.ld
    ${kvasir_cmake_dir}/../linker/common_flash.ld
    ${kvasir_cmake_dir}/../linker/common_ram.ld
    ${kvasir_cmake_dir}/../linker/common_ram_only.ld
    ${kvasir_cmake_dir}/../linker/common_eeprom.ld
    ${kvasir_cmake_dir}/../linker/common_text_body.inc.ld
    ${kvasir_cmake_dir}/../linker/common_vectors_body.inc.ld
    ${kvasir_cmake_dir}/../linker/common_data_body.inc.ld
    ${kvasir_cmake_dir}/../linker/common_ramExec.inc.ld
    ${kvasir_cmake_dir}/../linker/common_bss_body.inc.ld
    ${kvasir_cmake_dir}/../linker/common_noInit_body.inc.ld
    ${kvasir_cmake_dir}/../linker/common_noInitLowRam_body.inc.ld
    ${kvasir_cmake_dir}/../linker/common_stack_body.inc.ld
    ${kvasir_cmake_dir}/../linker/common_stack1_body.inc.ld
    ${kvasir_cmake_dir}/../linker/common_heap_body.inc.ld
    ${kvasir_cmake_dir}/tools/kvasir_postlink.py
    ${kvasir_cmake_dir}/tools/kvasir_elf.py
    ${kvasir_cmake_dir}/tools/linker_utils.py
    ${kvasir_cmake_dir}/tools/find_undefined_refs.py
    ${kvasir_cmake_dir}/tools/check_ram_funcs.py
    ${kvasir_cmake_dir}/tools/check_reset_entry.py
    ${kvasir_cmake_dir}/tools/patch_image_crc.py
    ${kvasir_cmake_dir}/tools/pretty_size.py
    ${kvasir_cmake_dir}/tools/elf_image.py
    ${kvasir_cmake_dir}/tools/beautify_lst.py)

# The image's post-link pipeline (cmake/tools/kvasir_postlink.py), one POST_BUILD command: the RAM function, reset entry
# and undefined-symbol checks, IMAGE_CRC's descriptor (before every artefact, so each one carries it), the flash
# artefacts from the load view (<target>_flash.{bin,hex,uf2,elf}, _eeprom.{bin,hex,uf2}, _eeprom_flash.{hex,uf2}), the
# listing, the memory table. A failed step removes the .elf, so the next build fails again instead of passing on a stale
# image. kvasir_chip(IMAGE_CRC_EXCLUDE): pairs of symbols bounding bytes a later step rewrites (the RP2350's picobin
# block); kvasir_chip(UF2_FAMILY): the .uf2 artefacts, none without it. Registered before the J-Link and uc_log steps,
# which read its artefacts.
function(_kvasir_postlink target linker_file image_crc)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(_args --linker-file "${linker_file}" --objcopy ${CMAKE_OBJCOPY} --objdump ${CMAKE_OBJDUMP})
    if(image_crc)
        list(APPEND _args --image-crc)
        kvasir_chip_get(_pair IMAGE_CRC_EXCLUDE) # pairs: kvasir_chip() checked
        while(_pair)
            list(POP_FRONT _pair _start _end)
            list(APPEND _args --exclude ${_start} ${_end})
        endwhile()
    endif()
    set(_byproducts)
    foreach(
        _suffix
        _flash.bin
        _flash.hex
        _flash.elf
        _eeprom.bin
        _eeprom.hex
        _eeprom_flash.hex
        .lst)
        list(APPEND _byproducts "${_out}${_suffix}")
    endforeach()
    kvasir_chip_get(_uf2_family UF2_FAMILY)
    if(NOT _uf2_family STREQUAL "")
        list(APPEND _args --uf2-family ${_uf2_family})
        list(APPEND _byproducts "${_out}_flash.uf2" "${_out}_eeprom.uf2" "${_out}_eeprom_flash.uf2")
    endif()
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
                ${kvasir_cmake_dir}/tools/kvasir_postlink.py "${_out}.elf" ${_args} --delete-on-failure
        BYPRODUCTS ${_byproducts}
        COMMENT "Post-link steps of ${target}.elf"
        VERBATIM)
endfunction()

# kvasir_executable_variants(<base> SOURCES <src>... [LIBRARIES <lib>...] [VARIANTS <v>...] [OPTIMIZATION <o>]
# [DEBUG_OPTIMIZATION <o>] [OUT_TARGETS <var>] [COVERAGE [COVERAGE_MODE bytes|counts] [COVERAGE_LIST <file>]]
# [<target_configure_kvasir keyword>...]) One image per variant (default: debug release release_log sanitize), named
# <base>_<variant> (<variant> for base ""), each `add_executable` + `target_configure_kvasir(... VARIANT <variant>)` +
# LIBRARIES. OPTIMIZATION (size) is every variant's but debug's, DEBUG_OPTIMIZATION (debug) debug's. Every keyword of
# target_configure_kvasir but VARIANT and OPTIMIZATION_STRATEGY is forwarded to each (KVASIR_CONFIGURE_*). OUT_TARGETS
# names the variable that gets the targets made. A per-variant define goes `target_compile_definitions(<t> PRIVATE ...)`
# after the call.
#
# COVERAGE: one more image, <base>_coverage: release_log's image with clang's source-coverage instrumentation on the
# firmware's own translation units, the bare-metal profile runtime (lib/compiler-rt/profile.cmake) and KVASIR_COVERAGE=1
# for Kvasir::Coverage (kvasir/Util/Coverage.hpp). COVERAGE_MODE `bytes` (default: one byte a region, covered or not,
# race-free) or `counts` (64-bit counters, for PGO); COVERAGE_LIST a -fprofile-list file restricting what is
# instrumented. clang only; never with RAM_ONLY; not in ALL, built when named. The runtime libraries are release_log's
# own archives, uninstrumented.
function(kvasir_executable_variants base)
    cmake_parse_arguments(
        PARSE_ARGV 1 V "COVERAGE;${KVASIR_CONFIGURE_OPTIONS}"
        "OPTIMIZATION;DEBUG_OPTIMIZATION;COVERAGE_MODE;COVERAGE_LIST;OUT_TARGETS;${KVASIR_CONFIGURE_ONE_VALUE}"
        "SOURCES;LIBRARIES;VARIANTS;${KVASIR_CONFIGURE_MULTI_VALUE}")
    if(V_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${V_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT V_SOURCES)
        message(FATAL_ERROR "SOURCES argument is required")
    endif()
    if(NOT V_OPTIMIZATION)
        set(V_OPTIMIZATION size)
    endif()
    if(NOT V_DEBUG_OPTIMIZATION)
        set(V_DEBUG_OPTIMIZATION debug)
    endif()
    if(NOT V_VARIANTS)
        set(V_VARIANTS ${KVASIR_VARIANTS})
    endif()
    foreach(_v IN LISTS V_VARIANTS)
        if(NOT _v IN_LIST KVASIR_VARIANTS)
            message(FATAL_ERROR "${base}: VARIANTS ${_v}: one of ${KVASIR_VARIANTS} (coverage: COVERAGE)")
        endif()
    endforeach()
    kvasir_forward_arguments(_common V "${KVASIR_CONFIGURE_OPTIONS}" "${KVASIR_CONFIGURE_ONE_VALUE}"
                             "${KVASIR_CONFIGURE_MULTI_VALUE}")

    set(_made)
    foreach(_v IN LISTS V_VARIANTS)
        if(base STREQUAL "")
            set(_t ${_v})
        else()
            set(_t ${base}_${_v})
        endif()
        kvasir_variant_row(${_v} _row)
        set(_exclude)
        if(_row_EXCLUDE)
            set(_exclude EXCLUDE_FROM_ALL)
        endif()
        set(_opt ${V_OPTIMIZATION})
        if(_v STREQUAL "debug")
            set(_opt ${V_DEBUG_OPTIMIZATION})
        endif()
        add_executable(${_t} ${_exclude} ${V_SOURCES})
        target_configure_kvasir(${_t} VARIANT ${_v} OPTIMIZATION_STRATEGY ${_opt} ${_common})
        if(V_LIBRARIES)
            target_link_libraries(${_t} ${V_LIBRARIES})
        endif()
        list(APPEND _made ${_t})
    endforeach()

    if(V_COVERAGE)
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(STATUS "${base}: no coverage variant with ${CMAKE_CXX_COMPILER_ID} (clang's format only)")
        elseif(V_RAM_ONLY)
            message(FATAL_ERROR "${base}: COVERAGE and RAM_ONLY together: a RAM image has no room for it")
        else()
            if(NOT V_COVERAGE_MODE)
                set(V_COVERAGE_MODE bytes)
            endif()
            if(NOT DEFINED coverage_option_${V_COVERAGE_MODE})
                message(FATAL_ERROR "COVERAGE_MODE ${V_COVERAGE_MODE}: bytes or counts")
            endif()
            if(base STREQUAL "")
                set(_t coverage)
            else()
                set(_t ${base}_coverage)
            endif()
            add_executable(${_t} EXCLUDE_FROM_ALL ${V_SOURCES})
            target_configure_kvasir(${_t} VARIANT coverage OPTIMIZATION_STRATEGY ${V_OPTIMIZATION} ${_common})
            # after the configure call: the string-constants TU (its COMPILE_OPTIONS snapshot) stays uninstrumented
            target_compile_options(${_t} PRIVATE ${coverage_option_${V_COVERAGE_MODE}})
            if(V_COVERAGE_LIST)
                get_filename_component(_list "${V_COVERAGE_LIST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
                target_compile_options(${_t} PRIVATE "-fprofile-list=${_list}")
                set_property(
                    SOURCE ${V_SOURCES}
                    APPEND
                    PROPERTY OBJECT_DEPENDS "${_list}")
            endif()
            target_add_shared_kvasir_lib(${_t} prof "${COMPILER_RT_PROFILE_SOURCE_FILES}" ${V_OPTIMIZATION} "")
            if(V_LIBRARIES)
                target_link_libraries(${_t} ${V_LIBRARIES})
            endif()
            list(APPEND _made ${_t})
        endif()
    endif()

    if(V_OUT_TARGETS)
        set(${V_OUT_TARGETS}
            ${_made}
            PARENT_SCOPE)
    endif()
endfunction()

# target_configure_kvasir(<target> VARIANT <debug|release|release_log|sanitize|coverage> [OPTIMIZATION_STRATEGY <o>]
# [<keyword>...]): makes <target> (an add_executable) a Kvasir image of that variant. The variant row
# (cmake/kvasir_variants.cmake) decides the log (USE_UC_LOG), the sanitizer, the log floor, the check levels, soft check
# escalation and the self-tests; OPTIMIZATION_STRATEGY defaults to the row's (debug: debug, else size). The other
# keywords are KVASIR_CONFIGURE_* (cmake/kvasir_variants.cmake). VARIANT is required.
#
# The order of the side effects below is part of the contract: target_add_uc_log_rtt_jlink (string constants) snapshots
# COMPILE_OPTIONS when it runs, so per-image options added after this call are not in the catalog TU; and the archive
# order (runtime archives, then kvasir::chip: the chip's registers, the core's, then kvasir::sdk: uc_log) moves LTO's
# code.
function(target_configure_kvasir target)
    cmake_parse_arguments(
        PARSE_ARGV 1 A "${KVASIR_CONFIGURE_OPTIONS}" "VARIANT;OPTIMIZATION_STRATEGY;${KVASIR_CONFIGURE_ONE_VALUE}"
        "${KVASIR_CONFIGURE_MULTI_VALUE}")
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${A_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT A_VARIANT)
        message(FATAL_ERROR "${target}: target_configure_kvasir needs VARIANT (${KVASIR_VARIANTS} coverage): it "
                            "decides the log, the sanitizer and the check policy")
    endif()
    kvasir_variant_policy(${A_VARIANT} _p A)
    get_target_property(_warnings kvasir_warnings INTERFACE_COMPILE_OPTIONS)
    target_compile_options(${target} PRIVATE ${_warnings})
    get_target_property(_link_warnings kvasir_warnings INTERFACE_LINK_OPTIONS)
    if(_link_warnings)
        target_link_options(${target} PRIVATE ${_link_warnings})
    endif()
    foreach(
        _k
        MPU
        CORE1_SCRATCH_SIZE
        LINKER_FILE
        RAM_ONLY_LINKER_FILE
        JLINK_CONNECT_COMMANDS
        JLINK_PARK_SIGNATURE
        JLINK_STALL_SIGNATURE
        JLINK_STALL_RECOVERY
        JLINK_FLASH_READY_SIGNATURE
        JLINK_FLASH_WINDOW
        JLINK_FLASH_UNCACHED_OFFSET)
        kvasir_chip_get(_chip_${_k} ${_k})
    endforeach()

    if(NOT A_MIN_STACK_SIZE)
        set(A_MIN_STACK_SIZE 4k)
    endif()

    # uc_log compile-time log filter, read via #embed: a uc_log_filter.txt next to the project's top CMakeLists.txt is
    # picked up with no argument, LOG_FILTER <dir>/uc_log_filter.txt names another. Editing it only recompiles (#embed
    # puts it into the depfile); only adding or removing the default file re-configures (the glob).
    set(_log_filter "")
    if(A_LOG_FILTER)
        get_filename_component(_log_filter "${A_LOG_FILTER}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_log_filter}")
            message(FATAL_ERROR "${target}: LOG_FILTER ${_log_filter} does not exist")
        endif()
    else()
        file(GLOB _log_filter CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/uc_log_filter.txt")
    endif()
    if(_log_filter)
        get_filename_component(_log_filter_name "${_log_filter}" NAME)
        if(NOT _log_filter_name STREQUAL "uc_log_filter.txt")
            message(FATAL_ERROR "${target}: the log filter file must be named uc_log_filter.txt (#embed finds it by "
                                "name), not ${_log_filter_name}")
        endif()
        get_filename_component(_log_filter_dir "${_log_filter}" DIRECTORY)
        target_compile_options(${target} PRIVATE "--embed-dir=${_log_filter_dir}")
        set_target_properties(${target} PROPERTIES UC_LOG_FILTER "${_log_filter}")
    endif()

    # The image's config header (kvasir_config.hpp): the one app-owned header with what the libraries look up by
    # uc_log::Tag::User - the log sink (ComBackend) and the log clock (LogClock), both complete. uc_log/uc_log.hpp
    # includes it first through UC_LOG_CONFIG_HEADER, so every TU of the image sees the same specialisations before any
    # log line, whatever it included before. src/kvasir_config.hpp of the project is picked up with no argument,
    # CONFIG_HEADER <file> names another (a missing one is an error). Like the log filter: editing it recompiles, adding
    # or removing the default file re-configures. Set in every variant: a release image names the sink too.
    set(_config_header "")
    if(A_CONFIG_HEADER)
        get_filename_component(_config_header "${A_CONFIG_HEADER}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_config_header}")
            message(FATAL_ERROR "${target}: CONFIG_HEADER ${_config_header} does not exist")
        endif()
    else()
        file(GLOB _config_header CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/kvasir_config.hpp")
    endif()
    if(_config_header)
        target_compile_definitions(${target} PRIVATE UC_LOG_CONFIG_HEADER="${_config_header}")
    endif()

    # JLINK_IP / JLINK_PROBE: this image's own J-Link, over the cache variables of the same name -- for a desk with one
    # probe per board, where one machine-wide setting would point every product at the same one. Either one given names
    # the whole connection: JLINK_PROBE alone is that probe on USB even when the cache's JLINK_IP is set.
    if(A_JLINK_IP OR A_JLINK_PROBE)
        set(_jlink_ip "${A_JLINK_IP}")
        set(_jlink_probe "${A_JLINK_PROBE}")
        set_target_properties(${target} PROPERTIES KVASIR_JLINK_IP "${A_JLINK_IP}" KVASIR_JLINK_PROBE
                                                                                   "${A_JLINK_PROBE}")
    else()
        set(_jlink_ip "${JLINK_IP}")
        set(_jlink_probe "${JLINK_PROBE}")
    endif()

    # A second core is opt-in. Without CORE1_STACK_SIZE the image is single-core and, by contract, identical to what it
    # was before multicore support existed: no define, an empty .stack1, nothing else.
    if(NOT A_CORE1_STACK_SIZE)
        set(A_CORE1_STACK_SIZE 0)
    endif()
    if(NOT A_CORE1_STACK_SIZE STREQUAL "0")
        target_compile_definitions(${target} PUBLIC KVASIR_MULTICORE=1)
        # the size in bytes, for compile-time checks against it (Fault::HandlerFor's core 1 reserve)
        if(NOT A_CORE1_STACK_SIZE MATCHES "^([0-9]+)([kK]?)$")
            message(FATAL_ERROR "${target}: CORE1_STACK_SIZE ${A_CORE1_STACK_SIZE}: expected <n> or <n>k")
        endif()
        if(CMAKE_MATCH_2)
            math(EXPR _core1_stack_bytes "${CMAKE_MATCH_1} * 1024")
        else()
            set(_core1_stack_bytes ${CMAKE_MATCH_1})
        endif()
        target_compile_definitions(${target} PUBLIC KVASIR_CORE1_STACK_BYTES=${_core1_stack_bytes})
    endif()

    # Where core 1's stack lives: `ram` (the default, .stack1 next to core 0's stack) or `scratch` (the chip's core 1
    # scratch bank, kvasir_chip(CORE1_SCRATCH_SIZE) bytes: RP2040 SRAM4). Opt-in: the bank caps the stack at 4 KiB, and
    # a core 1 overflow on the M0+ then runs silently into the top of striped RAM.
    set(_core1_stack_in_scratch 0)
    if(A_CORE1_STACK_PLACEMENT AND NOT A_CORE1_STACK_PLACEMENT STREQUAL "ram")
        if(NOT A_CORE1_STACK_PLACEMENT STREQUAL "scratch")
            message(FATAL_ERROR "${target}: CORE1_STACK_PLACEMENT ${A_CORE1_STACK_PLACEMENT}: expected ram or scratch")
        endif()
        if(A_CORE1_STACK_SIZE STREQUAL "0")
            message(FATAL_ERROR "${target}: CORE1_STACK_PLACEMENT scratch without CORE1_STACK_SIZE")
        endif()
        if(NOT _chip_CORE1_SCRATCH_SIZE)
            message(FATAL_ERROR "${target}: CORE1_STACK_PLACEMENT scratch: the chip (${_chip_MPU}) has no core 1 "
                                "scratch bank (its kvasir_chip() passes no CORE1_SCRATCH_SIZE)")
        endif()
        if(_core1_stack_bytes GREATER _chip_CORE1_SCRATCH_SIZE)
            message(
                FATAL_ERROR
                    "${target}: CORE1_STACK_PLACEMENT scratch: core 1's stack (${_core1_stack_bytes} bytes) "
                    "does not fit the ${_chip_CORE1_SCRATCH_SIZE}-byte scratch bank; lower "
                    "CORE1_STACK_SIZE (SecondaryCore::stackHighWater() says how much is used) or drop "
                    "CORE1_STACK_PLACEMENT")
        endif()
        set(_core1_stack_in_scratch 1)
        target_compile_definitions(${target} PUBLIC KVASIR_CORE1_STACK_SCRATCH=1)
    endif()

    # SCRATCH_BANKS: objects in the chip's per-core scratch banks. Defines KVASIR_CORE_SCRATCH, which turns on the
    # KVASIR_COREn_{DATA,BSS,CODE} attributes (Util/attributes.hpp; empty without it) and the chip's copy/zero of the
    # banks at boot (Startup::ExtraMemoryInit). Opt-in so that an image without it keeps every byte it had; the chip's
    # linker script refuses scratch-bank objects in an image without it (cmake_scratch_banks).
    set(_scratch_banks 0)
    if(A_SCRATCH_BANKS)
        if(NOT _chip_CORE1_SCRATCH_SIZE)
            message(FATAL_ERROR "${target}: SCRATCH_BANKS: the chip (${_chip_MPU}) has no scratch banks")
        endif()
        set(_scratch_banks 1)
        target_compile_definitions(${target} PUBLIC KVASIR_CORE_SCRATCH=1)
    endif()

    if(NOT A_HEAP_SIZE)
        set(A_HEAP_SIZE 0k)
    endif()
    if(NOT A_HEAP_SIZE MATCHES "^0[kKmM]?$")
        target_compile_definitions(${target} PUBLIC KVASIR_HEAP=1)
    endif()

    if(NOT A_OPTIMIZATION_STRATEGY)
        set(A_OPTIMIZATION_STRATEGY ${_p_OPT})
    endif()

    # RAM_ONLY: an image that lives entirely in RAM (kvasir_chip(RAM_ONLY_LINKER_FILE)): loaded by the bootrom's UF2
    # path or a debugger, never written to flash; the J-Link script and the printer start it in RAM instead of
    # resetting. LINKER_FILE: the firmware's own linker script instead of the chip's (e.g. a calibration sector kept out
    # of the image), relative to the project.
    set(_jlink_ram_only "")
    set(_uc_log_ram_image "")
    if(A_RAM_ONLY)
        if(A_LINKER_FILE)
            message(
                FATAL_ERROR "${target}: RAM_ONLY picks the chip's RAM-only linker file; do not pass LINKER_FILE too")
        endif()
        if(NOT _chip_RAM_ONLY_LINKER_FILE)
            message(FATAL_ERROR "${target}: RAM_ONLY needs the chip's kvasir_chip(RAM_ONLY_LINKER_FILE)")
        endif()
        set(_linker_file ${_chip_RAM_ONLY_LINKER_FILE})
        target_compile_definitions(${target} PUBLIC KVASIR_RAM_ONLY=1)
        set_property(TARGET ${target} PROPERTY KVASIR_RAM_ONLY TRUE)
        set(_jlink_ram_only RAM_ONLY)
        set(_uc_log_ram_image RAM_IMAGE)
    elseif(NOT A_LINKER_FILE)
        set(_linker_file ${_chip_LINKER_FILE})
    else()
        set(_linker_file ${CMAKE_CURRENT_SOURCE_DIR}/${A_LINKER_FILE})
    endif()

    # IMAGE_CRC: the descriptor (KVASIR_IMAGE_CRC) and the post-link step that fills it. With RAM_ONLY (or any linker
    # file that loads the image into SRAM) the check covers the running image's read-only part instead of the flash.
    if(A_IMAGE_CRC)
        target_compile_definitions(${target} PUBLIC KVASIR_IMAGE_CRC=1)
        set_property(TARGET ${target} PROPERTY KVASIR_IMAGE_CRC TRUE)
    endif()

    # the optimisation set: compile flags, LTO backend options and the whole link of the toolchain file
    # (optimize_option_<s>, optimize_link_option_<s>, linker_flags_<s>)
    set(_s ${A_OPTIMIZATION_STRATEGY})
    if(NOT _s MATCHES "^(size|speed|debug)$")
        message(FATAL_ERROR "${target}: OPTIMIZATION_STRATEGY ${_s}: size, speed or debug")
    endif()
    set(sanitize_flags)
    if(_p_SAN)
        set(sanitize_flags ${sanitize_option})
    endif()

    # the runtime archives, shared across all targets with identical compile flags
    if("${COMPILER_RT}" STREQUAL "clang")
        target_add_shared_kvasir_lib(${target} rt "${COMPILER_RT_SOURCE_FILES}" ${_s} "${sanitize_flags}")
    endif()
    if("${CPPLIB}" STREQUAL "libc++")
        target_add_shared_kvasir_lib(${target} cxx "${LIBCXX_SOURCE_FILES}" ${_s} "${sanitize_flags}")
    endif()
    if("${CLIB}" STREQUAL "llvm")
        target_add_shared_kvasir_lib(${target} c "${LIBC_SOURCE_FILES}" ${_s} "${sanitize_flags}")
        if(NOT A_HEAP_SIZE EQUAL 0)
            target_add_shared_kvasir_lib(${target} c_malloc "${LIBC_MALLOC_SOURCE_FILES}" ${_s} "${sanitize_flags}")
        endif()
    endif()

    set_target_properties(${target} PROPERTIES SUFFIX ".elf")
    # the chip (its headers, registers, definitions, sources and linker options, with the core's headers and registers)
    # and the SDK (with uc_log): one link each, nothing appended by hand
    target_link_libraries(${target} kvasir::chip kvasir::sdk)

    _kvasir_postlink(${target} ${_linker_file} "${A_IMAGE_CRC}")
    cmake_git_version_add_headers_with_type(${target} ${A_OPTIMIZATION_STRATEGY})

    set_property(
        TARGET ${target}
        APPEND
        PROPERTY LINK_DEPENDS ${KVASIR_LINK_INPUTS} ${_linker_file})

    get_filename_component(linker_file_path ${_linker_file} ABSOLUTE)
    get_filename_component(linker_file_path ${linker_file_path} DIRECTORY)
    target_link_options(
        ${target}
        PUBLIC
        "${LINKER_PREFIX}--library-path=${linker_file_path}"
        "${LINKER_PREFIX}--library-path=${kvasir_cmake_dir}/../linker"
        "${LINKER_PREFIX}--defsym=cmake_min_stack_size=${A_MIN_STACK_SIZE}"
        "${LINKER_PREFIX}--defsym=cmake_heap_size=${A_HEAP_SIZE}"
        "${LINKER_PREFIX}--defsym=cmake_core1_stack_size=${A_CORE1_STACK_SIZE}"
        "${LINKER_PREFIX}--defsym=cmake_core1_stack_in_scratch=${_core1_stack_in_scratch}"
        "${LINKER_PREFIX}--defsym=cmake_scratch_banks=${_scratch_banks}"
        "${LINKER_PREFIX}--script=${_linker_file}"
        "${LINKER_PREFIX}--Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
    set_property(
        TARGET ${target}
        APPEND
        PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_CURRENT_BINARY_DIR}/${target}.map)

    target_compile_options(${target} PUBLIC ${optimize_option_${_s}} ${sanitize_flags})
    target_link_options(${target} PUBLIC ${optimize_link_option_${_s}} ${linker_flags_${_s}})

    # The J-Link this image's flash and log targets reach: the target's own (JLINK_IP / JLINK_PROBE) or else the
    # JLINK_IP / JLINK_PROBE cache variables.
    target_add_flash_jlink(
        ${target}
        ${_jlink_ram_only}
        TARGET_MPU
        ${_chip_MPU}
        SWD_SPEED
        ${SWD_SPEED}
        JLINK_IP
        ${_jlink_ip}
        JLINK_PROBE
        "${_jlink_probe}"
        CONNECT_COMMANDS
        ${_chip_JLINK_CONNECT_COMMANDS})
    target_add_uc_log_rtt_jlink(
        ${target}
        ${_uc_log_ram_image}
        TARGET_MPU
        ${_chip_MPU}
        SWD_SPEED
        ${SWD_SPEED}
        JLINK_IP
        ${_jlink_ip}
        JLINK_PROBE
        "${_jlink_probe}"
        DUPLEX_BASE_PORT
        ${DUPLEX_BASE_PORT}
        TRANSPORT
        ${UC_LOG_TRANSPORT}
        PRE_RESET_COMMANDS
        ${_chip_JLINK_CONNECT_COMMANDS}
        PARK_SIGNATURE
        "${_chip_JLINK_PARK_SIGNATURE}"
        STALL_SIGNATURE
        "${_chip_JLINK_STALL_SIGNATURE}"
        FLASH_READY_SIGNATURE
        "${_chip_JLINK_FLASH_READY_SIGNATURE}"
        FLASH_WINDOW
        "${_chip_JLINK_FLASH_WINDOW}"
        FLASH_UNCACHED_OFFSET
        "${_chip_JLINK_FLASH_UNCACHED_OFFSET}"
        STALL_RECOVERY
        ${_chip_JLINK_STALL_RECOVERY}
        LOG_FILTER
        "${_log_filter}"
        MAP_FILE
        ${target}.map
        HEX_FILE
        "${target}_flash.hex")

    if(_p_LOG)
        # KVASIR_LOG_FORMATTERS: the libraries' remote_fmt formatters and the generated registers' log views exist
        # (RegisterFmt.hpp, RateLimiter.hpp, kvasir_devices, net, svd_converter's kvasir_bit.hpp) - a variant without
        # the log parses none of them. UC_LOG_USE_AGLIO: reflectable structs (aglio/glaze) are formattable,
        # LOG_REFLECTED_STRUCTS ON (the default) or OFF - an image that logs no struct saves glaze in every log TU, and
        # one that does fails to compile with OFF ("undefined template remote_fmt::formatter<S>"). Both PUBLIC on the
        # image: every TU agrees (formatter specializations), never set per TU.
        if(NOT DEFINED A_LOG_REFLECTED_STRUCTS OR A_LOG_REFLECTED_STRUCTS)
            set(_log_aglio 1)
        else()
            set(_log_aglio 0)
        endif()
        target_compile_definitions(${target} PUBLIC USE_UC_LOG KVASIR_LOG_FORMATTERS=1 UC_LOG_USE_AGLIO=${_log_aglio})
        # The registers' formatter comes with the log, not with the chip umbrella: forced into every C++ TU of a log
        # variant, found on the include path like an #include. A register is no reflectable struct, so a TU without it
        # could not log one at all (a compile error, never other text); no chip or driver header names it.
        target_compile_options(${target}
                               PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include kvasir/Register/RegisterFmt.hpp>")
    endif()
    # Compile-time floor for UC_LOG_*: call sites below it vanish entirely, catalog strings included. Passed through as
    # a uc_log::LogLevel enumerator name (trace debug info warn error crit); the compiler rejects anything else.
    if(NOT _p_MIN_LOG_LEVEL STREQUAL "")
        target_compile_definitions(${target} PUBLIC UC_LOG_MIN_LEVEL=${_p_MIN_LOG_LEVEL})
    endif()
    # Every TU of an image must see the same levels: set here per target, never in a header.
    target_compile_definitions(
        ${target} PRIVATE KVASIR_SELFTEST=${_p_SELFTEST} KVASIR_CHECK_LEVEL=${_p_CHECK}
                          KVASIR_DCHECK_LEVEL=${_p_DCHECK} KVASIR_SOFT_CHECK_ESCALATE=${_p_ESCALATE})
    if(A_VARIANT STREQUAL "coverage")
        target_compile_definitions(${target} PRIVATE KVASIR_COVERAGE=1)
    endif()

    kvasir_add_tidy_flags(${target} ${A_VARIANT})
    kvasir_add_cppcheck_flags(${target})
endfunction()

mark_as_advanced(FORCE CMAKE_INSTALL_PREFIX)
mark_as_advanced(FORCE CMAKE_BUILD_TYPE)
