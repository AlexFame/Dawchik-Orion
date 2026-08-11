#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

// Audio → editable MIDI. v1 is a self-contained monophonic transcriber (YIN pitch detection +
// note segmentation) — no external model, aimed at bass / vocal / single-line melody loops, which
// is the common "drop a melody → Convert to MIDI" case. A polyphonic path (bundled Spotify Basic
// Pitch, run as a separate process like Demucs/Chordino) can plug in behind the same result type.
namespace orion::transcribe
{
struct Note
{
    int    pitch { 60 };        // MIDI note number
    double startSec { 0.0 };    // seconds from the start of the analysed audio
    double durSec { 0.0 };
    float  velocity { 0.8f };   // 0..1 (from the note's loudness)
};

struct Options
{
    int    minMidi { 28 };      // ~E1  (covers bass)
    int    maxMidi { 96 };      // ~C7
    double minNoteSec { 0.06 }; // drop blips shorter than this
    float  silenceDb { -50.0f };// frames quieter than this are treated as unvoiced
};

// Monophonic transcription. `audio` may be multi-channel (it is downmixed to mono internally).
std::vector<Note> monophonic (const juce::AudioBuffer<float>& audio, double sampleRate, Options opt = {});
}
