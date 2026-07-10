#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

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

    // Pre-tag every audio file under `roots` on a low-priority background thread when a library folder
    // is added — so tags are ready (not popping in while you scroll). Persisted to disk (computed once).
    void indexFolders(const std::vector<juce::File>& roots);

private:
    juce::StringArray analyseFile(const juce::File& file);   // background thread
    void loadIndex();
    void saveIndex();
    juce::File indexFile() const;

    juce::AudioFormatManager formatManager;
    juce::ThreadPool pool { 1 };

    mutable std::mutex cacheMutex;
    std::map<std::string, juce::StringArray> cache;   // path -> tags
    std::set<std::string> pending;                    // in-flight paths
    std::atomic<int> unsaved { 0 };

    std::thread indexerThread;
    std::atomic<bool> indexerStop { false };
    std::atomic<bool> indexerRunning { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioTagger)
};
}  // namespace orion
