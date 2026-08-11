#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <vector>

// Audio → MIDI transcription via Spotify Basic Pitch (polyphonic, CoreML model), run as a
// separate Python process — same "bundled helper" pattern as the Demucs stem splitter and
// the Chordino chord detector. Replaces the old monophonic YIN toy.
//
// The helper lives under Resources/audio2midi/ :
//   venv/bin/python  +  transcribe.py  (+ the pip-installed basic-pitch model)
// Locations can be overridden with ORION_A2M_PYTHON / ORION_A2M_SCRIPT.
namespace orion::a2m
{
    struct Note
    {
        int    pitch    { 60 };   // MIDI note number
        double startSec { 0.0 };  // seconds from the start of the analysed region
        double endSec   { 0.0 };  // seconds
        float  amp      { 0.8f }; // 0..1 (maps to velocity)
    };

    struct Options
    {
        float  onsetThreshold { 0.5f };   // higher = fewer note onsets
        float  frameThreshold { 0.3f };   // higher = fewer/shorter notes
        double minNoteMs       { 90.0 };  // drop notes shorter than this
        double minFreqHz       { 0.0 };   // 0 = model default (no floor)
        double maxFreqHz       { 0.0 };   // 0 = model default (no ceiling)
        bool   multiPitchBends { false };
    };

    struct Result
    {
        bool               ok { false };
        juce::String       error;
        std::vector<Note>  notes;
    };

    // True when the Python helper (venv + script) is installed and runnable.
    bool isAvailable();

    // Transcribe an in-memory audio region. Writes a temp WAV, runs Basic Pitch, parses the
    // JSON note list (seconds). `cancel` is polled; heavy work — call off the message thread.
    Result transcribe (const juce::AudioBuffer<float>& audio, double sampleRate,
                       Options opt = {}, std::function<bool()> cancel = {});
}
