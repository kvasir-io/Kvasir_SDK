# The SDK's header-only targets, also included on its own by host test builds. kvasir::base is the SDK's src include
# directory and nothing else (Mpl, Util, Register on the host mock). kvasir::sdk is kvasir::base and, once
# uc_log::uc_log exists, uc_log: a firmware's util.cmake adds uc_log before it includes this file, and whichever include
# comes last links it, so a package that included this file before util.cmake (kvasir_devices) still gets it.
# kvasir::test_support is the SDK's tests/ (test_harness.hpp, support/...) over kvasir::sdk.
get_filename_component(_kvasir_sdk_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT TARGET kvasir_base)
    add_library(kvasir_base INTERFACE)
    target_include_directories(kvasir_base INTERFACE ${_kvasir_sdk_root}/src)
    add_library(kvasir::base ALIAS kvasir_base)
endif()

if(NOT TARGET kvasir_sdk)
    add_library(kvasir_sdk INTERFACE)
    target_link_libraries(kvasir_sdk INTERFACE kvasir_base)
    add_library(kvasir::sdk ALIAS kvasir_sdk)
endif()

if(TARGET uc_log::uc_log)
    get_target_property(_kvasir_sdk_links kvasir_sdk INTERFACE_LINK_LIBRARIES)
    if(NOT "uc_log::uc_log" IN_LIST _kvasir_sdk_links)
        target_link_libraries(kvasir_sdk INTERFACE uc_log::uc_log)
    endif()
endif()

if(NOT TARGET kvasir_test_support)
    add_library(kvasir_test_support INTERFACE)
    target_include_directories(kvasir_test_support INTERFACE ${_kvasir_sdk_root}/tests)
    target_link_libraries(kvasir_test_support INTERFACE kvasir_sdk)
    add_library(kvasir::test_support ALIAS kvasir_test_support)
endif()
