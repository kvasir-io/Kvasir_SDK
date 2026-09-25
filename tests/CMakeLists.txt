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

# kvasir::sdk and kvasir::test_support, as other repos' host tests use them
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/kvasir_sdk_targets.cmake)

# the plain Kvasir headers, for tests that do not touch hardware registers
add_library(kvasir_headers INTERFACE)
target_include_directories(kvasir_headers INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../src)

# the Kvasir headers with all register accesses routed through the test mock
add_library(kvasir_mocked INTERFACE)
target_link_libraries(kvasir_mocked INTERFACE kvasir_headers)
target_compile_definitions(kvasir_mocked INTERFACE KVASIR_REGISTER_MOCK)

find_package(Threads REQUIRED)

# a test that drives register accesses through the mock recorder
function(kvasir_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE kvasir_mocked)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# The cross-core primitives, exercised with two std::threads. KVASIR_MULTICORE selects the cross-core defaults (Queue's
# SyncThread, the shim lock) the way CORE1_STACK_SIZE does for a target build; run these with -DUSE_SANITIZER=thread to
# have TSan check the ordering.
function(kvasir_add_multicore_test name)
    kvasir_add_test(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE Threads::Threads)
    target_compile_definitions(${name} PRIVATE KVASIR_MULTICORE=1)
endfunction()

# a pure host test with no register mock involved
function(kvasir_add_host_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE kvasir_headers)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kvasir_add_test(kvasir_test_register_write write_tests.cpp)
kvasir_add_test(kvasir_test_register_read read_tests.cpp)
kvasir_add_test(kvasir_test_register_rmw rmw_tests.cpp)
kvasir_add_test(kvasir_test_register_sequence_point sequence_point_tests.cpp)
kvasir_add_test(kvasir_test_register_multi_register multi_register_tests.cpp)
kvasir_add_test(kvasir_test_register_special_access special_access_tests.cpp)
kvasir_add_test(kvasir_test_rate_limiter rate_limiter_tests.cpp)
target_link_libraries(kvasir_test_rate_limiter PRIVATE kvasir::test_support)
kvasir_add_test(kvasir_test_register_factories factories_tests.cpp)
kvasir_add_test(kvasir_test_io io_tests.cpp)

kvasir_add_multicore_test(kvasir_test_spinlock spinlock_tests.cpp)
kvasir_add_multicore_test(kvasir_test_seqlock seqlock_tests.cpp)
kvasir_add_multicore_test(kvasir_test_queue queue_tests.cpp)
kvasir_add_multicore_test(kvasir_test_task_queue task_queue_tests.cpp)

kvasir_add_host_test(kvasir_test_util_saturating_value util_saturating_value_tests.cpp)
kvasir_add_host_test(kvasir_test_util_literals util_literals_tests.cpp)
kvasir_add_host_test(kvasir_test_util_bitfield util_bitfield_tests.cpp)
kvasir_add_host_test(kvasir_test_util_static_vector util_static_vector_tests.cpp)
kvasir_add_host_test(kvasir_test_util_static_containers util_static_containers_tests.cpp)
kvasir_add_host_test(kvasir_test_util_static_function util_static_function_tests.cpp)
kvasir_add_host_test(kvasir_test_mpl mpl_tests.cpp)
kvasir_add_host_test(kvasir_test_resources resources_tests.cpp)
kvasir_add_host_test(kvasir_test_startup_list startup_list_tests.cpp)
kvasir_add_host_test(kvasir_test_atomic_queue atomic_queue_tests.cpp)

# ---------------------------------------------------------------------------
# header self containment
#
# Every public header must compile on its own. Each header gets a generated translation unit that includes it twice (the
# second include also checks that the include guard works).
#
# Headers that legitimately cannot be compiled standalone on the host are listed below together with the reason; they
# need either a chip support file or the uc_log/remote_fmt stack, neither of which is available in this build.
set(KVASIR_NON_SELF_CONTAINED_HEADERS
    kvasir/Atomic/detail/arm_Common_atomic.hpp # needs core/Nvic.hpp from a chip file
    kvasir/Atomic/detail/arm_Clang_atomic.hpp # needs core/Nvic.hpp from a chip file
    kvasir/Atomic/detail/arm_GCC_atomic.hpp # needs core/Nvic.hpp from a chip file
    kvasir/Util/FaultHandler.hpp # needs core/Fault.hpp from a chip file
    kvasir/Util/BootLog.hpp # includes FaultHandler.hpp (chip file), uc_log and the generated cmake_git_version header
    kvasir/Util/ubsan.hpp # needs the remote_fmt _sc literal
    kvasir/StartUp/StartUp.hpp # needs uc_log and a chip file
    kvasir/StartUp/SecondaryCore.hpp # includes StartUp.hpp, needs uc_log and a chip file
)

file(
    GLOB_RECURSE KVASIR_ALL_HEADERS
    RELATIVE ${CMAKE_CURRENT_LIST_DIR}/../src
    ${CMAKE_CURRENT_LIST_DIR}/../src/kvasir/*.hpp)
list(SORT KVASIR_ALL_HEADERS)

set(KVASIR_SELF_CONTAINED_SOURCES "")
foreach(header IN LISTS KVASIR_ALL_HEADERS)
    if(header IN_LIST KVASIR_NON_SELF_CONTAINED_HEADERS)
        continue()
    endif()
    string(REGEX REPLACE "[/.]" "_" mangled "${header}")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/self_contained/${mangled}.cpp")
    file(WRITE ${generated} "// generated by tests/CMakeLists.txt -- do not edit\n" "#include \"${header}\"\n"
                            "#include \"${header}\"\n")
    list(APPEND KVASIR_SELF_CONTAINED_SOURCES ${generated})
endforeach()

# brigand.hpp is a vendored third party header, do not hold it to -Wall -Wextra
add_library(kvasir_self_contained_headers OBJECT ${KVASIR_SELF_CONTAINED_SOURCES})
target_link_libraries(kvasir_self_contained_headers PRIVATE kvasir_mocked)
target_compile_options(kvasir_self_contained_headers PRIVATE -Wall -Wextra -Werror)

# make the check visible as a test as well so `ctest` reports on it
add_test(NAME kvasir_test_header_self_containment COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
                                                          kvasir_self_contained_headers)

# cmake/tools/check_ram_funcs.py (the post-link RAM function check) against ARM images built with every ARM toolchain
# found here; a missing toolchain is a skip inside the test, not a failure
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME kvasir_test_check_ram_funcs COMMAND ${Python3_EXECUTABLE}
                                                  ${CMAKE_CURRENT_LIST_DIR}/ram_funcs/test_check_ram_funcs.py)
