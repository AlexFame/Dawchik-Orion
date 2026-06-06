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
    bool isArmed() const noexcept { return activeTrack != nullptr || activeTrackIndex >= 0; }
    // Index of the track currently bound to the typing keyboard (-1 if none).
    int getActiveTrackIndex() const noexcept { return activeTrackIndex; }

    void paint(juce::Graphics& g) override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent& event) override;
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
    void ensureWaveformPeaksFor(const juce::String& sourcePath);
    void drawWaveform(juce::Graphics& g, juce::Rectangle<int> waveInner, const TrackState* track);

    struct WaveformPeaks
    {
        juce::String sourcePath;
        std::vector<float> minValues;
        std::vector<float> maxValues;
    };

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
    std::array<juce::Rectangle<int>, 3> modeButtonBounds;
    std::array<juce::Rectangle<int>, 3> sliceButtonBounds;
    juce::Rectangle<int> samplerWarpButtonBounds;
};
}  // namespace orion
