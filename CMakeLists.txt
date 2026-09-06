cmake_minimum_required(VERSION 3.20)
project(Corelock LANGUAGES CXX RC)

# Require modern C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ImGui Directory
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

# Core & GUI Source Files
set(SOURCES
    OptimizerCore.hpp
    OptimizerCore.cpp
    GuiApp.hpp
    GuiApp.cpp
    main.cpp
    resource.h
    Corelock.rc
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

add_executable(Corelock ${SOURCES})

# Include Directories
target_include_directories(Corelock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# Compiler warnings and options
if (MSVC)
    target_compile_options(Corelock PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(Corelock PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Link required Windows native libraries
target_link_libraries(Corelock PRIVATE
    d3d11.lib
    dxgi.lib
    dwmapi.lib
    Powrprof.lib
    Avrt.lib
    winmm.lib
    comdlg32.lib
)
