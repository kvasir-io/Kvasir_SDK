# The runtime archives (compiler-rt, libc++, llvm-libc, its heap, the profile runtime) are built once per tree and
# optimisation set and shared by every image of it: util.cmake's target_add_shared_kvasir_lib. Their compile flags are a
# function of the optimisation set and the sanitizer within one tree (the toolchain file's optimize_option_<set> and
# sanitize_option), so the archive is named by those two; a second image that would compile one archive with other flags
# is refused instead of silently linking the first image's copy. Script-safe (no target is made here): tests/
# cmake_api/test_runtime_archives.cmake runs it with cmake -P.

# kvasir_runtime_archive_name(<out> <lib> <strategy> <sanitize flags>): kvasir_<lib>_<strategy>, _san appended when the
# sanitize flags are not empty
function(kvasir_runtime_archive_name out lib strategy san_flags)
    if(NOT strategy MATCHES "^(size|speed|debug)$")
        message(FATAL_ERROR "runtime archive ${lib}: optimisation set '${strategy}': size, speed or debug")
    endif()
    set(_name kvasir_${lib}_${strategy})
    if(NOT "${san_flags}" STREQUAL "")
        string(APPEND _name _san)
    endif()
    set(${out}
        ${_name}
        PARENT_SCOPE)
endfunction()

# kvasir_runtime_archive_claim(<archive> <flags> <image>): records the first image's flags for <archive>; returns
# <archive>_NEW true for that first claim (the caller makes the target then). A later claim with other flags is a
# FATAL_ERROR naming both images and both flag lists.
function(kvasir_runtime_archive_claim archive flags image)
    get_property(
        _known GLOBAL
        PROPERTY KVASIR_ARCHIVE_FLAGS_${archive}
        SET)
    if(NOT _known)
        set_property(GLOBAL PROPERTY KVASIR_ARCHIVE_FLAGS_${archive} "${flags}")
        set_property(GLOBAL PROPERTY KVASIR_ARCHIVE_FIRST_${archive} "${image}")
        set(${archive}_NEW
            TRUE
            PARENT_SCOPE)
        return()
    endif()
    get_property(_first_flags GLOBAL PROPERTY KVASIR_ARCHIVE_FLAGS_${archive})
    if(NOT "${_first_flags}" STREQUAL "${flags}")
        get_property(_first GLOBAL PROPERTY KVASIR_ARCHIVE_FIRST_${archive})
        message(
            FATAL_ERROR
                "runtime archive ${archive}: ${image} compiles it with '${flags}', but ${_first} "
                "already made it with '${_first_flags}' (the flags of an optimisation set and the "
                "sanitizer must be the same for every image of one tree)")
    endif()
    set(${archive}_NEW
        FALSE
        PARENT_SCOPE)
endfunction()
