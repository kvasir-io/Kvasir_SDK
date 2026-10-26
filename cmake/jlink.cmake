# The JLinkExe side of an image: flash_<target> (its <target>_flash.hex, or a RAM image loaded and started by hand) and
# the tree's reset and connect targets, as J-Link Commander scripts. The printer's targets (log_<target>) are uc_log's
# (target_add_uc_log_rtt_jlink).

# OFF: no JLinkExe targets (a tree on a machine without JLinkExe); the printer's targets stay
set(KVASIR_ENABLE_JLINK
    true
    CACHE BOOL "Enable the JLinkExe flash, reset and connect targets")

function(target_add_flash_jlink target)
    if(NOT KVASIR_ENABLE_JLINK)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "RAM_ONLY" "TARGET_MPU;SWD_SPEED;JLINK_IP;JLINK_PROBE"
                          "CONNECT_COMMANDS")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT PARSED_ARGS_TARGET_MPU)
        message(FATAL_ERROR "needs TARGET_MPU")
    endif()

    # empty: uc_log's default (UC_LOG_SWD_DEFAULT_KHZ, uc_log/CMakeLists.txt), the printer's speed too. A probe whose
    # maximum is too fast for the target falls back to connect-under-reset, which resets the target even on an attach:
    # such a tree needs a lower SWD_SPEED.
    if(NOT PARSED_ARGS_SWD_SPEED)
        if(NOT DEFINED UC_LOG_SWD_DEFAULT_KHZ)
            message(FATAL_ERROR "target_add_flash_jlink: no SWD_SPEED and no UC_LOG_SWD_DEFAULT_KHZ (uc_log first)")
        endif()
        set(PARSED_ARGS_SWD_SPEED ${UC_LOG_SWD_DEFAULT_KHZ})
    endif()

    # Which J-Link on USB. The commander's USB command takes a serial number or a nickname (it resolves the nickname
    # itself; quoted, or a nickname with a space is cut at the space and the connect fails - but a QUOTED serial number
    # is taken for a nickname and fails, so a serial goes in bare); a bare USB with two probes on the bus fails to
    # connect, and -USB on the command line clashes with the USB line in the script, so the name goes into the script.
    # jlink_open, the start of every script: the probe line and `connect`. connect.jlink is only this - an attach that
    # changes nothing on the target.
    if(NOT PARSED_ARGS_JLINK_IP OR "${PARSED_ARGS_JLINK_IP}" STREQUAL "")
        if(PARSED_ARGS_JLINK_PROBE MATCHES "^[0-9]+$")
            set(jlink_open "USB ${PARSED_ARGS_JLINK_PROBE}\nconnect\n")
        elseif(PARSED_ARGS_JLINK_PROBE AND NOT PARSED_ARGS_JLINK_PROBE STREQUAL "")
            set(jlink_open "USB \"${PARSED_ARGS_JLINK_PROBE}\"\nconnect\n")
        else()
            set(jlink_open "USB\nconnect\n")
        endif()
    else()
        set(jlink_open "IP ${PARSED_ARGS_JLINK_IP}\nconnect\n")
    endif()

    # jlink_connect_command, before a reset or a download: the chip package's kvasir_chip(JLINK_CONNECT_COMMANDS) right
    # after `connect` (they make the chip safe to reset or write, e.g. a second core sent back to its boot ROM), then
    # reset and halt
    set(jlink_connect_command "${jlink_open}")
    if(PARSED_ARGS_CONNECT_COMMANDS)
        list(JOIN PARSED_ARGS_CONNECT_COMMANDS "\n" jlink_connect_extra)
        string(APPEND jlink_connect_command "${jlink_connect_extra}\n")
    endif()
    string(APPEND jlink_connect_command "r\nh\n")

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
            "${jlink_connect_command}loadfile ${target}_flash.hex\n${jlink_nvic_clear}w4 0xE000ED08 {vector_table}\nwreg MSP {initial_sp}\nwreg XPSR 0x01000000\nSetPC {reset_pc}\ng\nq"
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
            "${jlink_connect_command}loadfile ${target}_flash.hex\nr\nh\nr\ng\nq"
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
        DEPENDS ${target})

    # the tree's `reset` and `connect` targets, through the probe of the first image that configures them
    if(NOT TARGET reset)
        file(
            CONFIGURE
            OUTPUT
            reset.jlink
            CONTENT
            "${jlink_connect_command}r\ng\nq"
            NEWLINE_STYLE
            UNIX)

        add_custom_target(
            reset
            COMMAND ${jlinkexe} ${jlink_args} -CommandFile reset.jlink
            DEPENDS reset.jlink)

        file(
            CONFIGURE
            OUTPUT
            connect.jlink
            CONTENT
            "${jlink_open}"
            NEWLINE_STYLE
            UNIX)

        add_custom_target(
            connect
            COMMAND ${jlinkexe} ${jlink_args} -CommandFile connect.jlink
            DEPENDS connect.jlink)

        set_property(
            TARGET ${target}
            APPEND
            PROPERTY ADDITIONAL_CLEAN_FILES ${CMAKE_CURRENT_BINARY_DIR}/reset.jlink
                     ${CMAKE_CURRENT_BINARY_DIR}/connect.jlink)
    endif()

endfunction()
