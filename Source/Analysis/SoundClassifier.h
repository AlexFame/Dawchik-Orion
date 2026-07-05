#pragma once

#include <juce_core/juce_core.h>

namespace orion
{
// Classify a sample's instrument/content BY SOUND using Apple's built-in SoundAnalysis classifier
// (SNClassifySoundRequest, version1 — 300+ sounds incl. instruments). No external deps, no bundled
// model. Returns mapped tags (Violin, Piano, Guitar, Drums, Vocal…). Empty on non-macOS or failure.
// Runs synchronously — call it from a background thread.
juce::StringArray classifyWithSoundAnalysis(const juce::File& file);
}  // namespace orion
