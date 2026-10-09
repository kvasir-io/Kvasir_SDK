# J-Link Flash and Debug Configuration Provides functions to configure J-Link debugging and flashing capabilities
# Generates J-Link command files and CMake targets for embedded development

set(KVASIR_ENABLE_JLINK
    true
    CACHE BOOL "Enable J-Link flash and debug support")

function(target_add_flash_jlink target)
    if(POLICY CMP0174)
        cmake_policy(SET CMP0174 NEW)
    endif()
    cmake_parse_arguments(
        PARSE_ARGV 1 PARSED_ARGS "RAM_ONLY" "TARGET_MPU;SWD_SPEED;JLINK_IP;JLINK_PROBE;SUFFIX;AUXILIARY_TARGETS_PREFIX"
        "DEPENDS;CONNECT_COMMANDS")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT PARSED_ARGS_TARGET_MPU)
        message(FATAL_ERROR "needs TARGET_MPU")
    endif()

    # empty: the probe's maximum (the DLL clamps it), as uc_log's printer does
    if(NOT PARSED_ARGS_SWD_SPEED)
        set(PARSED_ARGS_SWD_SPEED 100000)
    endif()

    if(NOT PARSED_ARGS_SUFFIX)
        set(PARSED_ARGS_SUFFIX "")
    endif()

    if(NOT PARSED_ARGS_AUXILIARY_TARGETS_PREFIX)
        set(PARSED_ARGS_AUXILIARY_TARGETS_PREFIX "")
    endif()

    if(NOT KVASIR_ENABLE_JLINK)
        return()
    endif()

    # Which J-Link on USB. The commander's USB command takes a serial number or a nickname (it resolves the nickname
    # itself; quoted, or a nickname with a space is cut at the space and the connect fails - but a QUOTED serial number
    # is taken for a nickname and fails, so a serial goes in bare); a bare USB with two probes on the bus fails to
    # connect, and -USB on the command line clashes with the USB line in the script, so the name goes into the script.
    if(NOT PARSED_ARGS_JLINK_IP OR "${PARSED_ARGS_JLINK_IP}" STREQUAL "")
        if(PARSED_ARGS_JLINK_PROBE MATCHES "^[0-9]+$")
            set(jlink_connect_command "USB ${PARSED_ARGS_JLINK_PROBE}\nconnect\nr\nh\n")
        elseif(PARSED_ARGS_JLINK_PROBE AND NOT PARSED_ARGS_JLINK_PROBE STREQUAL "")
            set(jlink_connect_command "USB \"${PARSED_ARGS_JLINK_PROBE}\"\nconnect\nr\nh\n")
        else()
            set(jlink_connect_command "USB\nconnect\nr\nh\n")
        endif()
    else()
        set(jlink_connect_command "IP ${PARSED_ARGS_JLINK_IP}\nconnect\nr\nh\n")
    endif()

    # chip package's TARGET_JLINK_CONNECT_COMMANDS, run right after `connect`
    if(PARSED_ARGS_CONNECT_COMMANDS)
        list(JOIN PARSED_ARGS_CONNECT_COMMANDS "\n" jlink_connect_extra)
        string(REPLACE "connect\n" "connect\n${jlink_connect_extra}\n" jlink_connect_command "${jlink_connect_command}")
    endif()

    find_program(jlinkexe JLinkExe REQUIRED)

    mark_as_advanced(FORCE jlinkexe)

    if(PARSED_ARGS_RAM_ONLY)
        # No reset runs before the image, so clear every NVIC enable and pending bit: an interrupt the previous code
        # left on (e.g. the boot ROM's USB) would otherwise be taken before the image's first instruction.
        # NVIC_ICER/ICPR: one register on Armv6-M (DDI0419E B3.4), sixteen on Armv7-M/Armv8-M (DDI0553B.y D1.2.183/184).
        if(TARGET_CPU MATCHES "^cortex-m0|^cortex-m1$")
            set(nvic_words 1)
        else()
            set(nvic_words 16)
        endif()
        set(jlink_nvic_clear "")
        math(EXPR last_word "${nvic_words} - 1")
        foreach(n RANGE ${last_word})
            math(EXPR icer "0xE000E180 + 4 * ${n}" OUTPUT_FORMAT HEXADECIMAL)
            math(EXPR icpr "0xE000E280 + 4 * ${n}" OUTPUT_FORMAT HEXADECIMAL)
            string(APPEND jlink_nvic_clear "w4 ${icer} 0xFFFFFFFF\nw4 ${icpr} 0xFFFFFFFF\n")
        endforeach()
        # A reset runs the boot ROM, which boots flash (or BOOTSEL), never the image in RAM: load it, then do by hand
        # what a reset would do with ITS vector table - VTOR, MSP, EPSR.T, PC - and run it. The words are only known
        # after the link: the template goes through tools/ram_image_jlink.py after every build of the target.
        file(
            CONFIGURE
            OUTPUT
            ${target}_flash.jlink.in
            CONTENT
            "${jlink_connect_command}loadfile ${target}${PARSED_ARGS_SUFFIX}\n${jlink_nvic_clear}w4 0xE000ED08 {vector_table}\nwreg MSP {initial_sp}\nwreg XPSR 0x01000000\nSetPC {reset_pc}\ng\nq"
            NEWLINE_STYLE
            UNIX)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${Python3_EXECUTABLE} -X pycache_prefix=${CMAKE_BINARY_DIR}/__pycache__
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tools/ram_image_jlink.py "${CMAKE_CURRENT_BINARY_DIR}/${target}.elf"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.jlink.in"
                "${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.jlink"
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.jlink
            VERBATIM)
        # a POST_BUILD command has no dependencies of its own: relink when the script or the template changes - the
        # template carries the probe and the connect commands (file(CONFIGURE) rewrites it only when they change), and a
        # stale filled-in script would flash through the probe the tree had before
        set_property(
            TARGET ${target}
            APPEND
            PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tools/ram_image_jlink.py
                     ${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.jlink.in)
        set_property(
            TARGET ${target}
            APPEND
            PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.jlink.in)
    else()
        file(
            CONFIGURE
            OUTPUT
            ${target}_flash.jlink
            CONTENT
            "${jlink_connect_command}loadfile ${target}${PARSED_ARGS_SUFFIX}\nr\nh\nr\ng\nq"
            NEWLINE_STYLE
            UNIX)
    endif()

    set_property(
        TARGET ${target}
        APPEND
        PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_CURRENT_BINARY_DIR}/${target}_flash.jlink)

    set(jlink_args
        -If
        SWD
        -Device
        ${PARSED_ARGS_TARGET_MPU}
        -Speed
        ${PARSED_ARGS_SWD_SPEED}
        -ExitOnError
        1
        -NoGui
        1)

    add_custom_target(
        flash_${target}
        COMMAND ${jlinkexe} ${jlink_args} -CommandFile ${target}_flash.jlink
        DEPENDS ${target} ${PARSED_ARGS_DEPENDS})

    set(aux_suffix ${PARSED_ARGS_AUXILIARY_TARGETS_PREFIX})
    if(NOT TARGET ${aux_suffix}reset)
        file(
            CONFIGURE
            OUTPUT
            ${aux_suffix}reset.jlink
            CONTENT
            "${jlink_connect_command}r\ng\nq"
            NEWLINE_STYLE
            UNIX)

        add_custom_target(
            reset
            COMMAND ${jlinkexe} ${jlink_args} -CommandFile ${aux_suffix}reset.jlink
            DEPENDS ${aux_suffix}reset.jlink)

        file(
            CONFIGURE
            OUTPUT
            ${aux_suffix}connect.jlink
            CONTENT
            "${jlink_connect_command}"
            NEWLINE_STYLE
            UNIX)

        add_custom_target(
            ${aux_suffix}connect
            COMMAND ${jlinkexe} ${jlink_args} -CommandFile ${aux_suffix}connect.jlink
            DEPENDS ${aux_suffix}connect.jlink)

        set_property(
            TARGET ${target}
            APPEND
            PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_CURRENT_BINARY_DIR}/${aux_suffix}reset.jlink
                     ${CMAKE_CURRENT_BINARY_DIR}/${aux_suffix}connect.jlink)
    endif()

endfunction()
