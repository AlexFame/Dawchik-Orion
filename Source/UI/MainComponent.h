#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "../Audio/ExportService.h"
#include "../Audio/TransportController.h"
#include "../Audio/TransportEngine.h"
#include "../Core/ProjectSerializer.h"
#include "../Core/ProjectState.h"
#include "../Plugins/PluginEditorWindow.h"
#include "../Plugins/PluginManager.h"
#include "../Sampler/SamplerPanelComponent.h"
#include "ArrangementTimelineComponent.h"
#include "BrowserPanelComponent.h"
#include "MidiEditorOverlayComponent.h"
#include "MixerPanelComponent.h"

namespace orion
{
// Audio render sources now live in Audio/PlaybackSources.h. Forward-declared
// here so the unique_ptr members below only need the definition in the .cpp.
class BufferPreviewSource;
class ArrangementPlaybackSource;
class ClickTrackSource;
class MasterStripSource;

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
    void timerCallback() override;
    void buttonClicked(juce::Button* button) override;
    void updateTransportLabels();
    void playBrowserPreview(const BrowserItem& item);
    void loadBrowserItemIntoSampler(const BrowserItem& item);
    int findOrCreateSamplerTargetTrack();
    bool openSamplerForTrackIfAvailable(int trackIndex);

    // VST instrument hosting (right-click track header → menu).
    void showTrackInstrumentMenu(int trackIndex);
    void scanPluginsInteractively(std::function<void()> onFinished = {});
    void loadInstrumentOnTrack(int trackIndex, const juce::PluginDescription& description);
    void removeInstrumentFromTrack(int trackIndex);
    void openInstrumentEditor(int trackIndex);
    void closeInstrumentEditor(int trackIndex);
    void closeAllInstrumentEditors();
    void captureAllInstrumentStates();
    void restoreInstrumentsFromProject();
    void stopBrowserPreview(bool resetPosition);
    void toggleTransportFromUi();
    void stopTransportFromUi();
    void rewindTransportFromUi();
    void toggleLoopFromUi();
    void toggleMixerFromUi();
    void saveProjectInteractively();
    void openProjectInteractively();
    void loadProjectFromFile(const juce::File& file);
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
    MixerPanelComponent mixerPanel;
    PluginManager pluginManager;
    std::map<int, std::unique_ptr<PluginEditorWindow>> instrumentEditorWindows;

    juce::Label headerLabel;
    juce::Label statusLabel;
    juce::Label pluginScanNameLabel;
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

    // Tiny ▶ / ◀ triangle in the top-left corner that toggles the browser panel.
    // Lives next to the transport bar — much smaller than a full toolbar button.
    class BrowserCollapseArrow final : public juce::Button
    {
    public:
        BrowserCollapseArrow() : juce::Button("browserCollapse")
        {
            setClickingTogglesState(true);
            setTooltip("Show / hide browser");
        }
        void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
        {
            auto area = getLocalBounds().toFloat().reduced(3.0f);
            const auto open = getToggleState();
            juce::Path tri;
            // open ⇒ arrow points LEFT (◀, "collapse"); closed ⇒ RIGHT (▶, "expand")
            if (open)
                tri.addTriangle(area.getRight(), area.getY(),
                                area.getRight(), area.getBottom(),
                                area.getX(),     area.getCentreY());
            else
                tri.addTriangle(area.getX(),     area.getY(),
                                area.getX(),     area.getBottom(),
                                area.getRight(), area.getCentreY());

            const float alpha = isButtonDown ? 1.0f : (isMouseOverButton ? 0.95f : 0.65f);
            g.setColour(juce::Colours::white.withAlpha(alpha));
            g.fillPath(tri);
        }
    };
    BrowserCollapseArrow browserCollapseArrow;
    juce::TextButton mixerButton;
    juce::TextButton openButton;
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
    std::unique_ptr<MasterStripSource> masterStripSource;
    double masterGainDb { 0.0 };
    // Decayed per-track output levels (0..1) for the timeline + mixer meters.
    // MainComponent's 60 Hz timer is the single consumer of the audio-thread peaks
    // so the two meter views never steal peaks from each other.
    std::vector<float> trackMeterLevels;   // fast bar level (linear), responsive
    std::vector<float> trackPeakHoldDb;    // Logic-style held peak in dB (numeric readout)
    std::vector<int>   trackPeakHoldFrames;// remaining hold frames before the number falls
    void updateTrackMeterLevels();
    std::unique_ptr<juce::FileChooser> saveFileChooser;
    std::unique_ptr<juce::FileChooser> openFileChooser;
    std::unique_ptr<juce::FileChooser> exportFileChooser;
    juce::File currentProjectFile;
    juce::File currentPreviewFile;
    double currentPreviewTempoBpm { 0.0 };
    std::optional<std::pair<int, int>> selectedArrangementClip;

    // Live MIDI recording state — captures keys pressed on the laptop keyboard
    // while transport is playing AND record-armed AND a MIDI track is R-armed.
    struct PendingMidiNote
    {
        int    velocity { 100 };
        double startBeatInClip { 0.0 };
    };
    struct RecordingSession
    {
        int    trackIndex { -1 };
        int    clipIndex { -1 };
        double clipStartBeat { 0.0 };
        std::map<int, PendingMidiNote> pendingNotes; // keyed by pitch
    };
    std::optional<RecordingSession> recordingSession;

    void recordNoteOn(int pitch, int velocity);
    void recordNoteOff(int pitch);
    void finalizeRecordingClip();
    int browserPanelWidth { 300 };
    int exportSampleRate { 44100 };
    // When false the browser panel is hidden and the playlist expands to fill the window.
    bool browserPanelVisible { true };
    bool isResizingBrowserPanel { false };
    int browserResizeStartX { 0 };
    int browserResizeStartWidth { 300 };
    double pluginScanProgress { 0.0 };
    bool pluginScanVisible { false };
};
}  // namespace orion
