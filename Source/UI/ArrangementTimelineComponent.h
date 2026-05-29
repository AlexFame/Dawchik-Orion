#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../Audio/TransportEngine.h"
#include "../Core/ProjectState.h"

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
    std::function<void(int, int)> onClipSelectionChanged;
    std::function<void(int)> onTrackHeaderDoubleClick;
    std::function<void(int)> onTrackHeaderRightClick;

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
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;
    bool undo();
    bool redo();
    void addAudioTrack();
    void addMidiTrack();

private:
    struct SelectedClip
    {
        int trackIndex { -1 };
        int clipIndex { -1 };
    };

    enum class DragMode
    {
        move,
        resizeRight,
        stretchRight
    };

    struct ClipHit
    {
        SelectedClip clip;
        juce::Rectangle<int> bounds;
        bool overResizeHandle { false };
    };

    enum class TrackHeaderControl
    {
        none,
        mute,
        solo,
        record,
        inspector,
        volume
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
        int mouseDownY { 0 };
        int originalHeight { 0 };
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
    void pushUndoSnapshot();
    void restoreSnapshot(const TimelineSnapshot& snapshot);
    bool hasTimelineChangedSince(const TimelineSnapshot& snapshot) const noexcept;
    void clampScrollOffsets();
    void adjustZoom(double horizontalDelta, std::optional<juce::Point<int>> focusPoint);
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
    void updateTrackVolumeFromPoint(int trackIndex, juce::Rectangle<int> sliderBounds, int x);

    struct AudioPeaks
    {
        std::vector<float> minVals;
        std::vector<float> maxVals;
        int samplesPerBucket { 256 };
    };
    const AudioPeaks* getOrComputePeaks(const juce::String& path);
    std::map<std::string, AudioPeaks> waveformCache;

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
    SelectionBoxState selectionBoxState;
    TrackVolumeDragState trackVolumeDragState;
    std::optional<ClipHit> hoverClip;
    std::vector<TimelineSnapshot> undoStack;
    std::vector<TimelineSnapshot> redoStack;
    
    double pixelsPerBeat { 6.0 };
    double scrollX { 0.0 };
    double scrollY { 0.0 };
    double pendingMagnifyDelta { 0.0 };
    double ignoreWheelUntilMs { 0.0 };
    int selectedTrackExpandedHeight { 104 };
    std::optional<juce::Rectangle<int>> browserDropPreviewBounds;
    juce::Colour browserDropPreviewColour { juce::Colour(0xffeb6f3a) };
};
}  // namespace orion
