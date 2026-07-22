cmake_minimum_required(VERSION 3.28)

project(kvasir_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

set(USE_SANITIZER
    ""
    CACHE STRING "sanitizer to enable (address, undefined, ...)")
set(USE_STD_LIB
    ""
    CACHE STRING "standard library to use (libc++ or empty for the default)")
set(USE_STDLIB_DEBUG
    false
    CACHE BOOL "enable the standard library debug/hardening mode")

if(USE_STDLIB_DEBUG)
    if("${USE_STD_LIB}" STREQUAL "libc++")
        add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)
    else()
        add_compile_definitions(_GLIBCXX_DEBUG _GLIBCXX_DEBUG_PEDANTIC)
    endif()
endif()

if("${USE_STD_LIB}" STREQUAL "libc++")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++ -fuse-ld=lld)
endif()

if(USE_SANITIZER)
    add_compile_options(-fsanitize=${USE_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${USE_SANITIZER})
endif()

# the Kvasir headers with all register accesses routed through the test mock
add_library(kvasir_mocked INTERFACE)
target_include_directories(kvasir_mocked INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../src)
target_compile_definitions(kvasir_mocked INTERFACE KVASIR_REGISTER_MOCK)

function(kvasir_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE kvasir_mocked)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kvasir_add_test(kvasir_test_register_write write_tests.cpp)
kvasir_add_test(kvasir_test_register_read read_tests.cpp)
kvasir_add_test(kvasir_test_register_rmw rmw_tests.cpp)
kvasir_add_test(kvasir_test_register_sequence_point sequence_point_tests.cpp)
kvasir_add_test(kvasir_test_register_multi_register multi_register_tests.cpp)
kvasir_add_test(kvasir_test_register_special_access special_access_tests.cpp)
