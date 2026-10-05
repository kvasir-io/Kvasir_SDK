# Kvasir Utility Functions Core utility functions and target configuration for Kvasir framework Provides build system
# integration, size reporting, and target setup

include(${KVASIR_ROOT_DIR}/cmake_git_version/CMakeLists.txt)

add_subdirectory(${KVASIR_ROOT_DIR}/uc_log ${CMAKE_BINARY_DIR}/kvasir_uc_log)

find_package(
    Python3
    COMPONENTS Interpreter
    REQUIRED)

include(${kvasir_cmake_dir}/jlink.cmake)

include(${kvasir_cmake_dir}/kvasir_sdk_targets.cmake)
target_link_libraries(kvasir_sdk INTERFACE uc_log::uc_log)

# packages registered by the chip package (kvasir_add_package)
get_property(_kvasir_packages GLOBAL PROPERTY KVASIR_PACKAGES)
foreach(_package IN LISTS _kvasir_packages)
    string(REPLACE "|" ";" _package "${_package}")
    list(GET _package 0 _source)
    list(GET _package 1 _binary)
    list(GET _package 2 _guard)
    if(TARGET ${_guard})
        continue()
    endif()
    if(NOT EXISTS "${_source}/CMakeLists.txt")
        message(FATAL_ERROR "Kvasir: the chip package needs ${_binary}, but ${_source} has no CMakeLists.txt")
    endif()
    add_subdirectory(${_source} ${CMAKE_BINARY_DIR}/${_binary})
    message(STATUS "Kvasir: added ${_binary} from ${_source}")
endforeach()

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
            ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
            ${kvasir_cmake_dir}/tools/pretty_size.py "${CMAKE_SIZE}" "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
            "${TARGET_FLASH_SIZE}" "${TARGET_RAM_SIZE}" "${TARGET_EEPROM_SIZE}" "${linker_file}"
        COMMENT "Print memory usage for ${target}")
endfunction()

# Every KVASIR_RAM_FUNC_ATTRIBUTES function (KVASIR_RAM_FUNC_MARK()) really runs from RAM, and nothing it reaches from
# flash; removes the .elf when not, so the next build fails again instead of passing on a stale image.
function(check_ram_funcs target)
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
            ${kvasir_cmake_dir}/tools/check_ram_funcs.py --delete-on-failure "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
        COMMENT "Checking that the RAM functions of ${target}.elf are in RAM"
        VERBATIM)
endfunction()

# IMAGE_CRC: writes the image CRC descriptor (kvasir/Util/ImageDescriptor.hpp) into the ELF for Kvasir::ImageCheck. Must
# run before every artefact made from the ELF; removes the .elf on failure so the next build fails again.
function(patch_image_crc target)
    set(_sections .vectors .text .data ${TARGET_EXTRA_FLASH_SECTIONS})
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
            ${kvasir_cmake_dir}/tools/patch_image_crc.py "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf" --delete-on-failure
            --objcopy ${CMAKE_OBJCOPY} --sections ${_sections}
        COMMENT "Writing the image CRC descriptor into ${target}.elf"
        VERBATIM)
endfunction()

function(check_undefined_refs target)
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
                ${kvasir_cmake_dir}/tools/find_undefined_refs.py "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
        COMMENT "Checking for undefined references in ${target}.elf"
        VERBATIM)
endfunction()

function(generate_object target suffix type)

    list(TRANSFORM TARGET_EXTRA_FLASH_SECTIONS PREPEND "--only-section=" OUTPUT_VARIABLE extra_flash_sections)

    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            ${CMAKE_OBJCOPY} --output-target ${type} --only-section=.vectors --only-section=.text --only-section=.data
            ${extra_flash_sections} "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash${suffix}"
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
                ${CMAKE_OBJCOPY} --output-target ${type} --only-section=.vectors --only-section=.text
                --only-section=.data --only-section=.eeprom ${extra_flash_sections}
                "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash${suffix}"
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash${suffix})

        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${CMAKE_OBJCOPY} --only-section=.vectors --only-section=.text --only-section=.data
                ${extra_flash_sections} "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.elf"
            COMMAND
                ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
                ${kvasir_cmake_dir}/tools/strip_empty_segments.py "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.elf"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.elf"
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.elf)

        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
                ${kvasir_cmake_dir}/tools/ihex_to_uf2.py "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom${suffix}"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom.uf2" ${TARGET_UF2_CODE}
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom.uf2)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
                ${kvasir_cmake_dir}/tools/ihex_to_uf2.py "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash${suffix}"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.uf2" ${TARGET_UF2_CODE}
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.uf2)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
                ${kvasir_cmake_dir}/tools/ihex_to_uf2.py "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash${suffix}"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash.uf2" ${TARGET_UF2_CODE}
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_eeprom_flash.uf2)
    endif()
endfunction()

function(generate_lst target)
    list(TRANSFORM TARGET_EXTRA_FLASH_SECTIONS PREPEND "--section=" OUTPUT_VARIABLE extra_flash_sections)

    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            ${CMAKE_OBJDUMP} --section=.vectors --section=.text --section=.data ${extra_flash_sections} --disassemble
            --demangle --all-headers "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf" >
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_raw.lst"
        COMMAND
            ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
            ${kvasir_cmake_dir}/tools/beautify_lst.py "${CMAKE_CURRENT_BINARY_DIR}/${target}_raw.lst"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}.lst"
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_raw.lst ${CMAKE_CURRENT_BINARY_DIR}/${target}.lst
        VERBATIM)
endfunction()

function(add_bootloader_app_data target application)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/${target})
    get_target_property(app_bin_dir ${application} BINARY_DIR)
    add_custom_command(
        TARGET ${application}
        COMMAND
            ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
            ${kvasir_cmake_dir}/tools/gen_app_data_for_bootloader.py ${app_bin_dir}/${application}_flash.bin
            ${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/bootloader/app_data.inl DEPENDS
            ${kvasir_cmake_dir}/tools/gen_app_data_for_bootloader.py
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/bootloader/app_data.inl
        COMMENT "Generating bootloader app data for ${target}")

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
        # libstdc++'s headers are configured against newlib and reach for its internals (ctype table, wide-character
        # API, errno values llvm-libc does not define)
        message(FATAL_ERROR "libstdc++ does not work with llvm libc")
    endif()
endif()

function(target_add_kvasir_lib target name sources)
    set(libraries
        ${libraries} "${name}-${target}"
        PARENT_SCOPE)
    add_library("${name}-${target}" ${sources})
endfunction()

# Create (or reuse) a runtime library shared across all targets that have identical compile flags. The library name
# encodes a full MD5 of the flags so that targets with different optimisation / sanitiser settings still get their own
# copy.
function(target_add_shared_kvasir_lib target name sources opt_flags san_flags)
    string(MD5 _hash "${opt_flags}_${san_flags}")
    set(_shared "${name}_${_hash}")

    if(NOT TARGET "${_shared}")
        add_library("${_shared}" ${sources})
        target_compile_options("${_shared}" PUBLIC ${opt_flags} ${san_flags})
    endif()

    # --whole-archive bracketed around this archive only: a bare --whole-archive link option stays in force for every
    # archive after it (KVASIR_WHOLE_ARCHIVE is defined in the toolchain files)
    target_link_libraries(${target} "$<LINK_LIBRARY:KVASIR_WHOLE_ARCHIVE,${_shared}>")
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
    bootloader
    core1_stack_size
    core1_stack_in_scratch
    scratch_banks)

    # Compute flags first so they are available for the shared-library key.
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

    # Create or reuse shared runtime libraries keyed on the compile flags.
    if("${COMPILER_RT}" STREQUAL "clang")
        target_add_shared_kvasir_lib(${name} rt "${COMPILER_RT_SOURCE_FILES}" "${optimize_flags}" "${sanitize_flags}")
    endif()

    if("${CPPLIB}" STREQUAL "libc++")
        target_add_shared_kvasir_lib(${name} cxx "${LIBCXX_SOURCE_FILES}" "${optimize_flags}" "${sanitize_flags}")
    endif()

    if("${CLIB}" STREQUAL "llvm")
        target_add_shared_kvasir_lib(${name} c "${LIBC_SOURCE_FILES}" "${optimize_flags}" "${sanitize_flags}")
        if(NOT heap_size EQUAL 0)
            target_add_shared_kvasir_lib(${name} c_malloc "${LIBC_MALLOC_SOURCE_FILES}" "${optimize_flags}"
                                         "${sanitize_flags}")
        endif()
    endif()

    set_target_properties(${name} PROPERTIES SUFFIX ".elf")

    target_link_libraries(${name} peripherals)
    target_link_libraries(${name} core_peripherals)
    target_link_libraries(${name} uc_log::uc_log)
    target_include_directories(${name} PUBLIC ${KVASIR_ROOT_DIR}/src)
    target_include_directories(${name} PUBLIC ${CHIP_ROOT_DIR}/src)
    target_include_directories(${name} PUBLIC ${CHIP_ROOT_DIR}/core/src)
    check_ram_funcs(${name})
    check_undefined_refs(${name})
    get_target_property(_image_crc ${name} KVASIR_IMAGE_CRC)
    if(_image_crc)
        patch_image_crc(${name}) # before generate_object: every artefact carries the descriptor
    endif()
    generate_object(${name} .bin binary)
    generate_object(${name} .hex ihex)
    generate_lst(${name})
    print_size(${name} ${linker_file})
    cmake_git_version_add_headers_with_type(${name} ${optimize})

    if(NOT ${application} STREQUAL FALSE)
        add_bootloader_app_data(${name} ${application})
    endif()

    # Main linker composition files
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_flash.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_ram.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_ram_only.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_eeprom.ld)

    # Section body include files
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_text_body.inc.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_vectors_body.inc.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_data_body.inc.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_bss_body.inc.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_noInit_body.inc.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_noInitLowRam_body.inc.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_stack_body.inc.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_stack1_body.inc.ld)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/../linker/common_heap_body.inc.ld)

    add_target_linker_dependency(${name} ${linker_file})
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/linker_utils.py)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/find_undefined_refs.py)
    # POST_BUILD commands have no dependencies of their own: relink when one of their scripts changes
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/check_ram_funcs.py)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/patch_image_crc.py)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/pretty_size.py)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/strip_empty_segments.py)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/ihex_to_uf2.py)
    add_target_linker_dependency(${name} ${kvasir_cmake_dir}/tools/beautify_lst.py)

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
            "${LINKER_PREFIX}--defsym=cmake_heap_size=${heap_size}"
            "${LINKER_PREFIX}--defsym=cmake_core1_stack_size=${core1_stack_size}"
            "${LINKER_PREFIX}--defsym=cmake_core1_stack_in_scratch=${core1_stack_in_scratch}"
            "${LINKER_PREFIX}--defsym=cmake_scratch_banks=${scratch_banks}"
            "${LINKER_PREFIX}--script=${linker_file}"
            "${LINKER_PREFIX}--Map=${CMAKE_CURRENT_BINARY_DIR}/${name}.map")
        add_clean_file(${name} ${CMAKE_CURRENT_BINARY_DIR}/${name}.map)
        add_clean_file(${name} ${CMAKE_CURRENT_BINARY_DIR}/${name}.step1)

        set_property(
            TARGET ${name}
            APPEND
            PROPERTY SOURCES ${CHIP_SOURCES})

        target_compile_options(${name} PUBLIC ${optimize_flags} ${sanitize_flags} ${CHIP_OPTIONS})

        # CHIP_LINKER_OPTIONS are bare linker options (`--wrap=...`): the gcc driver needs them behind -Wl, (clang links
        # with ld.lld directly, LINKER_PREFIX is empty there)
        set(chip_linker_options ${CHIP_LINKER_OPTIONS})
        list(TRANSFORM chip_linker_options PREPEND "${LINKER_PREFIX}")
        foreach(current_linker_flag ${linker_flags})
            if(${used_specs} STREQUAL ${SPEC_REPLACEMENT_EMPTY_MARKER})
                string(REPLACE ${SPEC_REPLACEMENT_STRING} "" current_linker_flag ${current_linker_flag})
            else()
                string(REPLACE ${SPEC_REPLACEMENT_STRING} ${used_specs} current_linker_flag ${current_linker_flag})
            endif()
            target_link_options(${name} PUBLIC "${current_linker_flag}" ${chip_linker_options})
        endforeach(current_linker_flag)

        get_target_property(_kvasir_ram_only ${name} KVASIR_RAM_ONLY)
        if(_kvasir_ram_only)
            set(_jlink_ram_only RAM_ONLY)
            # the printer's flash and reset start it in RAM too (uc_log_printer --ram_image)
            set(_uc_log_ram_image RAM_IMAGE)
        else()
            set(_jlink_ram_only "")
            set(_uc_log_ram_image "")
        endif()
        if(${application} STREQUAL FALSE)
            target_add_flash_jlink(
                ${name}
                ${_jlink_ram_only}
                TARGET_MPU
                ${TARGET_MPU}
                SWD_SPEED
                ${SWD_SPEED}
                JLINK_IP
                ${JLINK_IP}
                JLINK_PROBE
                "${JLINK_PROBE}"
                CONNECT_COMMANDS
                ${TARGET_JLINK_CONNECT_COMMANDS}
                SUFFIX
                "_flash.hex")
        else()
            target_add_flash_jlink(
                ${name}
                ${_jlink_ram_only}
                TARGET_MPU
                ${TARGET_MPU}
                SWD_SPEED
                ${SWD_SPEED}
                JLINK_IP
                ${JLINK_IP}
                JLINK_PROBE
                "${JLINK_PROBE}"
                CONNECT_COMMANDS
                ${TARGET_JLINK_CONNECT_COMMANDS}
                SUFFIX
                "_eeprom_flash.hex")
        endif()

        if(NOT ${bootloader} STREQUAL FALSE)
            add_dependencies(flash_${name} flash_${bootloader})
        endif()

        get_target_property(_uc_log_filter ${name} UC_LOG_FILTER)
        if(NOT _uc_log_filter)
            set(_uc_log_filter "")
        endif()
        target_add_uc_log_rtt_jlink(
            ${name}
            ${_uc_log_ram_image}
            TARGET_MPU
            ${TARGET_MPU}
            SWD_SPEED
            ${SWD_SPEED}
            JLINK_IP
            ${JLINK_IP}
            JLINK_PROBE
            "${JLINK_PROBE}"
            DUPLEX_BASE_PORT
            ${DUPLEX_BASE_PORT}
            TRANSPORT
            ${UC_LOG_TRANSPORT}
            PRE_RESET_COMMANDS
            ${TARGET_JLINK_CONNECT_COMMANDS}
            LOG_FILTER
            "${_uc_log_filter}"
            MAP_FILE
            ${name}.map
            HEX_FILE
            "${name}_flash.hex")

    endif()
endfunction()

function(kvasir_executable_variants base_name)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        PARSED_ARGS
        "RAM_ONLY;SCRATCH_BANKS;IMAGE_CRC"
        "OPTIMIZATION;MIN_STACK_SIZE;CORE1_STACK_SIZE;CORE1_STACK_PLACEMENT;HEAP_SIZE;MIN_LOG_LEVEL;MIN_LOG_LEVEL_DEBUG;MIN_LOG_LEVEL_RELEASE;LOG_FILTER;LINKER_FILE;SELFTEST_VARIANT;CHECK_LEVEL;CHECK_LEVEL_RELEASE;DCHECK_LEVEL;DCHECK_LEVEL_RELEASE"
        "SOURCES;LIBRARIES;ADDITIONAL_FLAGS;ADDITIONAL_DEBUG_FLAGS;ADDITIONAL_RELEASE_FLAGS;ADDITIONAL_SANITIZE_FLAGS")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT PARSED_ARGS_SOURCES)
        message(FATAL_ERROR "SOURCES argument is required")
    endif()

    if(NOT PARSED_ARGS_OPTIMIZATION)
        set(PARSED_ARGS_OPTIMIZATION size)
    endif()

    # Forwarded to every variant. Left empty when unset so target_configure_kvasir keeps applying its own default -
    # passing MIN_STACK_SIZE here is opt-in.
    set(_min_stack "")
    if(PARSED_ARGS_MIN_STACK_SIZE)
        set(_min_stack MIN_STACK_SIZE ${PARSED_ARGS_MIN_STACK_SIZE})
    endif()

    # Opt-in to a second core: sizes .stack1 and defines KVASIR_MULTICORE for every variant.
    set(_core1_stack "")
    if(PARSED_ARGS_CORE1_STACK_SIZE)
        set(_core1_stack CORE1_STACK_SIZE ${PARSED_ARGS_CORE1_STACK_SIZE})
    endif()
    if(PARSED_ARGS_CORE1_STACK_PLACEMENT)
        list(APPEND _core1_stack CORE1_STACK_PLACEMENT ${PARSED_ARGS_CORE1_STACK_PLACEMENT})
    endif()
    # Opt-in to the chip's per-core scratch banks for objects (KVASIR_COREn_{DATA,BSS,CODE}); see
    # target_configure_kvasir.
    if(PARSED_ARGS_SCRATCH_BANKS)
        list(APPEND _core1_stack SCRATCH_BANKS)
    endif()

    set(_heap "")
    if(PARSED_ARGS_HEAP_SIZE)
        set(_heap HEAP_SIZE ${PARSED_ARGS_HEAP_SIZE})
    endif()

    set(_log_filter "")
    if(PARSED_ARGS_LOG_FILTER)
        get_filename_component(_log_filter_path "${PARSED_ARGS_LOG_FILTER}" ABSOLUTE BASE_DIR
                               "${CMAKE_CURRENT_SOURCE_DIR}")
        set(_log_filter LOG_FILTER ${_log_filter_path})
    endif()

    # The firmware's own linker script instead of the chip's (e.g. a calibration sector kept out of the image), relative
    # to the project.
    set(_linker_file "")
    if(PARSED_ARGS_LINKER_FILE)
        set(_linker_file LINKER_FILE ${PARSED_ARGS_LINKER_FILE})
    endif()

    # Opt-in to an image that lives entirely in RAM (the chip's LINKER_FILE_RAM_ONLY): loaded by the bootrom's UF2 path
    # or a debugger, never written to flash. What a flash eraser is built as.
    set(_ram_only "")
    if(PARSED_ARGS_RAM_ONLY)
        set(_ram_only RAM_ONLY)
    endif()
    # Opt-in to the image CRC descriptor (Kvasir::ImageCheck), in every variant.
    set(_image_crc "")
    if(PARSED_ARGS_IMAGE_CRC)
        set(_image_crc IMAGE_CRC)
    endif()

    # Log floor per variant: MIN_LOG_LEVEL applies to every logging variant, MIN_LOG_LEVEL_DEBUG overrides it for the
    # debug variant, MIN_LOG_LEVEL_RELEASE for release_log and sanitize. Unset means uc_log's default (keep everything).
    set(_min_log_debug "")
    set(_min_log_release "")
    if(PARSED_ARGS_MIN_LOG_LEVEL)
        set(_min_log_debug MIN_LOG_LEVEL ${PARSED_ARGS_MIN_LOG_LEVEL})
        set(_min_log_release MIN_LOG_LEVEL ${PARSED_ARGS_MIN_LOG_LEVEL})
    endif()
    if(PARSED_ARGS_MIN_LOG_LEVEL_DEBUG)
        set(_min_log_debug MIN_LOG_LEVEL ${PARSED_ARGS_MIN_LOG_LEVEL_DEBUG})
    endif()
    if(PARSED_ARGS_MIN_LOG_LEVEL_RELEASE)
        set(_min_log_release MIN_LOG_LEVEL ${PARSED_ARGS_MIN_LOG_LEVEL_RELEASE})
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
    target_configure_kvasir(
        ${debug_target}
        OPTIMIZATION_STRATEGY
        debug
        USE_LOG
        ${_min_stack}
        ${_core1_stack}
        ${_heap}
        ${_log_filter}
        ${_ram_only}
        ${_image_crc}
        ${_linker_file}
        ${_min_log_debug}
        ${PARSED_ARGS_ADDITIONAL_FLAGS}
        ${PARSED_ARGS_ADDITIONAL_DEBUG_FLAGS})
    if(PARSED_ARGS_LIBRARIES)
        target_link_libraries(${debug_target} ${PARSED_ARGS_LIBRARIES})
    endif()

    # Release variant - no logging, no sanitizer, configurable optimization
    add_executable(${release_target} ${PARSED_ARGS_SOURCES})
    target_configure_kvasir(
        ${release_target}
        OPTIMIZATION_STRATEGY
        ${PARSED_ARGS_OPTIMIZATION}
        ${_min_stack}
        ${_core1_stack}
        ${_heap}
        ${_log_filter}
        ${_ram_only}
        ${_image_crc}
        ${_linker_file}
        ${PARSED_ARGS_ADDITIONAL_FLAGS}
        ${PARSED_ARGS_ADDITIONAL_RELEASE_FLAGS})
    if(PARSED_ARGS_LIBRARIES)
        target_link_libraries(${release_target} ${PARSED_ARGS_LIBRARIES})
    endif()

    # Release with log variant - with logging, no sanitizer, configurable optimization
    add_executable(${release_log_target} ${PARSED_ARGS_SOURCES})
    target_configure_kvasir(
        ${release_log_target}
        OPTIMIZATION_STRATEGY
        ${PARSED_ARGS_OPTIMIZATION}
        USE_LOG
        ${_min_stack}
        ${_core1_stack}
        ${_heap}
        ${_log_filter}
        ${_ram_only}
        ${_image_crc}
        ${_linker_file}
        ${_min_log_release}
        ${PARSED_ARGS_ADDITIONAL_FLAGS}
        ${PARSED_ARGS_ADDITIONAL_RELEASE_FLAGS})
    if(PARSED_ARGS_LIBRARIES)
        target_link_libraries(${release_log_target} ${PARSED_ARGS_LIBRARIES})
    endif()

    # Sanitize variant - with logging and sanitizer, configurable optimization
    add_executable(${sanitize_target} ${PARSED_ARGS_SOURCES})
    target_configure_kvasir(
        ${sanitize_target}
        OPTIMIZATION_STRATEGY
        ${PARSED_ARGS_OPTIMIZATION}
        USE_SANITIZER
        USE_LOG
        ${_min_stack}
        ${_core1_stack}
        ${_heap}
        ${_log_filter}
        ${_ram_only}
        ${_image_crc}
        ${_linker_file}
        ${_min_log_release}
        ${PARSED_ARGS_ADDITIONAL_FLAGS}
        ${PARSED_ARGS_ADDITIONAL_SANITIZE_FLAGS})
    if(PARSED_ARGS_LIBRARIES)
        target_link_libraries(${sanitize_target} ${PARSED_ARGS_LIBRARIES})
    endif()

    # Compile-time self-tests (kvasir/Util/SelfTest.hpp) prove the same thing in every variant, so only one evaluates
    # them (they are slow to compile): debug unless the firmware names another.
    if(NOT PARSED_ARGS_SELFTEST_VARIANT)
        set(PARSED_ARGS_SELFTEST_VARIANT debug)
    endif()
    set(_selftest_variants debug release release_log sanitize)
    if(NOT PARSED_ARGS_SELFTEST_VARIANT IN_LIST _selftest_variants
       AND NOT PARSED_ARGS_SELFTEST_VARIANT STREQUAL "all"
       AND NOT PARSED_ARGS_SELFTEST_VARIANT STREQUAL "none")
        message(FATAL_ERROR "SELFTEST_VARIANT ${PARSED_ARGS_SELFTEST_VARIANT}: one of ${_selftest_variants}, all, none")
    endif()
    foreach(_variant IN LISTS _selftest_variants)
        if(PARSED_ARGS_SELFTEST_VARIANT STREQUAL _variant OR PARSED_ARGS_SELFTEST_VARIANT STREQUAL "all")
            target_compile_definitions(${${_variant}_target} PRIVATE KVASIR_SELFTEST=1)
        else()
            target_compile_definitions(${${_variant}_target} PRIVATE KVASIR_SELFTEST=0)
        endif()
    endforeach()

    # Runtime checks (kvasir/Util/Check.hpp): 0 off, 1 bare (panic, no log), 2 full (the operands logged). Every TU of
    # an image must see the same level: set here per target, never in a header. CHECK_LEVEL defaults to 2 (release and
    # release_log: CHECK_LEVEL_RELEASE); DCHECK follows it in debug and sanitize and is 0 in release and release_log
    # (DCHECK_LEVEL / DCHECK_LEVEL_RELEASE). A failed KVASIR_SOFT_CHECK panics in sanitize and only logs elsewhere.
    foreach(_level CHECK_LEVEL CHECK_LEVEL_RELEASE DCHECK_LEVEL DCHECK_LEVEL_RELEASE)
        if(DEFINED PARSED_ARGS_${_level} AND NOT PARSED_ARGS_${_level} MATCHES "^[012]$")
            message(FATAL_ERROR "${_level} ${PARSED_ARGS_${_level}}: 0 (off), 1 (bare) or 2 (full)")
        endif()
    endforeach()
    set(_check 2)
    if(DEFINED PARSED_ARGS_CHECK_LEVEL)
        set(_check ${PARSED_ARGS_CHECK_LEVEL})
    endif()
    set(_check_release ${_check})
    if(DEFINED PARSED_ARGS_CHECK_LEVEL_RELEASE)
        set(_check_release ${PARSED_ARGS_CHECK_LEVEL_RELEASE})
    endif()
    set(_dcheck ${_check})
    if(DEFINED PARSED_ARGS_DCHECK_LEVEL)
        set(_dcheck ${PARSED_ARGS_DCHECK_LEVEL})
    endif()
    set(_dcheck_release 0)
    if(DEFINED PARSED_ARGS_DCHECK_LEVEL_RELEASE)
        set(_dcheck_release ${PARSED_ARGS_DCHECK_LEVEL_RELEASE})
    endif()
    target_compile_definitions(${debug_target} PRIVATE KVASIR_CHECK_LEVEL=${_check} KVASIR_DCHECK_LEVEL=${_dcheck}
                                                       KVASIR_SOFT_CHECK_ESCALATE=0)
    target_compile_definitions(${sanitize_target} PRIVATE KVASIR_CHECK_LEVEL=${_check} KVASIR_DCHECK_LEVEL=${_dcheck}
                                                          KVASIR_SOFT_CHECK_ESCALATE=1)
    foreach(_t ${release_target} ${release_log_target})
        target_compile_definitions(${_t} PRIVATE KVASIR_CHECK_LEVEL=${_check_release}
                                                 KVASIR_DCHECK_LEVEL=${_dcheck_release} KVASIR_SOFT_CHECK_ESCALATE=0)
    endforeach()

endfunction()

function(target_configure_kvasir target)
    cmake_parse_arguments(
        PARSE_ARGV
        1
        PARSED_ARGS
        "USE_LOG;NOT_USE_ASSERT;ENABLE_SELF_OVERRIDE;USE_SANITIZER;RAM_ONLY;SCRATCH_BANKS;IMAGE_CRC"
        "LOG;MIN_LOG_LEVEL;LOG_FILTER;MIN_STACK_SIZE;CORE1_STACK_SIZE;CORE1_STACK_PLACEMENT;HEAP_SIZE;OPTIMIZATION_STRATEGY;LINKER_FILE;LINKER_FILE_TEMPLATE;APPLICATION;BOOTLOADER;BOOTLOADER_SIZE"
        "")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT PARSED_ARGS_MIN_STACK_SIZE)
        set(PARSED_ARGS_MIN_STACK_SIZE 4k)
    endif()

    # uc_log compile-time log filter, read via #embed: a uc_log_filter.txt next to the project's top CMakeLists.txt is
    # picked up with no argument, LOG_FILTER <dir>/uc_log_filter.txt names another. Editing it only recompiles (#embed
    # puts it into the depfile); only adding or removing the default file re-configures (the glob).
    set(_log_filter "")
    if(PARSED_ARGS_LOG_FILTER)
        get_filename_component(_log_filter "${PARSED_ARGS_LOG_FILTER}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_log_filter}")
            message(FATAL_ERROR "${target}: LOG_FILTER ${_log_filter} does not exist")
        endif()
    else()
        file(GLOB _log_filter CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/uc_log_filter.txt")
    endif()
    if(_log_filter)
        get_filename_component(_log_filter_name "${_log_filter}" NAME)
        if(NOT _log_filter_name STREQUAL "uc_log_filter.txt")
            message(FATAL_ERROR "${target}: the log filter file must be named uc_log_filter.txt (#embed finds it by "
                                "name), not ${_log_filter_name}")
        endif()
        get_filename_component(_log_filter_dir "${_log_filter}" DIRECTORY)
        target_compile_options(${target} PRIVATE "--embed-dir=${_log_filter_dir}")
        set_target_properties(${target} PROPERTIES UC_LOG_FILTER "${_log_filter}")
    endif()

    # A second core is opt-in. Without CORE1_STACK_SIZE the image is single-core and, by contract, identical to what it
    # was before multicore support existed: no define, an empty .stack1, nothing else.
    if(NOT PARSED_ARGS_CORE1_STACK_SIZE)
        set(PARSED_ARGS_CORE1_STACK_SIZE 0)
    endif()
    if(NOT PARSED_ARGS_CORE1_STACK_SIZE STREQUAL "0")
        target_compile_definitions(${target} PUBLIC KVASIR_MULTICORE=1)
        # the size in bytes, for compile-time checks against it (Fault::Handler's core 1 reserve)
        if(NOT PARSED_ARGS_CORE1_STACK_SIZE MATCHES "^([0-9]+)([kK]?)$")
            message(FATAL_ERROR "${target}: CORE1_STACK_SIZE ${PARSED_ARGS_CORE1_STACK_SIZE}: expected <n> or <n>k")
        endif()
        if(CMAKE_MATCH_2)
            math(EXPR _core1_stack_bytes "${CMAKE_MATCH_1} * 1024")
        else()
            set(_core1_stack_bytes ${CMAKE_MATCH_1})
        endif()
        target_compile_definitions(${target} PUBLIC KVASIR_CORE1_STACK_BYTES=${_core1_stack_bytes})
    endif()

    # Where core 1's stack lives: `ram` (the default, .stack1 next to core 0's stack) or `scratch` (the chip's core 1
    # scratch bank, TARGET_CORE1_SCRATCH_SIZE bytes: RP2040 SRAM4). Opt-in: the bank caps the stack at 4 KiB, and a core
    # 1 overflow on the M0+ then runs silently into the top of striped RAM.
    set(_core1_stack_in_scratch 0)
    if(PARSED_ARGS_CORE1_STACK_PLACEMENT AND NOT PARSED_ARGS_CORE1_STACK_PLACEMENT STREQUAL "ram")
        if(NOT PARSED_ARGS_CORE1_STACK_PLACEMENT STREQUAL "scratch")
            message(FATAL_ERROR "${target}: CORE1_STACK_PLACEMENT ${PARSED_ARGS_CORE1_STACK_PLACEMENT}: "
                                "expected ram or scratch")
        endif()
        if(PARSED_ARGS_CORE1_STACK_SIZE STREQUAL "0")
            message(FATAL_ERROR "${target}: CORE1_STACK_PLACEMENT scratch without CORE1_STACK_SIZE")
        endif()
        if(NOT TARGET_CORE1_SCRATCH_SIZE)
            message(FATAL_ERROR "${target}: CORE1_STACK_PLACEMENT scratch: the chip (${TARGET_MPU}) has no core 1 "
                                "scratch bank (its chip.cmake sets no TARGET_CORE1_SCRATCH_SIZE)")
        endif()
        if(_core1_stack_bytes GREATER TARGET_CORE1_SCRATCH_SIZE)
            message(
                FATAL_ERROR
                    "${target}: CORE1_STACK_PLACEMENT scratch: core 1's stack (${_core1_stack_bytes} bytes) "
                    "does not fit the ${TARGET_CORE1_SCRATCH_SIZE}-byte scratch bank; lower "
                    "CORE1_STACK_SIZE (SecondaryCore::stackHighWater() says how much is used) or drop "
                    "CORE1_STACK_PLACEMENT")
        endif()
        set(_core1_stack_in_scratch 1)
        target_compile_definitions(${target} PUBLIC KVASIR_CORE1_STACK_SCRATCH=1)
    endif()

    # SCRATCH_BANKS: objects in the chip's per-core scratch banks. Defines KVASIR_CORE_SCRATCH, which turns on the
    # KVASIR_COREn_{DATA,BSS,CODE} attributes (Util/attributes.hpp; empty without it) and the chip's copy/zero of the
    # banks at boot (Startup::ExtraMemoryInit). Opt-in so that an image without it keeps every byte it had; the chip's
    # linker script refuses scratch-bank objects in an image without it (cmake_scratch_banks).
    set(_scratch_banks 0)
    if(PARSED_ARGS_SCRATCH_BANKS)
        if(NOT TARGET_CORE1_SCRATCH_SIZE)
            message(FATAL_ERROR "${target}: SCRATCH_BANKS: the chip (${TARGET_MPU}) has no scratch banks")
        endif()
        set(_scratch_banks 1)
        target_compile_definitions(${target} PUBLIC KVASIR_CORE_SCRATCH=1)
    endif()

    if(NOT PARSED_ARGS_HEAP_SIZE)
        set(PARSED_ARGS_HEAP_SIZE 0k)
    endif()
    if(NOT PARSED_ARGS_HEAP_SIZE MATCHES "^0[kKmM]?$")
        target_compile_definitions(${target} PUBLIC KVASIR_HEAP=1)
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

    if(PARSED_ARGS_RAM_ONLY)
        if(PARSED_ARGS_LINKER_FILE OR PARSED_ARGS_LINKER_FILE_TEMPLATE)
            message(
                FATAL_ERROR "${target}: RAM_ONLY picks the chip's RAM-only linker file; do not pass LINKER_FILE too")
        endif()
        if(NOT LINKER_FILE_RAM_ONLY)
            message(FATAL_ERROR "${target}: RAM_ONLY needs the chip to define LINKER_FILE_RAM_ONLY")
        endif()
        set(PARSED_ARGS_LINKER_FILE ${LINKER_FILE_RAM_ONLY})
        target_compile_definitions(${target} PUBLIC KVASIR_RAM_ONLY=1)
        # read by target_kvasir_config_internal: the J-Link script starts the image in RAM instead of resetting
        set_property(TARGET ${target} PROPERTY KVASIR_RAM_ONLY TRUE)
    elseif(NOT PARSED_ARGS_LINKER_FILE)
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

    # IMAGE_CRC: the descriptor (KVASIR_IMAGE_CRC) and the post-link step that fills it (patch_image_crc, read by
    # target_kvasir_config_internal).
    if(PARSED_ARGS_IMAGE_CRC)
        if(PARSED_ARGS_RAM_ONLY)
            message(FATAL_ERROR "${target}: IMAGE_CRC checks the flash image; a RAM_ONLY image has none")
        endif()
        target_compile_definitions(${target} PUBLIC KVASIR_IMAGE_CRC=1)
        set_property(TARGET ${target} PROPERTY KVASIR_IMAGE_CRC TRUE)
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
        ${PARSED_ARGS_BOOTLOADER}
        ${PARSED_ARGS_CORE1_STACK_SIZE}
        ${_core1_stack_in_scratch}
        ${_scratch_banks})

    if(NOT PARSED_ARGS_USE_LOG)
        if(PARSED_ARGS_LOG)
            set(PARSED_ARGS_USE_LOG ${PARSED_ARGS_LOG})
        endif()
    endif()

    kvasir_set_target_property(${target} LOG ${PARSED_ARGS_USE_LOG})

    # Compile-time floor for UC_LOG_*: call sites below it vanish entirely, catalog strings included. Passed through as
    # a uc_log::LogLevel enumerator name (trace debug info warn error crit); the compiler rejects anything else.
    if(DEFINED PARSED_ARGS_MIN_LOG_LEVEL)
        if(NOT PARSED_ARGS_USE_LOG)
            message(WARNING "${target}: MIN_LOG_LEVEL has no effect without USE_LOG")
        endif()
        target_compile_definitions(${target} PUBLIC UC_LOG_MIN_LEVEL=${PARSED_ARGS_MIN_LOG_LEVEL})
    endif()

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
