#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "../Core/ProjectState.h"
#include "ChordSelectorComponent.h"
#include "../CameraChordWheel/CameraChordWheelComponent.h"

#include <memory>

namespace orion
{
class MidiEditorOverlayComponent final : public juce::Component,
                                         public juce::DragAndDropContainer,
                                         public juce::DragAndDropTarget,
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
    // Chord mode synced from the project (shared with sampler / track header / hardware MIDI).
    void setChordModeExternally(bool enabled, int chordSize);
    // Lock taken around note/slide vector edits so the audio thread never reads them mid-reallocation.
    void setAudioEditLock(juce::CriticalSection* lock) noexcept { audioEditLock = lock; }
    void closeEditor();
    std::function<void()> onClose;
    std::function<void()> onTogglePlayback;
    std::function<void()> onStopAndRewindToClipStart;
    std::function<double()> onRequestPlayheadBeat;
    std::function<bool()> onRequestPlayingState;
    std::function<void(bool)> onScaleLockChanged; // fired when the in-editor toggle is flipped
    std::function<void(bool)> onChordModeChanged; // fired when the in-editor Chord toggle is flipped
    std::function<void(int)> onChordSizeChanged;  // fired when the chord size (3/7/9/11/13) is picked
    std::function<void(int, int)> onPreviewNoteOn;
    std::function<void(int)> onPreviewNoteOff;
    std::function<void()> onPreviewChordRetrigger;
    std::function<void(double)> onStartGlobalSpacePreview;
    std::function<void()> onStopGlobalSpacePreview;
    // A short space TAP promotes the running preview into normal playback (keeps playing,
    // no rewind); a HOLD stops on release. This commits the tap case.
    std::function<void()> onCommitGlobalSpacePreview;
    // Live portamento toggle: when on, every note played glides from the previous pitch.
    std::function<void(bool)> onGlideChanged;

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

    // DragAndDropTarget: receive a chord dragged out of the chord selector.
    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDragMove(const SourceDetails& details) override;
    void itemDragExit(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;

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
        resizeEnd,
        curve          // drag a segment's midpoint handle to bend it
    };

    enum class PianoRollTool
    {
        select,
        draw,
        cut,
        erase,
        quantize,
        velocity,
        slide,
        step,
        audition
    };

    struct SlideHit
    {
        int slideIndex { -1 };
        SlideEditMode mode { SlideEditMode::move };
        int segmentIndex { -1 };   // for curve mode: index of the segment's leading point
    };

    struct SlideEditState
    {
        int slideIndex { -1 };
        SlideEditMode mode { SlideEditMode::move };
        int segmentIndex { -1 };
        juce::Point<int> mouseDownPosition;
        PitchSlide originalSlide;
        bool historyCaptured { false };
    };

    void buttonClicked(juce::Button* button) override;
    juce::Rectangle<int> getTopBarBounds() const noexcept;
    juce::Rectangle<int> getEditToolbarBounds() const noexcept;
    juce::Rectangle<int> getToolButtonBounds(int index) const noexcept;
    juce::Rectangle<int> getKeyboardBounds() const noexcept;
    juce::Rectangle<int> getGridBounds() const noexcept;
    juce::Rectangle<int> getVelocityLaneBounds() const noexcept;
    juce::Rectangle<int> getNoteBounds(const MidiNote& note) const noexcept;
    juce::Rectangle<int> getVelocityBarBounds(int noteIndex) const noexcept;
    double getPixelsPerBeat() const noexcept;
    double getVisibleBeatRange() const noexcept;
    double getContentBeats() const noexcept;   // clip length + editing headroom (drawable width)
    void   growClipToFit(double endBeat);       // extend the clip (snapped up to a bar) so notes fit
    void   shrinkClipToFitNotes();              // pull the clip back to its content (bar-snapped, min 1 bar)
    void   preserveHorizontalScaleAcross(double oldContentBeats); // keep pixels-per-beat when the clip length changes
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
    // Chord mode: expand a base pitch into a diatonic chord in the editor's scale (or {base} when off).
    std::vector<int> chordPitchesForNote(int basePitch) const;
    std::set<int> chordPitchSetForNote(int basePitch) const;
    bool isBlackKey(int pitch) const noexcept;
    bool isNoteSelected(int noteIndex) const noexcept;
    void selectSingleNote(int noteIndex);
    void transposeSelectedNotes(int semitones);
    void duplicateSelectedNotes();
    void deleteSelectedNotes();
    // MPE-style glide: JOIN merges the selected notes into one voice that glides between
    // their pitches (drawn as a connected slide line); SPLIT turns a slide back into notes.
    void joinSelectedNotes();
    void splitSelectedSlide();
    void deleteSelectedSlide();
    // Pitch-curve presets (MidiSketch-style): build a ready-made slide shape on each selected
    // note, and adjust the selected slide's vibrato depth/rate.
    void showSlidePresetMenu();
    void applySlidePreset(int presetId);
    void setSelectedSlideVibrato(double depthSemitones, double rateCyclesPerBeat, int shape);
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
    void paintEditToolbar(juce::Graphics& g);
    bool handleEditToolbarClick(juce::Point<int> position);
    bool splitNoteAtPoint(juce::Point<int> position);
    bool auditionNoteAtPoint(juce::Point<int> position);
    void setPianoRollTool(PianoRollTool tool);
    void activateToolByIndex(int index);   // shared by toolbar clicks and 1..9 number-key shortcuts
    // FL-style tool cursors: the pointer becomes the active tool's glyph over the grid. Built lazily & cached.
    const juce::MouseCursor& toolCursor(PianoRollTool tool);
    std::map<PianoRollTool, juce::MouseCursor> toolCursorCache;
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
    bool isTextInputFocused() const noexcept;
    bool canStartGlobalSpacePreview() const noexcept;
    bool startGlobalSpacePreviewFromMouse();
    // Hold-Command preview: starts from the hovered beat (or the playhead if the mouse isn't
    // over the grid) and plays while Cmd is held.
    bool startGlobalSpacePreviewHeld();
    void stopGlobalSpacePreview();

    juce::String trackName;
    juce::String clipName;
    juce::Colour trackColour { juce::Colour(0xff5b84d6) };
    juce::Colour clipColour { juce::Colour(0xff5b84d6) };
    TrackState* activeTrack { nullptr };
    TimelineClip* activeClip { nullptr };
    std::set<int> selectedNotes;
    std::optional<int> selectedSlide;
    std::set<int> liveKeyboardPitches;
    std::set<int> liveKeyboardBasePitches;
    std::optional<int> mousePreviewPitch;
    std::optional<int> placedNotePreviewPitch;
    double placedNotePreviewOffMs { 0.0 };
    std::optional<double> hoveredGridBeat;
    bool globalSpacePreviewActive { false };
    // Hold-Command preview: start is debounced so Cmd+key shortcuts (Cmd+Z…) don't blip it.
    bool cmdWasDown { false };
    bool cmdPreviewPending { false };
    juce::uint32 cmdDownMs { 0 };
    std::optional<NoteHit> hoverNote;
    std::optional<NoteDragState> noteDragState;
    std::optional<MarqueeState> marqueeState;
    std::optional<VelocityDragState> velocityDragState;
    std::optional<SlideDrawState> slideDrawState;
    std::optional<SlideEditState> slideEditState;
    PianoRollTool currentTool { PianoRollTool::select };

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
    juce::TextButton joinButton;
    juce::TextButton splitButton;
    juce::TextButton modButton;   // cycles the selected glide's LFO/vibrato shape
    juce::TextButton presetsButton;  // popup of pitch-curve presets (scoop/fall/vibrato/…)
    juce::TextButton glideButton;
    juce::TextButton closeButton;
    juce::ToggleButton scaleLockToggle;
    juce::TextButton chordToggle;
    juce::TextButton cameraChordButton { "Camera" };
    juce::Label scaleLockLabel;
    std::unique_ptr<ChordSelectorComponent> chordSelector;   // opened from the Chord button
    std::unique_ptr<CameraChordWheelComponent> cameraChordWheel;
    void openChordSelector();
    void closeChordSelector();
    void openCameraChordWheel();
    void closeCameraChordWheel();
    void auditionChord(const std::vector<int>& pitches);
    void releaseChordPreview();
    std::vector<int> chordPreviewPitches;
    double chordPreviewOffMs { 0.0 };
    // Drag a chord out of the selector and drop it on the grid to place its notes.
    void beginChordDrag(const std::vector<int>& pitches);
    void placeChordNotes(const std::vector<int>& pitches, juce::Point<int> gridPos);
    std::vector<int> pendingChordDrag;
    std::optional<juce::Point<int>> chordDropPreviewPos;
    juce::CriticalSection* audioEditLock { nullptr };   // guards note/slide edits vs the audio thread
    double horizontalZoom { 1.0 };
    double verticalZoom { 1.0 };
    double scrollX { 0.0 };
    double scrollY { 0.0 };
    std::optional<int> lastTimerPlayheadX;
    std::set<int> lastTimerActivePlaybackPitches;
    // Smooth (FL-style) zoom + scroll: gestures set targets and a pinned focus; the 120 Hz timer eases
    // the actual values toward them, so panning and zooming glide instead of snapping frame to frame.
    double targetHorizontalZoom { 1.0 };
    double targetVerticalZoom { 1.0 };
    double targetScrollX { 0.0 };
    double targetScrollY { 0.0 };
    bool   zoomAnimating { false };
    double zoomFocusBeat { 0.0 };
    double zoomFocusLane { 0.0 };
    double zoomFocusXInView { 0.0 };
    double zoomFocusYInView { 0.0 };
    void   syncZoomScrollTargets();   // set targets = current (call after any direct scroll/zoom set)
    double pendingMagnifyDelta { 0.0 };
    double ignoreWheelUntilMs { 0.0 };
    int scaleRoot { 0 };
    int scalePatternIndex { 0 };
    double snapSizeInBeats { 0.25 };
    // Default length for a freshly drawn note. Starts at the "Len" grid step (1/16) and then follows
    // the Len button / whatever length the user last drew, stretched, or clicked (FL-style).
    double lastNoteLengthInBeats { 0.25 };
    double stepWriteCursorBeat { 0.0 };
    double stepWriteStepLengthInBeats { 0.25 };
    bool focusModeEnabled { false };
    bool scaleLockEnabled { true };  // new notes snap to in-scale pitches when true
    bool chordModeEnabled { false }; // one placed/played note becomes a diatonic chord when true
    int  chordSizeNotes { 3 };       // 3=triad, 4=7th, 5=9th, 6=11th, 7=13th
    bool stepWriteEnabled { false };
    bool slidePenEnabled { false };
    bool glideEnabled { false };  // live portamento
    SlideVisibilityMode slideVisibilityMode { SlideVisibilityMode::ghost };
    bool hasStoredViewportBeforeFocus { false };
    bool ignoreNextMouseDown { false };
    std::set<int> stepWriteLivePitches;
    std::map<int, std::vector<int>> stepWriteLiveVoicing;
    std::map<int, int> stepWritePendingVelocities;
    std::vector<NoteSnapshot> undoStack;
    std::vector<NoteSnapshot> redoStack;
};
}  // namespace orion
