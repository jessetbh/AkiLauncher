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
)
if(MSVC)
    # App icon + version resource (title bar / taskbar / Explorer)
    target_sources(AkiLauncher PRIVATE icons/app.rc)
endif()
target_link_libraries(AkiLauncher PRIVATE imgui_lib miniz_lib d3d11 dxgi xinput9_1_0
                      windowscodecs ole32 winmm winhttp bcrypt)

# Console checks for the dev-mode Rebuild game action (no window needed):
# cmake -DAKI_LAUNCHER_TESTS=ON, then build-msvc\AkiLauncherTests.exe.
option(AKI_LAUNCHER_TESTS "Build AkiLauncherTests" OFF)
if(AKI_LAUNCHER_TESTS)
    add_executable(AkiLauncherTests tests/tests_main.cpp tests/rebuild_tests.cpp tests/romscan_tests.cpp
                   tests/mods_tests.cpp src/rebuild.cpp src/romscan.cpp src/mods.cpp src/settings.cpp
                   src/launch.cpp src/games.cpp)
    target_compile_definitions(AkiLauncherTests PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(AkiLauncherTests PRIVATE miniz_lib xinput9_1_0 bcrypt)
endif()
