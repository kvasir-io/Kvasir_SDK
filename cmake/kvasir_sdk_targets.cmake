# The SDK's header-only targets, also included on its own by host test builds.
get_filename_component(_kvasir_sdk_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT TARGET kvasir_sdk)
    add_library(kvasir_sdk INTERFACE)
    target_include_directories(kvasir_sdk INTERFACE ${_kvasir_sdk_root}/src)
    add_library(kvasir::sdk ALIAS kvasir_sdk)
endif()

if(NOT TARGET kvasir_test_support)
    add_library(kvasir_test_support INTERFACE)
    target_include_directories(kvasir_test_support INTERFACE ${_kvasir_sdk_root}/tests)
    target_link_libraries(kvasir_test_support INTERFACE kvasir_sdk)
    add_library(kvasir::test_support ALIAS kvasir_test_support)
endif()
