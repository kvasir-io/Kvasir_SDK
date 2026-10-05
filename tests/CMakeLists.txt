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
# the RP alias backend on the mock (chip_rp_common/RegisterAlias.hpp, reached as rp_common_alias/); only where the
# chip_rp_common checkout sits next to the SDK, not in the SDK's own CI
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../chip_rp_common/RegisterAlias.hpp)
    kvasir_add_test(kvasir_test_register_atomic atomic_tests.cpp)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/alias_include)
    file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/../../chip_rp_common
         ${CMAKE_CURRENT_BINARY_DIR}/alias_include/rp_common_alias SYMBOLIC)
    target_include_directories(kvasir_test_register_atomic PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/alias_include)
endif()
kvasir_add_test(kvasir_test_register_model model_tests.cpp)
kvasir_add_test(kvasir_test_register_wait register_wait_tests.cpp)
kvasir_add_test(kvasir_test_shared_isr shared_isr_tests.cpp)
kvasir_add_test(kvasir_test_rate_limiter rate_limiter_tests.cpp)
target_link_libraries(kvasir_test_rate_limiter PRIVATE kvasir::test_support)
kvasir_add_test(kvasir_test_register_factories factories_tests.cpp)
kvasir_add_test(kvasir_test_io io_tests.cpp)

kvasir_add_multicore_test(kvasir_test_spinlock spinlock_tests.cpp)
kvasir_add_multicore_test(kvasir_test_seqlock seqlock_tests.cpp)
kvasir_add_multicore_test(kvasir_test_queue queue_tests.cpp)
kvasir_add_multicore_test(kvasir_test_bip_buffer bip_buffer_tests.cpp)
kvasir_add_multicore_test(kvasir_test_task_queue task_queue_tests.cpp)
kvasir_add_multicore_test(kvasir_test_critical_section critical_section_tests.cpp)
kvasir_add_test(kvasir_test_critical_section_injected critical_section_injected_tests.cpp)

kvasir_add_host_test(kvasir_test_util_saturating_value util_saturating_value_tests.cpp)
kvasir_add_host_test(kvasir_test_util_rollover util_rollover_tests.cpp)
kvasir_add_host_test(kvasir_test_util_crc util_crc_tests.cpp)
kvasir_add_host_test(kvasir_test_persistent persistent_tests.cpp)
kvasir_add_host_test(kvasir_test_crash_record crash_record_tests.cpp)
kvasir_add_host_test(kvasir_test_boot_guard boot_guard_tests.cpp)
kvasir_add_host_test(kvasir_test_health health_tests.cpp)
kvasir_add_host_test(kvasir_test_image_check image_check_tests.cpp)
kvasir_add_host_test(kvasir_test_cell_store cell_store_tests.cpp)
kvasir_add_host_test(kvasir_test_executor executor_tests.cpp)
kvasir_add_host_test(kvasir_test_loop_profiler loop_profiler_tests.cpp)
target_compile_definitions(kvasir_test_image_check PRIVATE KVASIR_IMAGE_CRC_TEST=1)
# Util/Check.hpp without USE_UC_LOG: the checks themselves, their log lines in the never-evaluated form
kvasir_add_host_test(kvasir_test_check check_tests.cpp)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME kvasir_test_check_must_fail
         COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/check_must_fail/test_check_must_fail.py
                 ${CMAKE_CXX_COMPILER} ${CMAKE_CURRENT_LIST_DIR}/../src)
add_test(NAME kvasir_test_cell_store_must_fail
         COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/cell_store_must_fail/test_cell_store_must_fail.py
                 ${CMAKE_CXX_COMPILER} ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_CURRENT_LIST_DIR})
# Crc32 against the copies that stay outside the SDK: uc_log's (nested here, absent in a checkout without its
# submodules) and fs's log store (only where the fs checkout sits next to the SDK)
set(_crc_uc_log ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/src/uc_log/detail/HexImage.hpp)
set(_crc_fs ${CMAKE_CURRENT_SOURCE_DIR}/../../fs/src/fs/log/Store.hpp)
if(EXISTS ${_crc_uc_log} OR EXISTS ${_crc_fs})
    kvasir_add_host_test(kvasir_test_util_crc_cross util_crc_cross_tests.cpp)
    if(EXISTS ${_crc_uc_log})
        target_include_directories(kvasir_test_util_crc_cross PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/src)
        target_compile_definitions(kvasir_test_util_crc_cross PRIVATE KVASIR_CRC_CROSS_UC_LOG=1)
    endif()
    if(EXISTS ${_crc_fs})
        target_include_directories(kvasir_test_util_crc_cross PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../fs/src)
        target_compile_definitions(kvasir_test_util_crc_cross PRIVATE KVASIR_CRC_CROSS_FS=1)
    endif()
endif()
kvasir_add_host_test(kvasir_test_util_prescaler util_prescaler_tests.cpp)
# the RP PLL search (chip_rp_common/PllSearch.hpp) against the one it replaced, where the chip_rp_common checkout sits
# next to the SDK
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../chip_rp_common/PllSearch.hpp)
    target_include_directories(kvasir_test_util_prescaler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../chip_rp_common)
endif()
# Prescaler's failures are compile errors with the numbers in them: snippets that must fail, with this compiler
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME kvasir_test_util_prescaler_must_fail
         COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/prescaler_must_fail/test_prescaler_must_fail.py
                 ${CMAKE_CXX_COMPILER} ${CMAKE_CURRENT_LIST_DIR}/../src)
# Named compile errors (Util/Diagnostic.hpp, Register/Diagnostic.hpp, the Startup reports): snippets that must fail with
# the register, field or resource named, with this compiler
add_test(NAME kvasir_test_diagnostics_must_fail
         COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/diagnostics_must_fail/test_diagnostics_must_fail.py
                 ${CMAKE_CXX_COMPILER} ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_CURRENT_LIST_DIR})
kvasir_add_host_test(kvasir_test_diagnostic diagnostic_tests.cpp)
# Crc's refusals (a wrong check value, a table size, resume where refin != refout), with this compiler
add_test(NAME kvasir_test_util_crc_must_fail
         COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_LIST_DIR}/crc_must_fail/test_crc_must_fail.py
                 ${CMAKE_CXX_COMPILER} ${CMAKE_CURRENT_LIST_DIR}/../src)
kvasir_add_host_test(kvasir_test_util_periodic util_periodic_tests.cpp)
kvasir_add_host_test(kvasir_test_hooks hooks_tests.cpp)
kvasir_add_host_test(kvasir_test_panic panic_tests.cpp)
kvasir_add_host_test(kvasir_test_announced_reset announced_reset_tests.cpp)
kvasir_add_host_test(kvasir_test_util_control_pid util_control_pid_tests.cpp)
kvasir_add_host_test(kvasir_test_util_control_filters util_control_filters_tests.cpp)
target_link_libraries(kvasir_test_util_periodic PRIVATE kvasir::test_support)
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
    kvasir/Util/BootGuard.hpp # includes FaultHandler.hpp (chip file); its pure part, BootGuardRules.hpp, is tested
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

# linker/common_stack_body.inc.ld: the stack fills what RAM the other sections leave, in one link, with lld and GNU ld
# (a missing linker is a skip inside the test)
add_test(NAME kvasir_test_stack_fill COMMAND ${Python3_EXECUTABLE} -B
                                             ${CMAKE_CURRENT_LIST_DIR}/stack_fill/test_stack_fill.py)

add_test(NAME kvasir_test_bench_tool COMMAND ${Python3_EXECUTABLE}
                                             ${CMAKE_CURRENT_LIST_DIR}/../tools/test_kvasir_bench.py)

# cmake/tools/patch_image_crc.py on ARM images built here (a missing toolchain is a skip inside the test)
add_test(NAME kvasir_test_patch_image_crc COMMAND ${Python3_EXECUTABLE} -B
                                                  ${CMAKE_CURRENT_LIST_DIR}/image_crc/test_patch_image_crc.py)
