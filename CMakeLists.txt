cmake_minimum_required(VERSION 3.16)
project(ramplayer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(NOT MSVC)
    set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
endif()

find_package(Threads REQUIRED)

# ---- dependencies ----------------------------------------------------------
# CMake config packages first (vcpkg, and any distribution that ships them),
# pkg-config as the fallback.

find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2 OR TARGET SDL2::SDL2-static)
    set(RP_SDL2_LIBS $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
                     $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(RP_SDL2_LIBS PkgConfig::SDL2)
endif()

# We only use the pure-C OpenEXRCore API, so link that library alone rather
# than dragging in the C++ halves of OpenEXR.
find_package(OpenEXR CONFIG QUIET)
if(TARGET OpenEXR::OpenEXRCore)
    set(RP_EXR_LIBS OpenEXR::OpenEXRCore)
    set(RP_EXR_INCLUDE_DIRS "")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPENEXR REQUIRED IMPORTED_TARGET OpenEXR)
    find_library(OPENEXRCORE_LIBRARY
        NAMES OpenEXRCore-3_5 OpenEXRCore-3_4 OpenEXRCore-3_3 OpenEXRCore-3_2 OpenEXRCore
        HINTS ${OPENEXR_LIBRARY_DIRS})
    if(OPENEXRCORE_LIBRARY)
        set(RP_EXR_LIBS ${OPENEXRCORE_LIBRARY})
    else()
        message(STATUS "OpenEXRCore not found standalone; linking full OpenEXR")
        set(RP_EXR_LIBS PkgConfig::OPENEXR)
    endif()
    set(RP_EXR_INCLUDE_DIRS ${OPENEXR_INCLUDE_DIRS})
endif()

# ---- compiler settings -----------------------------------------------------

if(MSVC)
    # <stdatomic.h> in C mode is still behind an experimental switch in MSVC.
    # Release keeps debug information, matching "-O2 -g" on the other side.
    set(RP_WARNINGS /W4 /wd4100)
    set(RP_COMPILE_OPTIONS /experimental:c11atomics /utf-8 $<$<CONFIG:Release>:/Zi>)
    set(RP_LINK_OPTIONS $<$<CONFIG:Release>:/DEBUG>)
    set(RP_FEATURE_DEFS _CRT_SECURE_NO_WARNINGS)
    set(RP_SYSTEM_LIBS "")
else()
    set(RP_WARNINGS -Wall -Wextra -Wno-unused-parameter)
    set(RP_COMPILE_OPTIONS "")
    set(RP_LINK_OPTIONS "")
    # We use POSIX threads, clock_gettime, opendir and strcasecmp under
    # -std=c11, so ask libc for them explicitly rather than relying on a
    # dependency's flags.
    set(RP_FEATURE_DEFS _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE=1)
    set(RP_SYSTEM_LIBS m)
endif()

function(rp_configure_target target)
    target_compile_options(${target} PRIVATE ${RP_WARNINGS} ${RP_COMPILE_OPTIONS})
    target_compile_definitions(${target} PRIVATE ${RP_FEATURE_DEFS})
    target_link_options(${target} PRIVATE ${RP_LINK_OPTIONS})
    target_link_libraries(${target} PRIVATE Threads::Threads ${RP_SYSTEM_LIBS})
endfunction()

# ---- targets ---------------------------------------------------------------

add_executable(ramplayer
    src/main.c
    src/platform.c
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
if(WIN32)
    target_sources(ramplayer PRIVATE src/ramplayer.rc)
endif()
rp_configure_target(ramplayer)
target_include_directories(ramplayer PRIVATE ${RP_EXR_INCLUDE_DIRS})
target_link_libraries(ramplayer PRIVATE ${RP_SDL2_LIBS} ${RP_EXR_LIBS})

# Behavioural tests. They drive app.c/ui.c directly, so no window is needed,
# but they do decode real frames and therefore need a sequence to point at.
add_executable(test_player EXCLUDE_FROM_ALL
    tests/test_player.c
    src/platform.c src/util.c src/color.c src/image.c src/font.c src/draw.c
    src/sequence.c src/reader_exr.c src/cache.c src/ui.c src/app.c
)
rp_configure_target(test_player)
target_include_directories(test_player PRIVATE src ${RP_EXR_INCLUDE_DIRS})
target_link_libraries(test_player PRIVATE ${RP_EXR_LIBS})

add_executable(test_reader EXCLUDE_FROM_ALL
    tests/test_reader.c
    src/platform.c src/util.c src/color.c src/image.c src/sequence.c src/reader_exr.c
)
rp_configure_target(test_reader)
target_include_directories(test_reader PRIVATE src ${RP_EXR_INCLUDE_DIRS})
target_link_libraries(test_reader PRIVATE ${RP_EXR_LIBS})

add_executable(test_draw EXCLUDE_FROM_ALL
    tests/test_draw.c
    src/platform.c src/util.c src/image.c src/font.c src/draw.c
)
rp_configure_target(test_draw)
target_include_directories(test_draw PRIVATE src)

# Fixture generator for the awkward EXRs the tests need.
add_executable(mkexr EXCLUDE_FROM_ALL tools/mkexr.c)
rp_configure_target(mkexr)
