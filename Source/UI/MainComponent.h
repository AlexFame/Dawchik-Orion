#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <memory>
#include <optional>

#include "../Audio/ExportService.h"
#include "../Audio/TransportController.h"
#include "../Audio/TransportEngine.h"
#include "../Core/ProjectSerializer.h"
#include "../Core/ProjectState.h"
#include "../Sampler/SamplerPanelComponent.h"
#include "ArrangementTimelineComponent.h"
#include "BrowserPanelComponent.h"
#include "MidiEditorOverlayComponent.h"

namespace orion
{
class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            private juce::Timer,
                            private juce::Button::Listener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    void resetToPlaylistView();

private:
    class BufferPreviewSource;
    class ArrangementPlaybackSource;
    class ClickTrackSource;

    void timerCallback() override;
    void buttonClicked(juce::Button* button) override;
    void updateTransportLabels();
    void playBrowserPreview(const BrowserItem& item);
    void loadBrowserItemIntoSampler(const BrowserItem& item);
    int findOrCreateSamplerTargetTrack();
    bool openSamplerForTrackIfAvailable(int trackIndex);
    void stopBrowserPreview(bool resetPosition);
    void toggleTransportFromUi();
    void stopTransportFromUi();
    void rewindTransportFromUi();
    void toggleLoopFromUi();
    void saveProjectInteractively();
    void exportProjectInteractively();
    void openSettingsDialog();
    void refreshAudioClipWarpLengths();
    void refreshClipInspector();
    void setClipInspectorVisible(bool shouldShow);
    void applyGainFromInspectorText();
    void applyTempoFromTransportText();
    void beginTempoEditing();
    void endTempoEditing(bool applyChanges);
    void showKeySelectionMenu();
    TimelineClip* getSelectedTimelineClip() noexcept;
    const TimelineClip* getSelectedTimelineClip() const noexcept;
    juce::Rectangle<int> getBrowserResizeHandleBounds() const noexcept;

    ProjectState projectState;
    TransportEngine transportEngine;
    TransportController transportController;
    ArrangementTimelineComponent arrangementTimeline;
    BrowserPanelComponent browserPanel;
    MidiEditorOverlayComponent midiEditorOverlay;
    SamplerPanelComponent samplerPanel;

    juce::Label headerLabel;
    juce::Label statusLabel;
    juce::Label tempoLabel;
    juce::Label meterLabel;
    juce::Label playheadLabel;
    juce::Label bpmCaptionLabel;
    juce::Label bpmValueLabel;
    juce::Label meterCaptionLabel;
    juce::Label meterValueLabel;
    juce::TextEditor bpmEditor;
    juce::Rectangle<int> cachedKeyCardBounds;
    juce::Label playlistLabel;
    juce::Label pianoRollLabel;
    juce::Label clipInspectorEmptyLabel;
    juce::Label clipInspectorTitleLabel;
    juce::Label clipInspectorTrackLabel;
    juce::Label clipInspectorFileLabel;
    juce::Label clipWarpLabel;
    juce::Label clipWarpInfoLabel;
    juce::Label clipSourceBpmLabel;
    juce::Label clipBarsLabel;
    juce::Label clipGainLabel;
    juce::Label clipGainValueLabel;
    juce::Slider clipGainSlider;
    juce::ToggleButton clipWarpToggle;
    juce::ToggleButton clipMuteToggle;
    juce::ToggleButton clipSoloToggle;
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextButton recordButton;
    juce::TextButton rewindButton;
    juce::TextButton undoButton;
    juce::TextButton redoButton;
    juce::TextButton metronomeButton;
    juce::TextButton loopButton;
    juce::TextButton countInButton;
    juce::TextButton browserButton;
    juce::TextButton scanPluginsButton;
    juce::TextButton saveButton;
    juce::TextButton exportButton;
    juce::TextButton settingsButton;
    juce::AudioFormatManager audioFormatManager;
    juce::AudioDeviceManager audioDeviceManager;
    juce::AudioSourcePlayer previewSourcePlayer;
    juce::MixerAudioSource masterMixerSource;
    juce::AudioTransportSource previewTransportSource;
    std::unique_ptr<BufferPreviewSource> previewBufferSource;
    std::unique_ptr<ArrangementPlaybackSource> arrangementPlaybackSource;
    std::unique_ptr<ClickTrackSource> clickTrackSource;
    std::unique_ptr<juce::FileChooser> saveFileChooser;
    std::unique_ptr<juce::FileChooser> exportFileChooser;
    juce::File currentProjectFile;
    juce::File currentPreviewFile;
    double currentPreviewTempoBpm { 0.0 };
    std::optional<std::pair<int, int>> selectedArrangementClip;
    int browserPanelWidth { 300 };
    int exportSampleRate { 44100 };
    bool isResizingBrowserPanel { false };
    int browserResizeStartX { 0 };
    int browserResizeStartWidth { 300 };
};
}  // namespace orion
