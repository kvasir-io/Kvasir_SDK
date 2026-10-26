# cmake -P tests/cmake_api/test_runtime_archives.cmake: cmake/kvasir_runtime_archives.cmake - the archive names per
# optimisation set and sanitizer, a second claim with the same flags, and the refusals (another flag list for one
# archive, an unknown set), each refusal in a child cmake -P that must fail with its text.
cmake_minimum_required(VERSION 3.28)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/kvasir_runtime_archives.cmake)

if(DEFINED CASE)
    if(CASE STREQUAL "other_flags")
        kvasir_runtime_archive_claim(kvasir_rt_size "-Oz;-flto" image_a)
        kvasir_runtime_archive_claim(kvasir_rt_size "-O2;-flto" image_b)
    elseif(CASE STREQUAL "unknown_set")
        kvasir_runtime_archive_name(_n rt fast "")
    endif()
    return()
endif()

function(expect what got want)
    if(NOT "${got}" STREQUAL "${want}")
        message(SEND_ERROR "${what}: got '${got}', want '${want}'")
    endif()
endfunction()

foreach(_case "rt|size||kvasir_rt_size" "cxx|speed||kvasir_cxx_speed" "c|debug|-fsanitize=x|kvasir_c_debug_san"
              "c_malloc|size|-fsanitize=x,-fstack-protector-strong|kvasir_c_malloc_size_san")
    string(REPLACE "|" ";" _parts "${_case}")
    list(GET _parts 0 _lib)
    list(GET _parts 1 _set)
    list(GET _parts 2 _san)
    list(GET _parts 3 _want)
    string(REPLACE "," ";" _san "${_san}")
    kvasir_runtime_archive_name(_got ${_lib} ${_set} "${_san}")
    expect("name ${_lib} ${_set} '${_san}'" "${_got}" "${_want}")
endforeach()

kvasir_runtime_archive_claim(kvasir_rt_size "-Oz;-flto" image_a)
expect("first claim" "${kvasir_rt_size_NEW}" "TRUE")
kvasir_runtime_archive_claim(kvasir_rt_size "-Oz;-flto" image_b)
expect("same flags again" "${kvasir_rt_size_NEW}" "FALSE")
kvasir_runtime_archive_claim(kvasir_rt_size_san "-Oz;-flto;-fsanitize=x" image_b)
expect("another archive" "${kvasir_rt_size_san_NEW}" "TRUE")

foreach(
    _refusal
    "other_flags|runtime archive kvasir_rt_size: image_b compiles it with '-O2;-flto', but image_a already made it with '-Oz;-flto'"
    "unknown_set|runtime archive rt: optimisation set 'fast': size, speed or debug")
    string(FIND "${_refusal}" "|" _bar)
    string(SUBSTRING "${_refusal}" 0 ${_bar} _name)
    math(EXPR _bar "${_bar} + 1")
    string(SUBSTRING "${_refusal}" ${_bar} -1 _text)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DCASE=${_name} -P ${CMAKE_CURRENT_LIST_FILE}
        RESULT_VARIABLE _rc
        ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _err "${_err}")
    string(FIND "${_err}" "${_text}" _at)
    if(_rc EQUAL 0 OR _at EQUAL -1)
        message(SEND_ERROR "refusal ${_name}: rc ${_rc}, want the text '${_text}' in: ${_err}")
    endif()
endforeach()
