#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
#include "BrowserPanelComponent.h"
#include "ClipEditorComponent.h"
#include "JamSessionComponent.h"
#include "MidiEditorOverlayComponent.h"
#include "AddTrackDialogComponent.h"
#include "MixerPanelComponent.h"
#include "MpcSampleHardwareBridge.h"
#include "MpcSampleMapping.h"
#include "MpcSamplePanelComponent.h"
#include "PluginPickerComponent.h"
#include "SelectionInspectorComponent.h"
#include "StepSequencerComponent.h"
#include "SidebarNavComponent.h"
#include "TransportBarComponent.h"

namespace orion
{
// Audio render sources now live in Audio/PlaybackSources.h. Forward-declared
// here so the unique_ptr members below only need the definition in the .cpp.
class BufferPreviewSource;
class StreamingFilePreviewSource;
class StreamingWarpPreviewSource;
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
    // Hardware MIDI keyboard support. handleIncomingMidiMessage marshals each
    // message to the message thread and hands it to routeLiveMidiMessage, which
    // plays it through the active track's instrument/sampler, records it when armed,
    // forwards controllers to hosted VST instruments, and step-writes into the MIDI
    // editor when it's open.
    void routeLiveMidiMessage(const juce::MidiMessage& message, const juce::String& sourceName = {});
    int  resolveLiveMidiTargetTrack();
    int  resolveArmedMidiTrack();
    void triggerMpcPad(int padIndex, int velocity);
    void handleMpcCommand(MpcSamplePanelComponent::Command command);
    void beginMpcCommandLearn(MpcSamplePanelComponent::Command command);
    int  mpcTuneMidiNoteForPad(int padIndex) const;
    void updateMpcPerformanceState();
    void mpcTapTempo();
    void liveMidiNoteOn(int trackIndex, int midiNote, int velocity);
    void liveMidiNoteOff(int trackIndex, int midiNote);
    // Chord-lane audition: play a chord through the selected (or first) instrument track, auto-released.
    void auditionArrangementChord(const std::vector<int>& pitches);
    void stopArrangementChordAudition();
    std::vector<std::pair<int, int>> activeChordAuditionNotes;   // (trackIndex, pitch)
    int chordAuditionGeneration { 0 };
    std::atomic<bool> reharmRebuildRunning { false };   // coalesces background re-harmonise renders
    std::atomic<bool> warpRebuildRunning { false };     // coalesces background warp-marker cache rebuilds
    unsigned lastMenuStateHash { 0xFFFFFFFFu };         // skip per-tick menu-bar rebuilds when unchanged
    // Chord mode: expand a single played key into the diatonic chord for the project key
    // (or just {note} when chord mode / tonality is off). liveChordVoicing remembers each
    // held key's actual pitches so note-off releases the whole chord even if the mode was
    // toggled mid-hold.
    std::vector<int> chordPitchesForNote(int midiNote) const;
    // True when a sampler track is Classic without Full Sample (Ableton) → live key-up stops the note.
    bool samplerTrackGatesByNoteLength(int trackIndex) const;
    // Push the shared chord-mode state to every surface that shows it (piano roll, sampler,
    // track headers, transport label) — the single source of truth is projectState.
    void syncChordModeToSurfaces();
    std::map<int, std::vector<int>> liveChordVoicing;      // hardware-MIDI held keys → sounded pitches
    std::map<int, std::vector<int>> samplerChordVoicing;   // sampler-keyboard held keys → sounded pitches
    std::map<int, std::vector<int>> mpcChordVoicing;       // MPC 16 Levels held pads → sounded pitches
    std::map<int, int> mpcPadActiveNotes;                  // pad index → exact note sounded at note-on
    std::set<int> mpcHeldHardwareNoteKeys;                 // channel/note latch: ignore repeat note-ons while held
    std::map<int, double> mpcHardwareNoteReleaseTimes;     // delayed re-arm: filters MPC pressure/note-repeat chatter
    std::map<int, int> mpcHardwareNotePads;                // channel/note → Orion pad index for delayed note-off
    // Enables + attaches every available MIDI input device, skipping ones already
    // connected. Called at launch and polled so freshly plugged-in keyboards work
    // without a restart.
    void refreshMidiInputDevices();
    void updateTransportLabels();
    void playBrowserPreview(const BrowserItem& item);
    void loadBrowserItemIntoSampler(const BrowserItem& item, bool addClipToPlaylist = true);
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
    // Re-home hosted instruments onto the current (post undo/redo) track layout by reusing the
    // live instances kept in the engine's stash — instant and lossless. Only reinstantiates a
    // plugin when no matching live instance exists (e.g. stash overflowed).
    void resyncInstrumentsAfterHistory();
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
    void startGlobalSpacePreview(double startBeat);
    void stopGlobalSpacePreview();
    void commitGlobalSpacePreview();   // space tap → keep playing as normal (no rewind)
    void toggleTransportFromUi();
    void stopTransportFromUi();
    void finishRecordingAndDisarm();
    void startMidiRecordingFromRecordButtonIfNeeded();
    void rewindTransportFromUi();
    void toggleLoopFromUi();
    void toggleMixerFromUi();
    void toggleClipEditorFromUi();
    void toggleStepSequencerFromUi();
    void toggleMpcSampleFromUi();
    void toggleJamSessionFromUi();
    void saveProjectInteractively();
    void newProjectInteractively();
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
    // Refresh the arrangement's warp render after a warp/pitch/key change WITHOUT blocking
    // the message thread: configure streamers + kick the background producer when realtime
    // warp is on (instant), else fall back to the synchronous cache build.
    void rebuildArrangementWarpNonBlocking();
    bool startClipEditorPreview();
    // Loads a prepared (already stretched/pitched) buffer into the clip-editor preview
    // transport and starts it. `resumeSeconds` < 0 starts from the top.
    void playClipEditorPreviewBuffer(juce::AudioBuffer<float> buffer, double sampleRate,
                                     double startRatio, double endRatio,
                                     double rawDurationSeconds, double resumeSeconds);
    void stopClipEditorPreview(bool resetToStart);
    void updateClipEditorPreviewPlayhead();
    bool setClipEditorPreviewPlaybackPosition(double sourceRatio);
    void rebuildClipEditorWaveform(const juce::String& sourcePath);
    void normalizeSelectedAudioClip();
    // Peak magnitude and integrated loudness of the region the clip actually plays (its trim).
    struct ClipLevels
    {
        float peak { -1.0f };            // <0 = unreadable
        double lufs { 0.0 };             // -inf = silent / too short to gate
    };
    ClipLevels measureClipLevels(const TimelineClip& clip);
    // Peak magnitude of the region the clip actually plays (its trim), or -1 if unreadable.
    float measureClipPeak(const TimelineClip& clip);
    // Bring every selected clip up (or down) to the loudness of the loudest one, by LUFS rather
    // than by peak, capping each gain so nothing is pushed into clipping.
    void matchClipLoudness(const std::vector<ArrangementTimelineComponent::SelectedClip>& selection);
    // Normalize a whole selection. relativeToLoudest applies ONE shared offset, derived from the
    // loudest clip, so the clips keep their balance against each other; otherwise each clip is
    // brought to the target on its own.
    void normalizeClips(const std::vector<ArrangementTimelineComponent::SelectedClip>& selection, bool relativeToLoudest);
    void setClipsGainDb(const std::vector<ArrangementTimelineComponent::SelectedClip>& selection, double gainDb);
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
    JamSessionComponent jamSession;
    MidiEditorOverlayComponent midiEditorOverlay;
    ClipEditorComponent clipEditorPanel;
    SamplerPanelComponent samplerPanel;
    StepSequencerComponent stepSequencer { projectState, transportEngine };
    MixerPanelComponent mixerPanel;
    MpcSamplePanelComponent mpcSamplePanel;
    MpcSampleHardwareBridge mpcHardwareBridge;
    bool mpcFullLevel { false };
    bool mpcSixteenLevels { false };
    bool mpcChopMode { false };
    juce::String mpcTuneSourcePath;
    juce::String mpcChopSourcePath;
    int mpcTuneRootNote { 36 };
    int mpcTuneOctaveOffset { 0 };
    int mpcRepeatedRootNoteCount { 0 };
    void appendLiveMidiDebugLog(const juce::MidiMessage& message,
                                const juce::String& sourceName,
                                int mappedPadIndex);
    std::optional<MpcSamplePanelComponent::Command> pendingMpcCommandLearn;
    std::map<int, MpcSamplePanelComponent::Command> mpcCcCommandMap;
    int mpcPadBank { 0 };
    int mpcSelectedPad { 0 };
    double mpcLastTapMs { 0.0 };
    std::vector<double> mpcTapIntervalsMs;
    AddTrackDialogComponent addTrackDialog;
    PluginPickerComponent pluginPicker;
    int pluginPickerTargetTrack { 0 };
    PluginManager pluginManager;
    std::map<int, std::unique_ptr<PluginEditorWindow>> instrumentEditorWindows;
    std::map<std::pair<int, int>, std::unique_ptr<PluginEditorWindow>> insertEditorWindows;
    std::map<std::string, std::shared_ptr<const ClipEditorWaveform>> clipEditorWaveformCache;
    // Cache of fully-prepared (trimmed + pitch-shifted) clip-editor preview buffers,
    // keyed by source|startSample|numSamples|semitones|backend, so re-triggering or
    // returning to a previous pitch is instant instead of re-stretching every time.
    std::map<std::string, std::shared_ptr<juce::AudioBuffer<float>>> clipEditorPreviewCache;
    // Heavy pitch-stretch for a NEW preview pitch runs on this background thread so
    // the UI never freezes; a generation counter discards stale builds.
    juce::ThreadPool clipEditorPreviewPool { 1 };
    std::atomic<int> clipEditorPreviewBuildGen { 0 };
    // Identifies what the preview transport is currently playing, so a high-quality
    // background render only swaps in if it's still the same region/pitch.
    int clipEditorPreviewPlayingGen { -1 };
    std::string clipEditorPreviewPlayingKey;
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
    juce::StringArray activeMidiInputDeviceIds;   // devices we've attached note callbacks to
    // Directly-opened MIDI inputs (bypasses AudioDeviceManager, which wasn't delivering on macOS).
    std::map<juce::String, std::unique_ptr<juce::MidiInput>> directMidiInputs;
    std::optional<double> globalSpacePreviewRestoreBeat;
    bool globalSpacePreviewWasRecordArmed { false };
    bool jamSessionOpen { false };
    juce::StringArray seenMidiInputDeviceIds;     // devices auto-enabled once (plug-and-play)
    bool mpcInputConnected { false };
    juce::String mpcInputName;
    double lastLiveMidiActivityMs { -10000.0 };
    juce::String lastLiveMidiSignalText { "MIDI --" };
    std::set<int> liveMidiDisplayNotes;
    int visibleMidiInputCount { 0 };
    int midiDeviceRescanCounter { 0 };            // throttles hot-plug rescans in timerCallback
    juce::AudioSourcePlayer previewSourcePlayer;
    juce::MixerAudioSource masterMixerSource;
    juce::AudioTransportSource previewTransportSource;
    std::unique_ptr<BufferPreviewSource> previewBufferSource;
    std::unique_ptr<StreamingFilePreviewSource> previewFileSource;
    juce::AudioTransportSource clipEditorPreviewTransportSource;
    std::unique_ptr<BufferPreviewSource> clipEditorPreviewBufferSource;
    // Instant streaming stand-in used while the high-quality buffer renders in the
    // background; one of the two is active at a time on the preview transport.
    std::unique_ptr<StreamingWarpPreviewSource> clipEditorPreviewStreamSource;
    // Identifies the rendered buffer in clipEditorPreviewStreamSource (path|semitones|
    // targetSamples). Stop keeps the source alive; replay reuses it when this key matches,
    // so re-pressing Play is instant (buffer already filled) instead of re-rendering.
    std::string clipEditorPreviewStreamKey;
    std::unique_ptr<ArrangementPlaybackSource> arrangementPlaybackSource;
    std::unique_ptr<ClickTrackSource> clickTrackSource;
    std::unique_ptr<MasterStripSource> masterStripSource;
    std::unique_ptr<AudioInputRecorder> audioInputRecorder;
    bool audioRecorderCallbackAttached { false };
    std::atomic<bool> audioInputConfiguring { false };   // background device start in flight
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
    bool   currentPreviewLooping { false };
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
    void startStreamingPreviewPlayback(std::unique_ptr<juce::AudioFormatReader> reader,
                                       int outputSamples, double sampleRate,
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
    // Wall-clock interpolation of the preview playhead: the transport reports position only per audio
    // block, so we advance a smoothed seconds value by real elapsed time (playback is 1x) and softly
    // pull it toward the actual position — that's what makes the line sweep smoothly, not in steps.
    double clipEditorSmoothPlayheadSec { 0.0 };
    double clipEditorLastActualPlayheadSec { 0.0 };
    double clipEditorPlayheadWallMs { 0.0 };

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

    void ensureMidiRecordingSession(int armedTrack);   // open the record clip/session if needed
    void recordNoteOn(int pitch, int velocity);
    void recordNoteOff(int pitch);
    // Trigger/stop an MPC Sample pad's own sample through the shared sampler engine (+ record).
    void playMpcPad(int padIndex, int velocity);
    // Ensure the MPC panel has an armed MIDI track to record pad hits into; returns its index.
    int ensureMpcRecordTrack();
    // Index of the MPC kit track (isMpcKit), creating one named "MPC" if none exists.
    int mpcKitTrackIndex();
    int findMpcKitTrack() const;   // like above but never creates (-1 if none)
    // Mirror a pad's sample into the MPC kit track so recorded MIDI plays it back.
    void assignMpcKitSample(int padIndex, const juce::String& sourcePath);
    // Push the panel's Tune (16-Levels) state into the kit track for playback + live.
    void syncMpcTuneMode();
    void finalizeRecordingClip();
    void startAudioRecordingClip(int trackIndex);
    void finalizeAudioRecordingClip();
    // Discards any in-progress take (audio + MIDI) and stops the transport.
    void cancelRecording();
    // Attaches/detaches the input callback so an armed audio track shows live input
    // level (monitoring) even before recording starts.
    // Callback wiring lifted out of the constructor (which had grown past 1700 lines). Called
    // from the constructor in this order; each is a straight cut of what used to be inline.
    void wireBrowserAndDialogs();   // browser, sidebar, plugin picker, add-track dialog
    void wireEditors();             // arrangement timeline, sampler panel, MIDI editor

    void updateInputMonitoring();
    bool ensureAudioInputReady(bool requestPermission);
    void beginAudioInputConfiguration();   // starts the audio device off the message thread
    bool ensureCameraReady(bool requestPermission);
    // Mirror each folder track's volume/mute onto its group bus (one-way), and propagate
    // the folder's solo state down to its children, so the timeline folder controls drive
    // the whole group via the existing bus engine.
    void syncFoldersToBuses();
    juce::File getAudioRecordingDirectory() const;
    int browserPanelWidth { 360 };
    int exportSampleRate { 44100 };
    // When false the browser panel is hidden and the playlist expands to fill the window.
    bool browserPanelVisible { true };
    // True when the sampler panel was opened by clicking a channel in the step sequencer, so
    // closing it returns to the step rack (instead of just the playlist).
    bool samplerOpenedFromStep { false };
    // Smooth slide-open animation: linear progress 0..1 eased toward browserPanelVisible.
    float browserAnim { 1.0f };
    int currentBrowserWidth() const noexcept;   // animated effective width (0 when fully closed)
    bool browserPanelShown() const noexcept;     // true while any sliver is visible
    bool isResizingBrowserPanel { false };
    int browserResizeStartX { 0 };
    int browserResizeStartWidth { 300 };
    // A thin drag bar sitting ON TOP of the browser/timeline seam so the resize gesture is actually
    // reachable (child components otherwise swallow the mouse before MainComponent sees it).
    struct DragBar final : juce::Component
    {
        std::function<void(const juce::MouseEvent&)> onDown, onDrag, onUp;
        void mouseDown(const juce::MouseEvent& e) override { if (onDown) onDown(e); }
        void mouseDrag(const juce::MouseEvent& e) override { if (onDrag) onDrag(e); }
        void mouseUp(const juce::MouseEvent& e) override   { if (onUp)   onUp(e); }
        // Fully invisible — it only provides the resize cursor + drag gesture, no drawn line at all.
        void paint(juce::Graphics&) override {}
    };
    DragBar browserResizeBar;
    double pluginScanProgress { 0.0 };
    bool pluginScanVisible { false };
    bool audioInputPermissionRequestInFlight { false };
    bool cameraPermissionRequestInFlight { false };
};
}  // namespace orion
