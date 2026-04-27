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

juce_add_gui_app(Orion PRODUCT_NAME "Orion")

target_sources(
    Orion
    PRIVATE
        Source/main.cpp
        Source/App/MainWindow.cpp
        Source/App/OrionApplication.cpp
        Source/UI/MainComponent.cpp
)

target_include_directories(
    Orion
    PRIVATE
        Source
)

target_compile_definitions(
    Orion
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
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
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
