cmake_minimum_required(VERSION 3.25)

project(OrbitDeck VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "OrbitDeck requires Windows 10 or Windows 11.")
endif()

option(
    ORBITDECK_DIAGNOSTIC_CONSOLE
    "Build with a development console instead of the Windows GUI subsystem"
    OFF)

option(ORBITDECK_WARNINGS_AS_ERRORS "Treat first-party C++ compiler warnings as errors" ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(OrbitDeckBuildOptions INTERFACE)
target_compile_features(OrbitDeckBuildOptions INTERFACE cxx_std_20)
target_compile_definitions(OrbitDeckBuildOptions INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_include_directories(OrbitDeckBuildOptions INTERFACE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/assets"
    "${PROJECT_SOURCE_DIR}/third_party")
if(MSVC)
    target_compile_options(OrbitDeckBuildOptions INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:/W4>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
    if(ORBITDECK_WARNINGS_AS_ERRORS)
        target_compile_options(OrbitDeckBuildOptions INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
    endif()
endif()

add_executable(OrbitDeck WIN32
    assets/orbitdeck.ico
    assets/orbitdeck.rc
    assets/orbitdeck.svg
    assets/resource.h
    src/actions/ActionExecutor.cpp
    src/actions/ActionExecutor.h
    src/main.cpp
    src/app/OrbitDeckApp.cpp
    src/app/OrbitDeckApp.h
    src/config/Configuration.h
    src/config/ConfigLoader.cpp
    src/config/ConfigLoader.h
    src/input/ActivationController.cpp
    src/input/ActivationController.h
    src/launch/Launcher.cpp
    src/launch/Launcher.h
    src/overlay/OverlayAnimation.cpp
    src/overlay/OverlayAnimation.h
    src/overlay/OverlayController.cpp
    src/overlay/OverlayController.h
    src/overlay/IconCache.cpp
    src/overlay/IconCache.h
    src/overlay/CachedTextLayout.h
    src/overlay/SelectionEngine.cpp
    src/overlay/SelectionEngine.h
    src/overlay/WheelLayout.cpp
    src/overlay/WheelLayout.h
    src/overlay/WheelPlacement.h
    src/platform/MainMessageWindow.cpp
    src/platform/MainMessageWindow.h
    src/platform/SingleInstanceCoordinator.cpp
    src/platform/SingleInstanceCoordinator.h
    src/platform/UniqueHandle.h
    src/shell/TrayIconController.cpp
    src/shell/TrayIconController.h
    src/shell/AppCatalog.cpp
    src/shell/AppCatalog.h
    src/settings/SettingsWindow.cpp
    src/settings/SettingsWindow.h
    src/settings/ActionChoices.h
    src/tools/MiniAppWindow.cpp
    src/tools/MiniAppWindow.h
    src/tools/DeveloperTools.cpp
    src/tools/DeveloperTools.h
    src/tools/DeveloperToolWindow.cpp
    src/tools/DeveloperToolWindow.h
    src/tools/FileConversion.cpp
    src/tools/FileConversion.h
    src/tools/FileConversionWindow.cpp
    src/tools/FileConversionWindow.h
    src/tools/ConversionEngine.cpp
    src/tools/ConversionEngine.h
    src/tools/ConversionProtocol.cpp
    src/tools/ConversionProtocol.h
    src/shell/FileSelection.cpp
    src/shell/FileSelection.h
    src/window/WindowActivator.cpp
    src/window/WindowActivator.h
)

target_link_libraries(OrbitDeck PRIVATE
    OrbitDeckBuildOptions
    user32
    shell32
    propsys
    comctl32
    uxtheme
    comdlg32
    ole32
    d3d11
    dxgi
    d2d1
    dcomp
    dwrite
    dwmapi
    windowscodecs
    vfw32
    winmm
    bcrypt
    wtsapi32
    shlwapi)

if(ORBITDECK_DIAGNOSTIC_CONSOLE)
    set_property(TARGET OrbitDeck PROPERTY WIN32_EXECUTABLE FALSE)
    target_compile_definitions(OrbitDeck PRIVATE ORBITDECK_DIAGNOSTIC_CONSOLE)
endif()

if(MSVC)
    target_link_options(OrbitDeck PRIVATE /MANIFEST:NO)
    set_property(TARGET OrbitDeck PROPERTY
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

find_program(ORBITDECK_CLANG_FORMAT NAMES clang-format
    HINTS "${PROJECT_SOURCE_DIR}/out/tools/format/Scripts")
if(ORBITDECK_CLANG_FORMAT)
    file(GLOB_RECURSE ORBITDECK_FORMAT_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.h"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp" "${PROJECT_SOURCE_DIR}/tests/*.h")
    string(REPLACE ";" "\n" ORBITDECK_FORMAT_MANIFEST "${ORBITDECK_FORMAT_SOURCES}")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/format-files.txt"
        CONTENT "${ORBITDECK_FORMAT_MANIFEST}\n")
    add_custom_target(format
        COMMAND "${ORBITDECK_CLANG_FORMAT}" -i
            "--files=${CMAKE_BINARY_DIR}/format-files.txt"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
    add_custom_target(format-check
        COMMAND "${ORBITDECK_CLANG_FORMAT}" --dry-run --Werror
            "--files=${CMAKE_BINARY_DIR}/format-files.txt"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
