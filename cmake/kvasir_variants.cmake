# The variant table and the keyword table of target_configure_kvasir / kvasir_executable_variants. Pure list code,
# included by util.cmake and testable with `cmake -P` (tests/cmake_api/test_variants.cmake).

# Every keyword target_configure_kvasir takes besides VARIANT and OPTIMIZATION_STRATEGY (the variant row and the wrapper
# own those two) - and so every keyword kvasir_executable_variants forwards to it. Both functions parse this one table:
# forwarding "${ARGN} minus the wrapper's own keywords" instead lets a multi-value keyword (SOURCES, LIBRARIES) swallow
# every word up to the next keyword its parser knows, so `LIBRARIES l MIN_STACK_SIZE 8k` would link -lMIN_STACK_SIZE.
set(KVASIR_CONFIGURE_OPTIONS RAM_ONLY SCRATCH_BANKS IMAGE_CRC)
set(KVASIR_CONFIGURE_ONE_VALUE
    MIN_STACK_SIZE
    CORE1_STACK_SIZE
    CORE1_STACK_PLACEMENT
    HEAP_SIZE
    LINKER_FILE
    LOG_FILTER
    LOG_REFLECTED_STRUCTS
    CONFIG_HEADER
    JLINK_IP
    JLINK_PROBE
    MIN_LOG_LEVEL
    MIN_LOG_LEVEL_DEBUG
    MIN_LOG_LEVEL_RELEASE
    SELFTEST_VARIANT
    CHECK_LEVEL
    CHECK_LEVEL_RELEASE
    DCHECK_LEVEL
    DCHECK_LEVEL_RELEASE)
set(KVASIR_CONFIGURE_MULTI_VALUE "")

# The variants kvasir_executable_variants makes by default, in this order; coverage is made only with COVERAGE.
set(KVASIR_VARIANTS debug release release_log sanitize)

# kvasir_forward_arguments(<out> <prefix> <options> <one-value> <multi-value>): the keywords cmake_parse_arguments put
# into <prefix>_* back as an argument list, in table order - an option when TRUE, a one-value keyword when DEFINED (its
# value as one word), a multi-value keyword with its values. A one-value keyword given "" is DEFINED and empty (CMP0174
# NEW, kvasir.cmake) and is forwarded: the receiver, under NEW too, sees it DEFINED and empty, as the caller wrote it.
function(kvasir_forward_arguments out prefix options one multi)
    set(_r "")
    foreach(_k IN LISTS options)
        if(${prefix}_${_k})
            list(APPEND _r ${_k})
        endif()
    endforeach()
    foreach(_k IN LISTS one)
        if(DEFINED ${prefix}_${_k})
            list(APPEND _r ${_k} "${${prefix}_${_k}}")
        endif()
    endforeach()
    foreach(_k IN LISTS multi)
        if(DEFINED ${prefix}_${_k})
            list(APPEND _r ${_k} ${${prefix}_${_k}})
        endif()
    endforeach()
    set(${out}
        "${_r}"
        PARENT_SCOPE)
endfunction()

# kvasir_variant_row(<variant> <prefix>): the variant's policy, one row each, as <prefix>_<column> in the caller's
# scope: EXCLUDE   TRUE: not in ALL (built when named, or by its flash/log target) LOG       TRUE: USE_UC_LOG SAN TRUE:
# the sanitizer OPT       the default optimisation (kvasir_executable_variants: DEBUG_OPTIMIZATION / OPTIMIZATION
# instead) FLOOR     the log floor's keyword: debug (MIN_LOG_LEVEL_DEBUG, else MIN_LOG_LEVEL), release
# (MIN_LOG_LEVEL_RELEASE, else MIN_LOG_LEVEL) or none LEVELS    debug (CHECK_LEVEL, DCHECK_LEVEL) or release
# (CHECK_LEVEL_RELEASE, DCHECK_LEVEL_RELEASE) ESCALATE  KVASIR_SOFT_CHECK_ESCALATE: a failed KVASIR_SOFT_CHECK panics
# instead of logging SELFTEST  ON: by SELFTEST_VARIANT; OFF: never (coverage)
function(kvasir_variant_row v p)
    # EXCLUDE LOG  SAN  OPT   FLOOR   LEVELS  ESCALATE SELFTEST
    if(v STREQUAL "debug")
        set(_row
            TRUE
            TRUE
            FALSE
            debug
            debug
            debug
            0
            ON)
    elseif(v STREQUAL "release")
        set(_row
            FALSE
            FALSE
            FALSE
            size
            none
            release
            0
            ON)
    elseif(v STREQUAL "release_log")
        set(_row
            FALSE
            TRUE
            FALSE
            size
            release
            release
            0
            ON)
    elseif(v STREQUAL "sanitize")
        set(_row
            FALSE
            TRUE
            TRUE
            size
            release
            debug
            1
            ON)
    elseif(v STREQUAL "coverage")
        set(_row
            TRUE
            TRUE
            FALSE
            size
            release
            release
            0
            OFF)
    else()
        message(FATAL_ERROR "VARIANT ${v}: one of ${KVASIR_VARIANTS} coverage")
    endif()
    set(_i 0)
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
        list(GET _row ${_i} _value)
        set(${p}_${_column}
            ${_value}
            PARENT_SCOPE)
        math(EXPR _i "${_i} + 1")
    endforeach()
endfunction()

# kvasir_variant_policy(<variant> <prefix> <parsed prefix>): the row of <variant> resolved against the keywords parsed
# into <parsed prefix>_*: <prefix>_MIN_LOG_LEVEL (empty: none), <prefix>_CHECK, <prefix>_DCHECK, <prefix>_ESCALATE,
# <prefix>_SELFTEST (0/1), plus the row's columns. The checks (kvasir/Runtime/Check.hpp): 0 off, 1 bare (panic, no log),
# 2 full (the operands logged); CHECK_LEVEL defaults to 2, CHECK_LEVEL_RELEASE to CHECK_LEVEL, DCHECK_LEVEL to
# CHECK_LEVEL and DCHECK_LEVEL_RELEASE to 0. Compile-time self-tests (kvasir/Util/SelfTest.hpp) prove the same thing in
# every variant, so only one evaluates them (they are slow to compile): SELFTEST_VARIANT, debug unless named (all,
# none).
function(kvasir_variant_policy v p a)
    kvasir_variant_row(${v} _row)
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
        set(${p}_${_column}
            ${_row_${_column}}
            PARENT_SCOPE)
    endforeach()

    set(_floor "")
    if(_row_FLOOR STREQUAL "debug")
        set(_floor "${${a}_MIN_LOG_LEVEL}")
        if(DEFINED ${a}_MIN_LOG_LEVEL_DEBUG)
            set(_floor "${${a}_MIN_LOG_LEVEL_DEBUG}")
        endif()
    elseif(_row_FLOOR STREQUAL "release")
        set(_floor "${${a}_MIN_LOG_LEVEL}")
        if(DEFINED ${a}_MIN_LOG_LEVEL_RELEASE)
            set(_floor "${${a}_MIN_LOG_LEVEL_RELEASE}")
        endif()
    endif()
    set(${p}_MIN_LOG_LEVEL
        "${_floor}"
        PARENT_SCOPE)

    foreach(_level CHECK_LEVEL CHECK_LEVEL_RELEASE DCHECK_LEVEL DCHECK_LEVEL_RELEASE)
        if(DEFINED ${a}_${_level} AND NOT ${a}_${_level} MATCHES "^[012]$")
            message(FATAL_ERROR "${_level} ${${a}_${_level}}: 0 (off), 1 (bare) or 2 (full)")
        endif()
    endforeach()
    set(_check 2)
    if(DEFINED ${a}_CHECK_LEVEL)
        set(_check ${${a}_CHECK_LEVEL})
    endif()
    if(_row_LEVELS STREQUAL "debug")
        set(_dcheck ${_check})
        if(DEFINED ${a}_DCHECK_LEVEL)
            set(_dcheck ${${a}_DCHECK_LEVEL})
        endif()
    else()
        if(DEFINED ${a}_CHECK_LEVEL_RELEASE)
            set(_check ${${a}_CHECK_LEVEL_RELEASE})
        endif()
        set(_dcheck 0)
        if(DEFINED ${a}_DCHECK_LEVEL_RELEASE)
            set(_dcheck ${${a}_DCHECK_LEVEL_RELEASE})
        endif()
    endif()
    set(${p}_CHECK
        ${_check}
        PARENT_SCOPE)
    set(${p}_DCHECK
        ${_dcheck}
        PARENT_SCOPE)
    set(${p}_ESCALATE
        ${_row_ESCALATE}
        PARENT_SCOPE)

    set(_selftest_variant debug)
    if(DEFINED ${a}_SELFTEST_VARIANT)
        set(_selftest_variant ${${a}_SELFTEST_VARIANT})
    endif()
    if(NOT _selftest_variant IN_LIST KVASIR_VARIANTS
       AND NOT _selftest_variant STREQUAL "all"
       AND NOT _selftest_variant STREQUAL "none")
        message(FATAL_ERROR "SELFTEST_VARIANT ${_selftest_variant}: one of ${KVASIR_VARIANTS}, all, none")
    endif()
    set(_selftest 0)
    if(_row_SELFTEST AND (_selftest_variant STREQUAL v OR _selftest_variant STREQUAL "all"))
        set(_selftest 1)
    endif()
    set(${p}_SELFTEST
        ${_selftest}
        PARENT_SCOPE)
endfunction()
