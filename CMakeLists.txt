cmake_minimum_required(VERSION 3.24...4.0)
project(wavpane VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── SDL3 ─────────────────────────────────────────────────────────────────────
# Uses the system's SDL3 (3.2+) when one is installed -- most current Linux
# distributions package it -- and otherwise downloads and builds SDL3 3.2.10.
# To force the download: -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER
#
# The downloaded copy is built static, so the wavpane binary (including an
# installed one) doesn't depend on a libSDL3.so that only exists in the
# build tree. SDL still loads X11/Wayland/audio libraries at runtime.
set(SDL_SHARED OFF CACHE BOOL "Build SDL3 as a shared library (bundled SDL3 only)" FORCE)
set(SDL_STATIC ON  CACHE BOOL "Build SDL3 as a static library (bundled SDL3 only)" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL3's test library (bundled SDL3 only)" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.10
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS 3.2 CONFIG
)
FetchContent_MakeAvailable(SDL3)
if(SDL3_FOUND)
    message(STATUS "wavpane: using system SDL3 ${SDL3_VERSION}")
else()
    message(STATUS "wavpane: using bundled SDL3 (static)")
endif()

add_executable(wavpane
    wav_main.cpp
)
target_link_libraries(wavpane PRIVATE SDL3::SDL3)
target_compile_definitions(wavpane PRIVATE WAVPANE_VERSION="${PROJECT_VERSION}")

# ── Install (Linux desktop integration) ──────────────────────────────────────
# cmake --install build [--prefix ~/.local]: the binary, a .desktop entry
# (app launcher + "Open With" for WAV files) and the icon.
include(GNUInstallDirs)
install(TARGETS wavpane RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/wavpane.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/wavpane.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# On Windows with a shared SDL3, copy the DLL next to the executable
if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET wavpane POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3-shared>
            $<TARGET_FILE_DIR:wavpane>
    )
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
# wav_reader.hpp is pure logic with no SDL entanglement. drawWaveform lives in
# waveform_render.hpp (pulled out of wav_main.cpp for this reason) and is
# SDL-rendering code, but it's tested too: SDL's software renderer draws into
# an in-memory surface with no window/display needed, so it's still headless
# and CI-safe. The rest of wav_main.cpp (input handling, HUD, windowing) stays
# best checked by actually running the app.
# Packagers can skip the tests (and the googletest download) with
# -DBUILD_TESTING=OFF.
option(BUILD_TESTING "Build the unit tests" ON)
if(BUILD_TESTING)
    set(INSTALL_GTEST OFF CACHE BOOL "Don't install googletest along with wavpane" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(wav_reader_test tests/wav_reader_test.cpp)
    target_link_libraries(wav_reader_test PRIVATE GTest::gtest_main)

    add_executable(waveform_render_test tests/waveform_render_test.cpp)
    target_link_libraries(waveform_render_test PRIVATE GTest::gtest_main SDL3::SDL3)

    add_executable(hover_stats_test tests/hover_stats_test.cpp)
    target_link_libraries(hover_stats_test PRIVATE GTest::gtest_main)

    add_executable(time_format_test tests/time_format_test.cpp)
    target_link_libraries(time_format_test PRIVATE GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(wav_reader_test)
    gtest_discover_tests(waveform_render_test)
    gtest_discover_tests(hover_stats_test)
    gtest_discover_tests(time_format_test)
endif()
