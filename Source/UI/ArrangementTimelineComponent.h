#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../Audio/TransportEngine.h"
#include "../Core/ProjectState.h"
#include "OrionTheme.h"

namespace orion
{
class ArrangementTimelineComponent final : public juce::Component,
                                           public juce::DragAndDropTarget,
                                           private juce::Timer
{
public:
    ArrangementTimelineComponent(ProjectState& projectState, TransportEngine& transportEngine);
    ~ArrangementTimelineComponent() override;

    std::function<void(int, int)> onMidiClipDoubleClick;
    std::function<void(int, int)> onAudioClipDoubleClick;
    std::function<void(int, int)> onClipSelectionChanged;
    std::function<void(int)> onTrackHeaderDoubleClick;
    std::function<void(int)> onTrackHeaderRightClick;
    // Fired when the instrument button on a MIDI track header is clicked. The host opens
    // the plugin editor if an instrument is loaded, or the instrument picker if not.
    std::function<void(int)> onTrackInstrumentClicked;
    std::function<void()> onTogglePlayback;
    // Fired when the playhead is moved by clicking/scrubbing the ruler, so the host can
    // re-sync the audio engine to the new position (jump even while playing).
    std::function<void()> onTransportSeek;
    // Returns the current 0..1 output level for a track (for the header meter).
    std::function<float(int)> onRequestTrackLevel;
    // Returns the current 0..1 left/right output levels for a track (stereo meter).
    std::function<std::pair<float, float>(int)> onRequestTrackLevelStereo;
    // Returns the current live signal level in dB (-100 ≈ silent → "-inf").
    std::function<float(int)> onRequestTrackLevelDb;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override;
    void itemDragEnter(const SourceDetails& dragSourceDetails) override;
    void itemDragMove(const SourceDetails& dragSourceDetails) override;
    void itemDragExit(const SourceDetails& dragSourceDetails) override;
    void itemDropped(const SourceDetails& dragSourceDetails) override;
    std::optional<juce::Rectangle<int>> getSelectedTrackInspectorBounds() const noexcept;
    std::optional<int> getSelectedTrackIndex() const noexcept;
    // Selects a track (used by the mixer when its name is clicked); clears clip selection.
    void selectTrack(int trackIndex);
    // Records an undo checkpoint of the current timeline state (used by the recorder so
    // a finished take can be removed with Cmd+Z).
    void captureUndoSnapshot();
    // Removes the most recent undo checkpoint (used when a take is discarded/cancelled
    // so it doesn't leave a no-op entry in the undo history).
    void dropLastUndoSnapshot();
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;
    bool undo();
    bool redo();
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
    // Live waveform for the clip currently being recorded (drawn until the file exists).
    void setLiveRecordingWaveform(int trackIndex, int clipIndex,
                                  std::vector<float> mins, std::vector<float> maxs);
    void clearLiveRecordingWaveform();

private:
    struct SelectedClip
    {
        int trackIndex { -1 };
        int clipIndex { -1 };
    };

    // Visible tool palette (top-left corner). Pointer = select/move/trim, Knife = split.
    enum class ToolMode
    {
        pointer,
        knife
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

    struct TimelineSnapshot
    {
        std::vector<TrackState> tracks;
        std::optional<SelectedClip> selectedClip;
    };

    // Single source of truth for the track-header card sub-rectangles, so paint(),
    // hit-testing and the inline volume editor stay pixel-aligned.
    struct HeaderLayout
    {
        juce::Rectangle<int> card;
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
    juce::Rectangle<int> getTrackLaneBounds(int trackIndex) const noexcept;
    std::optional<SelectedClip> hitTestClip(juce::Point<int> position, bool midiOnly) const;
    std::optional<ClipHit> hitTestClipDetailed(juce::Point<int> position, bool midiOnly) const;
    std::optional<TrackHeaderHit> hitTestTrackHeader(juce::Point<int> position) const;
    juce::Rectangle<int> getClipBounds(const TimelineClip& clip, int trackIndex) const noexcept;
    double xToBeatDelta(int xDelta) const noexcept;
    int trackIndexFromY(int y) const noexcept;
    double snapBeatValue(double beat) const noexcept;
    bool canClipLiveOnTrack(const TimelineClip& clip, int trackIndex) const noexcept;
    void moveSelectedClipToTrack(int targetTrackIndex);
    // Splits a clip at an absolute timeline beat into two clips (non-destructive).
    // Returns true if a split happened. splitBeat must lie strictly inside the clip.
    bool splitClipAtBeat(int trackIndex, int clipIndex, double splitBeat);
    // Splits every selected clip (or the clip under the playhead) at the playhead.
    void splitSelectionAtPlayhead();
    // Bounds of tool-palette button `index` (0 = pointer, 1 = knife) in the corner.
    juce::Rectangle<int> getToolButtonBounds(int index) const noexcept;
    void paintToolPalette(juce::Graphics& g);
    void pushUndoSnapshot();
    void restoreSnapshot(const TimelineSnapshot& snapshot);
    bool hasTimelineChangedSince(const TimelineSnapshot& snapshot) const noexcept;
    void clampScrollOffsets();
    void adjustZoom(double horizontalDelta, double verticalDelta, std::optional<juce::Point<int>> focusPoint);
    // Zooms out (only when needed) so the given end-beat is visible with some margin.
    // Used after dropping a clip so the user immediately sees its full extent.
    void ensureBeatVisible(double endBeat);
    double getTimelineEndBeats() const noexcept;
    int getLaneHeightForTrack(int trackIndex) const noexcept;
    int getTrackTopForIndex(int trackIndex) const noexcept;
    int getTotalTrackHeight() const noexcept;
    double xToBeatPosition(int x) const noexcept;
    void clearBrowserDropPreview();
    void updateBrowserDropPreview(const juce::Point<int>& position, const juce::var& description);
    void notifyClipSelectionChanged();
    bool isClipSelected(const SelectedClip& clip) const noexcept;
    void setSingleSelection(std::optional<SelectedClip> clip);
    void selectRangeTo(const SelectedClip& targetClip);
    void updateSelectionBox(const juce::Point<int>& position);
    juce::Rectangle<int> getSelectionBoxBounds() const noexcept;
    juce::String makeUniqueTrackName(const juce::String& baseName) const;
    void showAddTrackMenu();
    void createMidiClipAt(int trackIndex, double startBeat);
    void deleteSelectedTrack();
    juce::Rectangle<int> getAddTrackButtonBounds() const noexcept;
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
    std::optional<int> selectedTrackIndex;
    std::vector<SelectedClip> selectedClips;
    std::optional<DragState> dragState;
    std::optional<LoopSelectionState> loopSelectionState;
    PlayheadDragState playheadDragState;
    InspectorResizeState inspectorResizeState;
    TrackHeaderWidthResizeState trackHeaderWidthResizeState;
    SelectionBoxState selectionBoxState;
    TrackVolumeDragState trackVolumeDragState;
    juce::TextEditor trackVolumeInlineEditor;
    std::optional<int> volumeEditorTrackIndex;
    std::optional<ClipHit> hoverClip;
    std::vector<TimelineSnapshot> undoStack;
    std::vector<TimelineSnapshot> redoStack;
    
    double pixelsPerBeat { 6.0 };
    double verticalZoom { 1.0 };
    double scrollX { 0.0 };
    double scrollY { 0.0 };
    double pendingMagnifyDelta { 0.0 };
    double ignoreWheelUntilMs { 0.0 };
    int trackHeaderWidth { 214 };
    bool fitTrackLanesToVisibleArea { false };
    std::map<int, int> customTrackHeights;
    std::optional<juce::Rectangle<int>> browserDropPreviewBounds;
    juce::Colour browserDropPreviewColour { orion::theme::warm::red };
    bool browserDropCreatesNewTrack { false };
    ToolMode currentTool { ToolMode::pointer };

    int liveWaveformTrack { -1 };
    int liveWaveformClip { -1 };
    std::vector<float> liveWaveformMin;
    std::vector<float> liveWaveformMax;
};
}  // namespace orion
