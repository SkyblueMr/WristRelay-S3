cmake_minimum_required(VERSION 3.20)
project(wristrelay_host_tests LANGUAGES C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(FIRMWARE "${CMAKE_CURRENT_SOURCE_DIR}/../main" ABSOLUTE)
add_subdirectory("${FIRMWARE}/../firmware_core" firmware_core)

function(wr_host_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE "${FIRMWARE}")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

wr_host_test(frame_test frame_test.c)
wr_host_test(button_test button_test.c)
wr_host_test(power_test power_test.c "${FIRMWARE}/power/power.c")
target_include_directories(power_test PRIVATE shims)

option(WR_TEST_OPUS "Test the firmware codec using the pinned libopus source" ON)
if(WR_TEST_OPUS)
    include("${FIRMWARE}/../components/wristrelay_audio/opus_source.cmake")
    # Match the Xtensa portable fixed-point path, not x86-specific SIMD.
    set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "Use portable Opus in host tests" FORCE)
    wr_add_opus()
    wr_host_test(opus_test opus_test.c "${FIRMWARE}/audio/wr_opus.c")
    target_link_libraries(opus_test PRIVATE opus wristrelay_firmware_core)
endif()
