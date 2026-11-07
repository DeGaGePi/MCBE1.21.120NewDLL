cmake_minimum_required(VERSION 3.20)
project(NewDLL LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 全静态 CRT：DLL 不依赖 msvcp140/vcruntime140/ucrtbase，注入到别人进程里最省心
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 输出目录：放在构建目录下，保持纯 ASCII
# （链接器/rc 走 vs_link_dll 包装器，输出路径带中文会报 "Unknown system error -1"）
set(OUT_DIR "${CMAKE_BINARY_DIR}/out")

add_library(NewDLL SHARED
    src/dllmain.cpp
    src/hooks/DXGIHook.cpp
    src/game/Game.cpp
    src/input/Input.cpp
    src/render/Renderer.cpp
    src/ui/Module.cpp
    src/ui/Hud.cpp
    src/ui/ClickGui.cpp
)

target_include_directories(NewDLL PRIVATE "${CMAKE_SOURCE_DIR}/src")

target_compile_definitions(NewDLL PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

set_target_properties(NewDLL PROPERTIES
    PREFIX ""
    OUTPUT_NAME "NewDLL"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUT_DIR}"
)

target_compile_options(NewDLL PRIVATE
    "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W3>"
    "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>"
)

target_link_libraries(NewDLL PRIVATE
    d3d11
    dxgi
    d3d12
    d2d1
    dwrite
    dwmapi
    user32
    gdi32
    ole32
)

# 生成 .map：崩溃转储里只有 RVA（NewDLL.dll+0x6d5e 这种），有 map 才能立刻对上符号
if (MSVC)
    target_link_options(NewDLL PRIVATE "/MAP:${OUT_DIR}/NewDLL.map")
endif()

set_target_properties(NewDLL PROPERTIES
    PREFIX ""
    OUTPUT_NAME "NewDLL"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUT_DIR}"
)
