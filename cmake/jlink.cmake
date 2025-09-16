# J-Link Flash and Debug Configuration Provides functions to configure J-Link debugging and flashing capabilities
# Generates J-Link command files and CMake targets for embedded development

function(target_add_flash_jlink target)
    if(POLICY CMP0174)
        cmake_policy(SET CMP0174 NEW)
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS "" "TARGET_MPU;SWD_SPEED;JLINK_IP;SUFFIX;AUXILIARY_TARGETS_PREFIX"
                          "DEPENDS")

    if(PARSED_ARGS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "unknown argument ${PARSED_ARGS_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT PARSED_ARGS_TARGET_MPU)
        message(FATAL_ERROR "needs TARGET_MPU")
    endif()

    if(NOT PARSED_ARGS_SWD_SPEED)
        set(PARSED_ARGS_SWD_SPEED 4000)
    endif()

    if(NOT PARSED_ARGS_SUFFIX)
        set(PARSED_ARGS_SUFFIX "")
    endif()

    if(NOT PARSED_ARGS_AUXILIARY_TARGETS_PREFIX)
        set(PARSED_ARGS_AUXILIARY_TARGETS_PREFIX "")
    endif()

    if(NOT PARSED_ARGS_JLINK_IP OR "${PARSED_ARGS_JLINK_IP}" STREQUAL "")
        set(jlink_connect_command "USB\nconnect\nr\nh\n")
    else()
        set(jlink_connect_command "IP ${PARSED_ARGS_JLINK_IP}\nconnect\nr\nh\n")
    endif()

    find_program(jlinkexe JLinkExe REQUIRED)

    file(
        CONFIGURE
        OUTPUT
        ${target}_flash.jlink
        CONTENT
        "${jlink_connect_command}loadfile ${target}${PARSED_ARGS_SUFFIX}\nr\nh\nr\ng\nq"
        NEWLINE_STYLE
        UNIX)

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
