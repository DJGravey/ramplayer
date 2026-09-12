cmake_minimum_required(VERSION 3.16)
project(ramplayer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2 -g")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(OPENEXR REQUIRED IMPORTED_TARGET OpenEXR)

# We only use the pure-C OpenEXRCore API, so link that library alone rather than
# dragging in the C++ halves of OpenEXR.
find_library(OPENEXRCORE_LIBRARY
    NAMES OpenEXRCore-3_5 OpenEXRCore-3_4 OpenEXRCore-3_3 OpenEXRCore-3_2 OpenEXRCore
    HINTS ${OPENEXR_LIBRARY_DIRS})

add_executable(ramplayer
    src/main.c
    src/util.c
    src/color.c
    src/image.c
    src/font.c
    src/draw.c
    src/sequence.c
    src/reader_exr.c
    src/cache.c
    src/ui.c
    src/app.c
)

target_include_directories(ramplayer PRIVATE ${OPENEXR_INCLUDE_DIRS})
target_link_libraries(ramplayer PRIVATE PkgConfig::SDL2 Threads::Threads m)

if(OPENEXRCORE_LIBRARY)
    target_link_libraries(ramplayer PRIVATE ${OPENEXRCORE_LIBRARY})
else()
    message(STATUS "OpenEXRCore not found standalone; linking full OpenEXR")
    target_link_libraries(ramplayer PRIVATE PkgConfig::OPENEXR)
endif()

target_compile_options(ramplayer PRIVATE -Wall -Wextra -Wno-unused-parameter)

# We use POSIX threads, clock_gettime, opendir and strcasecmp under -std=c11,
# so ask libc for them explicitly rather than relying on a dependency's flags.
set(RP_FEATURE_DEFS _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE=1)
target_compile_definitions(ramplayer PRIVATE ${RP_FEATURE_DEFS})

# Behavioural tests. They drive app.c/ui.c directly, so no window is needed,
# but they do decode real frames and therefore need a sequence to point at.
add_executable(test_player EXCLUDE_FROM_ALL
    tests/test_player.c
    src/util.c src/color.c src/image.c src/font.c src/draw.c
    src/sequence.c src/reader_exr.c src/cache.c src/ui.c src/app.c
)
target_include_directories(test_player PRIVATE src ${OPENEXR_INCLUDE_DIRS})
target_link_libraries(test_player PRIVATE Threads::Threads m)
if(OPENEXRCORE_LIBRARY)
    target_link_libraries(test_player PRIVATE ${OPENEXRCORE_LIBRARY})
else()
    target_link_libraries(test_player PRIVATE PkgConfig::OPENEXR)
endif()
target_compile_options(test_player PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(test_player PRIVATE ${RP_FEATURE_DEFS})

add_executable(test_reader EXCLUDE_FROM_ALL
    tests/test_reader.c
    src/util.c src/color.c src/image.c src/sequence.c src/reader_exr.c
)
target_include_directories(test_reader PRIVATE src ${OPENEXR_INCLUDE_DIRS})
target_link_libraries(test_reader PRIVATE Threads::Threads m)
if(OPENEXRCORE_LIBRARY)
    target_link_libraries(test_reader PRIVATE ${OPENEXRCORE_LIBRARY})
else()
    target_link_libraries(test_reader PRIVATE PkgConfig::OPENEXR)
endif()
target_compile_options(test_reader PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(test_reader PRIVATE ${RP_FEATURE_DEFS})

add_executable(test_draw EXCLUDE_FROM_ALL
    tests/test_draw.c
    src/util.c src/image.c src/font.c src/draw.c
)
target_include_directories(test_draw PRIVATE src)
target_link_libraries(test_draw PRIVATE m)
target_compile_options(test_draw PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(test_draw PRIVATE ${RP_FEATURE_DEFS})

# Fixture generator for the awkward EXRs the tests need.
add_executable(mkexr EXCLUDE_FROM_ALL tools/mkexr.c)
target_compile_options(mkexr PRIVATE -Wall -Wextra)
