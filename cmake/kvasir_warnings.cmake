# The warning set of Kvasir code, one list per compiler, for the firmware and for host tests alike: the toolchain files
# (arm_clang.cmake, arm_gcc.cmake) take their lists from here into kvasir_image_warning_flags, which util.cmake's
# kvasir::warnings carries into each image's own code (never into CMAKE_<LANG>_FLAGS: the runtime libraries and fetched
# dependencies keep the compiler's defaults), and a host test tree calls kvasir_host_test_flags(), so the two cannot
# drift apart. Sets variables and defines a function only; safe to include more than once.

set(kvasir_clang_warning_flags
    -Weverything
    -Wno-switch-default
    -Wnan-infinity-disabled
    -Wno-c++98-compat
    -Wno-c++98-compat-pedantic
    -Wno-c++20-compat
    -Wno-pre-c2x-compat
    -Wno-pre-c11-compat
    -pedantic-errors
    -Wno-padded
    -Wno-covered-switch-default
    -Wno-switch-enum
    -Wno-class-varargs
    -Wno-gnu-zero-variadic-macro-arguments
    -Wno-float-equal
    -Wno-unknown-warning-option
    -Wno-reserved-identifier
    -ftemplate-backtrace-limit=0
    -fmacro-backtrace-limit=0
    # was for llvm-libc (src/string/strnlen_s.cpp compares a pointer with 0), which no longer builds with this list;
    # kept so the images' warning set stays as it was
    -Wno-zero-as-null-pointer-constant
    -Wno-documentation-unknown-command
    -Wno-documentation
    -Wno-declaration-after-statement
    -Wno-braced-scalar-init
    -Wno-unsafe-buffer-usage
    -Wno-nrvo
    # -Wlifetime-safety-* is part of -Weverything; the two suggestion groups only propose [[clang::lifetimebound]] marks
    # (hundreds, mostly in fetched fmt and llvm-libc). The checks that find dangling references stay on.
    -Wno-lifetime-safety-intra-tu-suggestions
    -Wno-lifetime-safety-intra-tu-constructor-suggestions)

set(kvasir_gcc_warning_flags
    -Wall
    -Wextra
    # errors, matching arm_clang.cmake: the ISO-conformance issues this catches (the extern "C" main declaration, the
    # __STDC_HOSTED__ redefine this flag was once disabled for) must fail the build, not scroll past as warnings
    -pedantic-errors
    -Wdouble-promotion
    -Wcast-align
    -Wcast-qual
    -Wdisabled-optimization
    -Wformat=2
    -Winit-self
    -Wlogical-op
    -Wredundant-decls
    -Wshadow
    # level 5 is too noisy: it fires inside libc++'s to_chars and on the KVASIR_START vector read
    -Wstrict-overflow=2
    -Wundef
    -Wno-unused
    # gcc, unlike clang, fires this for the unnameable base-class subobject of designated-initialized aggregates
    -Wno-missing-field-initializers
    -Wstrict-aliasing=1)

# kvasir_host_test_flags(<target> [CLANG_OPTIONS <option>...] [GCC_OPTIONS <option>...])
#
# The firmware's warnings and language flags for a host test target (no exceptions, no RTTI), plus what only a host test
# needs: static fixtures are fine there (-Wno-exit-time-destructors -Wno-global-constructors), gcc adds -Wconversion
# -Wsign-conversion (clang has them in -Weverything), and clang builds with ASan and UBSan that stop at the first
# report. Set UBSAN_OPTIONS=halt_on_error=1 on the tests as well.
function(kvasir_host_test_flags target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "CLANG_OPTIONS;GCC_OPTIONS")
    set(language_flags -fno-exceptions -fno-rtti)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(flags ${kvasir_clang_warning_flags})
        set(sanitizers -fsanitize=address,undefined -fno-sanitize-recover=all)
        target_compile_options(
            ${target}
            PRIVATE ${flags}
                    -Wno-exit-time-destructors
                    -Wno-global-constructors
                    ${language_flags}
                    ${sanitizers}
                    -fno-omit-frame-pointer
                    ${arg_CLANG_OPTIONS})
        target_link_options(${target} PRIVATE ${sanitizers})
    else()
        target_compile_options(${target} PRIVATE ${kvasir_gcc_warning_flags} -Wconversion -Wsign-conversion
                                                 ${language_flags} ${arg_GCC_OPTIONS})
    endif()
endfunction()
