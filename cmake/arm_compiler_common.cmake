# ARM Compiler Common Configuration Shared settings for ARM cross-compilation toolchains Sets up system properties and
# common compiler flags for embedded ARM targets

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
set(CMAKE_CROSSCOMPILING True)

set(arm_compiler_common_flags -ffreestanding)

set(SPEC_REPLACEMENT_STRING spec_replace)
set(SPEC_REPLACEMENT_EMPTY_MARKER NOTHING_TO_REPLACE)
