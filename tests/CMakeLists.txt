cmake_minimum_required(VERSION 3.16)

# Host tests for emulator components. Buildable two ways:
#  - standalone, with the host toolchain (sanitizers on):
#      cmake -S tests -B tests/build
#      cmake --build tests/build
#      ctest --test-dir tests/build
#  - as part of the cross build (compile-checks the headers for the target)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(minicast_tests CXX)
    enable_testing()
endif()

set(dyna ${CMAKE_CURRENT_SOURCE_DIR}/../libswirl/hw/sh4/dyna)

# regalloc.h can't be included without the full emulator headers; generate a
# copy with the emu includes stripped and pair it with the mocks in mockshil.h
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/regalloc_under_test.h
    COMMAND ${CMAKE_COMMAND}
        -DIN=${dyna}/regalloc.h
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/regalloc_under_test.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/strip_emu_includes.cmake
    DEPENDS ${dyna}/regalloc.h ${CMAKE_CURRENT_SOURCE_DIR}/strip_emu_includes.cmake
    COMMENT "Generating regalloc_under_test.h from regalloc.h"
)

add_executable(regalloc_fuzz
    regalloc_fuzz.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/regalloc_under_test.h
)
set_property(TARGET regalloc_fuzz PROPERTY CXX_STANDARD 17)
target_include_directories(regalloc_fuzz PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

if(NOT CMAKE_CROSSCOMPILING)
    target_compile_options(regalloc_fuzz PRIVATE -O1 -g -fsanitize=address,undefined)
    target_link_options(regalloc_fuzz PRIVATE -fsanitize=address,undefined)
endif()

# args: <base seed> <iterations per pool/flag combination>
add_test(NAME regalloc_fuzz COMMAND regalloc_fuzz 1 5000)
# the allocator intentionally keeps the last block's spans until the next
# DoAlloc; that reads as a leak at process exit
set_tests_properties(regalloc_fuzz PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

# constprop()/rw_related() are extracted verbatim from shil.cpp and compiled
# against the mocks in constprop_mocks.h
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/constprop_under_test.h
    COMMAND ${CMAKE_COMMAND}
        -DIN=${dyna}/shil.cpp
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/constprop_under_test.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/extract_constprop.cmake
    DEPENDS ${dyna}/shil.cpp ${CMAKE_CURRENT_SOURCE_DIR}/extract_constprop.cmake
            ${CMAKE_CURRENT_SOURCE_DIR}/constprop_mocks.h
    COMMENT "Generating constprop_under_test.h from shil.cpp"
)

add_executable(constprop_test
    constprop_test.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/constprop_under_test.h
)
set_property(TARGET constprop_test PROPERTY CXX_STANDARD 17)
target_include_directories(constprop_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT CMAKE_CROSSCOMPILING)
    target_compile_options(constprop_test PRIVATE -O1 -g -fsanitize=address,undefined)
    target_link_options(constprop_test PRIVATE -fsanitize=address,undefined)
endif()

# args: <base seed> <fuzz iterations>
add_test(NAME constprop_test COMMAND constprop_test 1 3000)

# RetroAchievements module (ra/) + rcheevos: memory mapping, dry-run
# responses, chime loader/mixer; hashes any images given on the command line
# (ra_test [--wav f] [--gameid] image.gdi ...). End to end against a fake
# server: ra_test --flow dry|live image.gdi tests/ra_fake_curl.sh
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    enable_language(C)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/../external/rcheevos.cmake)
add_executable(ra_test ra_test.cpp ${RA_SOURCES})
set_property(TARGET ra_test PROPERTY CXX_STANDARD 17)
target_include_directories(ra_test PRIVATE ${RA_INCLUDE_DIRS})
target_link_libraries(ra_test PRIVATE pthread)

if(NOT CMAKE_CROSSCOMPILING)
    target_compile_options(ra_test PRIVATE -O1 -g -fsanitize=address,undefined)
    target_link_options(ra_test PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME ra_test COMMAND ra_test --wav ${CMAKE_CURRENT_SOURCE_DIR}/../ra/achievement.wav)

# RetroAchievements overlay (ra_osd + ra_overlay against a fake VRAM, the
# frontend/audio hooks stubbed in the test): ra_osd_test [preview_dir]
add_executable(ra_osd_test ra_osd_test.cpp ${RA_SOURCES} ${RA_OVERLAY_SOURCES})
set_property(TARGET ra_osd_test PROPERTY CXX_STANDARD 17)
target_include_directories(ra_osd_test PRIVATE ${RA_INCLUDE_DIRS})
target_link_libraries(ra_osd_test PRIVATE pthread)

if(NOT CMAKE_CROSSCOMPILING)
    target_compile_options(ra_osd_test PRIVATE -O1 -g -fsanitize=address,undefined)
    target_link_options(ra_osd_test PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME ra_osd_test COMMAND ra_osd_test)

# In-game menu (ra_menu + ra_json): JSON parse/write, options, CRT_CTRL
# read-modify-write, launcher protocol files, page navigation
add_executable(ra_menu_test ra_menu_test.cpp ${RA_SOURCES})
set_property(TARGET ra_menu_test PROPERTY CXX_STANDARD 17)
target_include_directories(ra_menu_test PRIVATE ${RA_INCLUDE_DIRS})
target_link_libraries(ra_menu_test PRIVATE pthread)

if(NOT CMAKE_CROSSCOMPILING)
    target_compile_options(ra_menu_test PRIVATE -O1 -g -fsanitize=address,undefined)
    target_link_options(ra_menu_test PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME ra_menu_test COMMAND ra_menu_test)
