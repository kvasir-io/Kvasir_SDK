# The end of both toolchain files: the flag lists joined into CMAKE_<LANG>_FLAGS, and the per-configuration and linker
# flag strings left empty (a variant's flags come from the variant table, never from CMAKE_BUILD_TYPE).

list(JOIN cxx_flags " " CMAKE_CXX_FLAGS)
list(JOIN c_flags " " CMAKE_C_FLAGS)
list(JOIN asm_flags " " CMAKE_ASM_FLAGS)

foreach(_lang CXX C ASM)
    foreach(_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_${_lang}_FLAGS_${_config} "")
    endforeach()
endforeach()
set(CMAKE_EXE_LINKER_FLAGS "")
