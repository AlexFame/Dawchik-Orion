#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "../Core/ProjectState.h"

namespace orion
{
class SamplerPanelComponent final : public juce::Component,
                                    private juce::Timer
{
public:
    SamplerPanelComponent();

    std::function<void()> onClose;
    std::function<void(const juce::String&, int, int, int, double, SamplerPlaybackMode, int, int, bool, double)> onNoteOn;
    std::function<void(int, SamplerPlaybackMode)> onNoteOff;
    std::function<void()> onAllNotesOff;
    // Pushes the active sampler track's transient slice points to the audio engine so
    // live/pad playback uses them. Empty = equal slicing.
    std::function<void(const std::vector<double>&)> onSlicePointsChanged;
    std::function<double()> onRequestProjectTempoBpm;
    std::function<TrackState*(int)> onResolveTrack;
    // Scale lock support: lets the panel snap pitched keyboard input into the project key.
    std::function<int()>  onRequestProjectKeyRoot;
    std::function<bool()> onRequestProjectKeyIsMinor;
    std::function<bool()> onRequestScaleLockEnabled;

    void openTrack(TrackState& trackState);
    void openTrackIndex(int trackIndex);
    void closePanel();
    // Fully releases the active track so laptop keyboard no longer triggers anything.
    void disarmKeyboard();
    // Silences any audition note and clears the waveform playhead indicators. Called
    // when the transport stops so the simpler preview stops with it.
    void stopPreviewPlayback();
    bool isArmed() const noexcept { return activeTrack != nullptr || activeTrackIndex >= 0; }
    // Index of the track currently bound to the typing keyboard (-1 if none).
    int getActiveTrackIndex() const noexcept { return activeTrackIndex; }
    // Current gain (dB) of the armed sampler track, if any. Lets the host push it to the
    // audio engine live so the Gain knob is heard during playback.
    bool getActiveTrackGainDb(double& outDb) const;

    void paint(juce::Graphics& g) override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    void focusLost(FocusChangeType cause) override;
    void visibilityChanged() override;

private:
    void timerCallback() override;

    juce::Rectangle<int> getPanelBounds() const;
    static std::optional<int> pitchForKeyCode(int keyCode);
    static std::optional<int> sliceIndexForKeyCode(int keyCode, int sliceCount);
    static juce::String samplerModeName(SamplerPlaybackMode mode);
    TrackState* getActiveTrack() const;
    bool updateTypingPianoNotes();
    void releaseTypingPianoNotes();
    void startSlicePlaybackIndicator(int sliceIndex);
    void startLinearPlaybackIndicator(int playablePitch);
    void ensureWaveformPeaksFor(const juce::String& sourcePath);
    void drawWaveform(juce::Graphics& g, juce::Rectangle<int> waveInner, const TrackState* track);

    struct WaveformPeaks
    {
        juce::String sourcePath;
        std::vector<float> minValues;
        std::vector<float> maxValues;
    };

    // Horizontal waveform zoom for the slice view: visible window is [viewStart, viewStart+viewSpan]
    // in 0..1 of the sample. Two-finger pinch zooms; two-finger horizontal swipe pans.
    float waveViewStart { 0.0f };
    float waveViewSpan  { 1.0f };
    juce::Rectangle<int> waveformBounds;

    TrackState* activeTrack { nullptr };
    int activeTrackIndex { -1 };
    juce::AudioFormatManager waveformFormatManager;
    WaveformPeaks waveformPeaks;
    std::set<int> activeNotes;
    std::map<int, int> activeNotePitches;
    std::set<int> activeSliceIndices;
    std::optional<int> playbackSliceIndex;
    double playbackSliceStartedMs { 0.0 };
    double playbackSliceDurationMs { 0.0 };
    std::optional<int> padFlashSliceIndex;
    double padFlashStartedMs { 0.0 };
    // Playback indicator for the pitched (Classic / One-Shot) modes: a vertical line that
    // sweeps the waveform for the duration of the most recently triggered note.
    std::optional<double> playbackLinearStartedMs;
    double playbackLinearDurationMs { 0.0 };
    std::array<juce::Rectangle<int>, 3> modeButtonBounds;
    std::array<juce::Rectangle<int>, 4> sliceButtonBounds;
    juce::Rectangle<int> samplerWarpButtonBounds;

    // AKAI MPC-style chop pads: 4×4 grid showing 16 pads. With 32 slices there are two
    // banks (A: 1-16, B: 17-32); the visible bank follows the most recently played slice.
    std::array<juce::Rectangle<int>, 16> padBounds;
    juce::Rectangle<int> padBankButtonBounds;
    int padBank { 0 };
    void triggerSlicePad(int sliceIndex);

    // Transient chop: analyses the sample and fills the active track's slice points.
    juce::Rectangle<int> detectTransientsButtonBounds;
    juce::Rectangle<int> sensSliderBounds;   // sensitivity slider
    bool draggingSens { false };
    int  draggingSliceIndex { -1 };          // slice marker being dragged on the waveform
    void detectTransients();
    void pushSlicePointsToEngine();  // mirror active track's slice points to the audio engine

    // Interactive knobs (Ableton-style): click to select, then drag or use up/down
    // arrows to change the value.
    enum class Knob { none, transpose, root, gain };
    juce::Rectangle<int> transposeKnobBounds;
    juce::Rectangle<int> rootKnobBounds;
    juce::Rectangle<int> gainKnobBounds;
    Knob   selectedKnob { Knob::none };
    int    knobDragStartY { 0 };
    double knobDragStartValue { 0.0 };

    void   nudgeSelectedKnob(int direction, bool large);  // arrow keys: ±1 step (×12 with Shift)

    // Direct numeric entry: type a digit (or '-' / Return) while a knob is selected to
    // open a small inline editor over its value; Return commits, Esc cancels.
    std::unique_ptr<juce::TextEditor> knobEditor;
    Knob editingKnob { Knob::none };
    juce::Rectangle<int> knobBoundsFor(Knob which) const;
    juce::Rectangle<int> knobValueBoundsFor(juce::Rectangle<int> col) const;
    void beginKnobTextEntry(Knob which, const juce::String& seed);
    void commitKnobTextEntry();
    void cancelKnobTextEntry();
};
}  // namespace orion
