function(orbitdeck_add_test target test_name)
    cmake_parse_arguments(TEST "" "" "SOURCES;LIBRARIES" ${ARGN})
    if(TEST_UNPARSED_ARGUMENTS OR NOT TEST_SOURCES)
        message(FATAL_ERROR "Invalid source/library arguments for ${target}")
    endif()
    add_executable(${target} ${TEST_SOURCES})
    target_link_libraries(${target} PRIVATE OrbitDeckBuildOptions ${TEST_LIBRARIES})
    add_test(NAME ${test_name} COMMAND ${target})
endfunction()

orbitdeck_add_test(OrbitDeckCoreTests CoreDefaultsAndPlacement
    SOURCES CoreTests.cpp)

orbitdeck_add_test(OrbitDeckSelectionEngineTests SelectionEngine
    SOURCES SelectionEngineTests.cpp
        ../src/overlay/SelectionEngine.cpp ../src/overlay/WheelLayout.cpp)

orbitdeck_add_test(OrbitDeckSelectionPerformanceTests SelectionPerformance
    SOURCES SelectionPerformanceTests.cpp
        ../src/overlay/SelectionEngine.cpp ../src/overlay/WheelLayout.cpp)

orbitdeck_add_test(OrbitDeckWheelLayoutTests WheelLayout
    SOURCES WheelLayoutTests.cpp ../src/overlay/WheelLayout.cpp)

orbitdeck_add_test(OrbitDeckOverlayAnimationTests OverlayAnimation
    SOURCES OverlayAnimationTests.cpp ../src/overlay/OverlayAnimation.cpp)

orbitdeck_add_test(OrbitDeckCachedTextLayoutTests CachedTextLayout
    SOURCES CachedTextLayoutTests.cpp
    LIBRARIES dwrite)

orbitdeck_add_test(OrbitDeckIconCacheTests IconCache
    SOURCES IconCacheTests.cpp ../src/overlay/IconCache.cpp
    LIBRARIES ole32 shell32 windowscodecs)

orbitdeck_add_test(OrbitDeckAppCatalogTests AppCatalog
    SOURCES AppCatalogTests.cpp ../src/shell/AppCatalog.cpp
    LIBRARIES ole32 shell32 propsys)

orbitdeck_add_test(OrbitDeckLauncherTests Launcher
    SOURCES LauncherTests.cpp ../src/launch/Launcher.cpp
    LIBRARIES shell32)

orbitdeck_add_test(OrbitDeckActionExecutorTests ActionExecutor
    SOURCES ActionExecutorTests.cpp
        ../src/actions/ActionExecutor.cpp ../src/launch/Launcher.cpp
    LIBRARIES user32 shell32 ole32)

orbitdeck_add_test(OrbitDeckActivationControllerTests ActivationController
    SOURCES ActivationControllerTests.cpp ../src/input/ActivationController.cpp
    LIBRARIES user32)

orbitdeck_add_test(OrbitDeckFileConversionTests FileConversion
    SOURCES FileConversionTests.cpp ../src/tools/FileConversion.cpp
    LIBRARIES ole32 windowscodecs shlwapi)

orbitdeck_add_test(OrbitDeckConversionEngineTests ConversionEngine
    SOURCES ConversionEngineTests.cpp
        ../src/tools/ConversionEngine.cpp ../src/tools/ConversionProtocol.cpp
        ../src/shell/FileSelection.cpp
    LIBRARIES user32 shell32 ole32 oleaut32)

orbitdeck_add_test(OrbitDeckConversionProtocolTests ConversionProtocol
    SOURCES ConversionProtocolTests.cpp ../src/tools/ConversionProtocol.cpp)

orbitdeck_add_test(OrbitDeckPlatformOwnershipTests PlatformOwnership
    SOURCES PlatformOwnershipTests.cpp)

orbitdeck_add_test(OrbitDeckSettingsActionChoicesTests SettingsActionChoices
    SOURCES SettingsActionChoicesTests.cpp)

orbitdeck_add_test(OrbitDeckWindowActivatorTests WindowActivator
    SOURCES WindowActivatorTests.cpp ../src/window/WindowActivator.cpp
    LIBRARIES user32 dwmapi)

orbitdeck_add_test(OrbitDeckConfigurationTests Configuration
    SOURCES ConfigurationTests.cpp ../src/config/ConfigLoader.cpp
    LIBRARIES shell32 ole32)

orbitdeck_add_test(OrbitDeckMiniAppTests MiniApps
    SOURCES MiniAppTests.cpp ../src/tools/MiniAppWindow.cpp ../src/config/ConfigLoader.cpp
    LIBRARIES user32 shell32 ole32 vfw32 winmm)

orbitdeck_add_test(OrbitDeckDeveloperToolsTests DeveloperTools
    SOURCES DeveloperToolsTests.cpp ../src/tools/DeveloperTools.cpp
    LIBRARIES ole32 bcrypt)
