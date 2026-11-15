cmake_minimum_required(VERSION 3.21)
project(QuickView VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport Concurrent Network)

# 新增 .cpp 文件必须同时加到这里
set(QV_SOURCES
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/core/Types.h
    src/core/Config.h
    src/core/Config.cpp
    src/core/State.h
    src/core/State.cpp
    src/core/Strings.h
    src/core/Strings.cpp
    src/core/Commands.h
    src/core/Commands.cpp
    src/core/ImageLoader.h
    src/core/ImageLoader.cpp
    src/core/FolderList.h
    src/core/FolderList.cpp
    src/core/FileOps.h
    src/core/FileOps.cpp
    src/core/DescriptIon.h
    src/core/DescriptIon.cpp
    src/core/ThumbnailCache.h
    src/core/ThumbnailCache.cpp
    src/core/SingleInstance.h
    src/core/SingleInstance.cpp
    src/core/Cli.h
    src/core/Cli.cpp
    src/core/ExifReader.h
    src/core/ExifReader.cpp
    src/core/Theme.h
    src/core/Theme.cpp
    src/core/CustomCommands.h
    src/core/CustomCommands.cpp
    src/ui/ViewerWidget.h
    src/ui/ViewerWidget.cpp
    src/ui/BrowserWidget.h
    src/ui/BrowserWidget.cpp
    src/ui/FileListModel.h
    src/ui/FileListModel.cpp
    src/ui/InfoPanel.h
    src/ui/InfoPanel.cpp
    src/ui/FilterBar.h
    src/ui/FilterBar.cpp
    src/ui/PreviewPane.h
    src/ui/PreviewPane.cpp
    src/ui/OptionsDialog.h
    src/ui/OptionsDialog.cpp
    src/ui/ExtensionsDialog.h
    src/ui/ExtensionsDialog.cpp
    src/platform/FileAssoc.h
    src/platform/FileAssoc.cpp
    resources/resources.qrc
)

add_executable(QuickView WIN32 ${QV_SOURCES})
if(WIN32)
    # exe 文件本身的图标和版本信息(资源管理器、任务栏、默认应用列表显示的就是它)
    enable_language(RC)
    target_sources(QuickView PRIVATE resources/app.rc)
endif()
target_include_directories(QuickView PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(QuickView PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::PrintSupport Qt6::Concurrent Qt6::Network)

if(MSVC)
    target_compile_options(QuickView PRIVATE /utf-8 /W3)
    target_link_options(QuickView PRIVATE /ENTRY:mainCRTStartup)
endif()
if(WIN32)
    target_link_libraries(QuickView PRIVATE advapi32 shell32 user32 dwmapi)
endif()
