#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace orion
{
enum class ClipType
{
    audio,
    midi
};

enum class SamplerPlaybackMode
{
    classic,
    oneShot,
    slice
};

struct MidiNote
{
    int pitch { 60 };
    double startBeat { 0.0 };
    double lengthInBeats { 1.0 };
    int velocity { 100 };
};

struct TimelineClip
{
    juce::String name;
    ClipType type { ClipType::audio };
    double startBeat { 0.0 };
    double lengthInBeats { 4.0 };
    juce::Colour colour { juce::Colour(0xffeb6f3a) };
    std::vector<MidiNote> midiNotes;
    juce::String sourcePath;
    double gainDb { 0.0 };
    bool muted { false };
    bool solo { false };
    double sourceDurationSeconds { 0.0 };
    double sourceBpm { 0.0 };
    int detectedBars { 0 };
    bool warpEnabled { false };
    bool bpmGuessed { false };
    double warpTargetLengthInBeats { 0.0 };
    // Key data: rootSemitones in [0,11] (C=0..B=11), or -1 if unknown.
    int  sourceKeyRoot { -1 };
    bool sourceKeyIsMinor { false };
    // When true, audio clips are auto-pitch-shifted to match the project key.
    bool keyShiftEnabled { true };
};

struct TrackState
{
    juce::String name;
    bool isMidiTrack { false };
    juce::Colour colour { juce::Colour(0xffeb6f3a) };
    bool muted { false };
    bool solo { false };
    bool recordArmed { false };
    double volumeDb { 0.0 };
    std::vector<TimelineClip> clips;
    juce::String samplerSourcePath;
    int samplerRootMidiNote { 60 };
    SamplerPlaybackMode samplerMode { SamplerPlaybackMode::classic };
    int samplerKeyboardOctaveOffset { 0 };
    int samplerTransposeSemitones { 0 };
    int samplerSliceCount { 16 };
    bool samplerWarpEnabled { false };
    double samplerSourceBpm { 0.0 };
    double samplerSourceDurationSeconds { 0.0 };
    int samplerDetectedBars { 0 };
};

class ProjectState
{
public:
    ProjectState();

    double getTempoBpm() const noexcept;
    void setTempoBpm(double newTempoBpm) noexcept;

    // Project key: root semitone (0=C..11=B) + minor/major mode.
    int  getKeyRoot() const noexcept;
    bool isKeyMinor() const noexcept;
    void setKey(int rootSemitones, bool minor) noexcept;

    int getNumerator() const noexcept;
    int getDenominator() const noexcept;

    double getLoopLengthInBeats() const noexcept;
    double getProjectLengthInBeats() const noexcept;
    double getContentEndInBeats() const noexcept;
    bool hasLoopRange() const noexcept;
    double getLoopStartBeat() const noexcept;
    double getLoopEndBeat() const noexcept;
    void setLoopRange(double startBeat, double endBeat) noexcept;
    void clearLoopRange() noexcept;
    const std::vector<TrackState>& getTracks() const noexcept;
    std::vector<TrackState>& getTracks() noexcept;

private:
    double tempoBpm { 126.0 };
    int timeSigNumerator { 4 };
    int timeSigDenominator { 4 };
    double loopLengthInBeats { 32.0 };
    bool loopRangeActive { false };
    double loopStartBeat { 0.0 };
    double loopEndBeat { 8.0 };
    int  projectKeyRoot { 0 };       // C by default
    bool projectKeyIsMinor { true }; // A minor / C minor are common in production
    std::vector<TrackState> tracks;
};
}  // namespace orion
