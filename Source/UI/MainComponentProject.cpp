// Project persistence for MainComponent: save / open / load / export, plus the sidebar
// browser folder list.
//
// Split out of MainComponent.cpp purely to keep that file manageable — this is the SAME
// class, same members, identical logic; only the definitions live here. See MainComponent.h
// for the declarations.
#include "MainComponent.h"

#include "../Audio/PlaybackSources.h"
#include "../Core/ProjectSerializer.h"
#include "OrionTheme.h"

#include <memory>
#include <vector>

namespace orion
{
void MainComponent::confirmAndLoadProject(const juce::File& file)
{
    if (! file.existsAsFile())
        return;

    // Opening a project replaces the current arrangement and its live instruments.
    // Keep this confirmation in one place so Open... and Open Recent behave identically.
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Close Current Project?",
        "Close the current project and open \"" + file.getFileName() + "\"?",
        "Close and Open",
        "Cancel",
        this,
        juce::ModalCallbackFunction::create([this, file](int result)
        {
            if (result != 0)
                loadProjectFromFile(file);
        }));
}

void MainComponent::saveProjectInteractively()
{
    // Pull the latest plugin state into the project before it is serialized.
    captureAllInstrumentStates();

    auto saveToTarget = [this](const juce::File& targetFile)
    {
        juce::String errorMessage;
        if (ProjectSerializer::saveToFile(projectState, targetFile, &errorMessage))
        {
            currentProjectFile = targetFile;
            addRecentProject(targetFile);
            statusLabel.setText("Saved: " + targetFile.getFileName(), juce::dontSendNotification);
        }
        else
        {
            statusLabel.setText("Save failed: " + errorMessage, juce::dontSendNotification);
        }
    };

    if (currentProjectFile.existsAsFile())
    {
        saveToTarget(currentProjectFile);
        return;
    }

    auto defaultDirectory = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto defaultTarget = defaultDirectory.getChildFile("Untitled.orion");
    saveFileChooser = std::make_unique<juce::FileChooser>("Save Orion Project",
                                                          defaultTarget,
                                                          "*.orion");

    auto chooserFlags = juce::FileBrowserComponent::saveMode
                      | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    saveFileChooser->launchAsync(chooserFlags,
                                 [this, saveToTarget](const juce::FileChooser& chooser)
                                 {
                                     auto selectedFile = chooser.getResult();
                                     saveFileChooser.reset();

                                     if (selectedFile == juce::File())
                                     {
                                         statusLabel.setText("Save cancelled", juce::dontSendNotification);
                                         return;
                                     }

                                     if (! selectedFile.hasFileExtension("orion"))
                                         selectedFile = selectedFile.withFileExtension(".orion");

                                     saveToTarget(selectedFile);
                                 });
}

void MainComponent::newProjectInteractively()
{
    // This discards unsaved work, so always ask first.
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "New Project",
        "Start a new project? Any unsaved changes in the current project will be lost.",
        "New Project",
        "Cancel",
        this,
        juce::ModalCallbackFunction::create([this](int result)
        {
            if (result == 0)
                return;   // cancelled

            // Reset by round-tripping a fresh, empty ProjectState through the serializer and
            // loading it the normal way. ProjectState owns a CriticalSection so it can't just be
            // assigned over, and hand-clearing its ~48 members would eventually leak stale data
            // into the "new" project. This reuses the tested load path (and its UI refresh).
            const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("orion-new-project.orion");
            juce::String error;
            if (! ProjectSerializer::saveToFile(ProjectState{}, temp, &error))
            {
                statusLabel.setText("New project failed: " + error, juce::dontSendNotification);
                return;
            }

            loadProjectFromFile(temp);
            temp.deleteFile();

            currentProjectFile = juce::File();   // Save must now ask where to put it
            statusLabel.setText("New project", juce::dontSendNotification);
            updateTransportLabels();
        }));
}

void MainComponent::openProjectInteractively()
{
    auto defaultDirectory = currentProjectFile.existsAsFile()
                                ? currentProjectFile.getParentDirectory()
                                : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

    // Accept the new ".orion" extension and the legacy ".orion.json" so older
    // projects still open.
    openFileChooser = std::make_unique<juce::FileChooser>("Open Orion Project",
                                                          defaultDirectory,
                                                          "*.orion;*.orion.json");

    auto chooserFlags = juce::FileBrowserComponent::openMode
                      | juce::FileBrowserComponent::canSelectFiles;

    openFileChooser->launchAsync(chooserFlags,
                                 [this](const juce::FileChooser& chooser)
                                 {
                                     auto selectedFile = chooser.getResult();
                                     openFileChooser.reset();

                                     if (selectedFile == juce::File())
                                     {
                                         statusLabel.setText("Open cancelled", juce::dontSendNotification);
                                         return;
                                     }

                                     confirmAndLoadProject(selectedFile);
                                 });
}

void MainComponent::loadProjectFromFile(const juce::File& file)
{
    // Make sure nothing is playing/recording while we swap the project out from
    // under the audio engine.
    stopTransportFromUi();

    juce::String errorMessage;
    if (! ProjectSerializer::loadFromFile(projectState, file, &errorMessage))
    {
        statusLabel.setText("Open failed: " + errorMessage, juce::dontSendNotification);
        return;
    }

    currentProjectFile = file;
    addRecentProject(file);

    // Older builds created sampler/MIDI tracks with this fixed grey, which made every MIDI
    // clip on those tracks grey too. Restore palette colours on load without touching any
    // user-chosen colours.
    auto& loadedTracks = projectState.getTracks();
    constexpr auto legacySamplerGrey = juce::uint32 { 0xff9db0c4 };
    for (int i = 0; i < static_cast<int>(loadedTracks.size()); ++i)
    {
        auto& track = loadedTracks[static_cast<std::size_t>(i)];
        if (track.isMidiTrack
            && track.samplerSourcePath.isNotEmpty()
            && track.colour == juce::Colour(legacySamplerGrey))
        {
            const auto paletteColour = theme::tracks::colourForIndex(i);
            track.colour = paletteColour;
            for (auto& clip : track.clips)
                if (clip.colour == juce::Colour(legacySamplerGrey))
                    clip.colour = paletteColour.brighter(0.1f);
        }
    }

    // Re-instantiate hosted VST instruments from the loaded track state, and drop
    // any selection/history that referred to the previous project.
    restoreInstrumentsFromProject();
    restoreInsertsFromProject();

    // Rebuild the MPC panel (pad waveforms + live playback) from the loaded kit track, so the
    // panel matches what the arrangement will play back.
    for (const auto& t : projectState.getTracks())
        if (t.isMpcKit)
        {
            for (int pad = 0; pad < 16; ++pad)
                if (t.mpcKitSamples[static_cast<std::size_t>(pad)].isNotEmpty())
                    mpcSamplePanel.loadSampleOntoPad(pad, juce::File(t.mpcKitSamples[static_cast<std::size_t>(pad)]));
            mpc.sixteenLevels = t.isMpcTuneMode;   // restore Tune toggle for the panel indicator
            mpc.tuneSourcePath = t.isMpcTuneMode ? t.mpcTuneSample : juce::String();
            mpc.tuneRootNote = t.mpcTuneRoot;
            mpc.chopMode = t.isMpcChopMode;
            mpc.chopSourcePath = t.isMpcChopMode ? t.mpcChopSample : juce::String();
            if (mpc.chopMode)
                mpc.sixteenLevels = false;
            updateMpcPerformanceState();
            break;
        }

    arrangementTimeline.resetForNewProject();
    selectedArrangementClip.reset();

    // Recompute derived audio-clip lengths, refresh transport + inspector UI.
    refreshAudioClipWarpLengths();
    refreshClipInspector();
    refreshClipEditor();
    loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
    updateTransportLabels();

    if (arrangementPlaybackSource != nullptr)
    {
        arrangementPlaybackSource->syncToTransportPosition();

        // Warm the warp engine right after loading so the FIRST Play is instant — previously
        // warp streams / sampler buffers were only prepared on the first press, so opening a
        // project and hitting play had an audible delay before sound.
        if (arrangementPlaybackSource->isRealtimeWarpEnabled())
            arrangementPlaybackSource->prepareWarpStreams();
        else
            arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();

        for (const auto& track : projectState.getTracks())
            if (track.isMidiTrack && track.samplerSourcePath.isNotEmpty() && track.samplerWarpEnabled)
                arrangementPlaybackSource->prewarmSamplerWarp(track.samplerSourcePath, track.samplerSourceBpm);
    }

    rewindTransportFromUi();
    resetToPlaylistView();
    statusLabel.setText("Opened: " + file.getFileName(), juce::dontSendNotification);
}

void MainComponent::exportProjectInteractively()
{
    if (arrangementPlaybackSource == nullptr)
    {
        statusLabel.setText("Export failed: playback source unavailable", juce::dontSendNotification);
        return;
    }

    auto defaultDirectory = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto defaultTarget = defaultDirectory.getChildFile("Orion Export.wav");
    exportFileChooser = std::make_unique<juce::FileChooser>("Export Orion Mix",
                                                            defaultTarget,
                                                            "*.wav");

    auto chooserFlags = juce::FileBrowserComponent::saveMode
                      | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    exportFileChooser->launchAsync(chooserFlags,
                                   [this](const juce::FileChooser& chooser)
                                   {
                                       auto selectedFile = chooser.getResult();
                                       exportFileChooser.reset();

                                       if (selectedFile == juce::File())
                                       {
                                           statusLabel.setText("Export cancelled", juce::dontSendNotification);
                                           return;
                                       }

                                       if (! selectedFile.hasFileExtension("wav"))
                                           selectedFile = selectedFile.withFileExtension(".wav");

                                       juce::String errorMessage;
                                       const auto exported = ExportService::exportToWav(
                                           projectState,
                                           transportEngine.isLoopEnabled(),
                                           exportSampleRate,
                                           selectedFile,
                                           [this]()
                                           {
                                               if (arrangementPlaybackSource != nullptr)
                                                   arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
                                           },
                                           [this](juce::AudioBuffer<float>& buffer,
                                                  int startSample,
                                                  int numSamples,
                                                  double blockStartBeat,
                                                  double renderSampleRate)
                                           {
                                               if (arrangementPlaybackSource != nullptr)
                                                   arrangementPlaybackSource->renderOfflineBlock(buffer,
                                                                                                 startSample,
                                                                                                 numSamples,
                                                                                                 blockStartBeat,
                                                                                                 renderSampleRate);
                                           },
                                           &errorMessage);

                                       statusLabel.setText(exported
                                                               ? "Exported: " + selectedFile.getFileName()
                                                               : "Export failed: " + errorMessage,
                                                           juce::dontSendNotification);
                                   });
}
} // namespace orion
