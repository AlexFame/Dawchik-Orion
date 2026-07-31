#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../Audio/TransportEngine.h"
#include "../Core/ProjectState.h"
#include "ChordSelectorComponent.h"
#include "OrionTheme.h"

#include <memory>

namespace orion::stems { struct Result; }   // stem-separation result (Audio/StemSeparator.h)

namespace orion
{
class ArrangementTimelineComponent final : public juce::Component,
                                           public juce::DragAndDropTarget,
                                           public juce::FileDragAndDropTarget,
                                           private juce::Timer
{
public:
    ArrangementTimelineComponent(ProjectState& projectState, TransportEngine& transportEngine);
    ~ArrangementTimelineComponent() override;

    std::function<void(int, int)> onMidiClipDoubleClick;
    std::function<void(int, int)> onAudioClipDoubleClick;
    std::function<void(int, int)> onClipSelectionChanged;
    // Fired when a time-stretch / length edit finishes, so the host can re-prep warp live (while
    // playing) — the new speed is heard without stopping and replaying.
    std::function<void()> onClipWarpEdited;
    std::function<void(int)> onTrackHeaderDoubleClick;
    std::function<void(int)> onTrackHeaderRightClick;
    // Fired right after a track is removed from the project (passes the removed index) so the
    // host can keep the audio engine's per-track slots aligned (reindex instruments/inserts).
    std::function<void(int)> onTrackDeleted;
    // Fired after an undo/redo whose restored state changed the per-track instrument layout
    // (track added/removed/reordered). The host re-syncs the engine's instrument slots with
    // the restored project so a hosted plugin can't stay keyed to the wrong track.
    std::function<void()> onInstrumentLayoutChangedByHistory;
    // Fired when the instrument button on a MIDI track header is clicked. The host opens
    // the plugin editor if an instrument is loaded, or the instrument picker if not.
    std::function<void(int)> onTrackInstrumentClicked;
    std::function<void()> onTogglePlayback;
    // Fired when the "+" add-track button is clicked. The host opens the full Add Track
    // dialog. If unset, falls back to the inline popup menu.
    std::function<void()> onAddTrackRequested;
    // Fired when the playhead is moved by clicking/scrubbing the ruler, so the host can
    // re-sync the audio engine to the new position (jump even while playing).
    std::function<void()> onTransportSeek;
    // Returns the current 0..1 output level for a track (for the header meter).
    std::function<float(int)> onRequestTrackLevel;
    // Returns the current 0..1 left/right output levels for a track (stereo meter).
    std::function<std::pair<float, float>(int)> onRequestTrackLevelStereo;
    // Returns the current live signal level in dB (-100 ≈ silent → "-inf").
    std::function<float(int)> onRequestTrackLevelDb;

    // Arrangement chord lane (Fender-style). Host wires audition to a preview instrument.
    bool isChordLaneShown() const noexcept;
    void setChordLaneShown(bool shown);
    // The chord under `beat`, expressed as a key (root pitch class + minor/major) so the scale/pad
    // highlighting can follow the progression. Returns false when no chord covers that beat.
    bool chordKeyAtBeat(double beat, int& rootPc, bool& minor) const noexcept;
    bool duplicateSelectedChords();   // Cmd+D on selected chord blocks
    std::function<void(const std::vector<int>&)> onChordAudition;
    std::function<void()> onChordAuditionStop;
    // Fired when the chord lane changes, so the host can re-render re-harmonised audio clips.
    std::function<void()> onChordLaneChanged;

    // Public so the host can be handed a set of clips to act on (clip gain / normalize).
    struct SelectedClip
    {
        int trackIndex { -1 };
        int clipIndex { -1 };
    };

    // Clip gain / normalize. The target set is resolved at right-click time: the whole selection
    // when the clicked clip belongs to it, otherwise just the clicked clip.
    std::function<void(const std::vector<SelectedClip>&, bool relativeToLoudest)> onNormalizeClips;
    std::function<void(const std::vector<SelectedClip>&, double gainDb)> onSetClipsGainDb;
    // Match every selected clip to the loudness (LUFS, not peak) of the loudest one.
    std::function<void(const std::vector<SelectedClip>&)> onMatchClipLoudness;

    const std::vector<SelectedClip>& getSelectedClips() const noexcept { return selectedClips; }

    // A collaborator's live cursor. Carried in PROJECT coordinates (beat + track row), never
    // pixels, so it lands in the right musical place no matter how the local user has scrolled or
    // zoomed. The timeline simply draws what it is handed and knows nothing about the collab module.
    struct RemoteCursor
    {
        juce::String name;
        juce::Colour colour { juce::Colours::grey };
        double beat { 0.0 };
        double contentY { 0.0 };   // vertical position with scroll removed
    };

    void setRemoteCursors(std::vector<RemoteCursor> cursors);

    // Where a point over the arrangement sits musically (beat + track row). Used to broadcast our
    // own cursor in coordinates that mean the same thing on every collaborator's screen.
    bool pointToProjectPosition(juce::Point<int> point, double& beatOut, double& contentYOut) const;


    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void modifierKeysChanged(const juce::ModifierKeys& modifiers) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override;
    void itemDragEnter(const SourceDetails& dragSourceDetails) override;
    void itemDragMove(const SourceDetails& dragSourceDetails) override;
    void itemDragExit(const SourceDetails& dragSourceDetails) override;
    void itemDropped(const SourceDetails& dragSourceDetails) override;
    // External files dragged in from Finder / the OS (audio files → new clip/track).
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    std::optional<juce::Rectangle<int>> getSelectedTrackInspectorBounds() const noexcept;
    std::optional<int> getSelectedTrackIndex() const noexcept;
    // Selects a track (used by the mixer when its name is clicked); clears clip selection.
    void selectTrack(int trackIndex);
    // Adds an audio clip for `file` onto track `trackIndex` at `startBeat`, using the same
    // analysis / auto-warp logic as a browser drop. Returns the new clip index, or -1.
    // (Used by double-click-to-sampler so the sample also lands in the playlist.)
    int addAudioClipToTrack(const juce::File& file, int trackIndex, double startBeat);
    // Replaces the audio source of every selected AUDIO clip with `file` (keeps each clip's
    // position/length, re-analyses tempo/key). Returns how many clips were replaced.
    int replaceSelectedAudioClipsSource(const juce::File& file);
    // Records an undo checkpoint of the current timeline state (used by the recorder so
    // a finished take can be removed with Cmd+Z).
    void captureUndoSnapshot();
    // Removes the most recent undo checkpoint (used when a take is discarded/cancelled
    // so it doesn't leave a no-op entry in the undo history).
    void dropLastUndoSnapshot();

    // ---- Automation editing ----
    // When on, each track lane shows an editable envelope for `automationParam` (Volume/Pan): click
    // to add a point, drag to move, double/right-click a point to delete. Clip editing is suspended.
    void setAutomationMode(bool shouldEdit);
    bool isAutomationMode() const noexcept { return automationMode; }
    void setAutomationParam(AutomationParam p);
    AutomationParam getAutomationParam() const noexcept { return automationParam; }
    std::function<void(bool)> onAutomationModeChanged;
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;
    bool undo();
    bool redo();
    bool selectAllClips();
    bool duplicateSelectedClip();
    bool deleteSelectedClips();
    bool loopToSelectedClip();
    juce::var createPlaylistBlocksDragPayload(const std::vector<SelectedClip>& clips) const;
    void addAudioTrack();
    void addMidiTrack();
    // --- Group / folder tracks ---
    // Inserts a track at a specific index (used to place children inside a folder block).
    // Returns the index of the inserted track. Clears custom lane heights to keep them in sync.
    int  insertTrackAt(int atIndex, bool isMidi, const juce::String& name, juce::Colour colour, bool autoColour = false);
    // Index just past a folder's contiguous child block (where the next child should go).
    int  folderChildInsertIndex(int folderIndex) const noexcept;
    // The folder that owns trackIndex (if it's a folder, itself; if a child, its folder), else -1.
    int  owningFolderIndex(int trackIndex) const noexcept;
    // Toggle a folder's collapsed state (hides/shows its children).
    void toggleFolderCollapsed(int folderIndex);
    // True if the track is a child of a currently-collapsed folder (hidden from the timeline).
    bool isTrackHidden(int trackIndex) const noexcept;
    // Clears selection, hover and undo/redo history. Call after a project is
    // loaded so stale indices and snapshots from the previous project are dropped.
    void resetForNewProject();
    void setFitTrackLanesToVisibleArea(bool shouldFit);
    // Repaint ONLY the level-meter strip of the track headers, so input meters keep animating
    // while the transport is stopped. The meters are a 9 px column at the right edge of each
    // header (card reduced by 8 px, then 9 px taken off the right — see trackHeaderLayout), and
    // everything around them (name, buttons, fader) is static between frames. Invalidating the
    // whole 214 px header column would redraw all of that ~60x/second for nothing, which matters
    // here because rendering is software.
    void repaintTrackMeters()
    {
        constexpr int meterWidth = 9, rightInset = 8;
        repaint(trackHeaderWidth - rightInset - meterWidth - 1, 0, meterWidth + 2, getHeight());
    }
    // Live waveform for the clip currently being recorded (drawn until the file exists).
    void setLiveRecordingWaveform(int trackIndex, int clipIndex,
                                  std::vector<float> mins, std::vector<float> maxs);
    void clearLiveRecordingWaveform();

private:
    // Visible tool palette (top-left corner).
    enum class ToolMode
    {
        select,
        range,
        split,
        trim,
        stretch,
        fade,
        draw,
        mute,
        erase,
        audition
    };

    enum class SplitSnapMode
    {
        smart,
        bar,
        beat,
        halfBeat,
        quarterBeat,
        free
    };

    enum class DragMode
    {
        move,
        resizeRight,   // trim the right edge (constant speed, reveals/hides source)
        stretchRight,  // time-stretch from the right edge (Alt)
        resizeLeft,    // trim the left edge
        stretchLeft,   // time-stretch from the left edge (Alt)
        fadeIn,
        fadeOut,
        fadeInCurve,
        fadeOutCurve
    };

    struct ClipHit
    {
        SelectedClip clip;
        juce::Rectangle<int> bounds;
        bool overResizeHandle { false };
        bool overLeftResizeHandle { false };
        bool overFadeInHandle { false };
        bool overFadeOutHandle { false };
        bool overFadeInCurveHandle { false };
        bool overFadeOutCurveHandle { false };
    };

    enum class TrackHeaderControl
    {
        none,
        mute,
        solo,
        record,
        instrument,
        inspector,
        volume,
        volumeValue
    };

    struct TrackHeaderHit
    {
        int trackIndex { -1 };
        TrackHeaderControl control { TrackHeaderControl::none };
        juce::Rectangle<int> bounds;
    };

    struct DragState
    {
        struct ClipItem
        {
            SelectedClip clip;
            double originalStartBeat { 0.0 };
            double originalLengthInBeats { 0.0 };
        };

        SelectedClip clip;
        DragMode mode { DragMode::move };
        juce::Point<int> mouseDownPosition;
        double originalStartBeat { 0.0 };
        double originalLengthInBeats { 0.0 };
        double originalWarpTargetLengthInBeats { 0.0 };
        double originalSampleStartRatio { 0.0 };
        double originalSampleEndRatio { 1.0 };
        double originalFadeInBeats { 0.0 };
        double originalFadeOutBeats { 0.0 };
        double originalFadeInCurve { 0.0 };
        double originalFadeOutCurve { 0.0 };
        int originalTrackIndex { -1 };
        bool historyCaptured { false };
        bool copyOnDrag { false };
        bool copyCreated { false };
        std::vector<ClipItem> clipItems;
    };

    struct LoopSelectionState
    {
        enum class Mode
        {
            create,
            move,
            resizeStart,
            resizeEnd
        };

        Mode mode { Mode::create };
        double anchorBeat { 0.0 };
        double originalStartBeat { 0.0 };
        double originalEndBeat { 0.0 };
    };

    struct PlayheadDragState
    {
        bool active { false };
    };

    struct InspectorResizeState
    {
        bool active { false };
        int trackIndex { -1 };
        int mouseDownY { 0 };
        int originalHeight { 0 };
    };

    struct TrackHeaderWidthResizeState
    {
        bool active { false };
        int mouseDownX { 0 };
        int originalWidth { 0 };
    };

    struct SelectionBoxState
    {
        bool active { false };
        juce::Point<int> anchor;
        juce::Point<int> current;
    };

    struct TrackVolumeDragState
    {
        bool active { false };
        int trackIndex { -1 };
        juce::Rectangle<int> bounds;
    };

    // Drag state for the per-clip gain handle (a dot at the top-centre of each clip).
    struct ClipGainDragState
    {
        bool active { false };
        int trackIndex { -1 };
        int clipIndex { -1 };
        int startY { 0 };
        double startGainDb { 0.0 };
        bool historyCaptured { false };
    };

    enum class MultiFileDropMode
    {
        separateTracks,
        oneTrack
    };

    struct FileDropPreview
    {
        juce::Rectangle<int> bounds;
        juce::String label;
        juce::String sourcePath;
        juce::Colour colour;
        bool createsNewTrack { false };
    };

    struct TimelineSnapshot
    {
        std::vector<TrackState> tracks;
        std::optional<SelectedClip> selectedClip;
        std::vector<ChordEvent> chordTrack;
    };

    // Single source of truth for the track-header card sub-rectangles, so paint(),
    // hit-testing and the inline volume editor stay pixel-aligned.
    struct HeaderLayout
    {
        juce::Rectangle<int> card;
        juce::Rectangle<int> number;
        juce::Rectangle<int> title;
        juce::Rectangle<int> muteButton;
        juce::Rectangle<int> soloButton;
        juce::Rectangle<int> recordButton;
        juce::Rectangle<int> instrumentButton;   // MIDI tracks only (empty otherwise)
        juce::Rectangle<int> slider;
        juce::Rectangle<int> volumeValue;
        juce::Rectangle<int> meter;
        juce::Rectangle<int> collapseTriangle;   // folders only (empty otherwise)
    };
    HeaderLayout computeHeaderLayout(int trackIndex) const noexcept;

    static constexpr int folderChildIndentPx     = 16;   // child header card right-shift
    static constexpr int folderTriangleGutterPx  = 20;   // folder collapse-triangle gutter

    void timerCallback() override;
    float beatToX(double beat, juce::Rectangle<int> laneArea) const noexcept;

    // --- Chord lane ---
    juce::Rectangle<int> getChordLaneBounds() const noexcept;   // strip below the ruler
    juce::Rectangle<int> getChordLaneGridArea() const noexcept; // lane minus the track-header gutter
    int chordEventAtPoint(juce::Point<int> position) const;     // index into chordTrack, or -1
    void addChordAtBeat(double beat);
    void openChordEditorFor(int index);
    // Detect a chord progression from a dropped audio loop (background thread) and lay it on the lane.
    void detectChordsForClip(const juce::File& file, double startBeat, int numBars, int keyRoot, bool keyMinor,
                             double clipLengthBeats, double sampleStartRatio = 0.0, double sourceLengthBeats = 0.0);
    void drawChordLane(juce::Graphics& g);
    std::unique_ptr<ChordSelectorComponent> arrChordSelector;
    int editingChordIndex { -1 };
    std::set<int> selectedChords;          // chord-lane blocks selected for move/delete
    bool deleteSelectedChords();
    // Move: drag selected chord blocks in time. Marquee: rubber-band select a range on the lane.
    bool chordMoving { false };
    bool chordMoveCaptured { false };
    double chordDragAnchorBeat { 0.0 };
    double chordDragHomeStart { 0.0 };   // the "hole" the dragged block will drop into (live reorder)
    std::vector<std::pair<int, double>> chordDragOrig;   // (index, original startBeat)
    std::optional<juce::Point<int>> chordMarqueeStart;
    juce::Rectangle<int> chordMarqueeRect;
    void updateChordMarqueeSelection();
    // Drag the whole chord progression DOWN out of the lane onto a track → bake it into a MIDI clip
    // (Logic-style "chord track to MIDI"). Armed once a chord drag crosses below the lane.
    bool chordDragToTrack { false };
    int  chordDropTargetTrack { -1 };
    juce::Rectangle<int> chordDropGhost;
    void bakeChordsToMidiClip(int trackIndex);   // -1 → create a new MIDI track
    // True while a background chord analysis is running → shows an indeterminate progress bar in the lane.
    bool chordAnalysisRunning { false };
    // Auto-analyze chords when an audio loop is dropped. OFF by user request — chords are created only
    // via the explicit right-click "Analyze chords".
    bool autoDetectChordsOnImport { false };

    // --- Stem separation (Demucs, Logic-style Stem Splitter) --------------------------------------
    void separateStemsForClip(int trackIndex, int clipIndex, const std::vector<juce::String>& wantedStems);
    void applyStemResult(const orion::stems::Result& res, const TimelineClip& original, int originalTrackIndex);
    bool  stemRunning { false };
    float stemProgress { 0.0f };
    juce::String stemStatus;
    // Frames to keep repainting after playback stops so the header meters decay smoothly to zero,
    // instead of freezing (the idle timer otherwise skips repaint to save CPU).
    int meterSettleFrames { 0 };
    juce::Rectangle<int> getTrackLaneBounds(int trackIndex) const noexcept;
    juce::Rectangle<int> getClipGainHandleBounds(const TimelineClip& clip, int trackIndex) const noexcept;
    std::optional<SelectedClip> hitTestClip(juce::Point<int> position, bool midiOnly) const;
    std::optional<ClipHit> hitTestClipDetailed(juce::Point<int> position, bool midiOnly) const;
    std::optional<TrackHeaderHit> hitTestTrackHeader(juce::Point<int> position) const;
    juce::Rectangle<int> getClipBounds(const TimelineClip& clip, int trackIndex) const noexcept;
    double xToBeatDelta(int xDelta) const noexcept;
    int trackIndexFromY(int y) const noexcept;
    double snapBeatValue(double beat) const noexcept;
    double snapClipCreationBeat(double beat) const noexcept;
    bool canClipLiveOnTrack(const TimelineClip& clip, int trackIndex) const noexcept;
    void moveSelectedClipToTrack(int targetTrackIndex);
    // Shift every clip in the current drag by `deltaTracks` lanes, preserving their relative offsets.
    // Works for a single clip, a multi-selection, and Alt-drag copies. No-op unless ALL of them can
    // land on a compatible track (MIDI→MIDI, audio→audio) that exists.
    void moveDraggedClipsByTrackDelta(int deltaTracks);
    // Keyboard relocation (Logic-style: no dragging). The clips to act on are the current
    // selection, or the last-clicked clip if a track header selection cleared it.
    std::vector<SelectedClip> clipsToRelocate() const;
    // Core mover: each move is {sourceTrack, sourceClipIndex, targetTrack, newStartBeat}. Removes
    // originals and re-adds them on the targets; returns the clips' new positions for reselection.
    std::vector<SelectedClip> relocateClips(std::vector<std::tuple<int, int, int, double>> moves);
    void nudgeSelectedClipsByTracks(int delta);        // move up/down N tracks, keep the time position
    void moveSelectedClipsToSelectedTrack();           // move onto the selected track header, keep time
    void moveSelectedClipsToPlayhead();                // move onto the playhead, same track(s)
    // Splits a clip at an absolute timeline beat into two clips (non-destructive).
    // Returns true if a split happened. splitBeat must lie strictly inside the clip.
    bool splitClipAtBeat(int trackIndex, int clipIndex, double splitBeat);
    double snapSplitBeatToClipEdge(double splitBeat, const TimelineClip& clip) const noexcept;
    double snapSplitBeatForClip(double splitBeat, const TimelineClip& clip) const noexcept;
    double getSplitSnapStepInBeats() const noexcept;
    juce::String getSplitSnapName() const;
    // Splits every selected clip (or the clip under the playhead) at the playhead.
    void splitSelectionAtPlayhead();
    // Ableton-style split: split at the last clicked beat inside the selected clip.
    // Falls back to the playhead when no focused beat is available.
    void splitSelectionAtFocusedBeat();
    juce::Rectangle<int> getEditToolbarBounds() const noexcept;
    // Bounds of edit-toolbar button `index`:
    // 0 Cursor, 1 Range, 2 Cut, 3 Trim, 4 Stretch, 5 Draw, 6 Mute, 7 Erase, 8 Audition.
    juce::Rectangle<int> getToolButtonBounds(int index) const noexcept;
    juce::Rectangle<int> getSplitSnapButtonBounds() const noexcept;
    void paintToolPalette(juce::Graphics& g);
    bool handleEditToolbarClick(juce::Point<int> position);
    void showSplitSnapMenu();
    void pushUndoSnapshot();
    void restoreSnapshot(const TimelineSnapshot& snapshot);
    bool hasTimelineChangedSince(const TimelineSnapshot& snapshot) const noexcept;
    void clampScrollOffsets();
    void adjustZoom(double horizontalDelta, double verticalDelta, std::optional<juce::Point<int>> focusPoint);
    // Used after adding/dropping a clip to keep scroll bounds valid without changing the
    // user's current horizontal zoom. Max zoom-out handles content width separately.
    void ensureBeatVisible(double endBeat);
    double getTimelineEndBeats() const noexcept;
    // Content-adaptive max-zoom-out floor: the smallest pixels-per-beat allowed, so zooming out
    // fits the content (+margin) into the viewport instead of a tiny sliver in a huge empty grid.
    double minZoomPixelsPerBeat() const noexcept;
    double autoFitTimelineBeats() const noexcept;
    void applyTimelineAutoFit();
    // Ableton "Optimize Arrangement Width" (W / double-click ruler): fit all content to the width.
    void zoomToFitContent();
    int getLaneHeightForTrack(int trackIndex) const noexcept;
    int getTrackTopForIndex(int trackIndex) const noexcept;
    int getTotalTrackHeight() const noexcept;
    double xToBeatPosition(int x) const noexcept;
    void clearBrowserDropPreview();
    void updateBrowserDropPreview(const juce::Point<int>& position, const juce::var& description);
    void clearExternalFileDropPreview();
    void updateExternalFileDropPreview(const juce::StringArray& files, juce::Point<int> position);
    void importDroppedAudioFiles(const juce::StringArray& files, juce::Point<int> position, MultiFileDropMode mode);
    // Imports an audio file (from an external drag) as a clip on the track under the
    // cursor, or appends a new audio track. Returns false if the file isn't audio.
    bool importAudioFileAt(const juce::File& file,
                           juce::Point<int> position,
                           bool captureHistory = true,
                           std::optional<int> forcedTrackIndex = std::nullopt,
                           std::optional<double> forcedStartBeat = std::nullopt,
                           bool forceCreateNewTrack = false);
    void notifyClipSelectionChanged();
    bool isClipSelected(const SelectedClip& clip) const noexcept;
    void setSingleSelection(std::optional<SelectedClip> clip);
    void selectRangeTo(const SelectedClip& targetClip);
    void updateSelectionBox(const juce::Point<int>& position);
    juce::Rectangle<int> getSelectionBoxBounds() const noexcept;
    void createDragCopiesIfNeeded();
    juce::String makeUniqueTrackName(const juce::String& baseName) const;
    void showAddTrackMenu();
    void createMidiClipAt(int trackIndex, double startBeat);
    void deleteSelectedTrack();
    void deleteSelectedTracks();   // remove every track selected via its header (Cmd/Shift-click)
    juce::Rectangle<int> getAddTrackButtonBounds() const noexcept;
    juce::Rectangle<int> getChordLaneToggleBounds() const noexcept;
    juce::Rectangle<int> getChordOctaveUpBounds() const noexcept;
    juce::Rectangle<int> getChordOctaveDownBounds() const noexcept;
    juce::Rectangle<int> getTrackVolumeValueBounds(int trackIndex) const noexcept;
    void updateTrackVolumeFromPoint(int trackIndex, juce::Rectangle<int> sliderBounds, int x);
    void showTrackVolumeEditor(int trackIndex);
    void commitTrackVolumeEditor(bool applyChanges);

    struct AudioPeaks
    {
        std::vector<float> minVals;
        std::vector<float> maxVals;
        int samplesPerBucket { 256 };
    };
    const AudioPeaks* getOrComputePeaks(const juce::String& path);
    std::map<std::string, AudioPeaks> waveformCache;
    // Waveform peaks are built on a background thread so dropping a clip stays seamless
    // (reading a long file to compute peaks must not block the UI). Cache + pending set
    // are touched only on the message thread; results are posted back via callAsync.
    std::set<std::string> waveformPending;
    juce::ThreadPool waveformPool { 1 };

    ProjectState& project;
    TransportEngine& transport;
    std::optional<SelectedClip> selectedClip;
    std::optional<SelectedClip> lastClickedClip;
    std::optional<int> selectedTrackIndex;   // anchor / primary selected track
    std::set<int> selectedTrackIndices;      // all selected track headers (Cmd/Shift-click)
    std::vector<SelectedClip> selectedClips;
    std::optional<DragState> dragState;
    bool playlistBlocksDragStarted { false };
    std::optional<LoopSelectionState> loopSelectionState;
    PlayheadDragState playheadDragState;
    InspectorResizeState inspectorResizeState;
    TrackHeaderWidthResizeState trackHeaderWidthResizeState;
    SelectionBoxState selectionBoxState;
    TrackVolumeDragState trackVolumeDragState;
    ClipGainDragState clipGainDragState;
    juce::TextEditor trackVolumeInlineEditor;
    std::optional<int> volumeEditorTrackIndex;
    std::optional<ClipHit> hoverClip;
    std::optional<double> focusedSplitBeat;
    std::optional<double> knifePreviewBeat;
    std::vector<TimelineSnapshot> undoStack;
    std::vector<TimelineSnapshot> redoStack;
    
    double pixelsPerBeat { 6.0 };
    double verticalZoom { 1.0 };
    double scrollX { 0.0 };
    double scrollY { 0.0 };
    // Smooth zoom: wheel/pinch set targets + a pinned focus; the timer eases the actual zoom toward
    // them so the playlist glides open/closed instead of stepping a notch per event.
    double targetPixelsPerBeat { 6.0 };
    double targetVerticalZoom { 1.0 };
    bool zoomAnimating { false };
    double zoomFocusBeat { 0.0 };
    double zoomFocusXInView { 0.0 };
    double zoomFocusTrackRatio { 0.5 };
    double zoomFocusYInView { 0.0 };
    bool timelineAutoFitActive { true };
    double pendingMagnifyDelta { 0.0 };
    double ignoreWheelUntilMs { 0.0 };
    int trackHeaderWidth { 214 };

    // ---- Automation editing state ----
    bool automationMode { false };
    AutomationParam automationParam { AutomationParam::trackVolume };
    int automationDragTrack { -1 };   // track whose point is being dragged (-1 = none)
    int automationDragPoint { -1 };   // index of the dragged point
    // The strip within a track lane used to draw/edit the envelope (grid area, a little inset).
    juce::Rectangle<int> automationLaneGrid(int trackIndex) const noexcept;
    juce::Rectangle<int> automationParamChipBounds(int trackIndex) const noexcept;   // param selector in the header
    float automationValueToY(float value, juce::Rectangle<int> grid) const noexcept;
    float automationYToValue(float y, juce::Rectangle<int> grid) const noexcept;
    void drawAutomationOverlay(juce::Graphics&);
    int  automationPointAt(int trackIndex, juce::Point<int> pos) const;   // -1 = none
    bool handleAutomationMouseDown(const juce::MouseEvent& event);
    bool handleAutomationMouseDrag(const juce::MouseEvent& event);
    bool handleAutomationMouseUp();

    std::vector<RemoteCursor> remoteCursors;
    void drawRemoteCursors(juce::Graphics& g);
    bool fitTrackLanesToVisibleArea { false };
    std::map<int, int> customTrackHeights;
    std::optional<juce::Rectangle<int>> browserDropPreviewBounds;
    std::optional<double> browserDropSnapBeat;
    juce::String browserDropPreviewSourcePath;
    juce::Colour browserDropPreviewColour { orion::theme::warm::red };
    bool browserDropCreatesNewTrack { false };
    std::vector<FileDropPreview> externalFileDropPreviews;
    // When the playlist is full and a browser audio item is dragged in, an empty lane is
    // freed below the LAST track; the new track materialises there on drop. The lane
    // opens/closes smoothly via browserAppendAnim (0..1), driven by the timer.
    bool  browserAppendActive { false };
    float browserAppendAnim { 0.0f };
    ToolMode currentTool { ToolMode::select };
    SplitSnapMode splitSnapMode { SplitSnapMode::smart };

    int liveWaveformTrack { -1 };
    int liveWaveformClip { -1 };
    std::vector<float> liveWaveformMin;
    std::vector<float> liveWaveformMax;
};
}  // namespace orion
