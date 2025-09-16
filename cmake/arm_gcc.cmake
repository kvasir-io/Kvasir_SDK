# ARM GCC Compiler Configuration Configures the build environment for ARM targets using GCC toolchain Uses
# arm-none-eabi-gcc cross-compilation tools

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/arm_compiler_common.cmake)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_SIZE arm-none-eabi-size)
set(CMAKE_STRIP arm-none-eabi-strip)
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_NM arm-none-eabi-nm)
set(CMAKE_RANLIB arm-none-eabi-ranlib)
set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP arm-none-eabi-objdump)

find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)

set(CMAKE_C_LINK_EXECUTABLE "${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/two_stage_link.py ${CMAKE_SIZE} \
<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

set(CMAKE_CXX_LINK_EXECUTABLE "${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/two_stage_link.py ${CMAKE_SIZE} \
<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

if(TARGET_FPU MATCHES none)
    set(target_fpu auto)
else()
    set(target_fpu ${TARGET_FPU})
endif()

set(target_flags -mfloat-abi=${TARGET_FLOAT_ABI} -mfpu=${target_fpu} -mcpu=${TARGET_CPU} -march=arm${TARGET_ARCH}
                 -m${TARGET_ARM_INSTRUCTION_MODE} -m${TARGET_ENDIAN})

set(optimize_option_common -ggdb3 -flto)

set(optimize_option_speed ${optimize_option_common} -Ofast)
set(optimize_option_size ${optimize_option_common} -Os)
set(optimize_option_debug ${optimize_option_common} -Og)

set(optimize_specs_speed "nosys")
set(optimize_specs_size "nano")
set(optimize_specs_debug "nano")

set(common_warning_flags
    -Wall
    -Wextra
    -pedantic
    # -pedantic-errors
    -Wdouble-promotion
    -Wcast-align
    -Wcast-qual
    -Wdisabled-optimization
    -Wformat=2
    -Winit-self
    -Wlogical-op
    -Wredundant-decls
    -Wshadow
    -Wstrict-overflow=5
    -Wundef
    -Wno-unused
    -Wstrict-aliasing=1)

set(profile_flags -U__STDC_HOSTED__ -D__STDC_HOSTED__=1)

set(common_flags ${target_flags} ${common_warning_flags} ${profile_flags} ${compiler_common_flags}
                 ${arm_compiler_common_flags})

set(cxx_flags ${common_flags} ${compiler_common_cxx_flags} -Woverloaded-virtual -Wsign-promo -Wstrict-null-sentinel)

set(c_flags ${common_flags} ${compiler_common_c_flags})

set(LINKER_PREFIX "-Wl,")

list(TRANSFORM linker_common_flags PREPEND ${LINKER_PREFIX})

set(linker_flags ${linker_common_flags} -nostartfiles --specs=${SPEC_REPLACEMENT_STRING}.specs -Wl,--start-group
                 -Wl,--end-group)

include(${CMAKE_CURRENT_LIST_DIR}/compiler_common_end.cmake)
