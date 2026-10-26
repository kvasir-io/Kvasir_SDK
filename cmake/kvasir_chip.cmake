# The contract between the SDK and a chip or core package: kvasir_core() (core_cortex_*/cmake/core.cmake) and
# kvasir_chip() (chip_*/cmake/chip.cmake). Each validates what the package passes and makes an INTERFACE target the
# image links (target_configure_kvasir): kvasir::core and kvasir::chip. The signatures are the documentation. Included
# first by kvasir.cmake, before the chip package, so both run before project(); with them kvasir_resolve_root and
# kvasir_add_package, which kvasir.cmake, kvasir_chip(DEVICES) and firmwares with packages of their own use.
include_guard(GLOBAL)

# kvasir_resolve_root(<var> [<sibling>]): <var> from the CMake variable or the environment, else the directory <sibling>
# next to KVASIR_ROOT; without <sibling> it must be given. An absolute path, cached and set in the caller.
function(kvasir_resolve_root var)
    if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
        set(_dir "${${var}}")
        set(_from "CMake variable")
    elseif(DEFINED ENV{${var}})
        set(_dir "$ENV{${var}}")
        set(_from "environment")
    elseif(ARGC GREATER 1)
        get_filename_component(_parent "${KVASIR_ROOT}" DIRECTORY)
        set(_dir "${_parent}/${ARGV1}")
        set(_from "computed as sibling")
    else()
        message(FATAL_ERROR "Kvasir: ${var} is not set (a CMake variable or the environment)")
    endif()
    get_filename_component(_dir "${_dir}" ABSOLUTE)
    message(STATUS "Kvasir: ${var}=${_dir} (from ${_from})")
    set(${var}
        "${_dir}"
        CACHE INTERNAL "" FORCE)
    set(${var}
        "${_dir}"
        PARENT_SCOPE)
endfunction()

# Registers a package for util.cmake to add after project(), unless <guard_target> already exists
function(kvasir_add_package source_dir binary_name guard_target)
    set_property(GLOBAL APPEND PROPERTY KVASIR_PACKAGES "${source_dir}|${binary_name}|${guard_target}")
endfunction()

# _kvasir_contract_check(<function> <prefix> ONE <key>... [REQUIRED <key>...]): the cmake_parse_arguments(<prefix>)
# result of <function> - no unknown argument, no one-value key ONE without its value (a list key may be empty: an
# option's sources or flags), every REQUIRED key given
function(_kvasir_contract_check fn prefix)
    cmake_parse_arguments(PARSE_ARGV 2 C "" "" "ONE;REQUIRED")
    if(${prefix}_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${fn}: unknown argument(s) ${${prefix}_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_k IN LISTS ${prefix}_KEYWORDS_MISSING_VALUES)
        if(_k IN_LIST C_ONE)
            message(FATAL_ERROR "${fn}: no value given for ${_k}")
        endif()
    endforeach()
    foreach(_k IN LISTS C_REQUIRED)
        if(NOT DEFINED ${prefix}_${_k} OR "${${prefix}_${_k}}" STREQUAL "")
            message(FATAL_ERROR "${fn}: ${_k} is required")
        endif()
    endforeach()
endfunction()

# _kvasir_contract_match(<function> <prefix> <key> <regex> <what>): every element of <prefix>_<key> (if given) matches
function(_kvasir_contract_match fn prefix key regex what)
    foreach(_v IN LISTS ${prefix}_${key})
        if(NOT _v MATCHES "${regex}")
            message(FATAL_ERROR "${fn}: ${key} '${_v}' is not ${what}")
        endif()
    endforeach()
endfunction()

# _kvasir_contract_file(<function> <prefix> <key> <out>): <prefix>_<key> (if given) as an absolute path that exists.
# Absolute and normalised: CMake keeps a/../b verbatim on a -I line or in a link command.
function(_kvasir_contract_file fn prefix key out)
    set(_r "")
    foreach(_v IN LISTS ${prefix}_${key})
        get_filename_component(_abs "${_v}" ABSOLUTE)
        if(NOT EXISTS "${_abs}")
            message(FATAL_ERROR "${fn}: ${key} ${_abs} does not exist")
        endif()
        list(APPEND _r "${_abs}")
    endforeach()
    set(${out}
        "${_r}"
        PARENT_SCOPE)
endfunction()

# kvasir_core(CPU <cpu> ARCH <arch> FPU <fpu> FLOAT_ABI soft|softfp|hard TRIPLE <triple> INSTRUCTION_MODE thumb|arm
# ENDIAN little-endian|big-endian INCLUDE_DIR <dir> REGISTERS <target>) What a core package tells the SDK. Sets
# TARGET_CPU, TARGET_ARCH, TARGET_FPU, TARGET_FLOAT_ABI, TARGET_TRIPLE, TARGET_ARM_INSTRUCTION_MODE and TARGET_ENDIAN in
# the caller's scope - the toolchain files and the runtime libraries read those, also in try_compile projects where no
# target exists - and makes kvasir::core: the core's headers (INCLUDE_DIR) and its generated registers (REGISTERS, the
# core's svd_convert target, called before).
function(kvasir_core)
    set(_keys
        CPU
        ARCH
        FPU
        FLOAT_ABI
        TRIPLE
        INSTRUCTION_MODE
        ENDIAN
        INCLUDE_DIR
        REGISTERS)
    cmake_parse_arguments(PARSE_ARGV 0 A "" "${_keys}" "")
    _kvasir_contract_check(kvasir_core A ONE ${_keys} REQUIRED ${_keys})
    _kvasir_contract_match(kvasir_core A FLOAT_ABI "^(soft|softfp|hard)$" "soft, softfp or hard")
    _kvasir_contract_match(kvasir_core A INSTRUCTION_MODE "^(thumb|arm)$" "thumb or arm")
    _kvasir_contract_match(kvasir_core A ENDIAN "^(little|big)-endian$" "little-endian or big-endian")
    if(TARGET kvasir_core)
        message(FATAL_ERROR "kvasir_core: called twice")
    endif()
    if(NOT TARGET ${A_REGISTERS})
        message(FATAL_ERROR "kvasir_core: REGISTERS ${A_REGISTERS} is not a target (svd_convert it first)")
    endif()
    _kvasir_contract_file(kvasir_core A INCLUDE_DIR _inc)
    foreach(_k CPU ARCH FPU FLOAT_ABI TRIPLE ENDIAN)
        set(TARGET_${_k}
            "${A_${_k}}"
            PARENT_SCOPE)
    endforeach()
    set(TARGET_ARM_INSTRUCTION_MODE
        "${A_INSTRUCTION_MODE}"
        PARENT_SCOPE)
    add_library(kvasir_core INTERFACE)
    add_library(kvasir::core ALIAS kvasir_core)
    target_include_directories(kvasir_core INTERFACE "${_inc}")
    target_link_libraries(kvasir_core INTERFACE ${A_REGISTERS})
endfunction()

# kvasir_chip(MPU <name> LINKER_FILE <file> INCLUDE_DIR <dir> REGISTERS <target> [UF2_FAMILY 0x<8 hex digits>]
# [CORE1_SCRATCH_SIZE <bytes>] [RAM_ONLY_LINKER_FILE <file>] [SOURCES <file>...] [DEFINITIONS <NAME=value>...]
# [LINKER_OPTIONS <bare linker option>...] [IMAGE_CRC_EXCLUDE <start symbol> <end symbol>...] [JLINK_CONNECT_COMMANDS
# "w4 0x<a> 0x<v>"...] [JLINK_STALL_RECOVERY "w4 0x<a> 0x<v>"...] [JLINK_PARK_SIGNATURE <sig>] [JLINK_STALL_SIGNATURE
# <sig>] [JLINK_FLASH_READY_SIGNATURE <sig>] [JLINK_FLASH_WINDOW 0x<base>,0x<size>] [JLINK_FLASH_UNCACHED_OFFSET
# 0x<offset>] [DEVICES]) What a chip package tells the SDK, after kvasir_core() and its own svd_convert. Makes
# kvasir::chip: the chip's headers (INCLUDE_DIR), its generated registers (REGISTERS), kvasir::core, the DEFINITIONS
# (the chip identity macros) for every TU, the SOURCES for executables only (a library that links kvasir::chip gets no
# second copy), and LINKER_OPTIONS (util.cmake puts the toolchain's linker prefix in front after project()). Every value
# is also a KVASIR_CHIP_<KEY> property of kvasir::chip, for the image's link, post-link, J-Link and printer steps
# (util.cmake): - MPU: the J-Link device name (JLinkExe -device, uc_log_printer --device) - LINKER_FILE: the image's
# linker script; its MEMORY is the one statement of the flash, EEPROM and RAM sizes (the stack takes what LENGTH(ram)
# leaves, linker/common.ld) - UF2_FAMILY: the .uf2 artefacts' family id; without it no .uf2 is written -
# CORE1_SCRATCH_SIZE: core 1's scratch bank, for CORE1_STACK_PLACEMENT scratch and SCRATCH_BANKS; none without it -
# IMAGE_CRC_EXCLUDE: pairs of symbols bounding bytes a later step rewrites, left out of IMAGE_CRC -
# JLINK_CONNECT_COMMANDS: lines after `connect` in the J-Link scripts, and the printer's --pre_reset_command -
# JLINK_*_SIGNATURE ("<address>,<mask>,<value>[+...]"), JLINK_STALL_RECOVERY, JLINK_FLASH_WINDOW,
# JLINK_FLASH_UNCACHED_OFFSET: the printer's --park_signature, --stall_signature, --flash_ready_signature,
# --stall_recovery, --flash_window, --flash_uncached_offset - DEVICES: the chip's drivers (chip/drivers.hpp: queued
# buses, USB backend, input drivers) build on kvasir_devices; chip/chip.hpp does not. Its root is resolved
# (KVASIR_DEVICES_ROOT: variable, environment, else next to the SDK) and the package registered for util.cmake to add
# after project() unless the firmware has kvasir_devices already, so kvasir::bus / kvasir::devices exist for a firmware
# that links them; nothing links it into the chip. The J-Link lines are `w4 <address> <value>` only: uc_log_printer
# understands nothing else.
function(kvasir_chip)
    set(_required MPU LINKER_FILE INCLUDE_DIR REGISTERS)
    set(_one
        ${_required}
        UF2_FAMILY
        CORE1_SCRATCH_SIZE
        RAM_ONLY_LINKER_FILE
        JLINK_PARK_SIGNATURE
        JLINK_STALL_SIGNATURE
        JLINK_FLASH_READY_SIGNATURE
        JLINK_FLASH_WINDOW
        JLINK_FLASH_UNCACHED_OFFSET)
    set(_multi SOURCES DEFINITIONS LINKER_OPTIONS IMAGE_CRC_EXCLUDE JLINK_CONNECT_COMMANDS JLINK_STALL_RECOVERY)
    cmake_parse_arguments(PARSE_ARGV 0 A "DEVICES" "${_one}" "${_multi}")
    _kvasir_contract_check(kvasir_chip A ONE ${_one} REQUIRED ${_required})
    set(_kvasir_hex "0x[0-9a-fA-F]+")
    # "<address>,<mask>,<value>[,...][+<address>,...]": the register words uc_log_printer compares
    set(_kvasir_signature "^${_kvasir_hex}(,${_kvasir_hex})*(\\+${_kvasir_hex}(,${_kvasir_hex})*)*$")
    # the only J-Link Commander line uc_log_printer also understands (--pre_reset_command, --stall_recovery)
    set(_kvasir_w4 "^w4 ${_kvasir_hex} ${_kvasir_hex}$")
    _kvasir_contract_match(kvasir_chip A MPU "^[A-Za-z0-9_.-]+$" "a J-Link device name")
    _kvasir_contract_match(kvasir_chip A CORE1_SCRATCH_SIZE "^[0-9]+$" "a size in bytes (decimal)")
    set(_h8 "[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]")
    _kvasir_contract_match(kvasir_chip A UF2_FAMILY "^0x${_h8}$" "0x and 8 hex digits")
    foreach(_k JLINK_PARK_SIGNATURE JLINK_STALL_SIGNATURE JLINK_FLASH_READY_SIGNATURE)
        _kvasir_contract_match(kvasir_chip A ${_k} "${_kvasir_signature}" "<address>,<mask>,<value>[+...] in hex")
    endforeach()
    _kvasir_contract_match(kvasir_chip A JLINK_FLASH_WINDOW "^${_kvasir_hex},${_kvasir_hex}$" "<base>,<size> in hex")
    _kvasir_contract_match(kvasir_chip A JLINK_FLASH_UNCACHED_OFFSET "^${_kvasir_hex}$" "a hex offset")
    foreach(_k JLINK_CONNECT_COMMANDS JLINK_STALL_RECOVERY)
        _kvasir_contract_match(kvasir_chip A ${_k} "${_kvasir_w4}" "`w4 0x<address> 0x<value>` (uc_log_printer)")
    endforeach()
    _kvasir_contract_match(kvasir_chip A DEFINITIONS "^[A-Za-z_][A-Za-z0-9_]*(=.*)?$" "NAME or NAME=value (no -D)")
    _kvasir_contract_match(kvasir_chip A LINKER_OPTIONS "^--" "a bare linker option (--...)")
    list(LENGTH A_IMAGE_CRC_EXCLUDE _n)
    math(EXPR _odd "${_n} % 2")
    if(_odd)
        message(FATAL_ERROR "kvasir_chip: IMAGE_CRC_EXCLUDE is pairs of symbols, got ${_n}: ${A_IMAGE_CRC_EXCLUDE}")
    endif()
    if(TARGET kvasir_chip)
        message(FATAL_ERROR "kvasir_chip: called twice")
    endif()
    if(NOT TARGET kvasir_core)
        message(FATAL_ERROR "kvasir_chip: no kvasir::core - the chip's core.cmake calls kvasir_core() first")
    endif()
    if(NOT TARGET ${A_REGISTERS})
        message(FATAL_ERROR "kvasir_chip: REGISTERS ${A_REGISTERS} is not a target (svd_convert it first)")
    endif()
    foreach(_k INCLUDE_DIR LINKER_FILE RAM_ONLY_LINKER_FILE SOURCES)
        _kvasir_contract_file(kvasir_chip A ${_k} A_${_k})
    endforeach()

    add_library(kvasir_chip INTERFACE)
    add_library(kvasir::chip ALIAS kvasir_chip)
    target_include_directories(kvasir_chip INTERFACE "${A_INCLUDE_DIR}")
    # the chip's registers first, then the core's: the archive order the image had
    target_link_libraries(kvasir_chip INTERFACE ${A_REGISTERS} kvasir_core)
    if(A_SOURCES)
        target_sources(kvasir_chip INTERFACE "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${A_SOURCES}>")
    endif()
    if(A_DEFINITIONS)
        target_compile_definitions(kvasir_chip INTERFACE ${A_DEFINITIONS})
    endif()
    foreach(_k IN LISTS _one _multi)
        set_property(TARGET kvasir_chip PROPERTY KVASIR_CHIP_${_k} "${A_${_k}}")
    endforeach()
    set_property(TARGET kvasir_chip PROPERTY KVASIR_CHIP_DEVICES "${A_DEVICES}")
    if(A_DEVICES)
        kvasir_resolve_root(KVASIR_DEVICES_ROOT kvasir_devices)
        kvasir_add_package(${KVASIR_DEVICES_ROOT} kvasir_devices kvasir_devices)
        set(KVASIR_DEVICES_ROOT
            "${KVASIR_DEVICES_ROOT}"
            PARENT_SCOPE)
    endif()
endfunction()

# kvasir_chip_get(<var> <KEY>): kvasir::chip's KVASIR_CHIP_<KEY> ("" when the chip did not pass it)
function(kvasir_chip_get var key)
    get_target_property(_v kvasir_chip KVASIR_CHIP_${key})
    if("${_v}" MATCHES "-NOTFOUND$")
        set(_v "")
    endif()
    set(${var}
        "${_v}"
        PARENT_SCOPE)
endfunction()
