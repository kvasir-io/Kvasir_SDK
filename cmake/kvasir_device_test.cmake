# On-device tests as ctests (included by util.cmake): a test image (kvasir/Test/Runner.hpp) built like any image, and -
# with KVASIR_DEVICE_TESTS=ON - one ctest per variant that flashes it on the tree's board and judges the run with
# tools/kvasir_test.py (exit 77 = the tree's probe is not on USB: ctest reports the test skipped).
#
# add_kvasir_device_test(<name> SOURCES ... [VARIANTS v...] [TEST_VARIANTS v...] [TIMEOUT s] [FILTER glob...] [LABELS
# l...] <any other kvasir_executable_variants argument>) the image (kvasir_executable_variants(<name> ...)), then
# kvasir_device_test_ctests(<name> ...) kvasir_device_test_ctests(<name> [TEST_VARIANTS v...] [TIMEOUT s] [FILTER
# glob...] [LABELS l...] [DONE <regex> [PASS <regex>]]) the ctests alone, for a tree that makes its images its own way
# (its own wrapper of kvasir_executable_variants). DONE: an image that is not a Kvasir::Test image but judges itself in
# one line (kvasir_test.py's DONE-line mode, --done/--pass)
#
# TEST_VARIANTS: which variants run on the board (default sanitize: judge a change with the sanitize build). Each
# variant is two ctests: device.<name>.<v>.build (cmake --build the image; a fixture, ctest does not build by itself)
# and device.<name>.<v> (kvasir_test.py run, JUnit into <build>/kvasir_test/<name>_<v>.xml). Labels `device` and the
# given ones; RESOURCE_LOCK on the tree's J-Link, so `ctest -j` never runs two tests on one probe:
#
# cmake -DKVASIR_DEVICE_TESTS=ON <build> ctest --test-dir <build> -L device
#
# OFF (the default) adds no test and no target: the tree is what it was.
option(KVASIR_DEVICE_TESTS "register ctests that flash and run test images on the tree's board" OFF)

function(kvasir_device_test_ctests name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "TIMEOUT;DONE;PASS" "TEST_VARIANTS;FILTER;LABELS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "kvasir_device_test_ctests(${name}): unknown argument ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(ARG_PASS AND NOT ARG_DONE)
        message(FATAL_ERROR "kvasir_device_test_ctests(${name}): PASS without DONE")
    endif()
    if(NOT KVASIR_DEVICE_TESTS)
        return()
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    enable_testing()
    if(NOT ARG_TEST_VARIANTS)
        set(ARG_TEST_VARIANTS sanitize)
    endif()
    # TIMEOUT: the run's, kvasir_test.py --timeout; ctest gives the flash and the printer a minute on top
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif()
    math(EXPR _ctest_timeout "${ARG_TIMEOUT} + 60")
    set(_extra)
    foreach(f IN LISTS ARG_FILTER)
        list(APPEND _extra --filter ${f})
    endforeach()
    if(ARG_DONE)
        list(APPEND _extra --done ${ARG_DONE})
        if(ARG_PASS)
            list(APPEND _extra --pass ${ARG_PASS})
        endif()
    endif()
    foreach(v IN LISTS ARG_TEST_VARIANTS)
        set(_target ${name}_${v})
        if(NOT TARGET ${_target})
            message(FATAL_ERROR "kvasir_device_test_ctests(${name}): no image target ${_target}")
        endif()
        add_test(NAME device.${name}.${v}.build COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
                                                        ${_target})
        set_tests_properties(device.${name}.${v}.build PROPERTIES FIXTURES_SETUP device.${name}.${v} LABELS
                                                                  "device;device_build")
        add_test(
            NAME device.${name}.${v}
            COMMAND ${Python3_EXECUTABLE} -B ${KVASIR_ROOT}/tools/kvasir_test.py run ${CMAKE_BINARY_DIR} ${_target}
                    --timeout ${ARG_TIMEOUT} --junit ${CMAKE_BINARY_DIR}/kvasir_test/${_target}.xml ${_extra})
        set_tests_properties(
            device.${name}.${v}
            PROPERTIES LABELS
                       "device;${ARG_LABELS}"
                       SKIP_RETURN_CODE
                       77
                       RESOURCE_LOCK
                       "jlink:${JLINK_PROBE}${JLINK_IP}"
                       TIMEOUT
                       ${_ctest_timeout}
                       FIXTURES_REQUIRED
                       device.${name}.${v})
    endforeach()
endfunction()

function(add_kvasir_device_test name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "TIMEOUT" "TEST_VARIANTS;FILTER;LABELS")
    kvasir_executable_variants(${name} ${ARG_UNPARSED_ARGUMENTS})
    set(_ctest_args)
    if(ARG_TIMEOUT)
        list(APPEND _ctest_args TIMEOUT ${ARG_TIMEOUT})
    endif()
    if(ARG_TEST_VARIANTS)
        list(APPEND _ctest_args TEST_VARIANTS ${ARG_TEST_VARIANTS})
    endif()
    if(ARG_FILTER)
        list(APPEND _ctest_args FILTER ${ARG_FILTER})
    endif()
    if(ARG_LABELS)
        list(APPEND _ctest_args LABELS ${ARG_LABELS})
    endif()
    kvasir_device_test_ctests(${name} ${_ctest_args})
endfunction()
