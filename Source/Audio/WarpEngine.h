#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

// Audio analysis + time/pitch-warp engine.
//
// This module used to live inline inside MainComponent.cpp. It is pure DSP /
// filename-metadata logic with no dependency on the UI, so it lives in Audio/.
// Only the handful of functions/structs actually used elsewhere are exposed
// here; the lower-level helpers (onset detection, RubberBand glue, drum/melodic
// classifier, etc.) stay private to WarpEngine.cpp.
namespace orion
{
// Cache-busting tag baked into warped-audio cache keys. Changes whenever the
// stretch backend changes so stale cached renders are not reused.
extern const char* const warpBackendCacheVersion;

// Like warpBackendCacheVersion but also reflects the experimental Orion warp
// toggle, so switching backends produces fresh renders instead of reusing the
// other backend's cached audio.
juce::String currentWarpBackendTag();

// Convert (rootSemi 0..11, minor) -> display name like "Cm" / "F#" / "Bb minor".
juce::String formatKeyName(int rootSemi, bool minor, bool fullName = false);

// Returns the shortest pitch shift that aligns the source tonic with the project
// tonic. A uniform pitch shift cannot change chord quality, so major/minor must
// not move the target tonic to a relative key (that made every cross-mode import
// land three semitones away from the key shown in the transport).
constexpr int computeKeyMatchSemitones(int sourceRoot, bool sourceMinor,
                                       int projectRoot, bool projectMinor) noexcept
{
    if (sourceRoot < 0)
        return 0;

    sourceRoot  = ((sourceRoot % 12) + 12) % 12;
    projectRoot = ((projectRoot % 12) + 12) % 12;

    (void) sourceMinor;
    (void) projectMinor;
    int shift = projectRoot - sourceRoot;
    while (shift > 6)  shift -= 12;
    while (shift < -6) shift += 12;
    return shift;
}

static_assert(computeKeyMatchSemitones(7, false, 0, true) == 5,
              "G major must keep C as the tonic inside a C minor project");
static_assert(computeKeyMatchSemitones(11, true, 0, true) == 1,
              "B minor must move up one semitone into C minor");
static_assert(computeKeyMatchSemitones(10, true, 0, true) == 2,
              "Bb minor must move up two semitones into C minor");
static_assert(computeKeyMatchSemitones(1, false, 0, true) == -1,
              "Db major must keep C as the tonic inside a C minor project");
static_assert(computeKeyMatchSemitones(0, true, 0, true) == 0,
              "C minor must remain unchanged inside a C minor project");
static_assert(computeKeyMatchSemitones(7, false, 7, false) == 0,
              "matching roots and modes must remain unchanged");

struct AudioWarpAnalysis
{
    double durationSeconds { 0.0 };
    double sourceBpm { 0.0 };
    int detectedBars { 0 };
    juce::String bpmSource { "none" };
    double bpmConfidence { 0.0 };
    bool bpmGuessed { false };
    int  sourceKeyRoot { -1 };       // -1 = unknown, otherwise 0..11 (C..B)
    bool sourceKeyIsMinor { false };
    // Peak magnitude of the source in dBFS (<= 0). 1.0 = not measured. Used to
    // auto-normalise freshly dropped clips to 0 dBFS so they start at full level.
    double peakDb { 1.0 };
};

// Best-effort BPM extracted purely from a file's name (e.g. "Loop_120bpm").
// Returns 0.0 when nothing usable is found.
double parseBpmFromFileName(const juce::File& file);

// Reads a file's header + name to estimate tempo / key / bar count for warping.
// deepAnalysis=false skips the expensive signal analysis (audio decode + chroma key
// + autocorrelation tempo) and uses only filename + duration heuristics — fast enough
// to run synchronously on a clip drop. Pass true (default) for the full analysis,
// which a background worker runs to fill in key/tempo without stalling the UI.
AudioWarpAnalysis analyzeAudioWarpMetadata(const juce::File& file, double projectTempoBpm, int numerator,
                                           bool deepAnalysis = true);

// 4-point cubic Hermite interpolation used by the arrangement playback resampler.
inline float cubicHermite(float t, float y0, float y1, float y2, float y3) noexcept
{
    const auto c1 = 0.5f * (y2 - y0);
    const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

// Stretch `source` to `outputSamples` length (and optionally pitch-shift by
// `pitchScale`) using the best available backend, automatically choosing a
// drum-optimised or melodic path based on content/filename.
juce::AudioBuffer<float> stretchBufferToLengthWithExperimentalBackend(const juce::AudioBuffer<float>& source,
                                                                      int outputSamples,
                                                                      double sampleRate,
                                                                      const juce::String& sourcePath = {},
                                                                      double pitchScale = 1.0);

// Piecewise (variable-ratio) warp render for Ableton-style warp markers: the stretch ratio varies
// along the sample so each control point's source position lands on its target beat. `warpPointsOutIn`
// are (outputRatio, inputRatio) pairs spanning (0,0)..(1,1); empty falls back to a linear stretch.
// Offline (message/build thread) — used to fill the arrangement warp cache for marker'd clips.
juce::AudioBuffer<float> stretchBufferPiecewiseWarp(const juce::AudioBuffer<float>& source,
                                                    int outputSamples,
                                                    double sampleRate,
                                                    const std::vector<std::pair<double, double>>& warpPointsOutIn,
                                                    double pitchScale = 1.0);

// Fast signalsmith-only stretch (real-time grade) for INSTANT previews. Optionally
// pitch-shifts by `pitchScale`. Dramatically faster than the RubberBand backend, so
// playback can start immediately; a high-quality render can swap in afterwards.
juce::AudioBuffer<float> stretchBufferFastPreview(const juce::AudioBuffer<float>& source,
                                                  int outputSamples,
                                                  double sampleRate,
                                                  double pitchScale = 1.0);

// Convenience wrapper: tempo-fit a preview buffer from `sourceBpm` to `projectTempoBpm`.
juce::AudioBuffer<float> makeTempoFittedPreviewBuffer(const juce::AudioBuffer<float>& source,
                                                      double sourceBpm,
                                                      double projectTempoBpm,
                                                      double sampleRate,
                                                      const juce::String& sourcePath = {});
}  // namespace orion
