cmake_minimum_required(VERSION 3.24)
project(AkiLauncher C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the packaged launcher is a single self-contained exe (the game
# release zips carry their own runtime DLLs; the launcher shouldn't need any).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# Amalgamated miniz release (zip extraction for in-app game installs)
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniz)
add_library(miniz_lib STATIC ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(miniz_lib PUBLIC ${miniz_SOURCE_DIR})

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
                           ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(imgui_lib PUBLIC UNICODE _UNICODE NOMINMAX)

# assets/games.json (the game registry, aki-roadmap#24) is built in as the
# fallback: its bytes as a char array in games_json.inc (no string-literal size
# limit, no source-charset question). Edits re-run the configure; the .inc is
# rewritten only when it changes.
file(READ ${CMAKE_SOURCE_DIR}/assets/games.json AKI_GAMES_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," AKI_GAMES_BYTES "${AKI_GAMES_HEX}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" AKI_GAMES_BYTES "${AKI_GAMES_BYTES}")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/games_json.inc CONTENT "${AKI_GAMES_BYTES}0x00\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/games.json)

add_executable(AkiLauncher WIN32
    src/main.cpp
    src/games.cpp
    src/launch.cpp
    src/artwork.cpp
    src/settings.cpp
    src/audio.cpp
    src/installer.cpp
    src/rebuild.cpp
    src/romscan.cpp
    src/mods.cpp
    src/json.cpp
    src/selfupdate.cpp
    src/modelpacks.cpp
)
target_include_directories(AkiLauncher PRIVATE ${CMAKE_BINARY_DIR}/generated)
if(MSVC)
    # App icon + version resource (title bar / taskbar / Explorer)
    target_sources(AkiLauncher PRIVATE icons/app.rc)
endif()
target_link_libraries(AkiLauncher PRIVATE imgui_lib miniz_lib d3d11 dxgi xinput9_1_0
                      windowscodecs ole32 winmm winhttp bcrypt version)

# Console checks for the dev-mode Rebuild game action (no window needed):
# cmake -DAKI_LAUNCHER_TESTS=ON, then build-msvc\AkiLauncherTests.exe.
option(AKI_LAUNCHER_TESTS "Build AkiLauncherTests" OFF)
if(AKI_LAUNCHER_TESTS)
    add_executable(AkiLauncherTests tests/tests_main.cpp tests/rebuild_tests.cpp tests/romscan_tests.cpp
                   tests/mods_tests.cpp tests/portable_tests.cpp tests/registry_tests.cpp tests/selfupdate_tests.cpp tests/modelpacks_tests.cpp src/json.cpp src/modelpacks.cpp src/selfupdate.cpp
                   src/installer.cpp src/rebuild.cpp src/romscan.cpp src/mods.cpp src/settings.cpp
                   src/launch.cpp src/games.cpp)
    target_compile_definitions(AkiLauncherTests PRIVATE UNICODE _UNICODE NOMINMAX)
    target_include_directories(AkiLauncherTests PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(AkiLauncherTests PRIVATE miniz_lib xinput9_1_0 bcrypt winhttp version)
endif()
