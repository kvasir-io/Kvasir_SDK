# A configure-only project (no language) for cmake/kvasir_chip.cmake and cmake/kvasir_interface.cmake: one CASE per
# configure, run by test_chip_contract.py. `good`, `empty_lists`, `devices` and `devices_sibling` must configure and
# check what they made; every other case must fail with its message.
#
# cmake -S tests/chip_contract -B <dir> -DCASE=<case>
cmake_minimum_required(VERSION 3.28)
project(chip_contract NONE)

get_filename_component(_sdk "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(${_sdk}/cmake/kvasir_chip.cmake)
include(${_sdk}/cmake/kvasir_interface.cmake)

if(NOT CASE)
    message(FATAL_ERROR "chip_contract: -DCASE=<case>")
endif()

function(expect what got want)
    if(NOT "${got}" STREQUAL "${want}")
        message(FATAL_ERROR "chip_contract ${CASE}: ${what}: got '${got}', want '${want}'")
    endif()
endfunction()

set(F ${CMAKE_CURRENT_LIST_DIR}/fixture)
add_library(fake_core_registers INTERFACE)
add_library(fake_chip_registers INTERFACE)

# a core call as core.cmake makes it; the include dir spelled through `..` (CMake keeps that verbatim on a -I line)
set(core_args
    CPU
    cortex-m0plus
    ARCH
    v6-m
    FPU
    none
    FLOAT_ABI
    soft
    TRIPLE
    thumbv6-m-none-eabi
    INSTRUCTION_MODE
    thumb
    ENDIAN
    little-endian
    INCLUDE_DIR
    ${F}/core/cmake/../src
    REGISTERS
    fake_core_registers)

# a chip call with every key, shaped like the RP2040's
set(chip_args
    MPU
    FAKE_M0_0
    UF2_FAMILY
    0xE48BFF56
    CORE1_SCRATCH_SIZE
    4096
    LINKER_FILE
    ${F}/chip/chip.ld
    RAM_ONLY_LINKER_FILE
    ${F}/chip/cmake/../chip_ram_only.ld
    INCLUDE_DIR
    ${F}/chip/cmake/../src
    REGISTERS
    fake_chip_registers
    DEFINITIONS
    KVASIR_CHIP_FAKE=1
    KVASIR_CHIP_FAMILY_FAKE=1
    SOURCES
    ${F}/chip/src/chip/extra.S
    LINKER_OPTIONS
    --wrap=__aeabi_idiv
    --wrap=__aeabi_uidiv
    IMAGE_CRC_EXCLUDE
    start_a
    end_a
    start_b
    end_b
    JLINK_CONNECT_COMMANDS
    "w4 0x40012004 0x00010000"
    "w4 0x40013004 0x00010000"
    JLINK_PARK_SIGNATURE
    "0x40064008,0x00100000,0x00100000,0x30"
    JLINK_STALL_SIGNATURE
    "0x18000000,0x007f0300,0x00070000+0x18000028,0x1,0x0"
    JLINK_STALL_RECOVERY
    "w4 0x40010008 0x0001fffc"
    JLINK_FLASH_READY_SIGNATURE
    "0x18000000,0x300,0x300+0x18000008,0x1,0x1"
    JLINK_FLASH_WINDOW
    "0x10000000,0x04000000"
    JLINK_FLASH_UNCACHED_OFFSET
    "0x03000000")

# the value after <key> in chip_args replaced by <value>
macro(chip_arg key value)
    list(FIND chip_args ${key} _i)
    math(EXPR _i "${_i} + 1")
    list(REMOVE_AT chip_args ${_i})
    list(INSERT chip_args ${_i} "${value}")
endmacro()

if(CASE STREQUAL "good")
    kvasir_core(${core_args})
    kvasir_chip(${chip_args})
    expect("TARGET_CPU" "${TARGET_CPU}" "cortex-m0plus")
    expect("TARGET_ARCH" "${TARGET_ARCH}" "v6-m")
    expect("TARGET_FPU" "${TARGET_FPU}" "none")
    expect("TARGET_FLOAT_ABI" "${TARGET_FLOAT_ABI}" "soft")
    expect("TARGET_TRIPLE" "${TARGET_TRIPLE}" "thumbv6-m-none-eabi")
    expect("TARGET_ARM_INSTRUCTION_MODE" "${TARGET_ARM_INSTRUCTION_MODE}" "thumb")
    expect("TARGET_ENDIAN" "${TARGET_ENDIAN}" "little-endian")
    get_target_property(_v kvasir::core ALIASED_TARGET)
    expect("kvasir::core" "${_v}" "kvasir_core")
    get_target_property(_v kvasir_core INTERFACE_INCLUDE_DIRECTORIES)
    expect("core include dir (normalised)" "${_v}" "${F}/core/src")
    get_target_property(_v kvasir_core INTERFACE_LINK_LIBRARIES)
    expect("core links" "${_v}" "fake_core_registers")
    get_target_property(_v kvasir::chip ALIASED_TARGET)
    expect("kvasir::chip" "${_v}" "kvasir_chip")
    get_target_property(_v kvasir_chip INTERFACE_INCLUDE_DIRECTORIES)
    expect("chip include dir (normalised)" "${_v}" "${F}/chip/src")
    get_target_property(_v kvasir_chip INTERFACE_LINK_LIBRARIES)
    expect("chip links: its registers, then the core" "${_v}" "fake_chip_registers;kvasir_core")
    get_target_property(_v kvasir_chip INTERFACE_COMPILE_DEFINITIONS)
    expect("definitions" "${_v}" "KVASIR_CHIP_FAKE=1;KVASIR_CHIP_FAMILY_FAKE=1")
    get_target_property(_v kvasir_chip INTERFACE_SOURCES)
    expect("sources, executables only" "${_v}"
           "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${F}/chip/src/chip/extra.S>")
    get_target_property(_v kvasir_chip INTERFACE_LINK_OPTIONS)
    expect("no link options before util.cmake prefixes them" "${_v}" "_v-NOTFOUND")
    foreach(
        _case
        "MPU|FAKE_M0_0"
        "UF2_FAMILY|0xE48BFF56"
        "CORE1_SCRATCH_SIZE|4096"
        "LINKER_FILE|${F}/chip/chip.ld"
        "RAM_ONLY_LINKER_FILE|${F}/chip/chip_ram_only.ld"
        "LINKER_OPTIONS|--wrap=__aeabi_idiv;--wrap=__aeabi_uidiv"
        "IMAGE_CRC_EXCLUDE|start_a;end_a;start_b;end_b"
        "JLINK_CONNECT_COMMANDS|w4 0x40012004 0x00010000;w4 0x40013004 0x00010000"
        "JLINK_STALL_RECOVERY|w4 0x40010008 0x0001fffc"
        "JLINK_FLASH_WINDOW|0x10000000,0x04000000")
        string(FIND "${_case}" "|" _bar)
        string(SUBSTRING "${_case}" 0 ${_bar} _key)
        math(EXPR _bar "${_bar} + 1")
        string(SUBSTRING "${_case}" ${_bar} -1 _want)
        kvasir_chip_get(_v ${_key})
        expect("KVASIR_CHIP_${_key}" "${_v}" "${_want}")
    endforeach()

    # kvasir_quarantine_interface: a generator-expression option stripped through an ALIAS, the other options kept
    add_library(third_party INTERFACE)
    add_library(third::party ALIAS third_party)
    target_compile_options(third_party INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-braces> -Wno-other)
    target_include_directories(third_party INTERFACE ${F}/core/src)
    kvasir_quarantine_interface(third::party STRIP_OPTIONS "-Wno-missing-braces")
    get_target_property(_v third_party INTERFACE_COMPILE_OPTIONS)
    expect("stripped option" "${_v}" "-Wno-other")
    get_target_property(_v third_party INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    expect("no SYSTEM without SYSTEM" "${_v}" "_v-NOTFOUND")
    kvasir_quarantine_interface(third_party SYSTEM STRIP_OPTIONS "-Wno-other")
    get_target_property(_v third_party INTERFACE_COMPILE_OPTIONS)
    expect("every option stripped" "${_v}" "")
    get_target_property(_v third_party INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    expect("SYSTEM include dirs" "${_v}" "${F}/core/src")
    kvasir_quarantine_interface(not_a_target SYSTEM STRIP_OPTIONS "-W.*")
    # no DEVICES: no package registered
    kvasir_chip_get(_v DEVICES)
    expect("KVASIR_CHIP_DEVICES" "${_v}" "FALSE")
    get_property(_v GLOBAL PROPERTY KVASIR_PACKAGES)
    expect("no package without DEVICES" "${_v}" "")
    message(STATUS "chip_contract good: ok")
elseif(CASE STREQUAL "devices")
    # DEVICES with KVASIR_DEVICES_ROOT given: that root, registered for util.cmake, visible in the caller
    unset(ENV{KVASIR_DEVICES_ROOT})
    set(KVASIR_DEVICES_ROOT ${F}/devices/../devices)
    kvasir_core(${core_args})
    kvasir_chip(DEVICES ${chip_args})
    kvasir_chip_get(_v DEVICES)
    expect("KVASIR_CHIP_DEVICES" "${_v}" "TRUE")
    expect("KVASIR_DEVICES_ROOT (normalised)" "${KVASIR_DEVICES_ROOT}" "${F}/devices")
    get_property(_v GLOBAL PROPERTY KVASIR_PACKAGES)
    expect("the package util.cmake adds" "${_v}" "${F}/devices|kvasir_devices|kvasir_devices")
    message(STATUS "chip_contract devices: ok")
elseif(CASE STREQUAL "devices_sibling")
    # DEVICES without a root: kvasir_devices next to KVASIR_ROOT, cached
    unset(ENV{KVASIR_DEVICES_ROOT})
    set(KVASIR_ROOT ${F}/sdk)
    kvasir_core(${core_args})
    kvasir_chip(${chip_args} DEVICES)
    expect("KVASIR_DEVICES_ROOT" "${KVASIR_DEVICES_ROOT}" "${F}/kvasir_devices")
    get_property(
        _v
        CACHE KVASIR_DEVICES_ROOT
        PROPERTY VALUE)
    expect("KVASIR_DEVICES_ROOT cached" "${_v}" "${F}/kvasir_devices")
    get_property(_v GLOBAL PROPERTY KVASIR_PACKAGES)
    expect("the package util.cmake adds" "${_v}" "${F}/kvasir_devices|kvasir_devices|kvasir_devices")
    message(STATUS "chip_contract devices_sibling: ok")
elseif(CASE STREQUAL "devices_with_value")
    kvasir_core(${core_args})
    kvasir_chip(${chip_args} DEVICES ON)
elseif(CASE STREQUAL "empty_lists")
    # an option that leaves a list key empty (the RP2350 without its DCP sources), and no optional key at all
    kvasir_core(${core_args})
    kvasir_chip(
        MPU
        FAKE
        LINKER_FILE
        ${F}/chip/chip.ld
        INCLUDE_DIR
        ${F}/chip/src
        REGISTERS
        fake_chip_registers
        SOURCES
        LINKER_OPTIONS)
    kvasir_chip_get(_v UF2_FAMILY)
    expect("no UF2 family" "${_v}" "")
    get_target_property(_v kvasir_chip INTERFACE_SOURCES)
    expect("no sources" "${_v}" "_v-NOTFOUND")
    message(STATUS "chip_contract empty_lists: ok")
elseif(CASE STREQUAL "missing_mpu")
    kvasir_core(${core_args})
    list(REMOVE_ITEM chip_args MPU FAKE_M0_0)
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "scratch_size_k")
    kvasir_core(${core_args})
    chip_arg(CORE1_SCRATCH_SIZE 4K)
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "uf2_short")
    kvasir_core(${core_args})
    chip_arg(UF2_FAMILY 0xE48BFF)
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "odd_crc_exclude")
    kvasir_core(${core_args})
    list(REMOVE_ITEM chip_args end_b)
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "connect_not_w4")
    kvasir_core(${core_args})
    chip_arg(JLINK_CONNECT_COMMANDS "mww 0x40012004 0x00010000")
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "bad_signature")
    kvasir_core(${core_args})
    chip_arg(JLINK_PARK_SIGNATURE "0x40064008:0x1")
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "definition_with_d")
    kvasir_core(${core_args})
    chip_arg(DEFINITIONS -DKVASIR_CHIP_FAKE=1)
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "linker_file_missing")
    kvasir_core(${core_args})
    chip_arg(LINKER_FILE ${F}/chip/none.ld)
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "unknown_keyword")
    kvasir_core(${core_args})
    kvasir_chip(${chip_args} TARGET_MPU FAKE)
elseif(CASE STREQUAL "one_value_empty")
    kvasir_core(${core_args})
    set(_none "")
    kvasir_chip(${chip_args} JLINK_FLASH_WINDOW ${_none})
elseif(CASE STREQUAL "chip_twice")
    kvasir_core(${core_args})
    kvasir_chip(${chip_args})
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "chip_before_core")
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "registers_not_a_target")
    kvasir_core(${core_args})
    chip_arg(REGISTERS peripherals)
    kvasir_chip(${chip_args})
elseif(CASE STREQUAL "core_float_abi")
    list(FIND core_args FLOAT_ABI _i)
    math(EXPR _i "${_i} + 1")
    list(REMOVE_AT core_args ${_i})
    list(INSERT core_args ${_i} softfloat)
    kvasir_core(${core_args})
elseif(CASE STREQUAL "core_missing_cpu")
    list(REMOVE_ITEM core_args CPU cortex-m0plus)
    kvasir_core(${core_args})
else()
    message(FATAL_ERROR "chip_contract: unknown CASE ${CASE}")
endif()
