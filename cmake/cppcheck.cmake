# cppcheck over the images (USE_CPPCHECK=ON): kvasir_add_cppcheck_flags(<target>) sets the target's C/CXX_CPPCHECK.
# Extra suppressions: GLOBAL_{COMMON,CXX,C}_CPPCHECK_DISABLED_CHECKS. The SDK's own name beside cmake_helpers'
# target_add_cppcheck_flags, which the host builds of the same configure load (as tidy.cmake).

set(USE_CPPCHECK
    false
    CACHE BOOL "cppcheck")

if(USE_CPPCHECK)
    find_program(
        CPPCHECK_EXE
        NAMES "cppcheck"
        DOC "Path to cppcheck executable")

    mark_as_advanced(FORCE CPPCHECK_EXE)

    if(NOT CPPCHECK_EXE)
        message(FATAL_ERROR "cppcheck not found.")
    else()
        set(common_cppcheck ${CPPCHECK_EXE} --check-config -q --enable=all --suppress=*:*third_party \*)

        set(common_cppcheck_disabled_checks unknownMacro missingIncludeSystem unusedFunction unmatchedSuppression
                                            ${GLOBAL_COMMON_CPPCHECK_DISABLED_CHECKS})

        set(cxx_cppcheck_disabled_checks ${GLOBAL_CXX_CPPCHECK_DISABLED_CHECKS})
        set(c_cppcheck_disabled_checks ${GLOBAL_C_CPPCHECK_DISABLED_CHECKS})

        list(TRANSFORM common_cppcheck_disabled_checks PREPEND "--suppress=")
        list(JOIN common_cppcheck_disabled_checks ";" common_cppcheck_disabled_checks_)

        list(TRANSFORM cxx_cppcheck_disabled_checks PREPEND "--suppress=")
        list(JOIN cxx_cppcheck_disabled_checks ";" cxx_cppcheck_disabled_checks_)

        list(TRANSFORM c_cppcheck_disabled_checks PREPEND "--suppress=")
        list(JOIN c_cppcheck_disabled_checks ";" c_cppcheck_disabled_checks_)

        list(JOIN common_cppcheck ";" cppcheck)

        set(KVASIR_CPPCHECK_CXX ${cppcheck} ${common_cppcheck_disabled_checks_} ${cxx_cppcheck_disabled_checks_})

        set(KVASIR_CPPCHECK_C ${cppcheck} ${common_cppcheck_disabled_checks_} ${c_cppcheck_disabled_checks_})
    endif()
endif()

function(kvasir_add_cppcheck_flags target)
    if(USE_CPPCHECK)
        set_target_properties(${target} PROPERTIES CXX_CPPCHECK "${KVASIR_CPPCHECK_CXX}")
        set_target_properties(${target} PROPERTIES C_CPPCHECK "${KVASIR_CPPCHECK_C}")
    endif()
endfunction()
