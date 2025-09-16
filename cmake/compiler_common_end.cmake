# Compiler Configuration Finalization Finalizes compiler flag setup by converting lists to CMake flag strings Sets all
# compiler and linker flags for different build configurations

set(emptylist)

list(JOIN cxx_flags " " CMAKE_CXX_FLAGS)
list(JOIN c_flags " " CMAKE_C_FLAGS)
list(JOIN asm_flags " " CMAKE_ASM_FLAGS)

list(JOIN emptylist " " CMAKE_CXX_FLAGS_DEBUG)
list(JOIN emptylist " " CMAKE_CXX_FLAGS_RELEASE)
list(JOIN emptylist " " CMAKE_CXX_FLAGS_RELWITHDEBINFO)
list(JOIN emptylist " " CMAKE_CXX_FLAGS_MINSIZEREL)

list(JOIN emptylist " " CMAKE_C_FLAGS_DEBUG)
list(JOIN emptylist " " CMAKE_C_FLAGS_RELEASE)
list(JOIN emptylist " " CMAKE_C_FLAGS_RELWITHDEBINFO)
list(JOIN emptylist " " CMAKE_C_FLAGS_MINSIZEREL)

list(JOIN emptylist " " CMAKE_ASM_FLAGS_DEBUG)
list(JOIN emptylist " " CMAKE_ASM_FLAGS_RELEASE)
list(JOIN emptylist " " CMAKE_ASM_FLAGS_RELWITHDEBINFO)
list(JOIN emptylist " " CMAKE_ASM_FLAGS_MINSIZEREL)

list(JOIN emptylist " " CMAKE_EXE_LINKER_FLAGS)
