# Common Compiler Configuration Shared compiler flags and settings used across all compiler toolchains Defines
# optimization, warning, and language standard settings

set(compiler_common_flags
    -ffunction-sections
    -fdata-sections
    -fno-common
    -fomit-frame-pointer
    -fmerge-all-constants
    -fstack-protector-strong
    -Wno-unused-macros)

set(compiler_common_cxx_flags
    -std=c++23
    -fno-exceptions
    -fno-unwind-tables
    -fno-use-cxa-atexit
    -fno-rtti
    -fno-threadsafe-statics
    -fstrict-enums)

set(compiler_common_c_flags -std=c23)

set(compiler_common_asm_flags)

set(linker_common_flags --static --Bstatic --gc-sections --entry=ResetISR)
