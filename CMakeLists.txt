cmake_minimum_required(VERSION 3.24)
project(AkiLauncher CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

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
)
target_link_libraries(AkiLauncher PRIVATE imgui_lib d3d11 dxgi xinput9_1_0 windowscodecs ole32
                      winmm)
