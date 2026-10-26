# cmake -P tests/cmake_api/test_chip_package.cmake: cmake/chip_package.cmake - kvasir_chip_svd passes no write-only
# keyword (svd_convert's defaults are the profile), kvasir_chip_variant picks the default or the preset part, refuses an
# unknown one with the known parts, generates a GENERATED_SVD part's SVD (only that part, only when its content changed)
# and refuses a variant file that sets the generated SVD itself; kvasir_chip_generate_svd stops with the script's name.
# A refusal is a FATAL_ERROR, so each one runs as a second cmake -P of this file with -DCASE=<case>.
cmake_minimum_required(VERSION 3.28)
set(fixture ${CMAKE_CURRENT_LIST_DIR}/chip_package)
if(NOT DEFINED WORK)
    set(WORK ${CMAKE_CURRENT_BINARY_DIR}/test_chip_package_work)
endif()
set(CMAKE_BINARY_DIR ${WORK}/bin)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/chip_package.cmake)

# the stub records what kvasir_chip_svd passes
function(svd_convert)
    set_property(GLOBAL PROPERTY stub_svd_convert "${ARGN}")
endfunction()

function(expect what got want)
    if(NOT "${got}" STREQUAL "${want}")
        message(SEND_ERROR "${what}: got '${got}', want '${want}'")
    endif()
endfunction()

set(variant_args
    DIR
    ${fixture}/variants
    PACKAGE
    pkg
    GENERATED_SVD
    PARTB
    ${fixture}/gen_svd.py
    ${fixture}/in.txt
    SVD_VARIABLE
    fixture_svd_file)

if(DEFINED CASE)
    if(CASE STREQUAL "svd_extra")
        kvasir_chip_svd(t x.svd EXTRA)
    elseif(CASE STREQUAL "unknown_part")
        set(MY_MPU BOGUS)
        kvasir_chip_variant(_f VARIABLE MY_MPU DEFAULT PARTA ${variant_args})
    elseif(CASE STREQUAL "sets_generated_svd")
        set(MY_MPU PARTC)
        kvasir_chip_variant(
            _f
            VARIABLE
            MY_MPU
            DEFAULT
            PARTA
            DIR
            ${fixture}/variants
            PACKAGE
            pkg
            GENERATED_SVD
            PARTC
            ${fixture}/gen_svd.py
            SVD_VARIABLE
            fixture_svd_file)
    elseif(CASE STREQUAL "generator_fails")
        set(ENV{GEN_SVD_FAIL} 1)
        kvasir_chip_generate_svd(_svd PART PARTB SCRIPT ${fixture}/gen_svd.py)
    elseif(CASE STREQUAL "generated_without_variable")
        kvasir_chip_variant(
            _f
            VARIABLE
            MY_MPU
            DEFAULT
            PARTA
            DIR
            ${fixture}/variants
            PACKAGE
            pkg
            GENERATED_SVD
            PARTB
            ${fixture}/gen_svd.py)
    endif()
    return()
endif()

file(REMOVE_RECURSE ${WORK})

# (a) one form: target, SVD, the target's folder, no write-only keyword
kvasir_chip_svd(t x.svd)
get_property(_args GLOBAL PROPERTY stub_svd_convert)
expect("kvasir_chip_svd" "${_args}" "t;SVD_FILE;x.svd;OUTPUT_DIRECTORY;t")

# (c) the default part: its file, the variable set; the generated part not selected, so nothing generated
unset(MY_MPU)
unset(fixture_svd_file)
kvasir_chip_variant(_f VARIABLE MY_MPU DEFAULT PARTA ${variant_args})
expect("default part file" "${_f}" "${fixture}/variants/PARTA.cmake")
expect("default part variable" "${MY_MPU}" "PARTA")
expect("not generated: no SVD variable" "${fixture_svd_file}" "")
if(EXISTS ${WORK}/bin/chip_svd)
    message(SEND_ERROR "PARTA selected, but chip_svd/ was written")
endif()

# (d) a preset part wins; (f) it is the generated one: its SVD is written and its path set in the caller
set(MY_MPU PARTB)
kvasir_chip_variant(_f VARIABLE MY_MPU DEFAULT PARTA ${variant_args})
expect("preset part file" "${_f}" "${fixture}/variants/PARTB.cmake")
expect("generated SVD variable" "${fixture_svd_file}" "${WORK}/bin/chip_svd/PARTB.svd")
file(READ ${WORK}/bin/chip_svd/PARTB.svd _text)
expect("generated SVD text" "${_text}" "<device>generated</device>\n")
if(EXISTS ${WORK}/bin/chip_svd/PARTB.svd.tmp)
    message(SEND_ERROR "the .tmp file is left behind")
endif()

# (g) unchanged output keeps the file (and its mtime): the converter step is not re-run
file(TOUCH_NOCREATE ${WORK}/bin/chip_svd/PARTB.svd)
file(TIMESTAMP ${WORK}/bin/chip_svd/PARTB.svd _before "%s")
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.1)
kvasir_chip_generate_svd(_svd PART PARTB SCRIPT ${fixture}/gen_svd.py)
file(TIMESTAMP ${WORK}/bin/chip_svd/PARTB.svd _after "%s")
expect("unchanged generated SVD keeps its mtime" "${_after}" "${_before}")
expect("kvasir_chip_generate_svd path" "${_svd}" "${WORK}/bin/chip_svd/PARTB.svd")

# the refusals: (b) an extra argument, (e) an unknown part, (i) a generated part's variant file setting the SVD, (h) a
# failing generator, and GENERATED_SVD without SVD_VARIABLE
foreach(
    _case
    "svd_extra|kvasir_chip_svd: unknown arguments: EXTRA"
    "unknown_part|MY_MPU is 'BOGUS'; pkg knows: PARTA;PARTB;PARTC"
    "sets_generated_svd|PARTC.cmake: PARTC's SVD is generated; the variant file must not set fixture_svd_file"
    "generator_fails|gen_svd.py failed \\(3\\)"
    "generated_without_variable|GENERATED_SVD needs <part> <script> \\[<input>...\\] and SVD_VARIABLE")
    string(REPLACE "|" ";" _case "${_case}")
    list(GET _case 0 _name)
    list(GET _case 1 _want)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DCASE=${_name} -DWORK=${WORK}/${_name} -P ${CMAKE_CURRENT_LIST_FILE}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _out)
    string(REGEX REPLACE "[ \n]+" " " _out "${_out}") # CMake wraps a long message
    if(_rc EQUAL 0)
        message(SEND_ERROR "${_name}: passed, want a FATAL_ERROR '${_want}'")
    elseif(NOT _out MATCHES "${_want}")
        message(SEND_ERROR "${_name}: failed for another reason:\n${_out}")
    endif()
endforeach()
file(REMOVE_RECURSE ${WORK})
