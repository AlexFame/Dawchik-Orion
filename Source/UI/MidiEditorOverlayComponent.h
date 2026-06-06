#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <map>
#include <set>
#include <vector>

#include "../Core/ProjectState.h"

namespace orion
{
class MidiEditorOverlayComponent final : public juce::Component,
                                         private juce::Timer,
                                         private juce::Button::Listener
{
public:
    MidiEditorOverlayComponent();
    ~MidiEditorOverlayComponent() override;

    void openClip(TrackState& trackState, TimelineClip& clipState,
                  int projectKeyRoot = 0, bool projectKeyIsMinor = true,
                  bool initialScaleLock = true);
    // Sync the scale from outside (e.g. project key changed while editor is open).
    void setProjectKey(int rootSemitones, bool minor);
    void setScaleLockExternally(bool enabled);
    void closeEditor();
    std::function<void()> onClose;
    std::function<void()> onTogglePlayback;
    std::function<void()> onStopAndRewindToClipStart;
    std::function<double()> onRequestPlayheadBeat;
    std::function<bool()> onRequestPlayingState;
    std::function<void(bool)> onScaleLockChanged; // fired when the in-editor toggle is flipped
    std::function<void(int, int)> onPreviewNoteOn;
    std::function<void(int)> onPreviewNoteOff;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    bool stepWriteMidiNoteOn(int midiNote, int velocity);
    bool stepWriteMidiNoteOff(int midiNote);
    void focusLost(FocusChangeType cause) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;

private:
    void timerCallback() override;
    struct SelectedNote
    {
        int noteIndex { -1 };
    };

    enum class NoteDragMode
    {
        move,
        resizeRight
    };

    struct NoteHit
    {
        SelectedNote selected;
        juce::Rectangle<int> bounds;
        bool overResizeHandle { false };
    };

    struct NoteDragState
    {
        SelectedNote selected;
        NoteDragMode mode { NoteDragMode::move };
        juce::Point<int> mouseDownPosition;
        double originalStartBeat { 0.0 };
        double originalLengthInBeats { 0.0 };
        int originalPitch { 60 };
        std::vector<MidiNote> originalSelectedNotes;
        std::vector<int> selectedIndices;
        bool historyCaptured { false };
    };

    struct MarqueeState
    {
        juce::Point<int> origin;
        juce::Rectangle<int> bounds;
        bool movedEnough { false };
    };

    struct NoteSnapshot
    {
        std::vector<MidiNote> midiNotes;
        std::vector<PitchSlide> pitchSlides;
        std::set<int> selectedNotes;
        std::optional<int> selectedSlide;
        double horizontalZoom { 1.0 };
        double verticalZoom { 1.0 };
        int scaleRoot { 0 };
        int scalePatternIndex { 0 };
        double snapSizeInBeats { 0.25 };
        double stepWriteCursorBeat { 0.0 };
        double stepWriteStepLengthInBeats { 0.25 };
        bool focusModeEnabled { false };
    };

    struct VelocityDragState
    {
        std::vector<int> targetIndices;
        std::vector<int> originalVelocities;
        juce::Point<int> mouseDownPosition;
        bool historyCaptured { false };
    };

    struct SlideDrawState
    {
        PitchSlide slide;
        bool movedEnough { false };
    };

    enum class SlideVisibilityMode
    {
        off,
        ghost,
        active
    };

    enum class SlideEditMode
    {
        move,
        resizeStart,
        resizeEnd
    };

    struct SlideHit
    {
        int slideIndex { -1 };
        SlideEditMode mode { SlideEditMode::move };
    };

    struct SlideEditState
    {
        int slideIndex { -1 };
        SlideEditMode mode { SlideEditMode::move };
        juce::Point<int> mouseDownPosition;
        PitchSlide originalSlide;
        bool historyCaptured { false };
    };

    void buttonClicked(juce::Button* button) override;
    juce::Rectangle<int> getTopBarBounds() const noexcept;
    juce::Rectangle<int> getKeyboardBounds() const noexcept;
    juce::Rectangle<int> getGridBounds() const noexcept;
    juce::Rectangle<int> getVelocityLaneBounds() const noexcept;
    juce::Rectangle<int> getNoteBounds(const MidiNote& note) const noexcept;
    juce::Rectangle<int> getVelocityBarBounds(int noteIndex) const noexcept;
    double getPixelsPerBeat() const noexcept;
    double getVisibleBeatRange() const noexcept;
    std::optional<NoteHit> hitTestNote(juce::Point<int> position) const;
    std::optional<SlideHit> hitTestSlide(juce::Point<int> position) const;
    std::optional<int> hitTestVelocityBar(juce::Point<int> position) const;
    std::set<int> hitTestNotesInRect(juce::Rectangle<int> selection) const;
    int yToPitch(int y) const noexcept;
    double yToContinuousPitch(double y) const noexcept;
    float pitchToCentreY(double pitch) const noexcept;
    int laneIndexToPitch(int laneIndex) const noexcept;
    double xToBeat(double x) const noexcept;
    double snapBeat(double beat) const noexcept;
    double snapBeatNearest(double beat) const noexcept;
    int pitchToLane(int pitch) const noexcept;
    int getDisplayedLaneCount() const noexcept;
    bool isPitchInScale(int pitch) const noexcept;
    int  snapPitchToScale(int pitch) const noexcept;
    bool isBlackKey(int pitch) const noexcept;
    bool isNoteSelected(int noteIndex) const noexcept;
    void selectSingleNote(int noteIndex);
    void duplicateSelectedNotes();
    void deleteSelectedNotes();
    void deleteSelectedSlide();
    void quantizeSelectedNotes();
    void updateSubtitle();
    void clearSelection();
    void pushUndoSnapshot();
    void restoreSnapshot(const NoteSnapshot& snapshot);
    bool notesChangedSince(const NoteSnapshot& snapshot) const noexcept;
    void updateVelocityFromPosition(int y);
    void focusViewportAroundClipNotes();
    bool shouldConsumeFocusClick() const noexcept;
    void adjustZoom(double horizontalDelta, double verticalDelta, std::optional<juce::Point<int>> focusPoint = std::nullopt);
    void clampScrollOffsets();
    juce::Rectangle<int> getVisibleGridViewport() const noexcept;
    juce::String getScaleName() const;
    juce::String getSnapName() const;
    void showScaleMenu();
    void showSnapMenu();
    void showStepLengthMenu();
    bool updateLiveKeyboardPitches();
    bool updateStepWriteKeyboardPitches();
    void commitStepWritePendingChord();
    void advanceStepWriteCursor(int stepCount);
    void restStepWrite();
    void backstepStepWrite();
    void extendStepWritePreviousNotes();
    double getSnappedStepWriteCursorBeat() const noexcept;
    int defaultStepWriteVelocity() const noexcept;
    bool appendSlidePoint(PitchSlide& slide, juce::Point<int> position) const;
    std::optional<PitchSlide> makeSlideAt(juce::Point<int> position) const;
    void smoothSlide(PitchSlide& slide) const;
    bool slideTouchesSelectedNotes(const PitchSlide& slide) const noexcept;
    bool shouldDrawSlide(const PitchSlide& slide, int slideIndex, bool drawingPreview) const noexcept;
    juce::String getSlideVisibilityName() const;
    std::optional<int> keyboardPitchForPoint(juce::Point<int> position) const noexcept;
    void setMousePreviewPitch(std::optional<int> pitch);
    void releaseMousePreviewPitch();
    void releaseLiveKeyboardPitches();
    void auditionPlacedNote(int pitch, int velocity);
    void releasePlacedNotePreview();

    juce::String trackName;
    juce::String clipName;
    juce::Colour trackColour { juce::Colour(0xff5b84d6) };
    TrackState* activeTrack { nullptr };
    TimelineClip* activeClip { nullptr };
    std::set<int> selectedNotes;
    std::optional<int> selectedSlide;
    std::set<int> liveKeyboardPitches;
    std::optional<int> mousePreviewPitch;
    std::optional<int> placedNotePreviewPitch;
    double placedNotePreviewOffMs { 0.0 };
    std::optional<NoteHit> hoverNote;
    std::optional<NoteDragState> noteDragState;
    std::optional<MarqueeState> marqueeState;
    std::optional<VelocityDragState> velocityDragState;
    std::optional<SlideDrawState> slideDrawState;
    std::optional<SlideEditState> slideEditState;

    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::Label contextLabel;
    juce::Label scaleLabel;
    juce::Label snapLabel;
    juce::ToggleButton focusToggle;
    juce::TextButton scaleButton;
    juce::TextButton snapButton;
    juce::TextButton quantizeButton;
    juce::TextButton slidePenButton;
    juce::TextButton slideVisibilityButton;
    juce::TextButton stepWriteButton;
    juce::TextButton stepLengthButton;
    juce::TextButton stepRestButton;
    juce::TextButton stepBackButton;
    juce::TextButton stepTieButton;
    juce::TextButton closeButton;
    juce::ToggleButton scaleLockToggle;
    juce::Label scaleLockLabel;
    double horizontalZoom { 1.0 };
    double verticalZoom { 1.0 };
    double scrollX { 0.0 };
    double scrollY { 0.0 };
    double pendingMagnifyDelta { 0.0 };
    double ignoreWheelUntilMs { 0.0 };
    int scaleRoot { 0 };
    int scalePatternIndex { 0 };
    double snapSizeInBeats { 0.25 };
    double stepWriteCursorBeat { 0.0 };
    double stepWriteStepLengthInBeats { 0.25 };
    bool focusModeEnabled { false };
    bool scaleLockEnabled { true };  // new notes snap to in-scale pitches when true
    bool stepWriteEnabled { false };
    bool slidePenEnabled { false };
    SlideVisibilityMode slideVisibilityMode { SlideVisibilityMode::ghost };
    bool hasStoredViewportBeforeFocus { false };
    bool ignoreNextMouseDown { false };
    std::set<int> stepWriteLivePitches;
    std::map<int, int> stepWritePendingVelocities;
    std::vector<NoteSnapshot> undoStack;
    std::vector<NoteSnapshot> redoStack;
};
}  // namespace orion
