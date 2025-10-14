# Kvasir Utility Functions Core utility functions and target configuration for Kvasir framework Provides build system
# integration, size reporting, and target setup

include(${KVASIR_ROOT_DIR}/cmake_git_version/CMakeLists.txt)

find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)

include(${kvasir_cmake_dir}/jlink.cmake)

# Add kvasir_devices subdirectory if it exists and contains CMakeLists.txt
if(IS_DIRECTORY "${KVASIR_DEVICES_ROOT_DIR}" AND EXISTS "${KVASIR_DEVICES_ROOT_DIR}/CMakeLists.txt")
    add_subdirectory(${KVASIR_DEVICES_ROOT_DIR} ${CMAKE_BINARY_DIR}/kvasir_devices)
    message(STATUS "Kvasir: Added kvasir_devices subdirectory from ${KVASIR_DEVICES_ROOT_DIR}")
endif()

function(add_clean_file target file)
    get_target_property(cur_additional_clean_files ${target} ADDITIONAL_CLEAN_FILES)
    if("${cur_additional_clean_files}" MATCHES NOTFOUND)
        set(new_additional_clean_files ${file})
    else()
        list(APPEND cur_additional_clean_files ${file})
        set(new_additional_clean_files ${cur_additional_clean_files})
    endif()
    set_target_properties(${target} PROPERTIES ADDITIONAL_CLEAN_FILES "${new_additional_clean_files}")
endfunction()

function(print_size target linker_file)
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_SIZE} -x --format=sysv "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
        COMMAND
            ${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/pretty_size.py "${CMAKE_SIZE}"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf" "${TARGET_FLASH_SIZE}" "${TARGET_RAM_SIZE}"
            "${TARGET_EEPROM_SIZE}" "${linker_file}")
endfunction()

function(generate_object target suffix type)

    list(TRANSFORM TARGET_EXTRA_FLASH_SECTIONS PREPEND "--only-section=" OUTPUT_VARIABLE extra_flash_sections)

    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            ${CMAKE_OBJCOPY} --output-target ${type} --only-section=.data --only-section=.text ${extra_flash_sections}
            "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf" "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash${suffix}"
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_flash${suffix})

    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --output-target ${type} --only-section=.eeprom
                "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf" "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom${suffix}"
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom${suffix})

    if(${type} MATCHES ihex)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${CMAKE_OBJCOPY} --output-target ${type} --only-section=.data --only-section=.text
                --only-section=.eeprom ${extra_flash_sections} "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash${suffix}"
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash${suffix})

        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/ihex_to_uf2.py
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom${suffix}"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom.uf2" ${TARGET_UF2_CODE}
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom.uf2)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/ihex_to_uf2.py
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash${suffix}" "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.uf2"
                ${TARGET_UF2_CODE}
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.uf2)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/ihex_to_uf2.py
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash${suffix}"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash.uf2" ${TARGET_UF2_CODE}
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash.uf2)
    endif()
endfunction()

function(generate_lst target)
    list(TRANSFORM TARGET_EXTRA_FLASH_SECTIONS PREPEND "--section=" OUTPUT_VARIABLE extra_flash_sections)
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_OBJDUMP} --section=.text --section=.data ${extra_flash_sections} --disassemble --demangle
                --all-headers "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf" > "${CMAKE_CURRENT_BINARY_DIR}/${target}.lst"
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}.lst)
endfunction()

function(add_bootloader_app_data target application)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/${target})
    get_target_property(app_bin_dir ${application} BINARY_DIR)
    add_custom_command(
        TARGET ${application}
        COMMAND
            ${Python3_EXECUTABLE} ${kvasir_cmake_dir}/tools/gen_app_data_for_bootloader.py
            ${app_bin_dir}/${application}_flash.bin
            ${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/bootloader/app_data.inl DEPENDS
            ${kvasir_cmake_dir}/tools/gen_app_data_for_bootloader.py
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/bootloader/app_data.inl)

    add_dependencies(${target} ${application})
endfunction()

function(add_target_linker_dependency target dep)
    get_target_property(cur_link_deps ${target} LINK_DEPENDS)
    if("${cur_link_deps}" MATCHES NOTFOUND)
        set(new_link_deps ${dep})
    else()
        list(APPEND cur_link_deps ${dep})
        set(new_link_deps ${cur_link_deps})
    endif()
    set_target_properties(${target} PROPERTIES LINK_DEPENDS "${new_link_deps}")
endfunction()

function(kvasir_set_target_property target property value)
    if(property STREQUAL LOG)
        if(value STREQUAL TRUE)
            target_compile_definitions(${target} PUBLIC USE_UC_LOG)
        elseif(value STREQUAL FALSE)

        else()
            message(FATAL_ERROR "wrong LOG option specified!!!!")
        endif()
    elseif(property STREQUAL ASSERT)
        if(value STREQUAL TRUE)
            target_compile_definitions(${target} PUBLIC INPUT_USE_KASSERT)
        elseif(value STREQUAL FALSE)

        else()
            message(FATAL_ERROR "wrong ASSERT option specified!!!!")
        endif()
    else()
        message(FATAL_ERROR "wrong property option specified!!!!")
    endif()

endfunction()

enable_language(ASM)

include(${kvasir_cmake_dir}/../lib/compiler-rt/compiler-rt.cmake)
include(${kvasir_cmake_dir}/../lib/libcxx/libcxx.cmake)
include(${kvasir_cmake_dir}/../lib/libc/libc.cmake)

if("${CPPLIB}" STREQUAL "libstdc++")
    if("${CLIB}" STREQUAL "llvm")
        message(FATAL_ERROR "libstdc++ does not work with llvm libc")
    endif()
endif()

function(target_add_kvasir_lib target name sources)
    set(libraries
        ${libraries} "${name}-${target}"
        PARENT_SCOPE)
    add_library("${name}-${target}" ${sources})
endfunction()

function(
    target_kvasir_config_internal
    name
    min_stack_size
    heap_size
    optimize
    sanitize
    linker_file
    application
    bootloader)
    set(libraries)

    if("${COMPILER_RT}" STREQUAL "clang")
        target_add_kvasir_lib(${name} rt "${COMPILER_RT_SOURCE_FILES}")
    endif()

    if("${CPPLIB}" STREQUAL "libc++")
        target_add_kvasir_lib(${name} cxx "${LIBCXX_SOURCE_FILES}")
    endif()

    if("${CLIB}" STREQUAL "llvm")
        target_add_kvasir_lib(${name} c "${LIBC_SOURCE_FILES}")
    endif()

    set_target_properties(${name} PROPERTIES SUFFIX ".elf")

    target_link_libraries(${name} peripherals)
    target_link_libraries(${name} core_peripherals)
    target_link_libraries(${name} uc_log::uc_log)
    target_include_directories(${name} PUBLIC ${KVASIR_ROOT_DIR}/src)
    target_include_directories(${name} PUBLIC ${CHIP_ROOT_DIR}/src)
    target_include_directories(${name} PUBLIC ${CHIP_ROOT_DIR}/core/src)
    generate_object(${name} .bin binary)
    generate_object(${name} .hex ihex)
    generate_lst(${name})
    print_size(${name} ${linker_file})
    cmake_git_version_add_headers_with_type(${name} ${optimize})

    if(NOT ${application} STREQUAL FALSE)
        add_bootloader_app_data(${name} ${application})
    endif()

    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_flash.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_ram.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_eeprom.ld)

    add_target_linker_dependency(${name} ${linker_file})
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/two_stage_link.py)

    get_filename_component(linker_file_path ${linker_file} ABSOLUTE)
    get_filename_component(linker_file_path ${linker_file_path} DIRECTORY)

    if(CMAKE_CROSSCOMPILING)
        target_link_options(
            ${name}
            PUBLIC
            "${LINKER_PREFIX}--library-path=${linker_file_path}"
            "${LINKER_PREFIX}--library-path=${kvasir_cmake_dir}/../linker"
            "${LINKER_PREFIX}--defsym=cmake_ram_size=${TARGET_RAM_SIZE}"
            "${LINKER_PREFIX}--defsym=cmake_min_stack_size=${min_stack_size}"
            "${LINKER_PREFIX}--defsym=cmake_stack_size_extra=0"
            "${LINKER_PREFIX}--defsym=cmake_heap_size=${heap_size}"
            "${LINKER_PREFIX}--script=${linker_file}"
            "${LINKER_PREFIX}--Map=${CMAKE_CURRENT_BINARY_DIR}/${name}.map")
        add_clean_file(${name} ${CMAKE_CURRENT_BINARY_DIR}/${name}.map)
        add_clean_file(${name} ${CMAKE_CURRENT_BINARY_DIR}/${name}.step1)

        if(optimize STREQUAL size)
            set(optimize_flags ${optimize_option_size})
            set(used_specs ${optimize_specs_size})
        elseif(optimize STREQUAL speed)
            set(optimize_flags ${optimize_option_speed})
            set(used_specs ${optimize_specs_speed})
        elseif(optimize STREQUAL debug)
            set(optimize_flags ${optimize_option_debug})
            set(used_specs ${optimize_specs_debug})
        else()
            message(FATAL_ERROR "wrong OPTIMIZE option specified!!!!")
        endif()

        if(sanitize STREQUAL "TRUE")
            set(sanitize_flags ${sanitize_option})
        elseif(sanitize STREQUAL "FALSE")
            set(sanitize_flags)
        else()
            message(FATAL_ERROR "wrong SANITIZE option specified!!!! ${sanitize}")
        endif()

        set_property(
            TARGET ${name}
            APPEND
            PROPERTY SOURCES ${CHIP_SOURCES})

        target_compile_options(${name} PUBLIC ${optimize_flags} ${sanitize_flags} ${CHIP_OPTIONS})

        foreach(current_lib ${libraries})
            target_compile_options(${current_lib} PUBLIC ${optimize_flags} ${sanitize_flags})
            target_link_libraries(${name} ${current_lib})
        endforeach(current_lib)

        foreach(current_linker_flag ${linker_flags})
            if(${used_specs} STREQUAL ${SPEC_REPLACEMENT_EMPTY_MARKER})
                string(REPLACE ${SPEC_REPLACEMENT_STRING} "" current_linker_flag ${current_linker_flag})
            else()
                string(REPLACE ${SPEC_REPLACEMENT_STRING} ${used_specs} current_linker_flag ${current_linker_flag})
            endif()
            target_link_options(${name} PUBLIC "${current_linker_flag}" ${CHIP_LINKER_OPTIONS})
        endforeach(current_linker_flag)

        if(${application} STREQUAL FALSE)
            target_add_flash_jlink(
                ${name}
                TARGET_MPU
                ${TARGET_MPU}
                SWD_SPEED
                ${SWD_SPEED}
                JLINK_IP
                ${JLINK_IP}
                SUFFIX
                "_flash.hex")
        else()
            target_add_flash_jlink(
                ${name}
                TARGET_MPU
                ${TARGET_MPU}
                SWD_SPEED
                ${SWD_SPEED}
                JLINK_IP
                ${JLINK_IP}
                SUFFIX
                "_eeprom_flash.hex")
        endif()

        if(NOT ${bootloader} STREQUAL FALSE)
            add_dependencies(flash_${name} flash_${bootloader})
        endif()

        target_add_uc_log_rtt_jlink(
            ${name}
            TARGET_MPU
            ${TARGET_MPU}
            SWD_SPEED
            ${SWD_SPEED}
            JLINK_IP
            ${JLINK_IP}
            CHANNELS
            2
            MAP_FILE
            ${name}.map
            HEX_FILE
            "${name}_flash.hex")

    endif()
endfunction()

function(kvasir_executable_variants base_name)
    cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "" "OPTIMIZATION"
                          "SOURCES;LIBRARIES;ADDITIONAL_DEBUG_FLAGS;ADDITIONAL_RELEASE_FLAGS;ADDITIONAL_SANITIZE_FLAGS")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT PARSED_ARGS_SOURCES)
        message(FATAL_ERROR "SOURCES argument is required")
    endif()

    if(NOT PARSED_ARGS_OPTIMIZATION)
        set(PARSED_ARGS_OPTIMIZATION size)
    endif()

    # Handle empty base_name
    if(base_name STREQUAL "")
        set(debug_target "debug")
        set(release_target "release")
        set(release_log_target "release_log")
        set(sanitize_target "sanitize")
    else()
        set(debug_target "${base_name}_debug")
        set(release_target "${base_name}_release")
        set(release_log_target "${base_name}_release_log")
        set(sanitize_target "${base_name}_sanitize")
    endif()

    # Debug variant - with logging, debug optimization
    add_executable(${debug_target} ${PARSED_ARGS_SOURCES})
    target_configure_kvasir(${debug_target} OPTIMIZATION_STRATEGY debug USE_LOG ${PARSED_ARGS_ADDITIONAL_DEBUG_FLAGS})
    if(PARSED_ARGS_LIBRARIES)
        target_link_libraries(${debug_target} ${PARSED_ARGS_LIBRARIES})
    endif()

    # Release variant - no logging, no sanitizer, configurable optimization
    add_executable(${release_target} ${PARSED_ARGS_SOURCES})
    target_configure_kvasir(${release_target} OPTIMIZATION_STRATEGY ${PARSED_ARGS_OPTIMIZATION}
                            ${PARSED_ARGS_ADDITIONAL_RELEASE_FLAGS})
    if(PARSED_ARGS_LIBRARIES)
        target_link_libraries(${release_target} ${PARSED_ARGS_LIBRARIES})
    endif()

    # Release with log variant - with logging, no sanitizer, configurable optimization
    add_executable(${release_log_target} ${PARSED_ARGS_SOURCES})
    target_configure_kvasir(${release_log_target} OPTIMIZATION_STRATEGY ${PARSED_ARGS_OPTIMIZATION} USE_LOG
                            ${PARSED_ARGS_ADDITIONAL_RELEASE_FLAGS})
    if(PARSED_ARGS_LIBRARIES)
        target_link_libraries(${release_log_target} ${PARSED_ARGS_LIBRARIES})
    endif()

    # Sanitize variant - with logging and sanitizer, configurable optimization
    add_executable(${sanitize_target} ${PARSED_ARGS_SOURCES})
    target_configure_kvasir(${sanitize_target} OPTIMIZATION_STRATEGY ${PARSED_ARGS_OPTIMIZATION} USE_SANITIZER USE_LOG
                            ${PARSED_ARGS_ADDITIONAL_SANITIZE_FLAGS})
    if(PARSED_ARGS_LIBRARIES)
        target_link_libraries(${sanitize_target} ${PARSED_ARGS_LIBRARIES})
    endif()

endfunction()

function(target_configure_kvasir target)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        PARSED_ARGS
        "USE_LOG;NOT_USE_ASSERT;ENABLE_SELF_OVERRIDE;USE_SANITIZER"
        "LOG;MIN_STACK_SIZE;HEAP_SIZE;OPTIMIZATION_STRATEGY;LINKER_FILE;LINKER_FILE_TEMPLATE;APPLICATION;BOOTLOADER;BOOTLOADER_SIZE"
        "")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT PARSED_ARGS_MIN_STACK_SIZE)
        set(PARSED_ARGS_MIN_STACK_SIZE 4k)
    endif()

    if(NOT PARSED_ARGS_HEAP_SIZE)
        set(PARSED_ARGS_HEAP_SIZE 0k)
    endif()

    if(NOT PARSED_ARGS_OPTIMIZATION_STRATEGY)
        set(PARSED_ARGS_OPTIMIZATION_STRATEGY size)
    endif()

    if(PARSED_ARGS_APPLICATION AND PARSED_ARGS_BOOTLOADER)
        message(FATAL_ERROR "${target}: cant be bootloader and application")
    endif()

    if(PARSED_ARGS_ENABLE_SELF_OVERRIDE AND NOT PARSED_ARGS_APPLICATION)
        message(FATAL_ERROR "${target}: cant enable self override for an normal application")
    endif()
    if(PARSED_ARGS_ENABLE_SELF_OVERRIDE)
        set(PARSED_ARGS_ENABLE_SELF_OVERRIDE true)
    else()
        set(PARSED_ARGS_ENABLE_SELF_OVERRIDE false)
    endif()

    if(NOT PARSED_ARGS_APPLICATION)
        set(PARSED_ARGS_APPLICATION FALSE)
    else()
        if(PARSED_ARGS_BOOTLOADER_SIZE)
            message(FATAL_ERROR "${target}: do not set BOOTLOADER_SIZE in bootloader instead set in application")
        endif()
    endif()

    if(NOT PARSED_ARGS_BOOTLOADER)
        set(PARSED_ARGS_BOOTLOADER FALSE)
    else()
        if(NOT PARSED_ARGS_BOOTLOADER_SIZE)
            set(PARSED_ARGS_BOOTLOADER_SIZE 8192)
        endif()
        set_target_properties(${target} PROPERTIES bootloader_size ${PARSED_ARGS_BOOTLOADER_SIZE})
    endif()

    if(PARSED_ARGS_LINKER_FILE AND PARSED_ARGS_LINKER_FILE_TEMPLATE)
        message(FATAL_ERROR "${target}: only LINKER_FILE or LINKER_FILE_TEMPLATE allowed")
    endif()

    if(NOT ${PARSED_ARGS_APPLICATION} STREQUAL "FALSE")
        get_target_property(bootloader_size ${PARSED_ARGS_APPLICATION} bootloader_size)
    else()
        set(bootloader_size ${PARSED_ARGS_BOOTLOADER_SIZE})
    endif()

    if(NOT PARSED_ARGS_LINKER_FILE)
        if(PARSED_ARGS_LINKER_FILE_TEMPLATE)
            set(GEN_BOOTLOADER_SIZE ${bootloader_size})
            configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${PARSED_ARGS_LINKER_FILE_TEMPLATE}
                           ${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/generated.ld @ONLY)
            set(PARSED_ARGS_LINKER_FILE ${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/generated.ld)
        else()
            set(PARSED_ARGS_LINKER_FILE ${LINKER_FILE})
        endif()
    else()
        set(PARSED_ARGS_LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${PARSED_ARGS_LINKER_FILE})
    endif()

    if(PARSED_ARGS_USE_SANITIZER)
        set(USE_SANITIZER TRUE)
    else()
        set(USE_SANITIZER FALSE)
    endif()

    target_kvasir_config_internal(
        ${target}
        ${PARSED_ARGS_MIN_STACK_SIZE}
        ${PARSED_ARGS_HEAP_SIZE}
        ${PARSED_ARGS_OPTIMIZATION_STRATEGY}
        ${USE_SANITIZER}
        ${PARSED_ARGS_LINKER_FILE}
        ${PARSED_ARGS_APPLICATION}
        ${PARSED_ARGS_BOOTLOADER})

    if(NOT PARSED_ARGS_USE_LOG)
        if(PARSED_ARGS_LOG)
            set(PARSED_ARGS_USE_LOG ${PARSED_ARGS_LOG})
        endif()
    endif()

    kvasir_set_target_property(${target} LOG ${PARSED_ARGS_USE_LOG})

    if(PARSED_ARGS_NOT_USE_ASSERT)
        set(USE_ASSERT FALSE)
    else()
        set(USE_ASSERT TRUE)
    endif()

    kvasir_set_target_property(${target} ASSERT ${USE_ASSERT})
    target_add_tidy_flags(${target})
    target_add_cppcheck_flags(${target})

    if(NOT ${PARSED_ARGS_APPLICATION} STREQUAL "FALSE")
        target_compile_definitions(${target} PUBLIC INPUT_ENABLE_SELF_OVERRIDE=${PARSED_ARGS_ENABLE_SELF_OVERRIDE})
        target_compile_definitions(${target} PUBLIC INPUT_BOOTLOADER_SIZE=${bootloader_size})
    endif()

endfunction()

mark_as_advanced(FORCE CMAKE_INSTALL_PREFIX)
mark_as_advanced(FORCE CMAKE_BUILD_TYPE)
