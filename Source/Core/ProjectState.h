#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <cmath>
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

struct PitchSlidePoint
{
    double beat { 0.0 };
    double pitch { 60.0 };
};

struct PitchSlide
{
    std::vector<PitchSlidePoint> points;
    int sourcePitch { 60 };
    double sourceNoteStartBeat { 0.0 };
};

struct TimelineClip
{
    juce::String name;
    ClipType type { ClipType::audio };
    double startBeat { 0.0 };
    double lengthInBeats { 4.0 };
    juce::Colour colour { juce::Colour(0xffe8401f) };
    std::vector<MidiNote> midiNotes;
    std::vector<PitchSlide> pitchSlides;
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
    int transposeSemitones { 0 };
    double sampleStartRatio { 0.0 };
    double sampleEndRatio { 1.0 };
    // Fade in/out lengths in beats (Studio One style — drag the top corners).
    double fadeInBeats { 0.0 };
    double fadeOutBeats { 0.0 };
    // Fade curvature in [-1, 1]; 0 = linear, >0 = concave (slow start),
    // <0 = convex (fast start). Set by dragging the mid-point handle.
    double fadeInCurve { 0.0 };
    double fadeOutCurve { 0.0 };
    // Transient UI/audio state: a clip currently being recorded is visible on
    // the timeline but must not play back until the take is finalized.
    bool recording { false };
    // Transient: set when a dropped clip still needs the (expensive) signal
    // analysis for key/tempo. A background worker fills it in and clears this,
    // so dropping a clip stays instant (Ableton-style). Not serialized.
    bool signalAnalysisPending { false };
};

// Maps a linear fade progress t in [0,1] (0 = silent end, 1 = full level) to a
// gain, applying a curvature in [-1,1]. 0 = linear, >0 concave (slow start),
// <0 convex (fast start). Shared by the audio render and the timeline drawing.
inline double fadeCurveGain(double t, double curve) noexcept
{
    t = juce::jlimit(0.0, 1.0, t);
    if (std::abs(curve) < 1.0e-4)
        return t;
    const auto exponent = std::pow(4.0, juce::jlimit(-1.0, 1.0, curve));
    return std::pow(t, exponent);
}

// Equal-power pan: maps pan in [-1,1] to left/right gains (centre ≈ 0.707 each).
inline void panToGains(double pan, float& leftGain, float& rightGain) noexcept
{
    const auto p = juce::jlimit(-1.0, 1.0, pan);
    const auto angle = (p + 1.0) * 0.25 * juce::MathConstants<double>::pi; // 0..pi/2
    leftGain  = static_cast<float>(std::cos(angle));
    rightGain = static_cast<float>(std::sin(angle));
}

struct TrackState
{
    juce::String name;
    bool isMidiTrack { false };
    juce::Colour colour { juce::Colour(0xffe8401f) };
    bool muted { false };
    bool solo { false };
    bool recordArmed { false };
    double volumeDb { 0.0 };
    // Stereo pan in [-1, 1]: -1 = hard left, 0 = centre, +1 = hard right.
    double pan { 0.0 };
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

    // One hosted VST3 effect in a track's insert chain.
    struct InsertFx
    {
        juce::String pluginId;       // PluginDescription::createIdentifierString()
        juce::String pluginName;
        juce::String stateBase64;    // saved plugin state for persistence
        bool bypassed { false };
    };
    std::vector<InsertFx> inserts;

    // An aux send from this track to a bus (post-fader by default).
    struct SendFx
    {
        int busIndex { 0 };
        double level { 0.25 };   // linear 0..1
        bool prefader { false };
    };
    std::vector<SendFx> sends;

    // Main output routing: -1 = master (default), >=0 = aux bus index.
    int outputBus { -1 };

    // Hosted VST instrument (empty = none). instrumentPluginId is the plugin's
    // stable identifier (PluginDescription::createIdentifierString()), used to
    // find and re-instantiate it. instrumentStateBase64 holds the plugin's saved
    // state (getStateInformation, base64-encoded) for project persistence.
    juce::String instrumentPluginId;
    juce::String instrumentPluginName;
    juce::String instrumentStateBase64;
};

// An aux/FX bus: receives sends from tracks, runs its own insert chain, and feeds the
// master. Buses live outside the timeline (mixer-only), so they are kept separate from
// TrackState (which carries clips).
struct BusState
{
    juce::String name { "Bus" };
    juce::Colour colour { juce::Colour(0xff35c9d6) };
    double volumeDb { 0.0 };
    double pan { 0.0 };
    bool muted { false };
    std::vector<TrackState::InsertFx> inserts;
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

    // Global scale lock: when enabled, any pitched note input (sampler keyboard,
    // piano-roll click, drag) snaps to the nearest in-scale pitch.
    bool isScaleLockEnabled() const noexcept;
    void setScaleLockEnabled(bool enabled) noexcept;

    // Whether the metronome ticks (count-in + during playback) when recording.
    bool isRecordWithMetronome() const noexcept;
    void setRecordWithMetronome(bool enabled) noexcept;

    int getNumerator() const noexcept;
    int getDenominator() const noexcept;
    void setTimeSignature(int numerator, int denominator) noexcept;

    double getLoopLengthInBeats() const noexcept;
    void setLoopLengthInBeats(double newLength) noexcept;
    double getProjectLengthInBeats() const noexcept;
    double getContentEndInBeats() const noexcept;
    bool hasLoopRange() const noexcept;
    double getLoopStartBeat() const noexcept;
    double getLoopEndBeat() const noexcept;
    void setLoopRange(double startBeat, double endBeat) noexcept;
    void clearLoopRange() noexcept;
    const std::vector<TrackState>& getTracks() const noexcept;
    std::vector<TrackState>& getTracks() noexcept;
    const std::vector<BusState>& getBuses() const noexcept { return buses; }
    std::vector<BusState>& getBuses() noexcept { return buses; }

    // Master-bus insert chain (processed on the final stereo mix).
    const std::vector<TrackState::InsertFx>& getMasterInserts() const noexcept { return masterInserts; }
    std::vector<TrackState::InsertFx>& getMasterInserts() noexcept { return masterInserts; }

private:
    double tempoBpm { 126.0 };
    int timeSigNumerator { 4 };
    int timeSigDenominator { 4 };
    double loopLengthInBeats { 128.0 };
    bool loopRangeActive { false };
    double loopStartBeat { 0.0 };
    double loopEndBeat { 8.0 };
    int  projectKeyRoot { 0 };       // C by default
    bool projectKeyIsMinor { true }; // A minor / C minor are common in production
    bool scaleLockEnabled { true };
    bool recordWithMetronome { true };
    std::vector<TrackState> tracks;
    std::vector<BusState> buses;
    std::vector<TrackState::InsertFx> masterInserts;

    // Per-track output routing: -1 = master (default), >=0 = aux bus index.
    // Stored on TrackState; this comment documents the convention.
};
}  // namespace orion
