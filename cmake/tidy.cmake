set(
  USE_TIDY
  false
  CACHE BOOL "clang tidy"
)

if(USE_TIDY)
  find_program(
    CLANG_TIDY_EXE
    NAMES "clang-tidy"
    DOC "Path to clang-tidy executable"
  )

  mark_as_advanced(FORCE CLANG_TIDY_EXE)

  if(NOT CLANG_TIDY_EXE)
    message(FATAL_ERROR "clang-tidy not found.")
  else()
    set(
      common_clang_tidy
      ${CLANG_TIDY_EXE}
      -p=.
    )

    set(common_clang_tidy_disabled_checks ${GLOBAL_COMMON_CLANG_TIDY_DISABLED_CHECKS})

    set(
      cxx_clang_tidy_disabled_checks
      cppcoreguidelines-pro-type-reinterpret-cast
      readability-named-parameter
      modernize-concat-nested-namespaces
      modernize-use-trailing-return-type
      cppcoreguidelines-macro-usage
      cppcoreguidelines-pro-bounds-constant-array-index
      cppcoreguidelines-non-private-member-variables-in-classes
      cppcoreguidelines-pro-bounds-pointer-arithmetic
      bugprone-reserved-identifier
      bugprone-incorrect-roundings
      
      cppcoreguidelines-c-copy-assignment-signature #already in misc-unconventional-assign-operator
      cppcoreguidelines-avoid-c-arrays #already in modernize-avoid-c-arrays
      cppcoreguidelines-avoid-magic-numbers #already in readability-magic-numbers
      
      readability-misleading-indentation #TODO remove when constexpr bug is fixed
      readability-braces-around-statements  #TODO remove when constexpr bug is fixed
      bugprone-suspicious-semicolon #TODO remove when constexpr bug is fixed
      
      cppcoreguidelines-pro-type-member-init #TODO remove when template delegation constructor bug is fixed
      modernize-avoid-c-arrays #TODO remove when template function to get size is fixed
      
      readability-magic-numbers #TODO improve
      cppcoreguidelines-pro-type-vararg #TODO improve
      modernize-use-nodiscard #TODO improve
      
      bugprone-dynamic-static-initializers
      cppcoreguidelines-avoid-non-const-global-variables
      readability-qualified-auto
      ${GLOBAL_CXX_CLANG_TIDY_DISABLED_CHECKS}
    )

    set(
      enabled_clang_tidy_checks
      bugprone*
      clang-analyzer*
      cppcoreguidelines*
      #modernize*
      performance*
      #readability*
    )

    list(JOIN enabled_clang_tidy_checks "," enabled_clang_tidy_checks_)


    set(c_clang_tidy_disabled_checks ${GLOBAL_C_CLANG_TIDY_DISABLED_CHECKS})

    list(TRANSFORM common_clang_tidy_disabled_checks PREPEND "-")
    list(JOIN common_clang_tidy_disabled_checks "," common_clang_tidy_disabled_checks_)

    list(TRANSFORM cxx_clang_tidy_disabled_checks PREPEND "-")
    list(JOIN cxx_clang_tidy_disabled_checks "," cxx_clang_tidy_disabled_checks_)

    list(TRANSFORM c_clang_tidy_disabled_checks PREPEND "-")
    list(JOIN c_clang_tidy_disabled_checks "," c_clang_tidy_disabled_checks_)

    list(JOIN common_clang_tidy ";" clang_tidy)
    set(
      DO_CLANG_TIDY_CXX
      ${clang_tidy} -header-filter=.hpp -checks=-*,${enabled_clang_tidy_checks_},${common_clang_tidy_disabled_checks_},${cxx_clang_tidy_disabled_checks_}
    )
    set(
      DO_CLANG_TIDY_C
      ${clang_tidy} -header-filter=.h -checks=-*,${enabled_clang_tidy_checks_},${common_clang_tidy_disabled_checks_},${c_clang_tidy_disabled_checks_}
    )
  endif()
endif()

function(target_add_tidy_flags target)
  if(USE_TIDY)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${DO_CLANG_TIDY_CXX}")
    #        set_target_properties(${target} PROPERTIES C_CLANG_TIDY  "${DO_CLANG_TIDY_C}")
  endif()
endfunction()
