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
set(ORION_BUNDLE_ID "com.alexeypavlov.Orion")

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

juce_add_gui_app(Orion
    PRODUCT_NAME "Orion"
    BUNDLE_ID "${ORION_BUNDLE_ID}"
    # Required on macOS so the OS lets the app open the microphone/audio input.
    # Without NSMicrophoneUsageDescription the app is killed when input is opened.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Orion needs access to your microphone/audio input to record audio."
    CAMERA_PERMISSION_ENABLED TRUE
    CAMERA_PERMISSION_TEXT "Orion needs access to your camera for Orion Jam video sessions.")

# Embedded image assets (baked skeuomorphic panels, etc.). Available as BinaryData::<name>.
juce_add_binary_data(OrionAssets SOURCES
    Resources/images/mpc_sample_panel.png)

target_sources(
    Orion
    PRIVATE
        Source/main.cpp
        Source/App/MainWindow.cpp
        Source/App/OrionApplication.cpp
        Source/Analysis/AudioTagger.cpp
        Source/Analysis/SampleEmbedding.cpp
        Source/Analysis/AppleSoundClassifier.mm
        Source/Audio/AudioInputRecorder.cpp
        Source/Audio/AudioRenderPool.cpp
        Source/Audio/ChordDetector.cpp
        Source/Audio/ExportService.cpp
        Source/Audio/IndependentAudioInput.cpp
        Source/Audio/LoudnessMeter.cpp
        Source/Audio/OrionStretchEngine.cpp
        Source/Audio/StemSeparator.cpp
        Source/Audio/TransportController.cpp
        Source/Audio/TransportEngine.cpp
        Source/Audio/VoiceChatEngine.cpp
        Source/Audio/WarpEngine.cpp
        Source/Core/ProjectSerializer.cpp
        Source/Core/ProjectState.cpp
        Source/Collab/OpLog.cpp
        Source/Collab/CollabSession.cpp
        Source/Collab/CollabReconciler.cpp
        Source/Plugins/PluginManager.cpp
        Source/Sampler/SamplerEngine.cpp
        Source/Sampler/SamplerPanelComponent.cpp
        Source/UI/ArrangementTimelineComponent.cpp
        Source/UI/BottomStatusBarComponent.cpp
        Source/UI/BrowserPanelComponent.cpp
        Source/UI/ChordSelectorComponent.cpp
        Source/UI/ClipEditorComponent.cpp
        Source/UI/MainComponent.cpp
        Source/UI/MainComponentMpc.cpp
        Source/UI/MainComponentRecording.cpp
        Source/UI/MainComponentProject.cpp
        Source/UI/MainComponentInstruments.cpp
        Source/UI/MainComponentPreview.cpp
        Source/UI/MainComponentClipGain.cpp
        Source/UI/MainComponentRouting.cpp
        Source/UI/MainComponentMidi.cpp
        Source/UI/MainComponentWiring.cpp
        Source/UI/MainComponentJam.cpp
        Source/UI/MacCharacterPalette.mm
        Source/UI/MidiEditorOverlayComponent.cpp
        Source/UI/AddTrackDialogComponent.cpp
        Source/UI/MixerPanelComponent.cpp
        Source/UI/JamSessionComponent.cpp
        Source/UI/MpcSampleHardwareBridge.cpp
        Source/UI/MpcSampleMapping.cpp
        Source/UI/MpcSamplePanelComponent.cpp
        Source/UI/PluginPickerComponent.cpp
        Source/UI/SelectionInspectorComponent.cpp
        Source/UI/SidebarNavComponent.cpp
        Source/UI/StepSequencerComponent.cpp
        Source/UI/TransportBarComponent.cpp
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
        JUCE_USE_CAMERA=1
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
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_video
        signalsmith-stretch
        OrionAssets
        "-framework SoundAnalysis"
        "-framework CoreMedia"
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

if(APPLE)
    # Bundle the self-contained Chordino chord-detection helper into the app (BEFORE signing, so the
    # --deep codesign pass below also signs the helper binary + its dylibs).
    add_custom_command(
        TARGET Orion
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/Resources/chorddetect
                $<TARGET_BUNDLE_DIR:Orion>/Contents/Resources/chorddetect
        COMMENT "Bundling Chordino chord-detection helper into Orion.app"
        VERBATIM
    )

    # Bundle the self-contained Demucs stem-separation helper + 6-source model (same pre-sign ordering).
    add_custom_command(
        TARGET Orion
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/Resources/stemsplit
                $<TARGET_BUNDLE_DIR:Orion>/Contents/Resources/stemsplit
        COMMENT "Bundling Demucs stem-splitter helper into Orion.app"
        VERBATIM
    )

    # Make the app SELF-CONTAINED so it runs on any Mac (no Homebrew needed): copy every non-system
    # dylib the main binary pulls in (RubberBand + its libsamplerate, etc.) into Contents/Frameworks
    # and rewrite the load paths to @executable_path/../Frameworks. Runs BEFORE the codesign pass so
    # the bundled dylibs get signed too. Needs `dylibbundler` (brew install dylibbundler).
    find_program(DYLIBBUNDLER_EXE dylibbundler)
    if(DYLIBBUNDLER_EXE)
        add_custom_command(
            TARGET Orion
            POST_BUILD
            COMMAND ${DYLIBBUNDLER_EXE} -od -b -cd
                    -x $<TARGET_BUNDLE_DIR:Orion>/Contents/MacOS/Orion
                    -d $<TARGET_BUNDLE_DIR:Orion>/Contents/Frameworks
                    -p @executable_path/../Frameworks
            COMMENT "Bundling non-system dylibs (RubberBand, libsamplerate…) into Orion.app for portability"
            VERBATIM
        )
    else()
        message(WARNING "dylibbundler not found — Orion.app will depend on Homebrew dylibs and won't run on other Macs. Run: brew install dylibbundler")
    endif()

    # Sign with a STABLE code-signing identity (not ad-hoc) so macOS privacy (TCC)
    # remembers folder-access grants across rebuilds instead of re-prompting every time.
    # Defaults to the local Apple Development cert; override with -DORION_CODESIGN_IDENTITY=...
    # ("-" forces ad-hoc).
    set(ORION_CODESIGN_IDENTITY "85A371FB83A1D2DB4B026A765BCE16CA397C69A5"
        CACHE STRING "codesign identity for Orion.app")
    add_custom_command(
        TARGET Orion
        POST_BUILD
        COMMAND /usr/bin/codesign --force --deep --sign "${ORION_CODESIGN_IDENTITY}" --identifier "${ORION_BUNDLE_ID}" "$<TARGET_BUNDLE_DIR:Orion>"
        COMMENT "Signing Orion.app with a stable identity for persistent macOS privacy permissions"
        VERBATIM
    )
endif()

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

juce_add_console_app(OrionChordTest PRODUCT_NAME "Orion Chord Test")
target_sources(OrionChordTest PRIVATE Source/Tools/ChordTest.cpp Source/Audio/ChordDetector.cpp)
target_compile_definitions(OrionChordTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(OrionChordTest
    PRIVATE juce::juce_audio_basics juce::juce_audio_formats juce::juce_core juce::juce_graphics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

juce_add_console_app(OrionSimTest PRODUCT_NAME "Orion Similarity Test")
target_sources(OrionSimTest PRIVATE Source/Tools/SimTest.cpp Source/Analysis/SampleEmbedding.cpp)
target_compile_definitions(OrionSimTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(OrionSimTest
    PRIVATE juce::juce_audio_basics juce::juce_audio_formats juce::juce_core juce::juce_dsp juce::juce_events
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

juce_add_console_app(OrionKeyTest PRODUCT_NAME "Orion Key Test")

target_sources(OrionKeyTest PRIVATE Source/Tools/KeyTest.cpp)

target_compile_definitions(
    OrionKeyTest
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(
    OrionKeyTest
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

juce_add_console_app(OrionRenderPoolTest PRODUCT_NAME "Orion Render Pool Test")
target_sources(OrionRenderPoolTest PRIVATE Source/Tools/RenderPoolTest.cpp Source/Audio/AudioRenderPool.cpp)
target_compile_definitions(OrionRenderPoolTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(OrionRenderPoolTest
    PRIVATE juce::juce_audio_basics juce::juce_core
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

juce_add_console_app(OrionLoudnessTest PRODUCT_NAME "Orion Loudness Test")
target_sources(OrionLoudnessTest PRIVATE Source/Tools/LoudnessTest.cpp Source/Audio/LoudnessMeter.cpp)
target_compile_definitions(OrionLoudnessTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(OrionLoudnessTest
    PRIVATE juce::juce_audio_basics juce::juce_core
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Headless smoke tests for regressions that used to be caught only by ear.
#   cmake --build <dir> --target OrionSmokeTest && ./OrionSmokeTest
juce_add_console_app(OrionSmokeTest PRODUCT_NAME "Orion Smoke Test")
target_sources(OrionSmokeTest PRIVATE
    Source/Tools/SmokeTest.cpp
    Source/Audio/AudioInputRecorder.cpp
    Source/Core/ProjectSerializer.cpp
    Source/Core/ProjectState.cpp
    Source/Sampler/SamplerEngine.cpp)
target_compile_definitions(OrionSmokeTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(OrionSmokeTest
    PRIVATE juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
            juce::juce_core juce::juce_data_structures juce::juce_graphics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Collab op-log tests: two ProjectStates over a loopback hub must converge (Phase 1).
#   cmake --build <dir> --target OrionCollabTest && ./OrionCollabTest
juce_add_console_app(OrionCollabTest PRODUCT_NAME "Orion Collab Test")
target_sources(OrionCollabTest PRIVATE
    Source/Tools/CollabTest.cpp
    Source/Collab/OpLog.cpp
    Source/Collab/CollabSession.cpp
    Source/Collab/CollabReconciler.cpp
    Source/Core/ProjectSerializer.cpp
    Source/Core/ProjectState.cpp)
# JUCE_MODAL_LOOPS_PERMITTED lets the test pump the message loop (runDispatchLoopUntil) so async
# socket callbacks marshalled via callAsync actually get delivered.
target_compile_definitions(OrionCollabTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(OrionCollabTest
    PRIVATE juce::juce_audio_basics juce::juce_core juce::juce_data_structures
            juce::juce_events juce::juce_graphics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
