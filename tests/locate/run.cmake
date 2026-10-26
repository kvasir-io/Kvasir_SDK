# kvasir_locate (cmake/kvasir_locate.cmake): where each root comes from, what a fetch asks FetchContent for (a stand-in
# module, probe/FetchContent.cmake: no network), the SDK submodules of a fetched SDK (scratch git repositories: no
# network), and the refusals. Each case runs in a cmake -P child of its own (fresh variables).
#
# cmake -DSOURCE_DIR=<Kvasir_SDK> -DWORK_DIR=<dir> -DGIT=<git> -P run.cmake
cmake_minimum_required(VERSION 3.28)

if(DEFINED CASE)
    # ---- the child: one case
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/probe")
    set(LOCATE_FETCH_LOG "${WORK_DIR}/${CASE}.fetch")
    set(LOCATE_FETCH_DIR "${WORK_DIR}/fetched_${CASE}")
    include("${SOURCE_DIR}/cmake/kvasir_locate.cmake")
    set(ws "${WORK_DIR}/ws")
    if(CASE STREQUAL "sibling")
        set(KVASIR_ROOT "${ws}/Kvasir_SDK")
        kvasir_locate(CHIP chip_x DEVICES)
    elseif(CASE STREQUAL "variable")
        set(KVASIR_ROOT "${ws}/Kvasir_SDK")
        set(CHIP_ROOT "${WORK_DIR}/elsewhere/chip_x")
        kvasir_locate(CHIP chip_x)
    elseif(CASE STREQUAL "environment")
        set(KVASIR_ROOT "${ws}/Kvasir_SDK")
        set(ENV{CHIP_ROOT} "${WORK_DIR}/elsewhere/chip_x")
        kvasir_locate(USE_ENVIRONMENT CHIP chip_x)
    elseif(CASE STREQUAL "environment_not_asked")
        set(KVASIR_ROOT "${ws}/Kvasir_SDK")
        set(ENV{CHIP_ROOT} "${WORK_DIR}/elsewhere/chip_x")
        kvasir_locate(CHIP chip_x)
    elseif(CASE STREQUAL "fetch")
        set(KVASIR_ROOT "${WORK_DIR}/alone/Kvasir_SDK")
        kvasir_locate(
            CHIP
            chip_x
            DEVICES
            PAIRED_CHIP
            chip_p
            PAIRED_CHIP_SUBMODULES
            src/chip/rp_common)
    elseif(CASE STREQUAL "fetch_paired_bare")
        set(KVASIR_ROOT "${WORK_DIR}/alone/Kvasir_SDK")
        kvasir_locate(PAIRED_CHIP chip_p)
    elseif(CASE STREQUAL "bad_marker")
        set(KVASIR_ROOT "${ws}/Kvasir_SDK")
        set(CHIP_ROOT "${ws}/kvasir_devices")
        kvasir_locate(CHIP chip_x)
    elseif(CASE STREQUAL "unknown")
        set(KVASIR_ROOT "${ws}/Kvasir_SDK")
        kvasir_locate(CHIPS chip_x)
    elseif(CASE STREQUAL "no_value")
        set(KVASIR_ROOT "${ws}/Kvasir_SDK")
        kvasir_locate(DEVICES CHIP)
    elseif(CASE STREQUAL "subs_without_paired")
        set(KVASIR_ROOT "${ws}/Kvasir_SDK")
        kvasir_locate(PAIRED_CHIP_SUBMODULES core)
    elseif(CASE STREQUAL "not_an_sdk")
        set(KVASIR_ROOT "${ws}/chip_x")
        kvasir_locate(DEVICES)
    elseif(CASE MATCHES "^sub_(path|all|copy|checkout)$")
        set(KVASIR_ROOT "${WORK_DIR}/sub_${CMAKE_MATCH_1}")
        if(NOT CASE STREQUAL "sub_checkout")
            set(kvasir_sdk_SOURCE_DIR "${KVASIR_ROOT}")
        endif()
        if(CASE STREQUAL "sub_copy")
            set(FETCHCONTENT_SOURCE_DIR_KVASIR_SDK "${KVASIR_ROOT}")
        endif()
        if(CASE STREQUAL "sub_all")
            kvasir_locate(SDK_SUBMODULES ALL)
        else()
            kvasir_locate(SDK_SUBMODULES uc_log/remote_fmt)
        endif()
    endif()
    message("RESULT CHIP_ROOT=${CHIP_ROOT}")
    message("RESULT KVASIR_DEVICES_ROOT=${KVASIR_DEVICES_ROOT}")
    message("RESULT PAIRED_CHIP_ROOT=${PAIRED_CHIP_ROOT}")
    return()
endif()

# ---- the parent
file(REMOVE_RECURSE "${WORK_DIR}")
set(failures 0)
set(ENV{GIT_CONFIG_GLOBAL} /dev/null)
set(ENV{GIT_CONFIG_NOSYSTEM} 1)
# submodules from local paths (git >= 2.38.1 refuses the file transport for them by default)
set(ENV{GIT_CONFIG_COUNT} 1)
set(ENV{GIT_CONFIG_KEY_0} protocol.file.allow)
set(ENV{GIT_CONFIG_VALUE_0} always)
set(ENV{GIT_AUTHOR_NAME} "Test")
set(ENV{GIT_AUTHOR_EMAIL} "test@example.invalid")
set(ENV{GIT_COMMITTER_NAME} "Test")
set(ENV{GIT_COMMITTER_EMAIL} "test@example.invalid")
set(ENV{GIT_AUTHOR_DATE} "2001-02-03T04:05:06Z")
set(ENV{GIT_COMMITTER_DATE} "2001-02-03T04:05:06Z")
set(ENV{GIT_CEILING_DIRECTORIES} "${WORK_DIR}")
unset(ENV{CHIP_ROOT})
unset(ENV{KVASIR_DEVICES_ROOT})

# a workspace: the SDK with siblings, a chip elsewhere, an SDK alone
set(ws "${WORK_DIR}/ws")
file(COPY "${SOURCE_DIR}/cmake/kvasir_locate.cmake" DESTINATION "${ws}/Kvasir_SDK/cmake")
file(WRITE "${ws}/chip_x/cmake/chip.cmake" "")
file(WRITE "${ws}/kvasir_devices/CMakeLists.txt" "")
file(WRITE "${WORK_DIR}/elsewhere/chip_x/cmake/chip.cmake" "")
file(COPY "${SOURCE_DIR}/cmake/kvasir_locate.cmake" DESTINATION "${WORK_DIR}/alone/Kvasir_SDK/cmake")

function(run case)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DCASE=${case} -DSOURCE_DIR=${SOURCE_DIR} -DWORK_DIR=${WORK_DIR} -DGIT=${GIT} -P
                ${CMAKE_CURRENT_LIST_FILE}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out)
    string(REGEX REPLACE "[ \n]+" " " out "${out}")
    set(rc_${case}
        ${rc}
        PARENT_SCOPE)
    set(out_${case}
        "${out}"
        PARENT_SCOPE)
endfunction()

function(fail case text)
    message(SEND_ERROR "FAIL ${case}: ${text}\n  output: ${out_${case}}")
    set(failures
        1
        PARENT_SCOPE)
endfunction()

# expect(<case> <rc 0|fail> <text>...): exit code and the texts in the output
function(expect case want)
    run(${case})
    if(want STREQUAL "0" AND NOT rc_${case} EQUAL 0)
        fail(${case} "exit ${rc_${case}}")
    elseif(want STREQUAL "fail" AND rc_${case} EQUAL 0)
        fail(${case} "configured, a refusal expected")
    endif()
    foreach(t IN LISTS ARGN)
        string(FIND "${out_${case}}" "${t}" i)
        if(i EQUAL -1)
            fail(${case} "no '${t}'")
        endif()
    endforeach()
    if(failures)
        set(failures
            1
            PARENT_SCOPE)
    else()
        message(STATUS "ok ${case}")
    endif()
endfunction()

function(expect_fetch_log case)
    set(f "${WORK_DIR}/${case}.fetch")
    set(log "")
    if(EXISTS "${f}")
        file(READ "${f}" log)
    endif()
    string(JOIN "" want ${ARGN})
    if(NOT log STREQUAL want)
        message(SEND_ERROR "FAIL ${case}: FetchContent calls\n${log}\nwanted\n${want}")
        set(failures
            1
            PARENT_SCOPE)
    endif()
endfunction()

expect(sibling 0 "RESULT CHIP_ROOT=${ws}/chip_x" "RESULT KVASIR_DEVICES_ROOT=${ws}/kvasir_devices" "(next to the SDK)")
expect_fetch_log(sibling "")
expect(variable 0 "RESULT CHIP_ROOT=${WORK_DIR}/elsewhere/chip_x" "(CMake variable)")
expect(environment 0 "RESULT CHIP_ROOT=${WORK_DIR}/elsewhere/chip_x" "(environment)")
expect(environment_not_asked 0 "RESULT CHIP_ROOT=${ws}/chip_x")
set(_url https://github.com/kvasir-io)
expect(fetch 0 "RESULT CHIP_ROOT=${WORK_DIR}/fetched_fetch/chip_x-src" "(fetched)"
       "RESULT KVASIR_DEVICES_ROOT=${WORK_DIR}/fetched_fetch/kvasir_devices-src"
       "RESULT PAIRED_CHIP_ROOT=${WORK_DIR}/fetched_fetch/chip_p-src")
expect_fetch_log(
    fetch
    "declare chip_x GIT_REPOSITORY ${_url}/chip_x.git GIT_TAG master GIT_SHALLOW TRUE SOURCE_SUBDIR fetch-only\n"
    "make_available chip_x\n"
    "declare kvasir_devices GIT_REPOSITORY ${_url}/kvasir_devices.git GIT_TAG master GIT_SHALLOW TRUE SOURCE_SUBDIR fetch-only\n"
    "make_available kvasir_devices\n"
    "declare chip_p GIT_REPOSITORY ${_url}/chip_p.git GIT_TAG master GIT_SHALLOW TRUE GIT_SUBMODULES src/chip/rp_common SOURCE_SUBDIR fetch-only\n"
    "submodules chip_p [src/chip/rp_common]\n"
    "make_available chip_p\n")
expect(fetch_paired_bare 0 "RESULT PAIRED_CHIP_ROOT=${WORK_DIR}/fetched_fetch_paired_bare/chip_p-src")
expect_fetch_log(
    fetch_paired_bare
    "declare chip_p GIT_REPOSITORY ${_url}/chip_p.git GIT_TAG master GIT_SHALLOW TRUE GIT_SUBMODULES  SOURCE_SUBDIR fetch-only\n"
    "submodules chip_p []\n"
    "make_available chip_p\n")
expect(bad_marker fail
       "kvasir_locate: CHIP_ROOT '${ws}/kvasir_devices' (CMake variable) has no cmake/chip.cmake: not chip_x")
expect(unknown fail "kvasir_locate: unknown argument(s) CHIPS;chip_x")
expect(no_value fail "kvasir_locate: no value given for CHIP")
expect(subs_without_paired fail "kvasir_locate: PAIRED_CHIP_SUBMODULES without PAIRED_CHIP")
expect(not_an_sdk fail "is no Kvasir SDK")

# ---- the SDK's submodules: sdk -> uc_log -> {remote_fmt -> string_constant, rtt}, cloned without submodules
function(git dir)
    execute_process(
        COMMAND ${GIT} ${ARGN}
        WORKING_DIRECTORY "${dir}"
        RESULT_VARIABLE rc
        OUTPUT_QUIET
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} in ${dir}: ${err}")
    endif()
endfunction()
function(repo name)
    set(dir "${WORK_DIR}/repos/${name}")
    file(MAKE_DIRECTORY "${dir}")
    git("${dir}" init -q -b master)
    file(WRITE "${dir}/${name}.txt" "${name}\n")
    git("${dir}" add -A)
    foreach(sub IN LISTS ARGN)
        git("${dir}" submodule add -q "${WORK_DIR}/repos/${sub}" ${sub})
    endforeach()
    git("${dir}" commit -q -m ${name})
endfunction()
repo(string_constant)
repo(remote_fmt string_constant)
repo(rtt)
repo(uc_log remote_fmt rtt)
file(COPY "${SOURCE_DIR}/cmake/kvasir_locate.cmake" DESTINATION "${WORK_DIR}/repos/sdk_seed/cmake")
repo(sdk_seed)
set(sdk "${WORK_DIR}/repos/sdk_seed")
git("${sdk}" submodule add -q "${WORK_DIR}/repos/uc_log" uc_log)
git("${sdk}" commit -q -m uc_log)
foreach(c path all copy checkout)
    git("${WORK_DIR}" clone -q "${sdk}" "sub_${c}")
endforeach()

# populated(<case> <path> yes|no)
function(populated case path want)
    file(GLOB entries "${WORK_DIR}/sub_${case}/${path}/*")
    if(entries)
        set(have yes)
    else()
        set(have no)
    endif()
    if(NOT have STREQUAL want)
        message(SEND_ERROR "FAIL sub_${case}: ${path} populated: ${have}, wanted ${want}")
        set(failures
            1
            PARENT_SCOPE)
    endif()
endfunction()

expect(sub_path 0)
populated(path uc_log yes)
populated(path uc_log/remote_fmt yes)
populated(path uc_log/remote_fmt/string_constant yes)
populated(path uc_log/rtt no)
expect(sub_all 0)
populated(all uc_log/remote_fmt/string_constant yes)
populated(all uc_log/rtt yes)
# a local copy (FETCHCONTENT_SOURCE_DIR_KVASIR_SDK) and a checkout (not the FetchContent clone) are used as they are
expect(sub_copy 0)
populated(copy uc_log no)
expect(sub_checkout 0)
populated(checkout uc_log no)

if(failures)
    message(FATAL_ERROR "kvasir_locate: failures above")
endif()
message(STATUS "kvasir_locate: all cases ok")
