# What every chip and core package does the same way: convert its SVD, pick its part, generate a part's SVD.
# kvasir.cmake includes this after svd_converter and before the chip's chip.cmake; a package's tests include it alone
# (it only defines functions).
include_guard(GLOBAL)

# kvasir_chip_svd(<target> <svd>): the package's SVD as the INTERFACE target <target>, its generated headers in
# <build>/svd_converter_generated/<target>/, converted with svd_convert()'s defaults - Kvasir's write semantics (a new
# unclassified write-only field stops the build: classify it in the SVD). No keyword: every package converts the same
# way.
function(kvasir_chip_svd target svd)
    if(ARGN)
        message(FATAL_ERROR "kvasir_chip_svd: unknown arguments: ${ARGN}")
    endif()
    svd_convert(${target} SVD_FILE ${svd} OUTPUT_DIRECTORY ${target})
endfunction()

# kvasir_chip_generate_svd(<out> PART <part> SCRIPT <script> [INPUTS <file>...]): runs `<python> -B <script> --out
# <build>/chip_svd/<part>.svd` at configure time and sets <out> to that path. The file is replaced only when its content
# changed, so a re-configure leaves the svd_convert step alone. The script and the INPUTS (what it reads) re-run the
# configure when they change.
function(kvasir_chip_generate_svd out)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "PART;SCRIPT" "INPUTS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "kvasir_chip_generate_svd: unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_key PART SCRIPT)
        if("${ARG_${_key}}" STREQUAL "")
            message(FATAL_ERROR "kvasir_chip_generate_svd: ${_key} is required")
        endif()
    endforeach()
    find_package(
        Python3
        COMPONENTS Interpreter
        REQUIRED)
    set(_out ${CMAKE_BINARY_DIR}/chip_svd/${ARG_PART}.svd)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/chip_svd)
    get_filename_component(_script_name ${ARG_SCRIPT} NAME)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -B ${ARG_SCRIPT} --out ${_out}.tmp
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _output)
    if(_rc)
        message(FATAL_ERROR "${_script_name} failed (${_rc}):\n${_output}")
    endif()
    file(COPY_FILE ${_out}.tmp ${_out} ONLY_IF_DIFFERENT)
    file(REMOVE ${_out}.tmp)
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS ${ARG_SCRIPT} ${ARG_INPUTS})
    set(${out}
        ${_out}
        PARENT_SCOPE)
endfunction()

# kvasir_chip_variant(<out> VARIABLE <var> DEFAULT <part> DIR <dir> PACKAGE <name> [GENERATED_SVD <part> <script>
# [<input>...] SVD_VARIABLE <svd-var>]): which part of the package a project builds for. <var> (set by the project
# before include(kvasir.cmake), or -D<var>=...) names the file <dir>/<part>.cmake; without it the part is <part> of
# DEFAULT (and <var> is set to it in the caller). <out> receives that file: the caller include()s it, so its set()s land
# in the caller's scope. An unknown part stops with the parts the package knows. GENERATED_SVD: that part's SVD is made
# by <script> at configure time (kvasir_chip_generate_svd, <input>s = what it reads) and its path is set as <svd-var> in
# the caller; its variant file must not set <svd-var> itself.
function(kvasir_chip_variant out)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "VARIABLE;DEFAULT;DIR;PACKAGE;SVD_VARIABLE" "GENERATED_SVD")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "kvasir_chip_variant: unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_key VARIABLE DEFAULT DIR PACKAGE)
        if("${ARG_${_key}}" STREQUAL "")
            message(FATAL_ERROR "kvasir_chip_variant: ${_key} is required")
        endif()
    endforeach()
    list(LENGTH ARG_GENERATED_SVD _generated)
    if(_generated EQUAL 1 OR (_generated AND "${ARG_SVD_VARIABLE}" STREQUAL ""))
        message(FATAL_ERROR "kvasir_chip_variant: GENERATED_SVD needs <part> <script> [<input>...] and SVD_VARIABLE")
    endif()
    set(_part "${${ARG_VARIABLE}}")
    if("${_part}" STREQUAL "")
        set(_part ${ARG_DEFAULT})
        set(${ARG_VARIABLE}
            ${_part}
            PARENT_SCOPE)
    endif()
    set(_file ${ARG_DIR}/${_part}.cmake)
    if(NOT EXISTS ${_file})
        file(
            GLOB _known
            RELATIVE ${ARG_DIR}
            ${ARG_DIR}/*.cmake)
        list(TRANSFORM _known REPLACE "\\.cmake$" "")
        list(SORT _known)
        message(FATAL_ERROR "${ARG_VARIABLE} is '${_part}'; ${ARG_PACKAGE} knows: ${_known}")
    endif()
    if(_generated)
        list(POP_FRONT ARG_GENERATED_SVD _gen_part _gen_script) # the rest are the inputs
        if(_gen_part STREQUAL _part)
            file(STRINGS ${_file} _sets REGEX "set\\(${ARG_SVD_VARIABLE}[ )]")
            if(_sets)
                message(FATAL_ERROR "${_file}: ${_gen_part}'s SVD is generated; the variant file must not set "
                                    "${ARG_SVD_VARIABLE}")
            endif()
            kvasir_chip_generate_svd(
                _svd
                PART
                ${_gen_part}
                SCRIPT
                ${_gen_script}
                INPUTS
                ${ARG_GENERATED_SVD})
            set(${ARG_SVD_VARIABLE}
                ${_svd}
                PARENT_SCOPE)
        endif()
    endif()
    set(${out}
        ${_file}
        PARENT_SCOPE)
endfunction()
