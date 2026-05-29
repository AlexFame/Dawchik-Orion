cmake_minimum_required(VERSION 3.25)

project(
    Orion
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ORION_JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE" CACHE PATH "Path to a local JUCE checkout")

if(NOT EXISTS "${ORION_JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE was not found. Clone or copy JUCE into third_party/JUCE before configuring Orion.")
endif()

add_subdirectory("${ORION_JUCE_PATH}" JUCE)
add_subdirectory(third_party/signalsmith-linear)
add_subdirectory(third_party/signalsmith-stretch)

find_path(
    ORION_RUBBERBAND_INCLUDE_DIR
    rubberband/RubberBandStretcher.h
    PATHS
        /opt/homebrew/include
        /usr/local/include
)

find_library(
    ORION_RUBBERBAND_LIBRARY
    NAMES rubberband
    PATHS
        /opt/homebrew/lib
        /usr/local/lib
)

find_library(ORION_ACCELERATE_FRAMEWORK Accelerate)

if(ORION_RUBBERBAND_INCLUDE_DIR AND ORION_RUBBERBAND_LIBRARY)
    message(STATUS "Rubber Band experimental warp enabled: ${ORION_RUBBERBAND_LIBRARY}")
    set(ORION_HAVE_RUBBERBAND 1)
else()
    message(STATUS "Rubber Band not found; using Signalsmith warp only")
    set(ORION_HAVE_RUBBERBAND 0)
endif()

juce_add_gui_app(Orion PRODUCT_NAME "Orion")

target_sources(
    Orion
    PRIVATE
        Source/main.cpp
        Source/App/MainWindow.cpp
        Source/App/OrionApplication.cpp
        Source/Audio/ExportService.cpp
        Source/Audio/TransportController.cpp
        Source/Audio/TransportEngine.cpp
        Source/Audio/WarpEngine.cpp
        Source/Core/ProjectSerializer.cpp
        Source/Core/ProjectState.cpp
        Source/Plugins/PluginManager.cpp
        Source/Sampler/SamplerEngine.cpp
        Source/Sampler/SamplerPanelComponent.cpp
        Source/UI/ArrangementTimelineComponent.cpp
        Source/UI/BrowserPanelComponent.cpp
        Source/UI/MainComponent.cpp
        Source/UI/MidiEditorOverlayComponent.cpp
)

target_include_directories(
    Orion
    PRIVATE
        Source
        $<$<BOOL:${ORION_HAVE_RUBBERBAND}>:${ORION_RUBBERBAND_INCLUDE_DIR}>
)

target_compile_definitions(
    Orion
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_PLUGINHOST_VST3=1
        ORION_HAVE_RUBBERBAND=${ORION_HAVE_RUBBERBAND}
)

target_link_libraries(
    Orion
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        signalsmith-stretch
        $<$<BOOL:${ORION_HAVE_RUBBERBAND}>:${ORION_RUBBERBAND_LIBRARY}>
        $<$<BOOL:${ORION_HAVE_RUBBERBAND}>:${ORION_ACCELERATE_FRAMEWORK}>
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

juce_add_console_app(OrionPluginScanner PRODUCT_NAME "Orion Plugin Scanner")

target_sources(
    OrionPluginScanner
    PRIVATE
        Source/Tools/PluginScanner.cpp
)

target_compile_definitions(
    OrionPluginScanner
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_PLUGINHOST_VST3=1
)

target_link_libraries(
    OrionPluginScanner
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

add_dependencies(Orion OrionPluginScanner)
add_custom_command(
    TARGET Orion
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OrionPluginScanner>
            $<TARGET_FILE_DIR:Orion>/OrionPluginScanner
)

juce_add_console_app(OrionWarpCompare PRODUCT_NAME "Orion Warp Compare")

target_sources(
    OrionWarpCompare
    PRIVATE
        Source/Tools/WarpCompare.cpp
)

target_include_directories(
    OrionWarpCompare
    PRIVATE
        Source
        $<$<BOOL:${ORION_HAVE_RUBBERBAND}>:${ORION_RUBBERBAND_INCLUDE_DIR}>
)

target_compile_definitions(
    OrionWarpCompare
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        ORION_HAVE_RUBBERBAND=${ORION_HAVE_RUBBERBAND}
)

target_link_libraries(
    OrionWarpCompare
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_core
        signalsmith-stretch
        $<$<BOOL:${ORION_HAVE_RUBBERBAND}>:${ORION_RUBBERBAND_LIBRARY}>
        $<$<BOOL:${ORION_HAVE_RUBBERBAND}>:${ORION_ACCELERATE_FRAMEWORK}>
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
