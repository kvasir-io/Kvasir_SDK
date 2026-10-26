# cmake/jlink.cmake alone: two images through target_add_flash_jlink, driven by -D cache variables
# (tests/jlink_scripts/test_jlink_scripts.py). Configure only; -Djlinkexe=... stands in for JLinkExe.
cmake_minimum_required(VERSION 3.25)
project(jlink_scripts NONE)

set(Python3_EXECUTABLE python3)
# what uc_log's CMakeLists.txt sets before the SDK includes jlink.cmake
set(UC_LOG_SWD_DEFAULT_KHZ 100000)
include(${KVASIR_SDK_DIR}/cmake/jlink.cmake)

set(probe_args)
if(TEST_JLINK_IP)
    list(APPEND probe_args JLINK_IP ${TEST_JLINK_IP})
endif()
if(DEFINED TEST_JLINK_PROBE)
    list(APPEND probe_args JLINK_PROBE "${TEST_JLINK_PROBE}")
endif()
if(TEST_CONNECT_COMMANDS)
    list(APPEND probe_args CONNECT_COMMANDS ${TEST_CONNECT_COMMANDS})
endif()
if(TEST_RAM_ONLY)
    list(APPEND probe_args RAM_ONLY)
endif()

foreach(image IN ITEMS img_a img_b)
    add_custom_target(${image})
    target_add_flash_jlink(${image} TARGET_MPU ${TEST_MPU} ${probe_args})
endforeach()
