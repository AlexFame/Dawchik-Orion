#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "../Core/ChordTheory.h"

#include <vector>

// Offline chord-progression detection: decodes an audio loop, splits it into bars, extracts a
// 12-bin chroma per bar, and matches each against chord templates (maj/min/7ths/dim). Self-contained
// DSP (no ML, no ProjectState dependency) — reuses the same chroma approach as the key detector.
namespace orion::chorddetect
{
    struct Options
    {
        int  numBars { 0 };        // how many bars the loop spans (from warp analysis); <=0 → estimate 1
        int  beatsPerBar { 4 };    // time-sig numerator → analysis runs per beat (catches sub-bar changes)
        int  keyRoot { -1 };       // 0..11 to bias toward diatonic chords, -1 = no bias
        bool keyMinor { false };
        // Exact length the clip occupies on the timeline, in beats (warp-aware). Used by
        // detectTimedChords to map seconds→beats precisely; <=0 falls back to numBars*beatsPerBar.
        double clipLengthBeats { 0.0 };
    };

    // One chord per BEAT (numBars * beatsPerBar entries); host merges equal beats into blocks.
    std::vector<orion::chords::ChordSpec> detectBarChords(const juce::AudioBuffer<float>& mono,
                                                          double sampleRate, const Options& opts);

    // Convenience: decode the file to mono and run detectBarChords.
    std::vector<orion::chords::ChordSpec> detectBarChords(const juce::File& file, const Options& opts);

    // A detected chord placed in musical time (beats), relative to the start of the analysed audio.
    struct TimedChord
    {
        double startBeat { 0.0 };
        double lengthInBeats { 0.0 };
        orion::chords::ChordSpec spec {};
    };

    // Chord progression with EXACT (un-quantised) boundaries that follow the audio's real chord-change
    // points — like Logic's chord track — instead of snapping to the beat grid. Uses Chordino's actual
    // change timestamps; falls back to the per-beat native detector (integer-beat segments) when the
    // helper is unavailable.
    std::vector<TimedChord> detectTimedChords(const juce::File& file, const Options& opts);

    // A chord change expressed as a position in the FULL source file (0..1). The host maps this to the
    // timeline with the SAME transform it uses to draw the waveform, so chords land exactly under the
    // sample. Chordino only (empty if the helper is unavailable → caller uses detectTimedChords).
    struct ChordChange
    {
        double sourceRatio { 0.0 };
        orion::chords::ChordSpec spec {};
    };
    std::vector<ChordChange> detectChordChanges(const juce::File& file, const Options& opts);

    // Independent root evidence from the bundled NNLS analyser. Used only as a conservative
    // correction for an already-accepted native key estimate; -1 fields mean analysis failed.
    struct KeyRootEvidence
    {
        int nnlsRoot { -1 };
        int bassRoot { -1 };
        int octaveRoot { -1 };
    };
    KeyRootEvidence detectKeyRootEvidence(const juce::File& file);
}
