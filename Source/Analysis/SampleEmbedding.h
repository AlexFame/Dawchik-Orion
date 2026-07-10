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
// Timbral "fingerprint" for sample-similarity search (Ableton Live 12-style "find similar sounds"),
// computed WITHOUT any ML: a log-mel spectrum summary (per-band mean + variation) plus a few spectral/
// temporal descriptors, L2-normalised in groups so no group dominates. Cosine distance between two
// fingerprints ranks how alike two sounds are. Self-contained (JUCE audio only), background + cached —
// mirrors AudioTagger. Designed so a neural embedding (CLAP via ONNX) can later replace analyseFile()
// without touching the browser.
class SampleEmbedding
{
public:
    SampleEmbedding();
    ~SampleEmbedding();

    // Compute (or reuse cached) the embedding for a file. `onReady` fires ON THE MESSAGE THREAD with the
    // vector — immediately if cached, else after the background job. Deduped per path. Empty = failed.
    void requestEmbedding(const juce::File& file, std::function<void(std::vector<float>)> onReady);

    // Non-blocking cache peek (message thread). nullopt = not computed yet.
    std::optional<std::vector<float>> cachedEmbedding(const juce::File& file) const;

    // Synchronous compute (background/offline use, e.g. the test harness). May be slow.
    std::vector<float> computeNow(const juce::File& file);

    // Ableton-style: PRE-INDEX every audio file under `roots` on a low-priority background thread when a
    // library folder is added — so by search time the index is ready and results don't reshuffle. The
    // index is persisted to disk (computed once, ever). Cheap/no-op for already-cached files.
    void indexFolders(const std::vector<juce::File>& roots);

    // 1.0 = identical timbre, 0.0 = unrelated. Returns 0 for mismatched/empty vectors.
    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);

    struct Match { int index; float score; };   // score: 1 = identical, →0 = far. Sorted best-first.

    // Rank `candidates` by similarity to `query`, with per-dimension standardisation over the corpus
    // (this whitening is what makes the distances discriminative). Returns indices into `candidates`.
    static std::vector<Match> rankSimilar(const std::vector<float>& query,
                                          const std::vector<std::vector<float>>& candidates);

    // Per-dimension weights (applied after standardisation) that emphasise instrument-identity cues.
    static const std::vector<float>& featureWeights();

private:
    std::vector<float> analyseFile(const juce::File& file);   // background thread
    void loadIndex();
    void saveIndex();
    juce::File indexFile() const;

    juce::AudioFormatManager formatManager;
    juce::ThreadPool pool { 2 };

    mutable std::mutex cacheMutex;
    std::map<std::string, std::vector<float>> cache;   // path -> embedding
    std::set<std::string> pending;                     // in-flight paths
    std::atomic<int> unsaved { 0 };                    // entries added since last disk save

    std::thread indexerThread;
    std::atomic<bool> indexerStop { false };
    std::atomic<bool> indexerRunning { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleEmbedding)
};
}  // namespace orion
