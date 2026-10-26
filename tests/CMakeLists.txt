cmake_minimum_required(VERSION 3.28)

project(kvasir_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# no test imports a C++ module: without the scan a reconfigure leaves every object up to date (with it, the dyndep files
# are regenerated and all of them rebuild)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# tools/tidy.py reads the tree's compile_commands.json (CI configures without the justfile's flag)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# This tree's own names: cmake_helpers' USE_SANITIZER / USE_STD_LIB / USE_STDLIB_DEBUG (read by the nested svd_converter
# and remote_fmt builds, and forwarded to every host build) mean other values
set(KVASIR_TEST_SANITIZER
    ""
    CACHE STRING "sanitizer to enable, passed to -fsanitize= (address, undefined, thread, ...)")
set(KVASIR_TEST_STD_LIB
    ""
    CACHE STRING "standard library to use (libc++ or empty for the default)")
set(KVASIR_TEST_STDLIB_DEBUG
    false
    CACHE BOOL "enable the standard library debug/hardening mode")

# the standard library's debug mode: the directory's definitions, and the must-fail snippets'
# (kvasir_add_must_fail_test)
set(KVASIR_STDLIB_DEBUG_DEFINES)
if(KVASIR_TEST_STDLIB_DEBUG)
    if("${KVASIR_TEST_STD_LIB}" STREQUAL "libc++")
        set(KVASIR_STDLIB_DEBUG_DEFINES _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)
    else()
        set(KVASIR_STDLIB_DEBUG_DEFINES _GLIBCXX_DEBUG _GLIBCXX_DEBUG_PEDANTIC)
    endif()
    add_compile_definitions(${KVASIR_STDLIB_DEBUG_DEFINES})
endif()

if("${KVASIR_TEST_STD_LIB}" STREQUAL "libc++")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++ -fuse-ld=lld)
endif()

if(KVASIR_TEST_SANITIZER)
    add_compile_options(-fsanitize=${KVASIR_TEST_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${KVASIR_TEST_SANITIZER})
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
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The test kinds. Each labels its ctest entry, for `ctest -L/-LE` and `just test <config> "" <labels>`: unit
# kvasir_add_host_test      the plain headers mock       kvasir_add_mock_test      register accesses through the mock
# recorder (kvasir_test.hpp) threaded   kvasir_add_threaded_test  the cross-core primitives on std::threads,
# KVASIR_MULTICORE=1 must_fail  kvasir_add_must_fail_test snippets that must not compile (tests/tooling/must_fail.py)
# tool       kvasir_add_python_test    the Python and CMake tools, the ARM/LLVM toolchain tests compile    (added) the
# check is that it compiles, or a scan of the sources cross_repo (added)                   needs a nested checkout
# (uc_log, remote_fmt); left out without it Each function takes a trailing `LABELS <label>...` that adds labels.
function(_kvasir_add_cxx_test name label lib)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "" "LABELS")
    add_executable(${name} ${arg_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE ${lib})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${label};${arg_LABELS}")
endfunction()

# a pure host test with no register mock involved
function(kvasir_add_host_test name)
    _kvasir_add_cxx_test(${name} unit kvasir_headers ${ARGN})
endfunction()

# a test that drives register accesses through the mock recorder
function(kvasir_add_mock_test name)
    _kvasir_add_cxx_test(${name} mock kvasir_mocked ${ARGN})
endfunction()

# The cross-core primitives, exercised with std::threads. KVASIR_MULTICORE selects the cross-core defaults (Queue's
# SyncThread, the shim lock) the way CORE1_STACK_SIZE does for a target build. CI runs every test under TSan as well
# (ci.yml, -DKVASIR_TEST_SANITIZER=thread), which checks their ordering.
function(kvasir_add_threaded_test name)
    _kvasir_add_cxx_test(${name} threaded kvasir_headers ${ARGN})
    target_link_libraries(${name} PRIVATE Threads::Threads)
    target_compile_definitions(${name} PRIVATE KVASIR_MULTICORE=1)
endfunction()

# ${Python3_EXECUTABLE} -B <script> <args...>, labelled by the call (tool, compile)
function(kvasir_add_python_test name script)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "LABELS")
    if(NOT arg_LABELS)
        message(FATAL_ERROR "kvasir_add_python_test(${name}): no LABELS")
    endif()
    add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} -B ${script} ${arg_UNPARSED_ARGUMENTS})
    set_tests_properties(${name} PROPERTIES LABELS "${arg_LABELS}")
endfunction()

# kvasir_add_must_fail_test(<name> <dir> [TESTS_DIR]): tests/<dir>/test_<dir>.py, a HEAD + CASES suite of
# tooling/must_fail.py, compiled with this configuration's standard, standard library and its debug mode; TESTS_DIR puts
# tests/ on its include path (its fixtures). It compiles four cases at once, and tells ctest so.
function(kvasir_add_must_fail_test name dir)
    cmake_parse_arguments(PARSE_ARGV 2 arg "TESTS_DIR" "" "")
    set(args --cxx ${CMAKE_CXX_COMPILER} --src ${CMAKE_CURRENT_LIST_DIR}/../src --flag=-std=c++${CMAKE_CXX_STANDARD})
    if(arg_TESTS_DIR)
        list(APPEND args --tests ${CMAKE_CURRENT_LIST_DIR})
    endif()
    if("${KVASIR_TEST_STD_LIB}" STREQUAL "libc++")
        list(APPEND args --flag=-stdlib=libc++)
    endif()
    foreach(define IN LISTS KVASIR_STDLIB_DEBUG_DEFINES)
        list(APPEND args --flag=-D${define})
    endforeach()
    kvasir_add_python_test(${name} ${CMAKE_CURRENT_LIST_DIR}/${dir}/test_${dir}.py ${args} LABELS must_fail)
    set_tests_properties(${name} PROPERTIES PROCESSORS 4)
endfunction()

kvasir_add_mock_test(kvasir_test_register_write write_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_read read_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_rmw rmw_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_sequence_point sequence_point_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_multi_register multi_register_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_special_access special_access_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_whole whole_register_tests.cpp)
# Kvasir::Memory (Register/Memory.hpp) on the mock: loads, stores, loadWords in order
kvasir_add_mock_test(kvasir_test_memory memory_tests.cpp)
# Fault::Inject (Runtime/FaultInject.hpp) on the mock: one access each, at the hook's unmapped address or the code's
kvasir_add_mock_test(kvasir_test_fault_inject fault_inject_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_write_back_acts write_back_acts_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_merge merge_tests.cpp)
# apply()'s execution order on the mock, and its constexpr plan at compile time (executed action types, a 512-action
# list)
kvasir_add_mock_test(kvasir_test_register_apply_order apply_order_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_apply_plan apply_plan_tests.cpp LABELS compile)
kvasir_add_mock_test(kvasir_test_register_atomic_default atomic_default_tests.cpp)
# the RP alias backend on this mock (chip_rp_common's RegisterAlias.hpp) is tested in chip_rp_common/tests
kvasir_add_mock_test(kvasir_test_register_model model_tests.cpp)
kvasir_add_mock_test(kvasir_test_register_wait register_wait_tests.cpp)
# MicrosecondsBound without a chip's CpuHzCeiling<Tag::User> specialisation is a named compile error, with this compiler
kvasir_add_must_fail_test(kvasir_test_register_wait_must_fail wait_must_fail)
kvasir_add_mock_test(kvasir_test_shared_isr shared_isr_tests.cpp)
kvasir_add_host_test(kvasir_test_rate_limiter rate_limiter_tests.cpp)
target_link_libraries(kvasir_test_rate_limiter PRIVATE kvasir::test_support)
kvasir_add_mock_test(kvasir_test_register_factories factories_tests.cpp)
kvasir_add_mock_test(kvasir_test_io io_tests.cpp)
# the unqualified tag idioms through argument-dependent lookup only (std is associated through the tags' base)
kvasir_add_mock_test(kvasir_test_tag_adl tag_adl_tests.cpp)

# Register/RegisterFmt.hpp through remote_fmt's encoder and parser, once without the catalog (the register's descriptor
# inline) and once with it (a type entry, resolved from the json this build's catalog tools write). Needs the nested
# uc_log/remote_fmt checkout; remote_fmt fetches fmt, glaze and enchantum for its parser and catalog reader.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/remote_fmt/CMakeLists.txt)
    # an installed fmt is built against the default standard library: with libc++ or its debug mode, fetch it and build
    # it with this tree's flags
    if(KVASIR_TEST_STD_LIB OR KVASIR_TEST_STDLIB_DEBUG)
        set(USE_FORCE_FETCH ON)
    endif()
    # a fetched fmt builds its C++ module (fmt.cc) in a C++20+ Ninja tree; nothing here imports it, and under libc++ it
    # does not compile (fmt/std.h: no std::atomic_flag)
    set(FMT_MODULE OFF)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/remote_fmt ${CMAKE_CURRENT_BINARY_DIR}/remote_fmt)
    function(kvasir_add_register_fmt_test name)
        kvasir_add_mock_test(${name} register_fmt_tests.cpp LABELS cross_repo)
        target_link_libraries(${name} PRIVATE remote_fmt::remote_fmt remote_fmt::parser)
        # the formatters exist as in a firmware's log variant (Register/RegisterFmt.hpp's guard), without USE_UC_LOG
        target_compile_definitions(${name} PRIVATE KVASIR_LOG_FORMATTERS=1)
    endfunction()
    kvasir_add_register_fmt_test(kvasir_test_register_fmt)
    target_compile_definitions(kvasir_test_register_fmt PRIVATE REMOTE_FMT_USE_CATALOG=false)
    kvasir_add_register_fmt_test(kvasir_test_register_fmt_catalog)
    target_generate_string_constants(kvasir_test_register_fmt_catalog)
    target_compile_definitions(
        kvasir_test_register_fmt_catalog
        PRIVATE
            REMOTE_FMT_TEST_CATALOG_JSON="${CMAKE_CURRENT_BINARY_DIR}/kvasir_test_register_fmt_catalog_string_constants.json"
    )
endif()

kvasir_add_threaded_test(kvasir_test_spinlock spinlock_tests.cpp)
kvasir_add_threaded_test(kvasir_test_seqlock seqlock_tests.cpp)
kvasir_add_threaded_test(kvasir_test_queue queue_tests.cpp)
kvasir_add_threaded_test(kvasir_test_bip_buffer bip_buffer_tests.cpp)
kvasir_add_threaded_test(kvasir_test_task_queue task_queue_tests.cpp)
kvasir_add_threaded_test(kvasir_test_critical_section critical_section_tests.cpp)
kvasir_add_host_test(kvasir_test_critical_section_injected critical_section_injected_tests.cpp)

kvasir_add_host_test(kvasir_test_util_saturating_value util_saturating_value_tests.cpp)
kvasir_add_host_test(kvasir_test_util_rollover util_rollover_tests.cpp)
kvasir_add_host_test(kvasir_test_util_pass_count util_pass_count_tests.cpp)
kvasir_add_host_test(kvasir_test_util_crc util_crc_tests.cpp)
# Crc::ImageEngine and its hook: the image's set where it computes the CRC, the software elsewhere and in constant
# expressions, CellStore's default on it
kvasir_add_host_test(kvasir_test_util_crc_image_engine util_crc_image_engine_tests.cpp)
kvasir_add_host_test(kvasir_test_persistent persistent_tests.cpp)
kvasir_add_host_test(kvasir_test_crash_record crash_record_tests.cpp)
# the default format (Full<>, no specialisation), and a format of the application's own (the CrashRecord::Format
# extension point)
kvasir_add_host_test(kvasir_test_crash_record_default crash_record_default_tests.cpp)
kvasir_add_host_test(kvasir_test_crash_record_format crash_record_format_tests.cpp)
kvasir_add_host_test(kvasir_test_boot_guard boot_guard_tests.cpp)
kvasir_add_host_test(kvasir_test_boot_guard_halt boot_guard_halt_tests.cpp)
# the whole BootGuard::Guard Startup entry on the host: a fake PM::resetKind() declared before the include
kvasir_add_host_test(kvasir_test_boot_guard_runtime boot_guard_runtime_tests.cpp)
# CrashLog.hpp: take-then-format without a log, the formatters' lines with the stub uc_log, and with Full<> specialised
kvasir_add_host_test(kvasir_test_crash_log crash_log_tests.cpp)
kvasir_add_host_test(kvasir_test_crash_log_lines crash_log_tests.cpp)
target_include_directories(kvasir_test_crash_log_lines BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/support/uc_log_stub)
target_compile_definitions(kvasir_test_crash_log_lines PRIVATE USE_UC_LOG=1)
kvasir_add_host_test(kvasir_test_crash_log_full crash_log_full_tests.cpp)
# an injected format whose part hands back another shape than the record: the parts' concepts name it
kvasir_add_must_fail_test(kvasir_test_crash_record_must_fail crash_record_must_fail)
kvasir_add_host_test(kvasir_test_health health_tests.cpp)
kvasir_add_host_test(kvasir_test_health_scenarios health_scenarios_tests.cpp)
kvasir_add_host_test(kvasir_test_health_logonly health_logonly_tests.cpp)
kvasir_add_host_test(kvasir_test_health_recovery health_recovery_tests.cpp)
kvasir_add_host_test(kvasir_test_health_service health_service_tests.cpp)
kvasir_add_host_test(kvasir_test_health_link health_link_tests.cpp)
# the one gated watchdog front alone (Health/WatchdogFront.hpp): which forms exist, each calls its body once
kvasir_add_host_test(kvasir_test_watchdog_front watchdog_front_tests.cpp)
kvasir_add_host_test(kvasir_test_image_check image_check_tests.cpp)
target_compile_definitions(kvasir_test_image_check PRIVATE KVASIR_IMAGE_CRC_TEST=1)
kvasir_add_host_test(kvasir_test_cell_store cell_store_tests.cpp)
kvasir_add_host_test(kvasir_test_retry retry_tests.cpp)
kvasir_add_host_test(kvasir_test_store store_tests.cpp)
kvasir_add_host_test(kvasir_test_flash_region flash_region_tests.cpp)
kvasir_add_host_test(kvasir_test_executor executor_tests.cpp)
kvasir_add_host_test(kvasir_test_loop_profiler loop_profiler_tests.cpp)
# LoopReport's Period over half a wrap of its 32-bit time source is a compile error, with this compiler
kvasir_add_must_fail_test(kvasir_test_loop_profiler_must_fail loop_profiler_must_fail)
kvasir_add_host_test(kvasir_test_trace trace_tests.cpp)
# Trace::Ring's refusals (capacity, field count, record() arity), with this compiler
kvasir_add_must_fail_test(kvasir_test_trace_must_fail trace_must_fail)
# ImageCheck's policies on a fatal path or not, with log lines: USE_UC_LOG against a stub uc_log.hpp (no submodule)
kvasir_add_host_test(kvasir_test_image_check_fatal image_check_fatal_tests.cpp)
target_include_directories(kvasir_test_image_check_fatal BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/support/uc_log_stub)
target_compile_definitions(kvasir_test_image_check_fatal PRIVATE USE_UC_LOG=1)
# the fatal-site primitive (KVASIR_DETAIL_FATAL_AT / KVASIR_DETAIL_FATAL / KVASIR_PANIC, KVASIR_DETAIL_CALLER_PC): the
# order start - line - end, with log lines recorded by the stub uc_log.hpp
kvasir_add_host_test(kvasir_test_panic_fatal_site panic_fatal_site_tests.cpp)
target_include_directories(kvasir_test_panic_fatal_site BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/support/uc_log_stub)
target_compile_definitions(kvasir_test_panic_fatal_site PRIVATE USE_UC_LOG=1)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(kvasir_test_panic_fatal_site PRIVATE -Wc2y-extensions)
endif()
# Runtime/Check.hpp without USE_UC_LOG: the checks themselves, their log lines in the never-evaluated form
kvasir_add_host_test(kvasir_test_check check_tests.cpp)
# the Count and Call<&fn> reactions of the hooks' table (Port/Hooks.hpp), each through its component's late hook:
# Health, soft check, image check (Count); boot guard, Health, soft check, register wait on the mock (Call)
kvasir_add_host_test(kvasir_test_reactions_count reactions_count_tests.cpp)
kvasir_add_mock_test(kvasir_test_reactions_call reactions_call_tests.cpp)
# Ignore, the register wait's Log, BootGuard's Count, a panic's and a fault's Call (ignore), and every PanicAfter<N>
# (panic_after): through the hooks, the lines recorded by the stub uc_log
foreach(_t ignore panic_after)
    kvasir_add_mock_test(kvasir_test_reactions_${_t} reactions_${_t}_tests.cpp)
    target_include_directories(kvasir_test_reactions_${_t} BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/support/uc_log_stub)
    target_compile_definitions(kvasir_test_reactions_${_t} PRIVATE USE_UC_LOG=1)
endforeach()
kvasir_add_must_fail_test(kvasir_test_check_must_fail check_must_fail)
# Health's refusals of a recovery (rungs out of range, a budget the watchdog would not survive), with this compiler
kvasir_add_must_fail_test(kvasir_test_health_must_fail health_must_fail)
# the main-loop hook refuses a DrivenByHand component it also drives; Health::Service<> without its supervisor binding
kvasir_add_must_fail_test(kvasir_test_hooks_must_fail hooks_must_fail)
# BootGuard::Halt refuses a Stop that cannot stop the watchdog (a gated watchdog passed directly, an unstoppable one)
kvasir_add_must_fail_test(kvasir_test_boot_guard_must_fail boot_guard_must_fail)
# every fatal path calls Panic::beginFatal() before its log line (LossPolicy::Counted lets that line wait for room), and
# only a path that ends calls it - also through the wrappers (UBSAN_REPORT, KVASIR_DETAIL_CHECK_LOG, log_assert, the
# KVASIR_DETAIL_FATAL(_AT) primitive) and in #define bodies; each must-fail fixture is matched by its own finding (a
# crash does not pass), the right shapes in fatal_sites_ok/ must pass
kvasir_add_python_test(kvasir_test_fatal_sites ${CMAKE_CURRENT_LIST_DIR}/../tools/check_fatal_sites.py
                       ${CMAKE_CURRENT_LIST_DIR}/../src LABELS compile)
kvasir_add_python_test(kvasir_test_fatal_sites_ok ${CMAKE_CURRENT_LIST_DIR}/../tools/check_fatal_sites.py
                       ${CMAKE_CURRENT_LIST_DIR}/fatal_sites_ok LABELS compile)
foreach(
    fixture_and_expect
    "logs_first|logs_first\\.hpp:5: a fatal site logs \\(line 3\\)"
    "policy_structs|policy_structs\\.hpp:18: a fatal site logs \\(line 17\\)"
    "logs_then_asserts|logs_then_asserts\\.hpp:4: a fatal site logs \\(line 3\\)"
    "begin_without_end|begin_without_end\\.hpp:4: Kvasir::Panic::beginFatal\\(\\) in a function that neither"
    "wrapper_log_first|wrapper_log_first\\.hpp:6: a fatal site logs \\(line 4\\)"
    "define_log_first|define_log_first\\.hpp:8: a fatal site logs \\(line 6\\)"
    "log_before_macro|log_before_macro\\.hpp:5: a fatal site logs \\(line 4\\)"
    "crit_not_fatal|crit_not_fatal\\.hpp:4: crit log outside a fatal path"
    "crit_in_define|crit_in_define\\.hpp:5: crit log outside a fatal path"
    "crit_at|crit_at\\.hpp:4: crit log outside a fatal path")
    string(REPLACE "|" ";" fixture_and_expect "${fixture_and_expect}")
    list(GET fixture_and_expect 0 fixture)
    list(GET fixture_and_expect 1 expect)
    kvasir_add_python_test(
        kvasir_test_fatal_sites_must_fail_${fixture} ${CMAKE_CURRENT_LIST_DIR}/../tools/check_fatal_sites.py
        ${CMAKE_CURRENT_LIST_DIR}/fatal_sites/${fixture}.hpp LABELS compile)
    set_tests_properties(kvasir_test_fatal_sites_must_fail_${fixture} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}")
endforeach()
# an ALLOW entry whose file is scanned but has no function of its name is stale
kvasir_add_python_test(
    kvasir_test_fatal_sites_must_fail_stale_allow ${CMAKE_CURRENT_LIST_DIR}/../tools/check_fatal_sites.py
    ${CMAKE_CURRENT_LIST_DIR}/fatal_sites/stale_allow LABELS compile)
set_tests_properties(
    kvasir_test_fatal_sites_must_fail_stale_allow
    PROPERTIES PASS_REGULAR_EXPRESSION "ALLOW entry \\(Runtime/ubsan\\.hpp, ubsanFatalReport\\) is stale")
# the same rules over the packages (Runtime/Check.hpp's error classes are theirs too) run in each package's own CI:
# .github/workflows/package-layers.yml (cores, chips, commons, kvasir_devices), net's host-checks.yml
# tools/layer_check.py: the layer table against the SDK's own sources and its nested repositories (kept findings pass, a
# new one or a stale keep entry fails), and the tool's rules on a fixture tree
kvasir_add_python_test(
    kvasir_test_layers
    ${CMAKE_CURRENT_LIST_DIR}/../tools/layer_check.py
    --quiet
    --repo
    Kvasir_SDK=${CMAKE_CURRENT_LIST_DIR}/..
    LABELS
    compile)
kvasir_add_python_test(kvasir_test_layer_check ${CMAKE_CURRENT_LIST_DIR}/layer_check/test_layer_check.py LABELS tool)
# tools/compat_check.py: the shapes the no-compat rule forbids (its docstring lists them) in the SDK and its nested
# repositories against tools/compat_keep.toml (a new finding or a stale keep entry fails), and every pattern against its
# fixtures; the packages run the same check in package-layers.yml
kvasir_add_python_test(
    kvasir_test_compat
    ${CMAKE_CURRENT_LIST_DIR}/../tools/compat_check.py
    --quiet
    --repo
    Kvasir_SDK=${CMAKE_CURRENT_LIST_DIR}/..
    LABELS
    compile)
kvasir_add_python_test(kvasir_test_compat_check ${CMAKE_CURRENT_LIST_DIR}/../tools/compat_check.py --self-test LABELS
                       tool)
# tools/tidy.py, the clang-tidy runner: unit selection, ownership, filters, cache, --changed, shards, -j and admission,
# NOLINT forms and budget, keep tables, the check lock, fix-its, and one run of the real clang-tidy on a fixture
kvasir_add_python_test(kvasir_test_tidy_check ${CMAKE_CURRENT_LIST_DIR}/../tools/tidy.py --self-test LABELS tool)
kvasir_add_must_fail_test(kvasir_test_cell_store_must_fail cell_store_must_fail TESTS_DIR)
# Crc32 against uc_log's copy (the printer's firmware check, which ImageCheck's CRC must match): nested here, absent in
# a checkout without its submodules. fs checks its log store's copy against the same textbook loop util_crc_tests.cpp
# uses (fs/test/crc_test.cpp).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/host/uc_log/printer/HexImage.hpp)
    kvasir_add_host_test(kvasir_test_util_crc_cross util_crc_cross_tests.cpp LABELS cross_repo)
    target_include_directories(kvasir_test_util_crc_cross PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/src
                                                                  ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/host)
endif()
# the RP PLL search against the old one runs in chip_rp_common/tests (pll_search_test)
kvasir_add_host_test(kvasir_test_util_prescaler util_prescaler_tests.cpp)
# Prescaler's failures are compile errors with the numbers in them: snippets that must fail, with this compiler
kvasir_add_must_fail_test(kvasir_test_util_prescaler_must_fail prescaler_must_fail)
# Kvasir::literals' refusals (a suffix's range, past 64 bits, a non-digit), each named, with this compiler
kvasir_add_must_fail_test(kvasir_test_literals_must_fail literals_must_fail)
# Named compile errors (Util/Diagnostic.hpp, Register/Diagnostic.hpp, the Startup reports): snippets that must fail with
# the register, field or resource named, with this compiler
kvasir_add_must_fail_test(kvasir_test_diagnostics_must_fail diagnostics_must_fail TESTS_DIR)
# The port contract's named errors (kvasir/Port/Check.hpp): a fake chip and core with one piece left out each
kvasir_add_must_fail_test(kvasir_test_port_must_fail port_must_fail)
# A Startup list with Hook::SafeState entries and no fatal start that runs them does not compile (StartUp/SafeState.hpp)
kvasir_add_must_fail_test(kvasir_test_safe_state_must_fail safe_state_must_fail)
# kvasir/Hal/'s resources (owners, DMA channels, clocks) in Startup's checks: the errors name what collided
kvasir_add_must_fail_test(kvasir_test_hal_must_fail hal_must_fail)

# Headers generated by the nested svd_converter from its golden fixture (svd_converter/tests/golden/features.svd),
# compiled against src/kvasir/Register: every register, instance, group element and dim element instantiated
# (tools/instantiate_all.py over the json output; a static_assert in a generated register is only evaluated when the
# register is instantiated), plus mock writes, plain and NAMED_REGISTERS. The converter and its dependencies (fetched by
# its own CMakeLists) are built here; KVASIR_TEST_GENERATED_HEADERS=OFF leaves this out (offline).
option(KVASIR_TEST_GENERATED_HEADERS "compile svd_converter's generated fixture headers against the SDK" ON)
set(kvasir_converter_dir ${CMAKE_CURRENT_LIST_DIR}/../svd_converter)
if(KVASIR_TEST_GENERATED_HEADERS AND NOT EXISTS ${kvasir_converter_dir}/tests/golden/features.svd)
    message(
        STATUS
            "Kvasir tests: no svd_converter submodule with its golden fixture, the generated-header tests are left out")
elseif(KVASIR_TEST_GENERATED_HEADERS)
    # an installed pugixml is built against the default standard library: with libc++ or its debug mode, fetch it
    if(KVASIR_TEST_STD_LIB OR KVASIR_TEST_STDLIB_DEBUG)
        set(USE_FORCE_FETCH ON)
    endif()
    # the converter is a cmake_helpers project: its USE_* in cmake_helpers' terms. No sanitizer of its own (this tree's
    # flags reach it through the directory's compile options already), this tree's standard library and debug mode.
    block()
    set(USE_SANITIZER none)
    if("${KVASIR_TEST_STD_LIB}" STREQUAL "libc++")
        set(USE_STD_LIB libc++)
    else()
        set(USE_STD_LIB libstdc++)
    endif()
    set(USE_STDLIB_DEBUG ${KVASIR_TEST_STDLIB_DEBUG})
    add_subdirectory(${kvasir_converter_dir} ${CMAKE_CURRENT_BINARY_DIR}/svd_converter EXCLUDE_FROM_ALL)
    endblock()
    set(fixture_svd ${kvasir_converter_dir}/tests/golden/features.svd)
    # svd_convert()'s defaults are the write-only guard profile (error, derived, mask on). NAMED_REGISTERS is read when
    # svd_convert runs (configure time): a plain value per call; a tree-wide -DKVASIR_SVD_NAMED_REGISTERS overrides both
    svd_convert(
        kvasir_fixture_regs_plain
        SVD_FILE
        ${fixture_svd}
        OUTPUT_DIRECTORY
        fixture_plain
        NAMED_REGISTERS
        OFF)
    svd_convert(
        kvasir_fixture_regs_named
        SVD_FILE
        ${fixture_svd}
        OUTPUT_DIRECTORY
        fixture_named
        NAMED_REGISTERS
        ON)
    svd_convert(
        kvasir_fixture_json
        SVD_FILE
        ${fixture_svd}
        OUTPUT_DIRECTORY
        fixture_json
        GENERATOR
        json)
    set(fixture_json_dir ${CMAKE_CURRENT_BINARY_DIR}/svd_converter_generated/fixture_json)
    foreach(variant plain named)
        set(tu ${CMAKE_CURRENT_BINARY_DIR}/generated_registers_instances_${variant}.cpp)
        add_custom_command(
            OUTPUT ${tu}
            COMMAND ${Python3_EXECUTABLE} -B ${kvasir_converter_dir}/tools/instantiate_all.py ${fixture_json_dir}
                    --include-prefix fixture_${variant} -o ${tu}
            DEPENDS kvasir_fixture_json ${fixture_json_dir}/kvasir_fixture_json_dependency_dummy
                    ${kvasir_converter_dir}/tools/instantiate_all.py
            COMMENT "Instantiating every generated fixture register (${variant})")
    endforeach()
    kvasir_add_mock_test(kvasir_test_generated_registers generated_registers_tests.cpp
                         ${CMAKE_CURRENT_BINARY_DIR}/generated_registers_instances_plain.cpp)
    target_link_libraries(kvasir_test_generated_registers PRIVATE kvasir_fixture_regs_plain)
    target_compile_options(kvasir_test_generated_registers PRIVATE -include ${CMAKE_CURRENT_LIST_DIR}/kvasir_test.hpp)
    kvasir_add_mock_test(kvasir_test_generated_registers_named generated_registers_named_tests.cpp
                         ${CMAKE_CURRENT_BINARY_DIR}/generated_registers_instances_named.cpp)
    target_link_libraries(kvasir_test_generated_registers_named PRIVATE kvasir_fixture_regs_named)
    target_compile_options(kvasir_test_generated_registers_named PRIVATE -include
                                                                         ${CMAKE_CURRENT_LIST_DIR}/kvasir_test.hpp)
    # RegisterFmt over generated registers (register_fmt_tests.cpp's second half): the views' bytes
    foreach(fmt_test kvasir_test_register_fmt kvasir_test_register_fmt_catalog)
        if(TARGET ${fmt_test})
            target_link_libraries(${fmt_test} PRIVATE kvasir_fixture_regs_plain)
            target_compile_definitions(${fmt_test} PRIVATE KVASIR_TEST_GENERATED_REGISTERS=1)
        endif()
    endforeach()
    # ... and without RegisterFmt: a generated register and Flags<> have no formatter at all (static_asserts against a
    # reflection formatter's constraint), so a TU that misses the header cannot print one differently
    if(TARGET kvasir_test_register_fmt)
        kvasir_add_mock_test(kvasir_test_register_fmt_absent register_fmt_absent_tests.cpp LABELS cross_repo)
        target_link_libraries(kvasir_test_register_fmt_absent PRIVATE kvasir_fixture_regs_plain remote_fmt::remote_fmt
                                                                      remote_fmt::parser)
        target_compile_definitions(kvasir_test_register_fmt_absent PRIVATE KVASIR_LOG_FORMATTERS=1)
    endif()

    # The write-only guard's compile error on generated output: write_only/guard.svd's unclassified TC.CTRLA.SWRST,
    # converted with the mask switch on (guard off: the converter itself stays silent), and a write of ENABLE alone must
    # not compile, naming SWRST; the same write that names SWRST must compile.
    svd_convert(
        kvasir_fixture_regs_guard
        SVD_FILE
        ${kvasir_converter_dir}/tests/write_only/guard.svd
        OUTPUT_DIRECTORY
        fixture_guard
        WRITE_ONLY_GUARD
        off
        WRITE_ONLY_REGISTERS
        derived
        WRITE_ONLY_MASK
        ON)
    # The tests below compile the headers at ctest time: svd_convert's INTERFACE library owns the conversion (its source
    # is the step's output) and is part of ALL. No second target may list that output: the Makefile generators copy the
    # command into each such target and run it twice at once ("cannot rename .../TC.hpp.tmp").
    set(rmw_compile
        ${CMAKE_CXX_COMPILER} -std=c++${CMAKE_CXX_STANDARD} -fsyntax-only -I${CMAKE_CURRENT_LIST_DIR}/../src
        -I${CMAKE_CURRENT_BINARY_DIR}/svd_converter_generated -DKVASIR_REGISTER_MOCK)
    if("${KVASIR_TEST_STD_LIB}" STREQUAL "libc++")
        list(APPEND rmw_compile -stdlib=libc++)
    endif()
    add_test(NAME kvasir_generated_registers_rmw_must_fail
             COMMAND ${rmw_compile} ${CMAKE_CURRENT_LIST_DIR}/generated_registers_rmw_must_fail.cpp)
    set_tests_properties(
        kvasir_generated_registers_rmw_must_fail
        PROPERTIES PASS_REGULAR_EXPRESSION "would write back the write-only field\\(s\\) swrst" LABELS
                   "must_fail;compile")
    add_test(NAME kvasir_generated_registers_rmw_named_compiles
             COMMAND ${rmw_compile} -DGENERATED_RMW_NAMES_SWRST
                     ${CMAKE_CURRENT_LIST_DIR}/generated_registers_rmw_must_fail.cpp)
    set_tests_properties(kvasir_generated_registers_rmw_named_compiles PROPERTIES LABELS compile)
endif()
kvasir_add_host_test(kvasir_test_diagnostic diagnostic_tests.cpp)
# Crc's refusals (a wrong check value, a table size, resume where refin != refout), with this compiler
kvasir_add_must_fail_test(kvasir_test_util_crc_must_fail crc_must_fail)
kvasir_add_host_test(kvasir_test_util_periodic util_periodic_tests.cpp)
target_link_libraries(kvasir_test_util_periodic PRIVATE kvasir::test_support)
kvasir_add_host_test(kvasir_test_hooks hooks_tests.cpp)
# Startup::SafeStateOf<S>: both cores' Hook::SafeState entries in one call, and Start<S>'s wiring check
kvasir_add_host_test(kvasir_test_safe_state safe_state_tests.cpp)
kvasir_add_host_test(kvasir_test_panic panic_tests.cpp)
# StackProtector: an overwritten sentinel is Cause::stackOverflow, pc 0, detail = words overwritten
kvasir_add_host_test(kvasir_test_stack_protector stack_protector_tests.cpp)
# the default fatal end with a stand-in chip reboot (Port::Reboot): record, reboot; the nested stop;
# Fault::OnFault::Panic
kvasir_add_host_test(kvasir_test_panic_default_end panic_default_end_tests.cpp)
# The records against their one host decoder, uc_log's printer (ids, sizes, offsets, name tables). uc_log is a submodule
# of every SDK checkout: without it this is a configure error, not a skipped pin.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/host/uc_log/printer/TargetRecords.hpp)
    message(FATAL_ERROR "no uc_log in ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log: check out the SDK's submodules")
endif()
kvasir_add_host_test(kvasir_test_records_cross records_cross_tests.cpp LABELS cross_repo)
target_include_directories(kvasir_test_records_cross PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/src
                                                             ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/host)
# the sanitizer counter against the printer's Ubsan: compiled only, never linked (ubsan.hpp defines the __ubsan_handle_*
# handlers a sanitized host program already has); the cross test needs it built
add_library(kvasir_records_cross_ubsan OBJECT records_cross_ubsan.cpp)
target_link_libraries(kvasir_records_cross_ubsan PRIVATE kvasir_headers)
target_include_directories(
    kvasir_records_cross_ubsan
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/src ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/host
            ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/remote_fmt/string_constant/src)
target_compile_options(kvasir_records_cross_ubsan PRIVATE -Wall -Wextra -Werror)
add_dependencies(kvasir_test_records_cross kvasir_records_cross_ubsan)
# beginFatal() with uc_log: the backend's panic mode (LossPolicy::Counted)
kvasir_add_host_test(kvasir_test_panic_fatal panic_fatal_tests.cpp LABELS cross_repo)
target_include_directories(kvasir_test_panic_fatal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/src)
# Trace::Ring's descriptor and storage, decoded by uc_log's trace reader
kvasir_add_host_test(kvasir_test_trace_cross trace_cross_tests.cpp LABELS cross_repo)
target_include_directories(kvasir_test_trace_cross PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/src
                                                           ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/host)
kvasir_add_host_test(kvasir_test_announced_reset announced_reset_tests.cpp)
# the printer's side of the handshake, where the nested uc_log checkout is (its headers include each other)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/host/uc_log/printer/AnnouncedReset.hpp)
    target_include_directories(kvasir_test_announced_reset PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/src
                                                                   ${CMAKE_CURRENT_SOURCE_DIR}/../uc_log/host)
    target_compile_definitions(kvasir_test_announced_reset PRIVATE KVASIR_TEST_UC_LOG_PRINTER=1)
endif()
kvasir_add_host_test(kvasir_test_util_control_pid util_control_pid_tests.cpp)
kvasir_add_host_test(kvasir_test_util_control_filters util_control_filters_tests.cpp)
kvasir_add_host_test(kvasir_test_util_literals util_literals_tests.cpp)
kvasir_add_host_test(kvasir_test_util_bitfield util_bitfield_tests.cpp)
# BitField wider than 32 bits or with High below Low is a compile error naming the limit, with this compiler
kvasir_add_must_fail_test(kvasir_test_util_bitfield_must_fail bitfield_must_fail)
kvasir_add_host_test(kvasir_test_util_static_vector util_static_vector_tests.cpp)
kvasir_add_host_test(kvasir_test_util_static_containers util_static_containers_tests.cpp)
kvasir_add_host_test(kvasir_test_util_static_function util_static_function_tests.cpp LABELS threaded)
# its interrupt model is a signal handler, raised from a second thread
target_link_libraries(kvasir_test_util_static_function PRIVATE Threads::Threads)
kvasir_add_host_test(kvasir_test_mpl mpl_tests.cpp LABELS compile)
kvasir_add_host_test(kvasir_test_mpl_list mpl_list_tests.cpp LABELS compile)
kvasir_add_host_test(kvasir_test_brigand_subset brigand_subset_tests.cpp LABELS compile)
kvasir_add_host_test(kvasir_test_resources resources_tests.cpp LABELS compile)
kvasir_add_host_test(kvasir_test_hal hal_tests.cpp)
kvasir_add_host_test(kvasir_test_hal_block hal_block_tests.cpp)
kvasir_add_host_test(kvasir_test_isrs_for isrs_for_tests.cpp)
kvasir_add_host_test(kvasir_test_port port_tests.cpp)
kvasir_add_host_test(kvasir_test_startup_list startup_list_tests.cpp LABELS compile)
kvasir_add_host_test(kvasir_test_startup_checks startup_checks_tests.cpp LABELS compile)
# StartUp/VectorTable.hpp against expected tables, the ISR list transform
kvasir_add_host_test(kvasir_test_vector_table vector_table_tests.cpp)
kvasir_add_host_test(kvasir_test_atomic_queue atomic_queue_tests.cpp)
# OverflowPolicyAssert must not return when asserts are compiled out (it is [[noreturn]]); -Werror also proves the
# header has no -Winvalid-noreturn under NDEBUG
kvasir_add_host_test(kvasir_test_atomic_queue_ndebug atomic_queue_ndebug_tests.cpp)
target_compile_definitions(kvasir_test_atomic_queue_ndebug PRIVATE NDEBUG)
# the atomic shim's operations (StartUp/Runtime/LibAtomicBlocks.hpp): the exchange writes only on change
kvasir_add_host_test(kvasir_test_libatomic_blocks libatomic_blocks_tests.cpp)

# ---------------------------------------------------------------------------
# header self containment
#
# Every public header must compile on its own. Each header gets a generated translation unit that includes it twice (the
# second include also checks that the include guard works).
#
# Headers that legitimately cannot be compiled standalone on the host are listed below together with the reason; they
# need either a chip support file or the uc_log/remote_fmt stack, neither of which is available in this build.
set(KVASIR_NON_SELF_CONTAINED_HEADERS
    kvasir/Runtime/FaultHandler.hpp # needs core/Fault.hpp from a chip file
    kvasir/Log/RttSink.hpp # an image config header's backend: the chip's DebuggerPresent and CPU ceiling, uc_log
    kvasir/Log/SystickLogClock.hpp # an image config header's clock: needs core/Systick.hpp from a core package
    kvasir/Runtime/BootGuard.hpp # needs Kvasir::PM::resetKind() from a chip package; tested whole in
                                 # boot_guard_runtime_tests
    kvasir/Runtime/BootLog.hpp # includes uc_log and the generated cmake_git_version header
    kvasir/Runtime/BootReport.hpp # includes BootLog.hpp (uc_log, cmake_git_version)
    kvasir/Runtime/Standard.hpp # needs a chip package (BootGuard, BootReport) and core/Fault.hpp (FaultHandler)
    kvasir/Runtime/ubsan.hpp # needs the remote_fmt _sc literal
    kvasir/StartUp/StartUp.hpp # needs uc_log and a chip file
    kvasir/StartUp/SecondaryCore.hpp # includes StartUp.hpp, needs uc_log and a chip file
    kvasir/StartUp/RamVectorTable.hpp # needs core_peripherals/SCB.hpp from a core package; tested in core_cortex_common
                                      # tests/decoders
    kvasir/StartUp/Runtime/All.hpp # StartUp.hpp's main TU only: strong library definitions, uc_log, a chip file
    kvasir/StartUp/Runtime/PanicEntry.hpp # StartUp.hpp's main TU only: strong library definitions, uc_log, a chip file
    kvasir/StartUp/Runtime/Libc.hpp # StartUp.hpp's main TU only: strong library definitions, uc_log, a chip file
    kvasir/StartUp/Runtime/Aeabi.hpp # StartUp.hpp's main TU only: strong library definitions, uc_log, a chip file
    kvasir/StartUp/Runtime/FatalPaths.hpp # StartUp.hpp's main TU only: strong library definitions, uc_log, a chip file
    kvasir/StartUp/Runtime/Cxx.hpp # StartUp.hpp's main TU only: strong library definitions, uc_log, a chip file
    kvasir/StartUp/Runtime/LibAtomic.hpp # StartUp.hpp's main TU only: strong library definitions (its blocks,
                                         # LibAtomicBlocks.hpp, are tested in libatomic_blocks_tests)
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
    # written only when it changes, so a reconfigure recompiles nothing
    file(CONFIGURE OUTPUT ${generated} CONTENT
         "// generated by tests/CMakeLists.txt -- do not edit\n#include \"${header}\"\n#include \"${header}\"\n")
    list(APPEND KVASIR_SELF_CONTAINED_SOURCES ${generated})
endforeach()
# the TUs of headers that moved or went on the list above
file(GLOB _kvasir_stale_self_contained ${CMAKE_CURRENT_BINARY_DIR}/self_contained/*.cpp)
list(REMOVE_ITEM _kvasir_stale_self_contained ${KVASIR_SELF_CONTAINED_SOURCES})
if(_kvasir_stale_self_contained)
    file(REMOVE ${_kvasir_stale_self_contained})
endif()

# Each header twice: on the register mock (what the host tests see) and on the real volatile register path (what a
# firmware compiles). `cmake --build` (all) builds both; `just headers` builds only these.
foreach(mode IN ITEMS mocked volatile)
    add_library(kvasir_self_contained_headers_${mode} OBJECT ${KVASIR_SELF_CONTAINED_SOURCES})
    # -Wheader-hygiene: no SDK header puts a using-directive at global scope (using_literals.hpp, the firmware
    # convenience header, silences it for its own two on purpose)
    target_compile_options(kvasir_self_contained_headers_${mode} PRIVATE -Wall -Wextra -Werror
                                                                         $<$<CXX_COMPILER_ID:Clang>:-Wheader-hygiene>)
endforeach()
target_link_libraries(kvasir_self_contained_headers_mocked PRIVATE kvasir_mocked)
target_link_libraries(kvasir_self_contained_headers_volatile PRIVATE kvasir_headers)
add_custom_target(kvasir_self_contained_headers)
add_dependencies(kvasir_self_contained_headers kvasir_self_contained_headers_mocked
                 kvasir_self_contained_headers_volatile)

# The tool tests below skip a missing ARM/LLVM tool through tests/tooling/armtc.py: a skip, or a failure when
# KVASIR_TEST_REQUIRE names the tool's group (CI sets it to what each job installed). Its own rule, on a fake PATH:
kvasir_add_python_test(kvasir_test_armtc ${CMAKE_CURRENT_LIST_DIR}/tooling/test_armtc.py LABELS tool)
# tests/tooling/must_fail.py, the runner of every *_must_fail suite: its rules on a fake compiler
kvasir_add_python_test(kvasir_test_must_fail_runner ${CMAKE_CURRENT_LIST_DIR}/tooling/test_must_fail.py LABELS tool)
# cmake/tools/check_ram_funcs.py (the post-link RAM function check) against ARM images built with every ARM toolchain
# found here; a missing toolchain is a skip inside the test (see armtc above). Its CASE 11 (the core's PRIMASK API in a
# RAM function) needs core_cortex_common: the sibling checkout, else fetched
get_filename_component(_kvasir_siblings "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(EXISTS ${_kvasir_siblings}/core_cortex_common/InterruptMask.hpp)
    set(KVASIR_TEST_CORE_COMMON ${_kvasir_siblings}/core_cortex_common)
else()
    include(FetchContent)
    FetchContent_Declare(
        core_cortex_common
        GIT_REPOSITORY https://github.com/kvasir-io/core_cortex_common.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(core_cortex_common)
    set(KVASIR_TEST_CORE_COMMON ${core_cortex_common_SOURCE_DIR})
endif()
kvasir_add_python_test(kvasir_test_check_ram_funcs ${CMAKE_CURRENT_LIST_DIR}/ram_funcs/test_check_ram_funcs.py
                       --core-common ${KVASIR_TEST_CORE_COMMON} LABELS tool)
# lib/libcxx/libcxx.cmake's flags: libc++ built through the real toolchain files with every ARM toolchain found here,
# silent on each (clang-only options once reached gcc and noted on every file); a missing toolchain is a skip
kvasir_add_python_test(kvasir_test_runtime_lib_flags
                       ${CMAKE_CURRENT_LIST_DIR}/runtime_lib_flags/test_runtime_lib_flags.py LABELS tool)
# cmake/tools/check_reset_entry.py (the post-link reset entry check): its instruction rules on hand-assembled code
kvasir_add_python_test(kvasir_test_check_reset_entry ${CMAKE_CURRENT_LIST_DIR}/reset_entry/test_check_reset_entry.py
                       LABELS tool)
# cmake/jlink.cmake's J-Link Commander scripts: downloads and resets pinned byte for byte, connect.jlink attach only
# (configure only, no JLinkExe)
kvasir_add_python_test(kvasir_test_jlink_scripts ${CMAKE_CURRENT_LIST_DIR}/jlink_scripts/test_jlink_scripts.py LABELS
                       tool)
# cmake/tools/find_undefined_refs.py (the post-link undefined-symbol check) on ELF32 objects compiled here: exit codes,
# --delete-on-failure (no compiler for ELF32 objects is a skip inside the test)
kvasir_add_python_test(kvasir_test_find_undefined_refs
                       ${CMAKE_CURRENT_LIST_DIR}/undefined_refs/test_find_undefined_refs.py LABELS tool)

# linker/common_stack_body.inc.ld: the stack fills what RAM the other sections leave, in one link, with lld and GNU ld
# (a missing linker is a skip inside the test)
kvasir_add_python_test(kvasir_test_stack_fill ${CMAKE_CURRENT_LIST_DIR}/stack_fill/test_stack_fill.py LABELS tool)

# linker/common.ld's _LINKER_image_end_flash_ (the end of the flash image) with lld and GNU ld: the default, a chip
# script that loads a section after .data and moves it, one that does not (cmake/tools/elf_image.py refuses that image),
# RAM-only (a missing linker is a skip inside the test)
kvasir_add_python_test(kvasir_test_image_end ${CMAKE_CURRENT_LIST_DIR}/image_end/test_image_end.py LABELS tool)

kvasir_add_python_test(kvasir_test_bench_tool ${CMAKE_CURRENT_LIST_DIR}/../tools/test_kvasir_bench.py LABELS tool)

# lib/compiler-rt/builtins/kvasir/udivmoddi4_udiv.c (the 64-bit division of the cores with a 32-bit divide instruction)
# against the host's own division; C, as the routine. Its options are its own: the directory's are for C++
# (-stdlib=libc++ in the libc++ configurations, which the C compiler - gcc there - rejects).
enable_language(C)
set(kvasir_udivmoddi4_source ${CMAKE_CURRENT_LIST_DIR}/../lib/compiler-rt/builtins/kvasir/udivmoddi4_udiv.c)
add_executable(kvasir_udivmoddi4_tests udivmoddi4/udivmoddi4_tests.c ${kvasir_udivmoddi4_source})
# the routine under a name of its own, so the host's libgcc/compiler-rt __udivmoddi4 stays out of the comparison
set_source_files_properties(${kvasir_udivmoddi4_source} PROPERTIES COMPILE_DEFINITIONS
                                                                   __udivmoddi4=kvasir_udivmoddi4_under_test)
set(kvasir_udivmoddi4_options -O2 -Wall -Wextra -Werror)
set(kvasir_udivmoddi4_link_options)
if(KVASIR_TEST_SANITIZER)
    list(APPEND kvasir_udivmoddi4_options -fsanitize=${KVASIR_TEST_SANITIZER} -fno-omit-frame-pointer)
    list(APPEND kvasir_udivmoddi4_link_options -fsanitize=${KVASIR_TEST_SANITIZER})
endif()
set_target_properties(kvasir_udivmoddi4_tests PROPERTIES COMPILE_OPTIONS "${kvasir_udivmoddi4_options}"
                                                         LINK_OPTIONS "${kvasir_udivmoddi4_link_options}")
add_test(NAME kvasir_udivmoddi4_tests COMMAND kvasir_udivmoddi4_tests)
set_tests_properties(kvasir_udivmoddi4_tests PROPERTIES LABELS unit)

# tools/size_report.py: bucketing, parsers, diff, and a snapshot of an image linked here (a missing toolchain is a skip
# inside the test)
kvasir_add_python_test(kvasir_test_size_report ${CMAKE_CURRENT_LIST_DIR}/../tools/test_size_report.py LABELS tool)

# tools/cxx_names.py: the one short form of a C++ name (kvasir_bench's location lines, profile, size_report's buckets)
kvasir_add_python_test(kvasir_test_cxx_names ${CMAKE_CURRENT_LIST_DIR}/../tools/test_cxx_names.py LABELS tool)

# cmake/tools/patch_image_crc.py on ARM images built here (a missing toolchain is a skip inside the test)
kvasir_add_python_test(kvasir_test_patch_image_crc ${CMAKE_CURRENT_LIST_DIR}/image_crc/test_patch_image_crc.py LABELS
                       tool)

# cmake/tools/elf_image.py, the flash artefacts from the load view, against llvm-objcopy as the oracle on images linked
# here with lld and GNU ld (a missing toolchain is a skip inside the test)
kvasir_add_python_test(kvasir_test_elf_image ${CMAKE_CURRENT_LIST_DIR}/elf_image/test_elf_image.py LABELS tool)

# cmake/tools/kvasir_postlink.py, the one post-link command: step order, exit codes, the ELF removed on a failure (a
# missing toolchain is a skip inside the test)
kvasir_add_python_test(kvasir_test_postlink ${CMAKE_CURRENT_LIST_DIR}/postlink/test_postlink.py LABELS tool)

# cmake/tools/beautify_lst.py, the listing's name shortener: the line shapes it rewrites, its name rules, a long mangled
# name in linear time, the streaming entry points (no toolchain)
kvasir_add_python_test(kvasir_test_beautify_lst ${CMAKE_CURRENT_LIST_DIR}/beautify_lst/test_beautify_lst.py LABELS tool)

# cmake/tools/ram_image_jlink.py, the J-Link script of a RAM_ONLY image: the words from a linked image, its refusals (a
# missing toolchain is a skip inside the test)
kvasir_add_python_test(kvasir_test_ram_image_jlink ${CMAKE_CURRENT_LIST_DIR}/ram_image_jlink/test_ram_image_jlink.py
                       LABELS tool)

# cmake/kvasir_variants.cmake: the variant table, its resolved policy and kvasir_forward_arguments (cmake -P, no build)
add_test(NAME kvasir_test_cmake_variants COMMAND ${CMAKE_COMMAND} -P
                                                 ${CMAKE_CURRENT_LIST_DIR}/cmake_api/test_variants.cmake)

# cmake/chip_package.cmake: kvasir_chip_svd, kvasir_chip_variant (default, preset, unknown part, the generated part's
# SVD) and kvasir_chip_generate_svd (mtime kept when unchanged, a failing script) (cmake -P, no build)
add_test(NAME kvasir_test_cmake_chip_package
         COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cmake_chip_package -P
                 ${CMAKE_CURRENT_LIST_DIR}/cmake_api/test_chip_package.cmake)
# cmake/kvasir_runtime_archives.cmake: the runtime archives' names per optimisation set and sanitizer, and the refusal
# of a second image that would compile one with other flags (cmake -P, no build)
add_test(NAME kvasir_test_cmake_runtime_archives
         COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/cmake_api/test_runtime_archives.cmake)
set_tests_properties(kvasir_test_cmake_variants kvasir_test_cmake_chip_package kvasir_test_cmake_runtime_archives
                     PROPERTIES LABELS tool)

# cmake/kvasir_chip.cmake (kvasir_core, kvasir_chip: the targets, properties and variables, and every refusal) and
# cmake/kvasir_interface.cmake (kvasir_quarantine_interface): one configure of tests/chip_contract per case, no build
kvasir_add_python_test(kvasir_test_chip_contract ${CMAKE_CURRENT_LIST_DIR}/chip_contract/test_chip_contract.py
                       ${CMAKE_COMMAND} ${CMAKE_GENERATOR} LABELS tool)

# cmake/tools/kvasir_elf.py, the one ELF reader of the post-link tools, against llvm-readelf's JSON on images linked
# here with lld and GNU ld (a missing toolchain is a skip inside the test)
kvasir_add_python_test(kvasir_test_kvasir_elf ${CMAKE_CURRENT_LIST_DIR}/kvasir_elf/test_kvasir_elf.py LABELS tool)

# cmake/tools/pretty_size.py, the per-link memory table: flash rows count every section's load image, RAM rows what runs
# there, on images linked here with lld and GNU ld (a missing toolchain is a skip inside the test)
kvasir_add_python_test(kvasir_test_pretty_size ${CMAKE_CURRENT_LIST_DIR}/pretty_size/test_pretty_size.py LABELS tool)

# tools/llvm_port/libcport --clear-new-marks: every source list of its component table, profile.cmake included (bash,
# grep and sed only; no llvm-project)
kvasir_add_python_test(kvasir_test_clear_new_marks
                       ${CMAKE_CURRENT_LIST_DIR}/../tools/llvm_port/tests/test_clear_new_marks.py LABELS tool)

# tools/llvm_port/libcport_lib.py, libcport's logic (source-list sync, libc_flags check, owned.txt matcher, patch
# headers) on fixtures: no llvm-project needed
kvasir_add_python_test(kvasir_test_libcport_lib ${CMAKE_CURRENT_LIST_DIR}/../tools/llvm_port/tests/test_libcport_lib.py
                       LABELS tool)

# kvasir/Util/Coverage.hpp and the bare-metal profile runtime on the host: the manifest equals the runtime's own buffer
# writer, llvm-profdata/llvm-cov read it (needs clang and the llvm tools; otherwise a skip inside the test)
kvasir_add_python_test(kvasir_test_coverage ${CMAKE_CURRENT_LIST_DIR}/coverage/test_coverage.py LABELS tool)

# tools/fw.py, the firmware justfiles' build-tree guard, build-all filter, stale-tree scan and compiler/library mapping:
# the filter against the bash filter it replaced as an oracle, on a fake cmake (bash, awk or grep missing: a skip inside
# the test); `config` against the justfile expressions it replaced
kvasir_add_python_test(kvasir_test_fw ${CMAKE_CURRENT_LIST_DIR}/../tools/test_fw.py LABELS tool)

# tools/header_cost.py, a TU's preprocessed lines, files and generated share and the ceiling check, on a fixture tree
# compiled by the host compiler (none: a skip inside the test)
kvasir_add_python_test(kvasir_test_header_cost ${CMAKE_CURRENT_LIST_DIR}/header_cost/test_header_cost.py LABELS tool)

# tests/conformance/{ChipContract,CoreContract}.hpp, the rows the chip and core conformance trees assert: a fake chip
# with every row passes, and a REQUIRE of a missing row or a GAP of a present one fails with the row's message
kvasir_add_host_test(kvasir_test_conformance_contract conformance_contract_tests.cpp)
target_include_directories(kvasir_test_conformance_contract PRIVATE ${CMAKE_CURRENT_LIST_DIR})
kvasir_add_must_fail_test(kvasir_test_conformance_must_fail conformance_must_fail TESTS_DIR)

# kvasir/Util/Config.hpp: KVASIR_CONFIG_OR's shapes and deduced types, TypeOr; and what they must refuse (outside a
# template, a braced narrowing, a non-constant member)
kvasir_add_host_test(kvasir_test_util_config config_tests.cpp)
kvasir_add_must_fail_test(kvasir_test_config_must_fail config_must_fail)

# kvasir/Util/Result.hpp: Kvasir::Result (std::expected) and discard() of a [[nodiscard]] result, warning-free
kvasir_add_host_test(kvasir_test_result result_tests.cpp)
# kvasir/Util/HexString.hpp: bytes as upper-case hex text (the chips' serialNumberString())
kvasir_add_host_test(kvasir_test_util_hex_string hex_string_tests.cpp)
# kvasir/Util/Backoff.hpp: the doubling schedule, its restart, a jumped clock, doubledUpTo at its edges
kvasir_add_host_test(kvasir_test_backoff backoff_tests.cpp)
target_link_libraries(kvasir_test_backoff PRIVATE kvasir::test_support)
# kvasir/Log/FaultLog.hpp: the key and decision are the limiter's, per-kind counts saturate, and the sizes
kvasir_add_host_test(kvasir_test_fault_log fault_log_tests.cpp)
# kvasir/Util/Link.hpp: FailureStreak and LinkTracker
kvasir_add_host_test(kvasir_test_link link_tests.cpp)

# cmake_git_version (submodule): the generated headers for scratch repositories in every git state, the state file, a
# consumer's project header - its own tests (cmake_git_version/tests, cmake -P, no build)
find_package(Git REQUIRED)
add_test(
    NAME kvasir_test_cmake_git_version
    COMMAND
        ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/../cmake_git_version
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cmake_git_version -DGIT=${GIT_EXECUTABLE} -DGENERATOR=${CMAKE_GENERATOR}
        -P ${CMAKE_CURRENT_LIST_DIR}/../cmake_git_version/tests/run.cmake)
set_tests_properties(kvasir_test_cmake_git_version PROPERTIES LABELS tool)

# cmake/kvasir_locate.cmake (kvasir_locate): where CHIP_ROOT, KVASIR_DEVICES_ROOT and PAIRED_CHIP_ROOT come from, what a
# fetch asks FetchContent for (a stand-in module, no network), a fetched SDK's submodules (scratch git repositories)
add_test(
    NAME kvasir_test_cmake_locate
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/.. -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/locate
            -DGIT=${GIT_EXECUTABLE} -P ${CMAKE_CURRENT_LIST_DIR}/locate/run.cmake)
set_tests_properties(kvasir_test_cmake_locate PROPERTIES LABELS tool)

# Runtime/FullRecordBuild.hpp: the fault record FaultHandler.hpp's record() builds - every flag, the frame and entry
# registers, the SP before the frame, the stack copy's start and length
kvasir_add_host_test(kvasir_test_full_record_build full_record_build_tests.cpp)
# Runtime/BootLog.hpp with the stub uc_log and this tree's real cmake_git_version headers (as util.cmake includes it):
# takePrevious once and after a power-on, endedInFailure, the boot line's level, the records' lines, the chip's hooks.
# -Wno-deprecated-declarations: a source copy without .git gets cmake_git_version's "git not found" deprecation on
# fullVersion, which says nothing about the code under test.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake_git_version/CMakeLists.txt)
kvasir_add_host_test(kvasir_test_boot_log boot_log_tests.cpp)
target_include_directories(kvasir_test_boot_log BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/support/uc_log_stub)
target_compile_definitions(kvasir_test_boot_log PRIVATE USE_UC_LOG=1)
target_compile_options(kvasir_test_boot_log PRIVATE -Wno-deprecated-declarations)
# the test project has no VERSION: the generated project header needs numbers
block(SCOPE_FOR VARIABLES)
set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)
cmake_git_version_add_headers(kvasir_test_boot_log)
endblock()
# Multicore/ClockSync.hpp on two threads over one virtual time: the adopted epoch within the bracket, both timeouts
kvasir_add_threaded_test(kvasir_test_clock_sync clock_sync_tests.cpp)
# Runtime/StackUsage.hpp over a buffer: the paint, free(), StackHighWater's passes
kvasir_add_host_test(kvasir_test_stack_usage stack_usage_tests.cpp)
