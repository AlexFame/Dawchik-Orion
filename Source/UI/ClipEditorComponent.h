#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Core/ProjectState.h"

#include <functional>
#include <vector>

namespace orion
{
struct ClipEditorState
{
    bool hasSelection { false };
    bool isAudioClip { false };
    juce::String title;
    juce::String trackName;
    juce::String fileName;
    juce::String sourcePath;
    juce::Colour accent { 0xffff5a4d };
    double startBeat { 0.0 };
    double lengthInBeats { 0.0 };
    // Full source length in beats (independent of any trim). Used when dragging a
    // selected region out, so the new clip keeps the original playback speed.
    double sourceLengthBeats { 0.0 };
    double gainDb { 0.0 };
    double sourceBpm { 0.0 };
    int detectedBars { 0 };
    int transposeSemitones { 0 };
    // Auto key-shift into the project key (semitones), applied at playback on top of the manual PITCH.
    // Shown in CLIP INFO so the user can see the sample is being pitched to the project key.
    int autoKeyShiftSemitones { 0 };
    bool autoKeyShiftActive { false };   // key enabled + sample key known + clip keyShift on
    double sampleStartRatio { 0.0 };
    double sampleEndRatio { 1.0 };
    double previewSourceRatio { 0.0 };
    double playheadBeat { 0.0 };
    bool playing { false };
    // True when previewSourceRatio is a position in warped OUTPUT time (clip-editor preview) rather than
    // a source ratio — the playhead is then drawn directly on the beat axis, not through the warp map.
    bool playheadIsBeatTime { false };
    std::vector<float> waveformMin;
    std::vector<float> waveformMax;
    bool warpEnabled { false };
    bool keyShiftEnabled { false };
    std::vector<WarpMarker> warpMarkers;
};

class ClipEditorComponent final : public juce::Component,
                                  private juce::Timer
{
public:
    ClipEditorComponent();

    std::function<void(bool)> onWarpChanged;
    std::function<void(bool)> onKeyShiftChanged;
    std::function<void(int)> onTransposeChanged;
    std::function<void(double)> onGainChanged;
    std::function<void(double, double)> onSampleRangeChanged;
    // Fired when a START/END marker drag finishes (mouse released). bool = the START marker
    // was the one moved (so the host can jump playback to the new loop start).
    std::function<void(double, double, bool)> onSampleRangeFinalized;
    std::function<void(double)> onPreviewSeek;
    std::function<void()> onNormalize;
    // Warp markers (Ableton-style). Add = pin a source point to a grid beat; Move = drag the marker along
    // the beat ruler so its (pinned) source point lands on a new beat; Remove = delete.
    std::function<void(double sourceRatio, double beat)> onWarpMarkerAdded;
    std::function<void(int index, double beat)> onWarpMarkerMoved;
    std::function<void(int index)> onWarpMarkerRemoved;

    void setState(const ClipEditorState& newState);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    enum class WaveDragMode
    {
        none,
        start,
        end,
        playhead,
        warpMarker
    };

    // Index of the warp marker under x within the handle hit radius, or -1. Only when warp is on.
    int warpMarkerAtX(int x) const noexcept;
    // Nearest grid beat for a source ratio, used when placing a new marker (quantise transient to grid).
    double snappedBeatForSourceRatio(double sourceRatio) const noexcept;
    // Detect onset positions from the waveform envelope (Ableton shows these; a warp marker snaps to
    // the nearest one so you activate a transient instead of dropping a marker blindly).
    void recomputeTransients();
    // Nearest detected transient sourceRatio to the given one within maxDistRatio, or -1 if none.
    double nearestTransient(double sourceRatio, double maxDistRatio) const noexcept;

    static juce::String formatBeat(double beat);
    static juce::String formatDb(double db);

    void drawWaveformPreview(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawInfoCard(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawControlCard(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void updateControlsFromState();
    double visibleWaveSpan() const noexcept;
    double clampWaveViewStart(double start) const noexcept;
    // Adaptive beat grid step (in beats) for the current zoom — used for both drawing the grid and
    // snapping START/END to it. 0 = no usable grid (unknown source length).
    double currentGridStepBeats() const noexcept;
    double xToWaveRatio(int x) const noexcept;
    int waveRatioToX(double ratio) const noexcept;
    // The clip editor's horizontal axis is warped BEAT time (Ableton-style): a source point sits at the
    // x of the beat it maps to. These convert between a pixel and a normalised beat position [0,1], and
    // between source ratio and beat via the cached warp map (identity when there are no markers).
    double xToBeatNorm(int x) const noexcept;
    int beatNormToX(double beatNorm) const noexcept;
    double srcRatioToBeat(double sourceRatio) const noexcept;   // uses warpMap
    double beatToSrcRatio(double beat) const noexcept;          // uses warpMap
    void rebuildWarpMap();
    void zoomWaveformAt(int x, double zoomFactor);
    void updateSampleMarker(WaveDragMode mode, int x, bool shouldSeek);
    void setTransposeSemitones(int semitones);
    void beginPitchTextEntry();      // double-click PITCH to type a value
    void commitPitchTextEntry();
    void beginTrimmedClipDrag(const juce::MouseEvent& event);
    void setTransportButtonStyle(juce::Button& button);

    ClipEditorState state;
    juce::TextButton warpButton { "WARP" };
    juce::TextButton keyShiftButton { "KEY SHIFT" };
    juce::TextButton normalizeButton { "NORMALIZE" };
    juce::TextButton transposeDownButton { "-" };
    juce::TextButton transposeUpButton { "+" };
    juce::Slider gainSlider;
    std::unique_ptr<juce::TextEditor> pitchEditor;   // inline numeric entry for PITCH
    juce::Rectangle<int> lastWaveformBounds;
    juce::Rectangle<int> lastControlsBounds;   // control card (buttons/gain/pitch) — for cheap redraws
    juce::Rectangle<int> lastInfoBounds;       // CLIP INFO card — for cheap redraws
    juce::Rectangle<int> pitchValueBounds;
    WaveDragMode waveDragMode { WaveDragMode::none };
    int activeWarpMarker { -1 };   // marker being dragged (index into state.warpMarkers), or -1
    int hoveredWarpMarker { -1 };  // marker under the cursor (draws a grab handle), or -1
    double hoverCandidateSourceRatio { -1.0 };   // grey "ghost" marker shown under the cursor, or -1
    std::vector<double> transientRatios;   // detected onsets (source ratios), recomputed per source
    std::vector<WarpMarker> warpMap;       // cached control points (endpoints + markers), sorted by source
    double warpTotalBeats { 0.0 };         // beat span the map covers
    bool trimmedClipDragCandidate { false };
    bool trimmedClipDragStarted { false };
    bool snapBypass { false };   // Alt held → drag START/END freely without snapping to the grid
    juce::String lastSourcePath;
    double waveformZoom { 1.0 };
    double waveformViewStart { 0.0 };
    // Smooth zoom: wheel/pinch set a target and an anchor; a timer eases the zoom toward it while
    // keeping the anchor point pinned under the cursor, so zooming glides instead of jumping.
    void timerCallback() override;
    double targetWaveformZoom { 1.0 };
    double zoomAnchorRatio { 0.0 };
    double zoomAnchorLocalX { 0.5 };
};
}  // namespace orion
