# cmake -P tests/cmake_api/test_variants.cmake: cmake/kvasir_variants.cmake - every variant row and its resolved policy
# (log floor, check levels, escalation, self-tests), and kvasir_forward_arguments round trips (options, an empty
# one-value keyword, a multi-value keyword followed by a one-value one: the swallow trap).
cmake_minimum_required(VERSION 3.28)
cmake_policy(SET CMP0174 NEW) # as kvasir.cmake and util.cmake set it: an empty one-value keyword is defined
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/kvasir_variants.cmake)

function(expect what got want)
    if(NOT "${got}" STREQUAL "${want}")
        message(SEND_ERROR "${what}: got '${got}', want '${want}'")
    endif()
endfunction()

# variant: EXCLUDE LOG SAN OPT FLOOR LEVELS ESCALATE SELFTEST
foreach(
    _case
    "debug|TRUE|TRUE|FALSE|debug|debug|debug|0|ON" "release|FALSE|FALSE|FALSE|size|none|release|0|ON"
    "release_log|FALSE|TRUE|FALSE|size|release|release|0|ON" "sanitize|FALSE|TRUE|TRUE|size|release|debug|1|ON"
    "coverage|TRUE|TRUE|FALSE|size|release|release|0|OFF")
    string(REPLACE "|" ";" _case "${_case}")
    list(POP_FRONT _case _v)
    kvasir_variant_row(${_v} R)
    set(_got)
    foreach(
        _column
        EXCLUDE
        LOG
        SAN
        OPT
        FLOOR
        LEVELS
        ESCALATE
        SELFTEST)
        list(APPEND _got ${R_${_column}})
    endforeach()
    expect("row ${_v}" "${_got}" "${_case}")
endforeach()

# defaults: CHECK 2, DCHECK = CHECK in debug/sanitize and 0 in release/release_log/coverage, selftests in debug only
foreach(_case "debug|2|2|0|1|" "release|2|0|0|0|" "release_log|2|0|0|0|" "sanitize|2|2|1|0|" "coverage|2|0|0|0|")
    string(REPLACE "|" ";" _case "${_case}")
    list(POP_FRONT _case _v)
    kvasir_variant_policy(${_v} P NONE)
    expect("defaults ${_v}" "${P_CHECK};${P_DCHECK};${P_ESCALATE};${P_SELFTEST};${P_MIN_LOG_LEVEL}" "${_case}")
endforeach()

# every knob: the release levels and the log floors come from their own keywords
set(K_CHECK_LEVEL 1)
set(K_CHECK_LEVEL_RELEASE 0)
set(K_DCHECK_LEVEL 2)
set(K_DCHECK_LEVEL_RELEASE 1)
set(K_SELFTEST_VARIANT all)
set(K_MIN_LOG_LEVEL info)
set(K_MIN_LOG_LEVEL_DEBUG trace)
foreach(_case "debug|1|2|0|1|trace" "release|0|1|0|1|" "release_log|0|1|0|1|info" "sanitize|1|2|1|1|info"
              "coverage|0|1|0|0|info")
    string(REPLACE "|" ";" _case "${_case}")
    list(POP_FRONT _case _v)
    kvasir_variant_policy(${_v} P K)
    expect("knobs ${_v}" "${P_CHECK};${P_DCHECK};${P_ESCALATE};${P_SELFTEST};${P_MIN_LOG_LEVEL}" "${_case}")
endforeach()
set(K_MIN_LOG_LEVEL_RELEASE warn)
set(K_SELFTEST_VARIANT release_log)
kvasir_variant_policy(release_log P K)
expect("release floor" "${P_MIN_LOG_LEVEL};${P_SELFTEST}" "warn;1")
kvasir_variant_policy(debug P K)
expect("selftest elsewhere" "${P_SELFTEST}" "0")

# forwarding: what cmake_parse_arguments parsed comes back as the same arguments, in table order
function(round_trip want)
    cmake_parse_arguments(PARSE_ARGV 1 F "RAM_ONLY;IMAGE_CRC" "MIN_STACK_SIZE;JLINK_PROBE;HEAP_SIZE" "LIBRARIES")
    kvasir_forward_arguments(_out F "RAM_ONLY;IMAGE_CRC" "MIN_STACK_SIZE;JLINK_PROBE;HEAP_SIZE" "LIBRARIES")
    expect("forward ${ARGN}" "${_out}" "${want}")
endfunction()
round_trip("IMAGE_CRC;MIN_STACK_SIZE;8k" MIN_STACK_SIZE 8k IMAGE_CRC)
# the swallow trap: LIBRARIES stops at MIN_STACK_SIZE because the parser knows it
round_trip("MIN_STACK_SIZE;8k;LIBRARIES;a;b" LIBRARIES a b MIN_STACK_SIZE 8k)
# an empty one-value keyword is forwarded, and the receiver sees it DEFINED and empty
round_trip("RAM_ONLY;JLINK_PROBE;;HEAP_SIZE;2k" JLINK_PROBE "" RAM_ONLY HEAP_SIZE 2k)
function(receive)
    cmake_parse_arguments(PARSE_ARGV 0 R "RAM_ONLY" "JLINK_PROBE;HEAP_SIZE" "")
    if(NOT DEFINED R_JLINK_PROBE
       OR NOT "${R_JLINK_PROBE}" STREQUAL ""
       OR NOT R_RAM_ONLY
       OR NOT R_HEAP_SIZE STREQUAL "2k")
        message(SEND_ERROR "received JLINK_PROBE '${R_JLINK_PROBE}', "
                           "RAM_ONLY '${R_RAM_ONLY}', HEAP_SIZE '${R_HEAP_SIZE}'")
    endif()
endfunction()
function(forward_then_receive)
    cmake_parse_arguments(PARSE_ARGV 0 F "RAM_ONLY" "JLINK_PROBE;HEAP_SIZE" "")
    kvasir_forward_arguments(_out F "RAM_ONLY" "JLINK_PROBE;HEAP_SIZE" "")
    receive(${_out})
endfunction()
forward_then_receive(JLINK_PROBE "" RAM_ONLY HEAP_SIZE 2k)
round_trip("" "")
