cmake_minimum_required(VERSION 3.15)
project(swfrztool-gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 静态链接 CRT：目标机器无需安装 VC++ 运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(swfrztool-gui WIN32
    src/main.cpp
    src/app.cpp
    src/ui.cpp
    src/swfrztool_api.cpp
)

target_include_directories(swfrztool-gui PRIVATE src)

target_compile_definitions(swfrztool-gui PRIVATE UNICODE _UNICODE)

if(MSVC)
    # 源码为 UTF-8，含中文注释与字符串
    target_compile_options(swfrztool-gui PRIVATE /utf-8)
    # 要求以管理员身份运行
    target_link_options(swfrztool-gui PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

target_link_libraries(swfrztool-gui PRIVATE
    d3d11
    dxgi
    d2d1
    dwrite
    dwmapi
    advapi32
    psapi
)
