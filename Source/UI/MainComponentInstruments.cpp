// Hosted VST instruments for MainComponent: loading/removing them on a track, their editor
// windows, and capture/restore of their state with the project.
//
// Split out of MainComponent.cpp purely to keep that file manageable — this is the SAME
// class, same members, identical logic; only the definitions live here. See MainComponent.h
// for the declarations.
#include "MainComponent.h"

#include "../Audio/PlaybackSources.h"
#include "OrionTheme.h"

#include <memory>
#include <vector>

namespace orion
{
void MainComponent::loadInstrumentOnTrack(int trackIndex, const juce::PluginDescription& description)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()) || arrangementPlaybackSource == nullptr)
        return;

    if (! description.isInstrument)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "This plugin is not an instrument",
                                               description.name + " is a VST3 effect. Effects need an insert chain; "
                                                                  "MIDI tracks can only load instruments here.");
        return;
    }

    juce::String error;
    auto instance = pluginManager.createInstance(description,
                                                getCurrentPluginSampleRate(),
                                                getCurrentPluginBlockSize(),
                                                error);
    if (instance == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Could not load plugin",
                                               error.isNotEmpty() ? error : juce::String("Unknown error."));
        return;
    }

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    track.instrumentPluginId    = description.createIdentifierString();
    track.instrumentPluginName  = description.name;
    track.instrumentStateBase64 = {};

    closeInstrumentEditor(trackIndex);
    arrangementPlaybackSource->setTrackInstrument(trackIndex, std::move(instance));

    stepSequencer.setVisible(false);   // only one lower panel at a time
    samplerPanel.openTrackIndex(trackIndex);
    samplerPanel.setVisible(false);
    statusLabel.setText("Instrument loaded: " + description.name, juce::dontSendNotification);
    arrangementTimeline.repaint();
    openInstrumentEditor(trackIndex);
}

void MainComponent::removeInstrumentFromTrack(int trackIndex)
{
    closeInstrumentEditor(trackIndex);
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->clearTrackInstrument(trackIndex);

    auto& tracks = projectState.getTracks();
    if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
    {
        auto& track = tracks[static_cast<std::size_t>(trackIndex)];
        track.instrumentPluginId    = {};
        track.instrumentPluginName  = {};
        track.instrumentStateBase64 = {};
    }
    arrangementTimeline.repaint();
}

void MainComponent::restoreInsertsFromProject()
{
    if (arrangementPlaybackSource == nullptr)
        return;
    insertEditorWindows.clear();   // old windows reference instances we're about to replace
    auto& tracks = projectState.getTracks();
    for (int t = 0; t < static_cast<int>(tracks.size()); ++t)
    {
        arrangementPlaybackSource->clearTrackInserts(t);
        for (auto& fx : tracks[static_cast<std::size_t>(t)].inserts)
        {
            const auto desc = pluginManager.findDescription(fx.pluginId);
            if (! desc.has_value())
                continue;
            juce::String error;
            auto inst = pluginManager.createInstance(*desc, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
            if (inst != nullptr)
                arrangementPlaybackSource->addInsert(t, std::move(inst), fx.bypassed);
        }
    }

    // Bus insert chains (stored under the bus key offset).
    auto& buses = projectState.getBuses();
    for (int b = 0; b < static_cast<int>(buses.size()); ++b)
    {
        const auto key = ArrangementPlaybackSource::busInsertKey(b);
        arrangementPlaybackSource->clearTrackInserts(key);
        for (auto& fx : buses[static_cast<std::size_t>(b)].inserts)
        {
            const auto desc = pluginManager.findDescription(fx.pluginId);
            if (! desc.has_value())
                continue;
            juce::String error;
            auto inst = pluginManager.createInstance(*desc, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
            if (inst != nullptr)
                arrangementPlaybackSource->addInsert(key, std::move(inst), fx.bypassed);
        }
    }

    // Master insert chain.
    arrangementPlaybackSource->clearTrackInserts(ArrangementPlaybackSource::kMasterInsertKey);
    for (auto& fx : projectState.getMasterInserts())
    {
        const auto desc = pluginManager.findDescription(fx.pluginId);
        if (! desc.has_value())
            continue;
        juce::String error;
        auto inst = pluginManager.createInstance(*desc, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
        if (inst != nullptr)
            arrangementPlaybackSource->addInsert(ArrangementPlaybackSource::kMasterInsertKey, std::move(inst), fx.bypassed);
    }
}

void MainComponent::openInstrumentEditor(int trackIndex)
{
    if (arrangementPlaybackSource == nullptr)
        return;

    auto* instance = arrangementPlaybackSource->getTrackInstrument(trackIndex);
    if (instance == nullptr)
        return;

    if (auto it = instrumentEditorWindows.find(trackIndex);
        it != instrumentEditorWindows.end() && it->second != nullptr)
    {
        it->second->toFront(true);
        return;
    }

    auto& tracks = projectState.getTracks();
    juce::String title = "Instrument";
    if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
    {
        const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
        title = track.name + " - " + track.instrumentPluginName;
    }

    instrumentEditorWindows[trackIndex] = std::make_unique<PluginEditorWindow>(
        *instance,
        title,
        [this, trackIndex]() { closeInstrumentEditor(trackIndex); },
        [this](const juce::KeyPress& key) { return keyPressed(key); },
        [this](bool isKeyDown) { return keyStateChanged(isKeyDown); });
}

void MainComponent::closeInstrumentEditor(int trackIndex)
{
    instrumentEditorWindows.erase(trackIndex);
}

void MainComponent::closeAllInstrumentEditors()
{
    instrumentEditorWindows.clear();
}

void MainComponent::captureAllInstrumentStates()
{
    if (arrangementPlaybackSource == nullptr)
        return;

    auto& tracks = projectState.getTracks();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        auto* instance = arrangementPlaybackSource->getTrackInstrument(i);
        if (instance == nullptr)
            continue;

        juce::MemoryBlock block;
        instance->getStateInformation(block);
        tracks[static_cast<std::size_t>(i)].instrumentStateBase64 = block.toBase64Encoding();
    }
}

void MainComponent::restoreInstrumentsFromProject()
{
    if (arrangementPlaybackSource == nullptr)
        return;

    closeAllInstrumentEditors();
    auto& tracks = projectState.getTracks();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        arrangementPlaybackSource->clearTrackInstrument(i);

        auto& track = tracks[static_cast<std::size_t>(i)];
        if (track.instrumentPluginId.isEmpty())
            continue;

        const auto desc = pluginManager.findDescription(track.instrumentPluginId);
        if (! desc.has_value())
            continue;  // plugin not installed/scanned right now — keep id for a later rescan
        if (! desc->isInstrument)
            continue;  // old projects may contain an effect id in the instrument slot

        juce::String error;
        auto instance = pluginManager.createInstance(*desc,
                                                    getCurrentPluginSampleRate(),
                                                    getCurrentPluginBlockSize(),
                                                    error);
        if (instance == nullptr)
            continue;

        if (track.instrumentStateBase64.isNotEmpty())
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding(track.instrumentStateBase64) && block.getSize() > 0)
                instance->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        }

        arrangementPlaybackSource->setTrackInstrument(i, std::move(instance));
    }
    arrangementTimeline.repaint();
}

void MainComponent::resyncInstrumentsAfterHistory()
{
    if (arrangementPlaybackSource == nullptr)
        return;

    // Editor windows are keyed by the old track index — they'd point at the wrong slot now.
    closeAllInstrumentEditors();

    auto& tracks = projectState.getTracks();

    // Re-home every live instrument onto the restored layout by plugin id, in one atomic step.
    // Reused instances keep their exact state and need no reinstantiation (instant).
    std::vector<std::pair<int, juce::String>> wanted;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<std::size_t>(i)].instrumentPluginId.isNotEmpty())
            wanted.push_back({ i, tracks[static_cast<std::size_t>(i)].instrumentPluginId });

    const auto unsatisfied = arrangementPlaybackSource->rehomeInstrumentsFromStash(wanted);

    // Rare fallback: a wanted plugin had no live instance available (stash overflowed) —
    // reinstantiate just those from their saved state.
    for (const auto& [i, id] : unsatisfied)
    {
        const auto desc = pluginManager.findDescription(id);
        if (! desc.has_value() || ! desc->isInstrument)
            continue;

        juce::String error;
        auto instance = pluginManager.createInstance(*desc,
                                                     getCurrentPluginSampleRate(),
                                                     getCurrentPluginBlockSize(),
                                                     error);
        if (instance == nullptr)
            continue;

        const auto& base64 = tracks[static_cast<std::size_t>(i)].instrumentStateBase64;
        if (base64.isNotEmpty())
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding(base64) && block.getSize() > 0)
                instance->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        }

        arrangementPlaybackSource->setTrackInstrument(i, std::move(instance));
    }

    arrangementPlaybackSource->trimInstrumentStash(ArrangementPlaybackSource::kInstrumentStashLimit);
    arrangementTimeline.repaint();
}
} // namespace orion
