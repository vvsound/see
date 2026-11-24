cmake_minimum_required(VERSION 3.15)
project(Xview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "Xview 原生版只能在 Windows 上编译(GitHub 云端用 windows-latest)")
endif()

# 静态链接 C++ 运行库:exe 不依赖 VC++ 运行库,拷到任何 Windows 10/11 都能运行
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(Xview WIN32
  src/main.cpp
  resources/app.rc
)

target_compile_definitions(Xview PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(Xview PRIVATE /utf-8 /W3 /EHsc /O2)
target_link_options(Xview PRIVATE /OPT:REF /OPT:ICF /MANIFEST:NO)
target_include_directories(Xview PRIVATE src)
target_link_libraries(Xview PRIVATE d2d1 dwrite windowscodecs ole32 oleaut32 user32 gdi32 shell32 shlwapi comdlg32 winspool uuid d3d11 dxgi dwmapi comctl32 advapi32)
