#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "../Audio/ExportService.h"
#include "../Audio/WarpEngine.h"
#include "../Audio/TransportController.h"
#include "../Audio/TransportEngine.h"
#include "../Core/ProjectSerializer.h"
#include "../Core/ProjectState.h"
#include "../Plugins/PluginEditorWindow.h"
#include "../Plugins/PluginManager.h"
#include "../Sampler/SamplerPanelComponent.h"
#include "ArrangementTimelineComponent.h"
#include "BottomStatusBarComponent.h"
#include "BrowserPanelComponent.h"
#include "ClipEditorComponent.h"
#include "MidiEditorOverlayComponent.h"
#include "AddTrackDialogComponent.h"
#include "MixerPanelComponent.h"
#include "PluginPickerComponent.h"
#include "SelectionInspectorComponent.h"
#include "SidebarNavComponent.h"
#include "TransportBarComponent.h"

namespace orion
{
// Audio render sources now live in Audio/PlaybackSources.h. Forward-declared
// here so the unique_ptr members below only need the definition in the .cpp.
class BufferPreviewSource;
class ArrangementPlaybackSource;
class ClickTrackSource;
class MasterStripSource;
class AudioInputRecorder;

class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            private juce::MenuBarModel,
                            private juce::Timer,
                            private juce::Button::Listener,
                            private juce::MidiInputCallback
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
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    void timerCallback() override;
    void buttonClicked(juce::Button* button) override;
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void updateTransportLabels();
    void playBrowserPreview(const BrowserItem& item);
    void loadBrowserItemIntoSampler(const BrowserItem& item);
    int findOrCreateSamplerTargetTrack();
    bool openSamplerForTrackIfAvailable(int trackIndex);

    // VST instrument hosting (right-click track header → menu).
    void showTrackInstrumentMenu(int trackIndex);
    void showInstrumentPicker(int trackIndex);
    void scanPluginsInteractively(std::function<void()> onFinished = {});
    void loadInstrumentOnTrack(int trackIndex, const juce::PluginDescription& description);
    void removeInstrumentFromTrack(int trackIndex);
    void openInstrumentEditor(int trackIndex);
    void closeInstrumentEditor(int trackIndex);
    void closeAllInstrumentEditors();
    void captureAllInstrumentStates();
    void restoreInstrumentsFromProject();
    // Insert FX chain. The "id" is a track index, or a bus key (ArrangementPlaybackSource
    // ::busInsertKey(busIndex)) so buses reuse the same chain code. insertIndex < 0 = add.
    std::vector<TrackState::InsertFx>* insertChainForId(int id);
    juce::String insertOwnerName(int id) const;
    void addBus();
    void showSendMenuForTrack(int trackIndex, int sendIndex);
    void showOutputRouteMenuForTrack(int trackIndex);
    void showInsertMenuForTrack(int trackIndex, int insertIndex);
    void addInsertOnTrack(int trackIndex, const juce::PluginDescription& description);
    void replaceInsertOnTrack(int trackIndex, int insertIndex, const juce::PluginDescription& description);
    void removeInsertFromTrack(int trackIndex, int insertIndex);
    void openInsertEditor(int trackIndex, int insertIndex);
    void toggleInsertBypass(int trackIndex, int insertIndex);
    void moveInsert(int fromTrack, int fromIndex, int toTrack, int toIndex);
    void copyInsertToTrack(int fromTrack, int fromIndex, int toTrack);
    void restoreInsertsFromProject();
    double getCurrentPluginSampleRate() const noexcept;
    int getCurrentPluginBlockSize() const noexcept;
    void stopBrowserPreview(bool resetPosition);
    void toggleTransportFromUi();
    void stopTransportFromUi();
    void rewindTransportFromUi();
    void toggleLoopFromUi();
    void toggleMixerFromUi();
    void toggleClipEditorFromUi();
    void saveProjectInteractively();
    void openProjectInteractively();
    void loadProjectFromFile(const juce::File& file);
    void exportProjectInteractively();
    void openSettingsDialog();
    void loadSidebarBrowserFolders();
    void saveSidebarBrowserFolders() const;
    void refreshAudioClipWarpLengths();
    // Background key/tempo analysis for clips flagged signalAnalysisPending.
    void maybeStartBackgroundAnalysis();
    void applyBackgroundAnalysis(const std::map<juce::String, orion::AudioWarpAnalysis>& results);
    void refreshClipInspector();
    void refreshClipEditor();
    void setClipEditorLocalPreviewPosition(double sourceRatio);
    bool startClipEditorPreview();
    void stopClipEditorPreview(bool resetToStart);
    void updateClipEditorPreviewPlayhead();
    bool setClipEditorPreviewPlaybackPosition(double sourceRatio);
    void rebuildClipEditorWaveform(const juce::String& sourcePath);
    void normalizeSelectedAudioClip();
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
    SidebarNavComponent sidebarNav;
    std::vector<juce::File> sidebarBrowserFolders;
    SelectionInspectorComponent selectionInspector;
    TransportBarComponent transportBar;
    BottomStatusBarComponent bottomStatusBar;
    MidiEditorOverlayComponent midiEditorOverlay;
    ClipEditorComponent clipEditorPanel;
    SamplerPanelComponent samplerPanel;
    MixerPanelComponent mixerPanel;
    AddTrackDialogComponent addTrackDialog;
    PluginPickerComponent pluginPicker;
    int pluginPickerTargetTrack { 0 };
    PluginManager pluginManager;
    std::map<int, std::unique_ptr<PluginEditorWindow>> instrumentEditorWindows;
    std::map<std::pair<int, int>, std::unique_ptr<PluginEditorWindow>> insertEditorWindows;
    std::map<std::string, std::pair<std::vector<float>, std::vector<float>>> clipEditorWaveformCache;
    // Cache of fully-prepared (trimmed + pitch-shifted) clip-editor preview buffers,
    // keyed by source|startSample|numSamples|semitones|backend, so re-triggering or
    // returning to a previous pitch is instant instead of re-stretching every time.
    std::map<std::string, std::shared_ptr<juce::AudioBuffer<float>>> clipEditorPreviewCache;
    // Heavy pitch-stretch for a NEW preview pitch runs on this background thread so
    // the UI never freezes; a generation counter discards stale builds.
    juce::ThreadPool clipEditorPreviewPool { 1 };
    std::atomic<int> clipEditorPreviewBuildGen { 0 };
    // Background signal analysis (key/tempo) for freshly-dropped clips, so dropping is
    // instant; results are applied back on the message thread.
    juce::ThreadPool analysisThreadPool { 1 };
    std::atomic<bool> analysisJobActive { false };
    double clipEditorPreviewResumeSeconds { -1.0 };

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
    juce::StringArray activeMidiInputDeviceIds;
    juce::AudioSourcePlayer previewSourcePlayer;
    juce::MixerAudioSource masterMixerSource;
    juce::AudioTransportSource previewTransportSource;
    std::unique_ptr<BufferPreviewSource> previewBufferSource;
    juce::AudioTransportSource clipEditorPreviewTransportSource;
    std::unique_ptr<BufferPreviewSource> clipEditorPreviewBufferSource;
    std::unique_ptr<ArrangementPlaybackSource> arrangementPlaybackSource;
    std::unique_ptr<ClickTrackSource> clickTrackSource;
    std::unique_ptr<MasterStripSource> masterStripSource;
    std::unique_ptr<AudioInputRecorder> audioInputRecorder;
    bool audioRecorderCallbackAttached { false };
    double masterGainDb { 0.0 };
    // Decayed per-track output levels (0..1) for the timeline + mixer meters.
    // MainComponent's 60 Hz timer is the single consumer of the audio-thread peaks
    // so the two meter views never steal peaks from each other.
    std::vector<float> trackMeterLevels;   // fast bar level (linear), responsive
    std::vector<float> trackMeterLevelsL;  // per-channel fast bar level (linear) — left
    std::vector<float> trackMeterLevelsR;  // per-channel fast bar level (linear) — right
    std::vector<float> trackPeakHoldDb;    // numeric readout in dB (Logic-style peak hold)
    std::vector<float> trackPeakRecentDb;  // loudest dB seen since the last discrete update
    std::vector<int>   trackPeakHoldFrames;// remaining peak-hold frames before the number snaps
    // Master output metering — single consumer (this component) fetches the audio-thread
    // peaks once per tick; the mixer and the bottom bar both read these stored values.
    float masterRawPeakL { 0.0f };         // raw peak this tick (left), for the mixer
    float masterRawPeakR { 0.0f };         // raw peak this tick (right), for the mixer
    float masterMeterLevel { 0.0f };       // decayed 0..1 level for the bottom MASTER OUT bar
    float masterMeterLevelL { 0.0f };      // per-channel decayed linear level for the mixer master bar
    float masterMeterLevelR { 0.0f };
    float masterMeterDb { -100.0f };       // numeric dB readout for the bottom MASTER OUT text
    float masterMeterRecentDb { -100.0f }; // loudest dB since the last discrete update
    int   masterMeterDbHoldFrames { 0 };   // remaining peak-hold frames before it snaps
    // Aux-bus meters (mirror the track meter machinery).
    std::vector<float> busMeterLevelsL;
    std::vector<float> busMeterLevelsR;
    std::vector<float> busPeakHoldDb;
    std::vector<float> busPeakRecentDb;
    std::vector<int>   busPeakHoldFrames;
    void updateTrackMeterLevels();
    std::unique_ptr<juce::FileChooser> saveFileChooser;
    std::unique_ptr<juce::FileChooser> openFileChooser;
    std::unique_ptr<juce::FileChooser> exportFileChooser;
    juce::File currentProjectFile;
    juce::File currentPreviewFile;
    double currentPreviewTempoBpm { 0.0 };
    bool   currentPreviewBpmSync { false };
    bool   pendingBrowserPreviewStart { false };
    int    pendingBrowserPreviewGeneration { 0 };
    double pendingBrowserPreviewStartBeat { 0.0 };
    double pendingBrowserPreviewLastBeat { 0.0 };   // tracks playhead to catch loop wrap-around
    // Launch-quantize a browser preview: if BPM-sync is on AND the project transport is
    // playing, arm the preview to start on the next whole beat; otherwise start it now.
    void armOrStartBrowserPreview();
    // Async preview loading so flipping through samples on a slow drive never freezes the UI.
    juce::ThreadPool previewLoadPool { 1 };
    std::atomic<int> previewRequestGeneration { 0 };
    void startPreviewPlayback(juce::AudioBuffer<float> buffer, double sampleRate,
                              const juce::File& file, const juce::String& displayName);
    std::optional<std::pair<int, int>> selectedArrangementClip;
    std::optional<std::pair<int, int>> clipEditorPreviewClip;
    std::map<std::pair<int, int>, std::pair<double, double>> clipEditorSelectionRanges;
    double clipEditorPreviewStartRatio { 0.0 };
    double clipEditorPreviewEndRatio { 1.0 };
    double clipEditorPreviewPlayheadRatio { 0.0 };
    double clipEditorLocalPreviewStartRatio { 0.0 };
    double clipEditorLocalPreviewEndRatio { 1.0 };
    double clipEditorLocalPreviewDurationSeconds { 0.0 };

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

    struct AudioRecordingSession
    {
        int trackIndex { -1 };
        int clipIndex { -1 };
        double clipStartBeat { 0.0 };
        double sampleRate { 44100.0 };
        juce::File file;
    };
    std::optional<AudioRecordingSession> audioRecordingSession;

    void recordNoteOn(int pitch, int velocity);
    void recordNoteOff(int pitch);
    void finalizeRecordingClip();
    void startAudioRecordingClip(int trackIndex);
    void finalizeAudioRecordingClip();
    // Discards any in-progress take (audio + MIDI) and stops the transport.
    void cancelRecording();
    // Attaches/detaches the input callback so an armed audio track shows live input
    // level (monitoring) even before recording starts.
    void updateInputMonitoring();
    void requestMicrophonePermissionAtLaunch();
    bool ensureAudioInputReady(bool requestPermission);
    // Mirror each folder track's volume/mute onto its group bus (one-way), and propagate
    // the folder's solo state down to its children, so the timeline folder controls drive
    // the whole group via the existing bus engine.
    void syncFoldersToBuses();
    juce::File getAudioRecordingDirectory() const;
    int browserPanelWidth { 300 };
    int exportSampleRate { 44100 };
    // When false the browser panel is hidden and the playlist expands to fill the window.
    bool browserPanelVisible { true };
    // Smooth slide-open animation: linear progress 0..1 eased toward browserPanelVisible.
    float browserAnim { 1.0f };
    int currentBrowserWidth() const noexcept;   // animated effective width (0 when fully closed)
    bool browserPanelShown() const noexcept;     // true while any sliver is visible
    bool isResizingBrowserPanel { false };
    int browserResizeStartX { 0 };
    int browserResizeStartWidth { 300 };
    double pluginScanProgress { 0.0 };
    bool pluginScanVisible { false };
    bool audioInputPermissionRequestInFlight { false };
};
}  // namespace orion
