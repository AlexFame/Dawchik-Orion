#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Orion's own from-scratch time-stretch / pitch-shift backend (WSOLA).
//
// This is a SEPARATE, self-contained module. It does not touch the existing
// Signalsmith / RubberBand warp path — it is only used when the experimental
// "Orion warp engine" toggle is on. With the toggle off, nothing here runs and
// the app behaves exactly as before.
namespace orion
{
// Global A/B switch for the experimental backend (read on the render thread).
void setOrionWarpEnabled(bool enabled) noexcept;
bool isOrionWarpEnabled() noexcept;

// Stretch `source` to exactly `outputSamples` length and multiply pitch by
// `pitchScale`, using WSOLA. Mirrors the signature the rest of the engine
// expects from the warp backend.
juce::AudioBuffer<float> orionStretchWarp(const juce::AudioBuffer<float>& source,
                                          int outputSamples,
                                          double sampleRate,
                                          double pitchScale = 1.0);
}  // namespace orion
