# kvasir_quarantine_interface(<target> [SYSTEM] [STRIP_OPTIONS <regex>...]): a third-party target's usage requirements
# made safe for code compiled with the SDK's warning set. Entries of its INTERFACE_COMPILE_OPTIONS matching a
# STRIP_OPTIONS regex are removed (an option such as -Wno-missing-braces would reach every consumer's own code; a
# generator-expression entry that wraps it is matched as a whole), and with SYSTEM its include directories become system
# ones (its headers' warnings are not the consumer's to fix). An ALIAS is followed to its real target (an ALIAS cannot
# be set); a target that does not exist or is not visible here (imported in another directory) is left alone.
include_guard(GLOBAL)

function(kvasir_quarantine_interface target)
    cmake_parse_arguments(PARSE_ARGV 1 A "SYSTEM" "" "STRIP_OPTIONS")
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "kvasir_quarantine_interface: unknown argument(s) ${A_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET ${target})
        return()
    endif()
    get_target_property(_real ${target} ALIASED_TARGET)
    if(_real)
        set(target ${_real})
    endif()
    if(A_STRIP_OPTIONS)
        get_target_property(_opts ${target} INTERFACE_COMPILE_OPTIONS)
        if(_opts)
            foreach(_re IN LISTS A_STRIP_OPTIONS)
                list(FILTER _opts EXCLUDE REGEX "${_re}")
            endforeach()
            set_property(TARGET ${target} PROPERTY INTERFACE_COMPILE_OPTIONS "${_opts}")
        endif()
    endif()
    if(A_SYSTEM)
        get_target_property(_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_dirs)
            target_include_directories(${target} SYSTEM INTERFACE ${_dirs})
        endif()
    endif()
endfunction()
