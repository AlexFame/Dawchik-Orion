#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace orion
{
// Content-based ("by sound", not filename) auto-tagger for browser samples. Decodes a short head of
// the file on a background thread, extracts spectral/temporal features, and classifies into broad
// content tags (Drums/Kick/Snare/Hat, Bass, Melodic, Bright/Dark, Noise/FX, Loop/One-shot).
//
// This is the DSP stage. It is intentionally a small, self-contained module with a stable interface
// so a neural model (YAMNet/PANNs via ONNX Runtime) can later replace/augment classify() for precise
// instrument names — without touching the browser or MainComponent.
class AudioTagger
{
public:
    AudioTagger();
    ~AudioTagger();

    // Kick off (or reuse cached) analysis for a file. `onReady` is invoked ON THE MESSAGE THREAD with
    // the derived tags — immediately if cached, otherwise after the background job finishes. Safe to
    // call repeatedly for the same file (deduped). The callback is dropped if the tagger is destroyed.
    void requestTags(const juce::File& file, std::function<void(juce::StringArray)> onReady);

    // Non-blocking cache peek (message thread). nullopt = not analysed yet.
    std::optional<juce::StringArray> cachedTags(const juce::File& file) const;

private:
    juce::StringArray analyseFile(const juce::File& file);   // background thread

    juce::AudioFormatManager formatManager;
    juce::ThreadPool pool { 1 };

    mutable std::mutex cacheMutex;
    std::map<std::string, juce::StringArray> cache;   // path -> tags
    std::set<std::string> pending;                    // in-flight paths

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioTagger)
};
}  // namespace orion
