// Callback wiring for MainComponent, lifted out of the constructor.
//
// The constructor had grown to ~1700 lines of panel wiring, which is where concurrent edits
// collided most. These are straight cuts of what used to be inline, called from the constructor
// in the same order — same class, same members, identical logic.
//
// NB: the blocks that follow are deliberately bounded by the constructor's local lambdas
// (loadSamplerChannel, applyWarpMarkerChange, the track-level getters). Only regions that use
// none of them were moved, so nothing had to change to keep compiling.
#include "MainComponent.h"

#include "../Audio/PlaybackSources.h"
#include "../Sampler/SamplerEngine.h"
#include "OrionTheme.h"
#include "MainComponentInternal.h"

#include <memory>
#include <vector>

namespace orion
{
void MainComponent::buildLabelsAndInspector()
{
    headerLabel.setText("ORION", juce::dontSendNotification);
    headerLabel.setFont(juce::FontOptions(27.0f, juce::Font::bold));
    headerLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.96f));
    addAndMakeVisible(headerLabel);
    headerLabel.setVisible(false);

    bpmCaptionLabel.setText("TEMPO", juce::dontSendNotification);
    bpmCaptionLabel.setColour(juce::Label::textColourId, mutedText);
    bpmCaptionLabel.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    addAndMakeVisible(bpmCaptionLabel);

    bpmValueLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    bpmValueLabel.setFont(juce::FontOptions(26.0f, juce::Font::bold));
    bpmValueLabel.setJustificationType(juce::Justification::centred);
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmValueLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(bpmValueLabel);
    bpmValueLabel.setVisible(false);

    bpmEditor.setColour(juce::TextEditor::backgroundColourId, transportDarkPanel);
    bpmEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    bpmEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    bpmEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    bpmEditor.setColour(juce::TextEditor::highlightColourId, juce::Colours::white.withAlpha(0.18f));
    bpmEditor.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
    bpmEditor.setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
    bpmEditor.setJustification(juce::Justification::centred);
    // Match the transport readout's value font so the number doesn't change size
    // when you click to edit it.
    // Match the painted tempo value exactly (Avenir Next 21 bold, centred) so double-clicking to
    // edit doesn't shift or resize the number — see TransportBarComponent::drawReadout.
    const juce::Font bpmEditorFont(juce::FontOptions("Avenir Next", 21.0f, juce::Font::bold));
    bpmEditor.setFont(bpmEditorFont);
    bpmEditor.applyFontToAllText(bpmEditorFont);
    bpmEditor.setJustification(juce::Justification::centred);
    bpmEditor.setBorder(juce::BorderSize<int>(0));
    bpmEditor.setIndents(0, 0);
    bpmEditor.setInputRestrictions(6, "0123456789.");
    bpmEditor.setAlwaysOnTop(true);
    bpmEditor.onReturnKey = [this]() { endTempoEditing(true); };
    bpmEditor.onEscapeKey = [this]() { endTempoEditing(false); };
    bpmEditor.onFocusLost = [this]() { endTempoEditing(true); };
    // addChildComponent keeps the editor hidden by default so paint()'s g.drawText
    // renders the BPM number at startup. addAndMakeVisible would auto-show it empty.
    addChildComponent(bpmEditor);

    meterCaptionLabel.setText("BPM", juce::dontSendNotification);
    meterCaptionLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.68f));
    meterCaptionLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    addAndMakeVisible(meterCaptionLabel);

    meterValueLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.76f));
    meterValueLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    meterValueLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(meterValueLabel);

    statusLabel.setText("DAW", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.74f));
    statusLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(statusLabel);
    statusLabel.setVisible(false);

    pluginScanNameLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.72f));
    pluginScanNameLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    pluginScanNameLabel.setJustificationType(juce::Justification::centred);
    pluginScanNameLabel.setInterceptsMouseClicks(false, false);
    pluginScanNameLabel.setVisible(false);
    addAndMakeVisible(pluginScanNameLabel);

    tempoLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(tempoLabel);

    meterLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(meterLabel);

    playheadLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(playheadLabel);

    playlistLabel.setText("Playlist: arrangement-first timeline for clips and loops", juce::dontSendNotification);
    playlistLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(playlistLabel);

    pianoRollLabel.setText("Piano Roll: double-click a MIDI clip to open full focus editor", juce::dontSendNotification);
    pianoRollLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(pianoRollLabel);

    clipInspectorEmptyLabel.setText("Select audio clip", juce::dontSendNotification);
    clipInspectorEmptyLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.58f));
    clipInspectorEmptyLabel.setJustificationType(juce::Justification::centred);
    clipInspectorEmptyLabel.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorEmptyLabel);

    clipInspectorTitleLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.96f));
    clipInspectorTitleLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    addAndMakeVisible(clipInspectorTitleLabel);

    clipInspectorTrackLabel.setColour(juce::Label::textColourId, mutedText);
    clipInspectorTrackLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorTrackLabel);

    clipInspectorFileLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.82f));
    clipInspectorFileLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorFileLabel);

    clipWarpLabel.setText("Warp", juce::dontSendNotification);
    clipWarpLabel.setColour(juce::Label::textColourId, mutedText);
    addAndMakeVisible(clipWarpLabel);

    clipWarpInfoLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
    clipWarpInfoLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipWarpInfoLabel);

    clipSourceBpmLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    clipSourceBpmLabel.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    addAndMakeVisible(clipSourceBpmLabel);

    clipBarsLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    clipBarsLabel.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    addAndMakeVisible(clipBarsLabel);

    clipWarpToggle.setButtonText("");
    clipWarpToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipWarpToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            if ((clip->sourceDurationSeconds <= 0.0 || (clip->sourceBpm <= 0.0 && clip->detectedBars == 0)) && clip->sourcePath.isNotEmpty())
            {
                const auto analysis = analyzeAudioWarpMetadata(juce::File(clip->sourcePath), projectState.getTempoBpm(), projectState.getNumerator());
                if (clip->sourceDurationSeconds <= 0.0)
                    clip->sourceDurationSeconds = analysis.durationSeconds;
                if (clip->sourceBpm <= 0.0 && analysis.sourceBpm > 0.0)
                {
                    clip->sourceBpm = analysis.sourceBpm;
                    clip->bpmGuessed = analysis.bpmGuessed;
                }
                if (clip->detectedBars == 0 && analysis.detectedBars > 0)
                    clip->detectedBars = analysis.detectedBars;
                if (clip->sourceKeyRoot < 0 && analysis.sourceKeyRoot >= 0)
                {
                    clip->sourceKeyRoot    = analysis.sourceKeyRoot;
                    clip->sourceKeyIsMinor = analysis.sourceKeyIsMinor;
                }
            }

            clip->warpEnabled = clipWarpToggle.getToggleState();
            refreshAudioClipWarpLengths();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipWarpToggle);

    clipGainLabel.setText("Gain", juce::dontSendNotification);
    clipGainLabel.setColour(juce::Label::textColourId, mutedText);
    addAndMakeVisible(clipGainLabel);

    clipGainValueLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipGainValueLabel.setJustificationType(juce::Justification::centredRight);
    clipGainValueLabel.setEditable(false, true, false);
    clipGainValueLabel.onTextChange = [this]() { applyGainFromInspectorText(); };
    addAndMakeVisible(clipGainValueLabel);

    clipGainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    clipGainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    clipGainSlider.setRange(-24.0, 12.0, 0.1);
    clipGainSlider.setColour(juce::Slider::trackColourId, accentColour);
    clipGainSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    clipGainSlider.onDragStart = [this] { arrangementTimeline.captureUndoSnapshot(); };  // one undo step per drag
    clipGainSlider.onValueChange = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->gainDb = clipGainSlider.getValue();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipGainSlider);

    clipMuteToggle.setButtonText("M");
    clipMuteToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipMuteToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            arrangementTimeline.captureUndoSnapshot();
            clip->muted = clipMuteToggle.getToggleState();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipMuteToggle);

    clipSoloToggle.setButtonText("S");
    clipSoloToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipSoloToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            arrangementTimeline.captureUndoSnapshot();
            clip->solo = clipSoloToggle.getToggleState();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipSoloToggle);

    selectionInspector.onGainDragStart = [this] { arrangementTimeline.captureUndoSnapshot(); };
    selectionInspector.onGainChanged = [this](double gainDb)
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->gainDb = gainDb;
            clipGainSlider.setValue(gainDb, juce::dontSendNotification);
            clipGainValueLabel.setText(juce::String(gainDb, 1) + " dB", juce::dontSendNotification);
            arrangementTimeline.repaint();
            return;
        }

        if (const auto trackIndex = arrangementTimeline.getSelectedTrackIndex(); trackIndex.has_value())
        {
            auto& tracks = projectState.getTracks();
            if (*trackIndex >= 0 && *trackIndex < static_cast<int>(tracks.size()))
            {
                tracks[static_cast<std::size_t>(*trackIndex)].volumeDb = gainDb;
                arrangementTimeline.repaint();
            }
        }
    };

    selectionInspector.onMuteChanged = [this](bool shouldMute)
    {
        arrangementTimeline.captureUndoSnapshot();
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->muted = shouldMute;
            clipMuteToggle.setToggleState(shouldMute, juce::dontSendNotification);
            arrangementTimeline.repaint();
            return;
        }

        if (const auto trackIndex = arrangementTimeline.getSelectedTrackIndex(); trackIndex.has_value())
        {
            auto& tracks = projectState.getTracks();
            if (*trackIndex >= 0 && *trackIndex < static_cast<int>(tracks.size()))
            {
                tracks[static_cast<std::size_t>(*trackIndex)].muted = shouldMute;
                arrangementTimeline.repaint();
            }
        }
    };

    selectionInspector.onSoloChanged = [this](bool shouldSolo)
    {
        arrangementTimeline.captureUndoSnapshot();
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->solo = shouldSolo;
            clipSoloToggle.setToggleState(shouldSolo, juce::dontSendNotification);
            arrangementTimeline.repaint();
            return;
        }

        if (const auto trackIndex = arrangementTimeline.getSelectedTrackIndex(); trackIndex.has_value())
        {
            auto& tracks = projectState.getTracks();
            if (*trackIndex >= 0 && *trackIndex < static_cast<int>(tracks.size()))
            {
                tracks[static_cast<std::size_t>(*trackIndex)].solo = shouldSolo;
                arrangementTimeline.repaint();
            }
        }
    };

    selectionInspector.onWarpChanged = [this](bool shouldWarp)
    {
        arrangementTimeline.captureUndoSnapshot();
        if (auto* clip = getSelectedTimelineClip())
        {
            if ((clip->sourceDurationSeconds <= 0.0 || (clip->sourceBpm <= 0.0 && clip->detectedBars == 0)) && clip->sourcePath.isNotEmpty())
            {
                const auto analysis = analyzeAudioWarpMetadata(juce::File(clip->sourcePath), projectState.getTempoBpm(), projectState.getNumerator());
                if (clip->sourceDurationSeconds <= 0.0)
                    clip->sourceDurationSeconds = analysis.durationSeconds;
                if (clip->sourceBpm <= 0.0 && analysis.sourceBpm > 0.0)
                {
                    clip->sourceBpm = analysis.sourceBpm;
                    clip->bpmGuessed = analysis.bpmGuessed;
                }
                if (clip->detectedBars == 0 && analysis.detectedBars > 0)
                    clip->detectedBars = analysis.detectedBars;
                if (clip->sourceKeyRoot < 0 && analysis.sourceKeyRoot >= 0)
                {
                    clip->sourceKeyRoot    = analysis.sourceKeyRoot;
                    clip->sourceKeyIsMinor = analysis.sourceKeyIsMinor;
                }
            }

            clip->warpEnabled = shouldWarp;
            clipWarpToggle.setToggleState(shouldWarp, juce::dontSendNotification);
            refreshAudioClipWarpLengths();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    selectionInspector.onRequestLiveLevel = [this]() -> float
    {
        int trackIndex = -1;
        if (selectedArrangementClip.has_value())
            trackIndex = selectedArrangementClip->first;
        else if (const auto selectedTrack = arrangementTimeline.getSelectedTrackIndex(); selectedTrack.has_value())
            trackIndex = *selectedTrack;

        return (trackIndex >= 0 && trackIndex < static_cast<int>(meters.trackPeakHoldDb.size()))
            ? juce::jlimit(0.0f, 1.0f, juce::Decibels::decibelsToGain(meters.trackPeakHoldDb[static_cast<std::size_t>(trackIndex)]))
            : 0.0f;
    };
    selectionInspector.onRequestLiveLevelDb = [this]() -> float
    {
        int trackIndex = -1;
        if (selectedArrangementClip.has_value())
            trackIndex = selectedArrangementClip->first;
        else if (const auto selectedTrack = arrangementTimeline.getSelectedTrackIndex(); selectedTrack.has_value())
            trackIndex = *selectedTrack;

        return (trackIndex >= 0 && trackIndex < static_cast<int>(meters.trackPeakHoldDb.size()))
            ? meters.trackPeakHoldDb[static_cast<std::size_t>(trackIndex)]
            : -100.0f;
    };
    addAndMakeVisible(selectionInspector);
    selectionInspector.setVisible(false);
}

void MainComponent::wireBrowserAndDialogs()
{
    browserPanel.onPreviewItem = [this](const BrowserItem& item)
    {
        if (item.isDirectory)
        {
            statusLabel.setText("Folder: " + item.file.getFullPathName(), juce::dontSendNotification);
            return;
        }

        playBrowserPreview(item);
    };
    browserPanel.onActivateItem = [this](const BrowserItem& item)
    {
        // Double-click a sound → create a sampler track and open its UI only. Do NOT drop an audio
        // clip into the playlist (that's what dragging the sample onto a lane is for).
        loadBrowserItemIntoSampler(item, /*addClipToPlaylist*/ false);
    };
    browserPanel.onOpenInSampler = [this](const BrowserItem& item)
    {
        loadBrowserItemIntoSampler(item, /*addClipToPlaylist*/ false);   // sampler track only
    };
    browserPanel.onAddItemToPlaylist = [this](const BrowserItem& item)
    {
        // Enter in the browser: add the sound to the playlist as a sampler track + clip.
        loadBrowserItemIntoSampler(item, /*addClipToPlaylist*/ true);
    };
    browserPanel.onReplaceSelectedTrackSample = [this](const BrowserItem& item)
    {
        if (item.isDirectory || ! item.file.existsAsFile())
            return;

        // If audio clips are selected in the playlist, replace just those clips' sample.
        if (const int replaced = arrangementTimeline.replaceSelectedAudioClipsSource(item.file); replaced > 0)
        {
            statusLabel.setText("Replaced sample in " + juce::String(replaced)
                                + (replaced == 1 ? " clip" : " clips"), juce::dontSendNotification);
            return;
        }

        auto& tracks = projectState.getTracks();
        // Prefer the channel selected in the step sequencer (when it's open); otherwise the
        // track selected in the playlist.
        int target = -1;
        if (stepSequencer.isVisible() && stepSequencer.selectedChannelTrackIndex() >= 0)
            target = stepSequencer.selectedChannelTrackIndex();
        else if (const auto sel = arrangementTimeline.getSelectedTrackIndex(); sel.has_value())
            target = *sel;

        if (target >= 0 && target < static_cast<int>(tracks.size())
            && tracks[static_cast<std::size_t>(target)].isMidiTrack)
        {
            // Replace the selected sampler/instrument track's sound in place.
            auto& track = tracks[static_cast<std::size_t>(target)];
            const auto analysis = analyzeAudioWarpMetadata(item.file, projectState.getTempoBpm(), projectState.getNumerator());
            track.samplerSourcePath = item.file.getFullPathName();
            track.samplerSourceDurationSeconds = analysis.durationSeconds;
            track.samplerSourceBpm = analysis.sourceBpm;
            track.samplerDetectedBars = analysis.detectedBars;
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->prewarmSamplerWarp(track.samplerSourcePath, track.samplerSourceBpm);
            statusLabel.setText("Replaced sample: " + track.name, juce::dontSendNotification);
            stepSequencer.repaint();
            arrangementTimeline.repaint();
        }
        else
        {
            loadBrowserItemIntoSampler(item);   // no sampler track selected → load as a new one
        }
    };
    browserPanel.onCloseRequested = [this]
    {
        // Wired to the × button inside the browser header — collapses the panel and
        // lets the playlist expand into the freed horizontal space.
        browserPanelVisible = false;       // timer slides it closed smoothly
        startTimerHz(60);
        resized();
        repaint();
    };
    browserPanel.onTogglePreviewPlayback = [this]
    {
        if (previewTransportSource.isPlaying() || pendingBrowserPreviewStart)
        {
            pendingBrowserPreviewStart = false;
            browserPanel.setPreviewArmed(false);
            previewTransportSource.stop();
        }
        else if (previewBufferSource != nullptr)
        {
            armOrStartBrowserPreview();
        }
    };
    browserPanel.onSeekPreview = [this](float ratio)
    {
        if (previewBufferSource == nullptr)
            return;

        pendingBrowserPreviewStart = false;
        browserPanel.setPreviewArmed(false);
        const auto length = previewTransportSource.getLengthInSeconds();
        if (length > 0.0)
        {
            previewTransportSource.setPosition(static_cast<double>(juce::jlimit(0.0f, 1.0f, ratio)) * length);
            browserPanel.setPreviewPlayback(previewTransportSource.isPlaying(), ratio);
        }
    };
    browserPanel.onDragStarted = [this]
    {
        // Silence the browser preview as soon as the sample is dragged toward the playlist.
        stopBrowserPreview(true);
    };
    browserPanel.onPreviewBpmSyncToggled = [this]
    {
        // Reload preview with the new sync mode if a sample is already loaded.
        auto selected = browserPanel.getSelectedItem();
        if (selected.has_value() && previewBufferSource != nullptr)
        {
            currentPreviewTempoBpm = 0.0;  // invalidate cache
            playBrowserPreview(*selected);
        }
    };
    browserPanel.onRootFolderChosen = [this](const juce::File& folder)
    {
        if (! folder.isDirectory())
            return;

        const auto folderPath = folder.getFullPathName();
        const auto alreadyAdded = std::any_of(sidebarBrowserFolders.begin(), sidebarBrowserFolders.end(),
                                              [&folderPath](const juce::File& existing)
                                              {
                                                  return existing.getFullPathName() == folderPath;
                                              });
        if (! alreadyAdded)
        {
            sidebarBrowserFolders.push_back(folder);
            sidebarNav.setCustomFolders(sidebarBrowserFolders);
            browserPanel.setLibraryRoots(sidebarBrowserFolders);
            saveSidebarBrowserFolders();
        }

        browserPanelVisible = true;
        browserButton.setToggleState(true, juce::dontSendNotification);
        browserPanel.setVisible(true);
        browserPanel.openFolder(folder);
        sidebarNav.setActiveFolder(folder);
        startTimerHz(60);
        resized();
        repaint();
    };
    sidebarNav.onFolderSelected = [this](const juce::File& folder)
    {
        if (! folder.isDirectory())
            return;

        browserPanelVisible = true;
        browserButton.setToggleState(true, juce::dontSendNotification);
        browserPanel.setVisible(true);
        browserPanel.openFolder(folder);
        startTimerHz(60);
        resized();
        repaint();
    };
    sidebarNav.onItemSelected = [this](SidebarNavItem item)
    {
        if (item == SidebarNavItem::files || item == SidebarNavItem::samples)
        {
            const auto shouldCollapse = browserPanelVisible && browserPanel.isShowingRootLocations();
            browserPanelVisible = ! shouldCollapse;
            browserButton.setToggleState(browserPanelVisible, juce::dontSendNotification);
            if (browserPanelVisible)
            {
                browserPanel.setVisible(true);
                browserPanel.showRootLocations();
            }
            startTimerHz(60);
            resized();
            repaint();
            return;
        }

        if (item == SidebarNavItem::addFolder)
        {
            browserPanelVisible = true;
            browserButton.setToggleState(true, juce::dontSendNotification);
            browserPanel.setVisible(true);
            startTimerHz(60);
            resized();
            repaint();
            browserPanel.chooseRootFolder();
            return;
        }

        if (item == SidebarNavItem::vst)
        {
            // Open the instrument picker for the selected MIDI track. Do NOT create a track
            // up-front — a new MIDI (instrument) track is only materialised if the user
            // actually picks a plugin (handled in pluginPicker.onPick). Cancelling the picker
            // leaves the project untouched.
            auto& tracks = projectState.getTracks();
            const auto sel = arrangementTimeline.getSelectedTrackIndex();
            int target = -1;
            if (sel.has_value() && *sel >= 0 && *sel < static_cast<int>(tracks.size())
                && tracks[static_cast<std::size_t>(*sel)].isMidiTrack)
                target = *sel;

            showInstrumentPicker(target);   // target == -1 → create on pick
            return;
        }

        if (item == SidebarNavItem::add)
        {
            juce::StringArray busNames;
            for (const auto& b : projectState.getBuses())
                busNames.add(b.name);
            addTrackDialog.setBounds(getLocalBounds());
            addTrackDialog.show(static_cast<int>(projectState.getTracks().size()),
                                busNames,
                                pluginManager.getInstrumentDescriptions());
            addTrackDialog.toFront(true);
        }
    };

    // Modern instrument picker overlay (replaces the native instrument popup menu).
    pluginPicker.onPick = [this](const juce::PluginDescription& desc)
    {
        int target = pluginPickerTargetTrack;
        if (target < 0)
        {
            // No suitable MIDI track existed when the picker opened — create one now, only
            // because the user actually chose a plugin.
            arrangementTimeline.addMidiTrack();
            target = static_cast<int>(projectState.getTracks().size()) - 1;
            refreshClipInspector();
            resized();
            repaint();
        }
        loadInstrumentOnTrack(target, desc);
    };
    pluginPicker.onRescan = [this]()
    {
        scanPluginsInteractively([this]()
        {
            if (pluginPicker.isVisible())
                pluginPicker.show("Load Instrument", pluginManager.getInstrumentDescriptions(), pluginManager.isScanning());
        });
    };
    pluginPicker.onClose = [this]() { arrangementTimeline.grabKeyboardFocus(); };
    addChildComponent(pluginPicker);

    // Modern "Add Track" dialog (replaces the small +/menu).
    addTrackDialog.onCreate = [this](const AddTrackDialogComponent::Result& r)
    {
        using TT = AddTrackDialogComponent::TrackType;
        auto& tracks = projectState.getTracks();

        if (r.type == TT::folder)
        {
            // A folder/group track owns a dedicated aux bus; its children route into that bus,
            // so the existing bus engine handles group volume / mute / inserts for free.
            auto& buses = projectState.getBuses();
            const auto folderName = r.name.isNotEmpty() ? r.name : juce::String("Group");
            const auto folderColour = r.autoColour ? juce::Colour(0xff3a7bd5) : r.colour;

            BusState bus;
            bus.name = folderName;
            bus.colour = folderColour;
            buses.push_back(bus);
            const int busIndex = static_cast<int>(buses.size()) - 1;
            const int gid = projectState.allocateGroupId();

            TrackState folder;
            folder.name = folderName;
            folder.isFolder = true;
            folder.groupId = gid;
            folder.folderBusIndex = busIndex;
            folder.colour = folderColour;
            tracks.push_back(std::move(folder));
            arrangementTimeline.selectTrack(static_cast<int>(tracks.size()) - 1);

            syncFoldersToBuses();
            refreshClipInspector();
            resized();
            repaint();
            return;
        }

        const bool midiLike = (r.type == TT::midi || r.type == TT::sampler);
        auto baseName = r.name.isNotEmpty() ? r.name : juce::String(midiLike ? "MIDI" : "Audio");
        std::vector<int> createdTrackIndices;

        // If a folder (or one of its children) is currently selected, new tracks join that
        // folder: they're inserted at the end of its child block and routed to its bus.
        const auto sel = arrangementTimeline.getSelectedTrackIndex();
        int targetFolder = sel.has_value() ? arrangementTimeline.owningFolderIndex(*sel) : -1;

        for (int i = 0; i < r.count; ++i)
        {
            const auto trackName = r.count > 1 ? (baseName + " " + juce::String(i + 1)) : baseName;

            if (targetFolder >= 0)
            {
                const auto at = arrangementTimeline.folderChildInsertIndex(targetFolder);
                const auto newIdx = arrangementTimeline.insertTrackAt(at, midiLike, trackName, r.colour, r.autoColour);
                auto& nt = tracks[static_cast<std::size_t>(newIdx)];
                nt.parentGroup = tracks[static_cast<std::size_t>(targetFolder)].groupId;
                nt.outputBus   = tracks[static_cast<std::size_t>(targetFolder)].folderBusIndex;
                createdTrackIndices.push_back(newIdx);
            }
            else
            {
                if (midiLike) arrangementTimeline.addMidiTrack();
                else          arrangementTimeline.addAudioTrack();
                if (tracks.empty()) break;
                auto& t = tracks.back();
                t.name = trackName;
                if (! r.autoColour) t.colour = r.colour;
                t.outputBus = r.outputBus;
                createdTrackIndices.push_back(static_cast<int>(tracks.size()) - 1);
            }
        }
        if (midiLike && r.instrumentPluginId.isNotEmpty())
        {
            if (const auto desc = pluginManager.findDescription(r.instrumentPluginId); desc.has_value())
                for (const auto trackIndex : createdTrackIndices)
                    loadInstrumentOnTrack(trackIndex, *desc);
        }
        syncFoldersToBuses();
        refreshClipInspector();
        resized();
        repaint();
    };
    addTrackDialog.onClose = [this]() { arrangementTimeline.grabKeyboardFocus(); };
    addChildComponent(addTrackDialog);

    // The timeline "+" button opens the full Add Track dialog (same as the old sidebar +).
}

void MainComponent::wireEditors()
{
    arrangementTimeline.onAddTrackRequested = [this]()
    {
        juce::StringArray busNames;
        for (const auto& b : projectState.getBuses())
            busNames.add(b.name);
        addTrackDialog.setBounds(getLocalBounds());
        addTrackDialog.show(static_cast<int>(projectState.getTracks().size()),
                            busNames,
                            pluginManager.getInstrumentDescriptions());
        addTrackDialog.toFront(true);
    };

    samplerPanel.onClose = [this]()
    {
        // If we opened the sampler from the step rack, closing it returns there.
        if (samplerOpenedFromStep)
        {
            samplerOpenedFromStep = false;
            stepSequencer.setVisible(true);
        }
        resized();
        arrangementTimeline.grabKeyboardFocus();
    };
    samplerPanel.onRequestProjectTempoBpm = [this]() { return projectState.getTempoBpm(); };
    samplerPanel.onRequestProjectKeyRoot    = [this]() { return projectState.getKeyRoot(); };
    samplerPanel.onRequestProjectKeyIsMinor = [this]() { return projectState.isKeyMinor(); };
    samplerPanel.onRequestScaleLockEnabled  = [this]() { return projectState.isKeyEnabled() && projectState.isScaleLockEnabled(); };
    samplerPanel.onResolveTrack = [this](int trackIndex) -> TrackState*
    {
        auto& tracks = projectState.getTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
            return nullptr;

        return &tracks[static_cast<std::size_t>(trackIndex)];
    };
    samplerPanel.onNoteOn = [this](const juce::String& sourcePath,
                                   int midiNote,
                                   int velocity,
                                   int rootMidiNote,
                                   double gainDb,
                                   SamplerPlaybackMode playbackMode,
                                   int sliceIndex,
                                   int sliceCount,
                                   bool warpEnabled,
                                   double sourceBpm)
    {
        // Chord mode expands one key into a diatonic chord — but not for SLICE mode, where each key
        // is a distinct drum slice, not a pitch. Expansion happens here so the sampler component
        // itself stays single-note (no regression to its keyboard/slice logic).
        const auto pitches = (playbackMode == SamplerPlaybackMode::slice)
                                 ? std::vector<int>{ midiNote }
                                 : chordPitchesForNote(midiNote);
        samplerChordVoicing[midiNote] = pitches;

        if (arrangementPlaybackSource != nullptr)
        {
            const auto activeTrack = samplerPanel.getActiveTrackIndex();
            const bool onInstrument = activeTrack >= 0 && arrangementPlaybackSource->hasTrackInstrument(activeTrack);
            const bool fullSampleTrigger = activeTrack >= 0
                && activeTrack < static_cast<int>(projectState.getTracks().size())
                && projectState.getTracks()[static_cast<std::size_t>(activeTrack)].samplerFullSampleTrigger;
            for (const auto p : pitches)
            {
                if (onInstrument)
                    arrangementPlaybackSource->instrumentLiveNoteOn(activeTrack, p, velocity);
                else
                    arrangementPlaybackSource->samplerNoteOn(sourcePath, p, velocity, rootMidiNote,
                                                             gainDb, playbackMode, sliceIndex,
                                                             sliceCount, warpEnabled, sourceBpm,
                                                             fullSampleTrigger);
            }
        }
        for (const auto p : pitches)
            recordNoteOn(p, velocity);
    };
    samplerPanel.onNoteOff = [this](int midiNote, SamplerPlaybackMode playbackMode)
    {
        std::vector<int> pitches { midiNote };
        if (const auto it = samplerChordVoicing.find(midiNote); it != samplerChordVoicing.end())
        {
            pitches = it->second;
            samplerChordVoicing.erase(it);
        }

        if (arrangementPlaybackSource != nullptr)
        {
            const auto activeTrack = samplerPanel.getActiveTrackIndex();
            const bool onInstrument = activeTrack >= 0 && arrangementPlaybackSource->hasTrackInstrument(activeTrack);
            // Classic (без Full Sample) = Ableton: releasing the key stops the note live too.
            const bool gateByNoteLength = samplerTrackGatesByNoteLength(activeTrack);
            for (const auto p : pitches)
            {
                if (onInstrument)
                    arrangementPlaybackSource->instrumentLiveNoteOff(activeTrack, p);
                else
                    arrangementPlaybackSource->samplerNoteOff(p, playbackMode, gateByNoteLength);
            }
        }
        for (const auto p : pitches)
            recordNoteOff(p);
    };
    samplerPanel.onAllNotesOff = [this]()
    {
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allSamplerNotesOff();
            arrangementPlaybackSource->allInstrumentNotesOff();
        }
    };
    samplerPanel.onSlicePointsChanged = [this](const std::vector<double>& points)
    {
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->setSamplerLiveSlicePoints(points);
    };
    midiEditorOverlay.onClose = [this]() { arrangementTimeline.grabKeyboardFocus(); };
    midiEditorOverlay.onTogglePlayback = [this]() { toggleTransportFromUi(); };
    midiEditorOverlay.onStopAndRewindToClipStart = [this]()
    {
        // Stop playback and set the playhead to the open clip's start beat.
        finalizeRecordingClip();
        if (const auto* clip = getSelectedTimelineClip())
        {
            if (arrangementPlaybackSource != nullptr)
            {
                arrangementPlaybackSource->allSamplerNotesOff();
                arrangementPlaybackSource->allInstrumentNotesOff();
            }
            transportController.stop(
                [this]() { stopBrowserPreview(true); },
                [this]() { if (arrangementPlaybackSource != nullptr) arrangementPlaybackSource->syncToTransportPosition(); });
            transportEngine.setPlayheadBeat(clip->startBeat);
            if (arrangementPlaybackSource != nullptr)
            {
                arrangementPlaybackSource->allSamplerNotesOff();
                arrangementPlaybackSource->allInstrumentNotesOff();
                arrangementPlaybackSource->syncToTransportPosition();
            }
            updateTransportLabels();
            arrangementTimeline.repaint();
        }
        else
        {
            stopTransportFromUi();
        }
    };
    midiEditorOverlay.onPreviewNoteOn = [this](int midiNote, int velocity)
    {
        if (selectedArrangementClip.has_value())
            liveMidiNoteOn(selectedArrangementClip->first, midiNote, velocity);
    };
    midiEditorOverlay.onPreviewNoteOff = [this](int midiNote)
    {
        if (selectedArrangementClip.has_value())
            liveMidiNoteOff(selectedArrangementClip->first, midiNote);
    };
    arrangementTimeline.onChordAudition = [this](const std::vector<int>& pitches) { auditionArrangementChord(pitches); };

    arrangementTimeline.onChordLaneChanged = [this]()
    {
        if (arrangementPlaybackSource == nullptr)
            return;
        // Invalidate cached re-harmonised renders and rebuild them OFF the message thread so
        // editing/moving chords never stalls the UI. Coalesce rapid edits with a running flag.
        arrangementPlaybackSource->bumpChordGeneration();
        if (reharmRebuildRunning.exchange(true))
            return;
        juce::Thread::launch([this]
        {
            // Keep rebuilding until the cache catches up with the latest chord edit.
            int built = -1;
            while (arrangementPlaybackSource != nullptr)
            {
                const int gen = arrangementPlaybackSource->chordGenerationValue();
                if (gen == built) break;
                built = gen;
                arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
                juce::MessageManager::callAsync([this] { arrangementTimeline.repaint(); });
            }
            reharmRebuildRunning = false;
        });
    };

    midiEditorOverlay.onPreviewChordRetrigger = [this]()
    {
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allSamplerNotesOff();
            arrangementPlaybackSource->allInstrumentNotesOff();
        }
    };
    midiEditorOverlay.onGlideChanged = [this](bool on)
    {
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->setSamplerGlide(on, 0.08);
    };
    midiEditorOverlay.onRequestPlayheadBeat = [this]() { return transportEngine.getPlayheadBeat(); };
    midiEditorOverlay.onRequestPlayingState = [this]() { return transportEngine.isPlaying(); };
    midiEditorOverlay.onStartGlobalSpacePreview = [this](double startBeat) { startGlobalSpacePreview(startBeat); };
    midiEditorOverlay.onStopGlobalSpacePreview = [this]() { stopGlobalSpacePreview(); };
    midiEditorOverlay.onCommitGlobalSpacePreview = [this]() { commitGlobalSpacePreview(); };
    arrangementTimeline.onMidiClipDoubleClick = [this](int trackIndex, int clipIndex)
    {
        auto& track = projectState.getTracks()[static_cast<std::size_t>(trackIndex)];
        auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
        midiEditorOverlay.openClip(track, clip,
                                   projectState.getKeyRoot(),
                                   projectState.isKeyMinor(),
                                   projectState.isKeyEnabled() && projectState.isScaleLockEnabled());
        midiEditorOverlay.setChordModeExternally(projectState.isChordModeEnabled(), projectState.getChordSizeNotes());
    };
    arrangementTimeline.onAudioClipDoubleClick = [this](int trackIndex, int clipIndex)
    {
        selectedArrangementClip = std::pair { trackIndex, clipIndex };
        samplerPanel.setVisible(false);
        stepSequencer.setVisible(false);
        clipEditorPanel.setVisible(true);
        refreshClipInspector();
        refreshClipEditor();
        resized();
        updateTransportLabels();
    };
    arrangementTimeline.onTogglePlayback = [this]()
    {
        if (transportEngine.isPlaying() || transportEngine.isCountInActive())
            stopTransportFromUi();
        else
            toggleTransportFromUi();
    };
    arrangementTimeline.onTransportSeek = [this]()
    {
        // Jump the audio engine to the new playhead position (works while playing too).
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->syncToTransportPosition();
        updateTransportLabels();
    };
    midiEditorOverlay.onScaleLockChanged = [this](bool enabled)
    {
        projectState.setScaleLockEnabled(enabled);
    };
    midiEditorOverlay.onChordModeChanged = [this](bool enabled)
    {
        projectState.setChordModeEnabled(enabled);
        syncChordModeToSurfaces();
    };
    midiEditorOverlay.onChordSizeChanged = [this](int size)
    {
        projectState.setChordSizeNotes(size);
        syncChordModeToSurfaces();
    };
    // Share the audio-edit lock so the piano roll's note/slide edits can't race the audio render thread.
    midiEditorOverlay.setAudioEditLock(&projectState.getAudioEditLock());
    arrangementTimeline.onClipWarpEdited = [this]()
    {
        if (arrangementPlaybackSource == nullptr)
            return;
        // Live time-stretch: re-prep warp so the new speed is heard immediately. While playing,
        // use the non-blocking path (background producer builds the new-length stream and the
        // render swaps to it) so nothing stalls — no stop/replay needed.
        if (transportEngine.isPlaying() && arrangementPlaybackSource->isRealtimeWarpEnabled())
            arrangementPlaybackSource->prepareWarpStreams(false);
        else
            rebuildArrangementWarpNonBlocking();
    };
    arrangementTimeline.onClipSelectionChanged = [this](int trackIndex, int clipIndex)
    {
        if (trackIndex >= 0)
        {
            if (clipIndex >= 0)
                selectedArrangementClip = std::pair { trackIndex, clipIndex };
            else
                selectedArrangementClip.reset();

            const auto& tracks = projectState.getTracks();
            if (trackIndex < static_cast<int>(tracks.size()))
            {
                const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
                // FL-style sampler arm: any selection of a MIDI track with a sampler
                // OR a hosted VST instrument arms it for keyboard play, whether the
                // user clicked a clip OR just the track header.
                const bool hasSampler    = track.samplerSourcePath.isNotEmpty();
                const bool hasInstrument = track.instrumentPluginId.isNotEmpty();
                if (track.isMidiTrack && (hasSampler || hasInstrument))
                {
                    // Arm the (sampler or instrument) keyboard for this track WITHOUT
                    // popping the sampler panel. A single click only selects/arms; the
                    // piano roll opens on double-click and the sampler via its button or a
                    // header double-click. (Popping the panel here also re-laid out the
                    // lanes mid-gesture, which teleported the clip being dragged onto
                    // whatever track ended up under the cursor in the new layout.)
                    const bool wasVisible = samplerPanel.isVisible();
                    samplerPanel.openTrackIndex(trackIndex);
                    if (! wasVisible)
                        samplerPanel.setVisible(false);
                    else if (hasSampler)
                        resized();   // keep an already-open sampler following the selection
                }
                else if (samplerPanel.isArmed())
                {
                    // Switched to an audio (or non-sampled) track — disarm.
                    samplerPanel.disarmKeyboard();
                }

                if (clipIndex >= 0 && clipIndex < static_cast<int>(track.clips.size()))
                {
                    const auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
                    if (clip.type == ClipType::audio && clip.sourceKeyRoot >= 0)
                        DBG("[KeyDetect] clip='" + clip.name + "' sourceKey="
                            + formatKeyName(clip.sourceKeyRoot, clip.sourceKeyIsMinor)
                            + " projectKey="
                            + formatKeyName(projectState.getKeyRoot(), projectState.isKeyMinor()));
                }
            }
        }
        else
        {
            selectedArrangementClip.reset();
            // Clicking empty playlist space clears the selection — also close any open lower
            // panel (step sequencer / sampler) so it gets out of the way.
            bool closedPanel = false;
            if (stepSequencer.isVisible())
            {
                stepSequencer.setVisible(false);
                closedPanel = true;
            }
            if (samplerPanel.isVisible())
            {
                samplerOpenedFromStep = false;   // close straight to the playlist, don't bounce to step
                samplerPanel.setVisible(false);
                closedPanel = true;
            }
            if (closedPanel)
                resized();
        }

        if (! selectedArrangementClip.has_value() && clipEditorPanel.isVisible())
        {
            clipEditorPanel.setVisible(false);
            resized();
        }

        // Close the sampler panel if its bound track no longer exists or has no sample (e.g.
        // the clip/track it was opened for was just deleted) — otherwise it lingers showing
        // "No sample loaded".
        if (samplerPanel.isVisible())
        {
            const auto activeIdx = samplerPanel.getActiveTrackIndex();
            const auto& currentTracks = projectState.getTracks();
            const bool stillValid = activeIdx >= 0
                                    && activeIdx < static_cast<int>(currentTracks.size())
                                    && currentTracks[static_cast<std::size_t>(activeIdx)].samplerSourcePath.isNotEmpty();
            if (! stillValid)
            {
                samplerPanel.disarmKeyboard();
                samplerPanel.setVisible(false);
                resized();
            }
        }

        // NOTE: do NOT re-bake the warp cache here. This handler fires on every clip
        // selection (including the auto-select on drop), and prepareWarpCacheForCurrentTempo
        // synchronously RubberBand-stretches every warp clip — for a 1-minute clip that's
        // a multi-second UI freeze on drop. The warp cache is built on Play (togglePlayback)
        // where it belongs.
        refreshClipInspector();
        refreshClipEditor();
        updateTransportLabels();
    };
    arrangementTimeline.onTrackHeaderDoubleClick = [this](int trackIndex)
    {
        // Double-clicking an instrument track opens its plugin editor; otherwise
        // fall back to the sampler panel for sampled tracks.
        if (arrangementPlaybackSource != nullptr && arrangementPlaybackSource->hasTrackInstrument(trackIndex))
            openInstrumentEditor(trackIndex);
        else
            openSamplerForTrackIfAvailable(trackIndex);
    };
    arrangementTimeline.onTrackDeleted = [this](int trackIndex)
    {
        // Realign the engine's per-track instrument/insert slots with the project's new
        // track order so a deleted track can't leave a hosted instrument keyed wrong (which
        // left a held note ringing). Also drop any now-stale per-track UI references.
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->removeTrackAndReindex(trackIndex);

        if (samplerPanel.getActiveTrackIndex() == trackIndex)
        {
            samplerPanel.disarmKeyboard();
            samplerPanel.setVisible(false);
        }
        if (recordingSession.has_value() && recordingSession->trackIndex == trackIndex)
            recordingSession.reset();
        resized();
    };
    arrangementTimeline.onInstrumentLayoutChangedByHistory = [this]()
    {
        // Undo/redo restored a different track layout (e.g. a deleted track came back). Re-home
        // the LIVE instrument instances onto the restored layout (instant, lossless) so each
        // plugin is keyed to the right track again — a VST can't stay bound to what is now a
        // sampler track, and nothing is reinstantiated (no freeze).
        resyncInstrumentsAfterHistory();
        resized();
    };
    arrangementTimeline.onTrackHeaderRightClick = [this](int trackIndex)
    {
        showTrackInstrumentMenu(trackIndex);
    };
    arrangementTimeline.onTrackInstrumentClicked = [this](int trackIndex)
    {
        auto& tracks = projectState.getTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
            return;

        // Instrument loaded → open its editor window. Otherwise open the picker to load one.
        if (tracks[static_cast<std::size_t>(trackIndex)].instrumentPluginId.isNotEmpty())
            openInstrumentEditor(trackIndex);
        else
            showInstrumentPicker(trackIndex);
    };

    // Automation param picker needs the live plugins' parameter names. They live on the engine, so
    // the host answers these queries; the timeline only knows param indices.
    auto pluginParamNames = [](juce::AudioPluginInstance* inst) -> juce::StringArray
    {
        juce::StringArray names;
        if (inst != nullptr)
            for (auto* p : inst->getParameters())
                names.add(p->getName(48));
        return names;
    };

    arrangementTimeline.onRequestInstrumentParamNames = [this, pluginParamNames](int trackIndex)
    {
        if (arrangementPlaybackSource == nullptr)
            return juce::StringArray{};
        return pluginParamNames(arrangementPlaybackSource->getTrackInstrument(trackIndex));
    };

    arrangementTimeline.onRequestInsertParamNames = [this, pluginParamNames](int trackIndex, int insertIndex)
    {
        if (arrangementPlaybackSource == nullptr)
            return juce::StringArray{};
        return pluginParamNames(arrangementPlaybackSource->getInsertInstance(trackIndex, insertIndex));
    };
}
} // namespace orion
