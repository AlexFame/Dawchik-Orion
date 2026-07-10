#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <vector>

// Offline stem separation (Demucs htdemucs_6s), matching Logic Pro 12's Stem Splitter: 6 stems —
// drums, bass, other, vocals, guitar, piano. Self-contained: decodes the input to a clean WAV,
// runs a BUNDLED helper binary (built from sevagh/demucs.cpp, MIT) as a SEPARATE PROCESS, and hands
// back the produced stem files. No ProjectState/UI dependency — the arrangement just calls separate().
namespace orion::stems
{
    struct Stem
    {
        juce::String name;   // "drums", "bass", "other", "vocals", "guitar", "piano"
        juce::File   file;   // rendered stem WAV
    };

    struct Result
    {
        bool ok { false };
        juce::String error;          // human-readable reason when ok == false
        std::vector<Stem> stems;
    };

    // Long-running (tens of seconds) — call on a BACKGROUND thread. `progress` (0..1) is best-effort,
    // parsed from the helper's output. `shouldCancel` is polled; returning true aborts and kills the
    // helper. `outDir` receives the stem WAVs (and a temporary decoded input); it is created if needed.
    Result separate (const juce::File& input, const juce::File& outDir,
                     std::function<void (float)> progress = {},
                     std::function<bool ()> shouldCancel = {});

    // True if the bundled helper + model are present (feature can be offered).
    bool isAvailable();
}
