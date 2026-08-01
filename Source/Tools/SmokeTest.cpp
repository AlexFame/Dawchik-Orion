// Headless smoke tests for the things that have actually broken in Orion — the regressions that
// were previously only caught by ear, after they had already made it into a running build.
//
//   ./OrionSmokeTest
//
// Each check corresponds to a real bug:
//   1. An armed audio input blasted stale memory out of the speakers, because
//      AudioInputRecorder never wrote to the output block it was handed.
//   2. Project save/load must round-trip, including the MPC kit (the pad samples are the whole
//      point of the kit track, and they were unserialised at first).
//   3. A kit clip must actually make sound. Playback of recorded pads was silent because the
//      panel's samples were not wired to the track the engine renders.

#include "../Audio/AudioInputRecorder.h"
#include "../Core/ProjectSerializer.h"
#include "../Core/ProjectState.h"
#include "../Sampler/SamplerEngine.h"

#include <cstdio>

namespace
{
int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what);
    if (! ok)
        ++failures;
}

// ---------------------------------------------------------------------------------------------
// 1. An input-only AudioIODeviceCallback must still clear the output block it is given.
//    AudioDeviceManager sums each callback's output and does NOT clear the scratch buffer it
//    passes to secondary callbacks, so leaving it untouched plays whatever was in that memory.
void testInputRecorderSilencesOutput()
{
    orion::AudioInputRecorder recorder;

    constexpr int numSamples = 256;
    constexpr int numChannels = 2;
    float left[numSamples], right[numSamples];
    float* outputs[numChannels] = { left, right };

    // Dirty the "scratch" buffer the way a previous block would have.
    for (int i = 0; i < numSamples; ++i)
        left[i] = right[i] = 0.75f;

    const juce::AudioIODeviceCallbackContext context {};
    // No input at all — the early-return path, which is exactly where the old code escaped
    // without touching the output.
    recorder.audioDeviceIOCallbackWithContext(nullptr, 0, outputs, numChannels, numSamples, context);

    bool silent = true;
    for (int ch = 0; ch < numChannels && silent; ++ch)
        for (int i = 0; i < numSamples; ++i)
            if (outputs[ch][i] != 0.0f) { silent = false; break; }

    check(silent, "audio input callback leaves the output block silent (no stale memory to speakers)");
}

// ---------------------------------------------------------------------------------------------
// 2. Project round-trip, including the MPC kit.
void testProjectRoundTrip()
{
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("orion-smoke-roundtrip.orion");
    file.deleteFile();

    orion::TrackState kit;
    kit.name = "MPC";
    kit.isMidiTrack = true;
    kit.isMpcKit = true;
    kit.mpcKitSamples[0] = "/tmp/kick.wav";
    kit.mpcKitSamples[3] = "/tmp/snare.wav";
    kit.isMpcTuneMode = true;
    kit.mpcTuneSample = "/tmp/lead.wav";
    kit.mpcTuneRoot = 39;

    orion::TimelineClip clip;
    clip.type = orion::ClipType::midi;
    clip.startBeat = 4.0;
    clip.lengthInBeats = 8.0;
    clip.midiNotes.push_back({ 36, 0.0, 1.0, 100 });
    clip.midiNotes.push_back({ 39, 2.0, 0.5, 80 });
    kit.clips.push_back(clip);

    orion::ProjectState saved;
    saved.setTempoBpm(140.0);
    saved.getTracks().push_back(kit);

    juce::String error;
    if (! orion::ProjectSerializer::saveToFile(saved, file, &error))
    {
        check(false, "project saves to disk");
        return;
    }

    orion::ProjectState loaded;
    if (! orion::ProjectSerializer::loadFromFile(loaded, file, &error))
    {
        check(false, "project loads back from disk");
        file.deleteFile();
        return;
    }

    check(loaded.getTracks().size() == 1, "round-trip keeps the track");
    if (! loaded.getTracks().empty())
    {
        const auto& t = loaded.getTracks().front();
        check(t.isMpcKit, "round-trip keeps isMpcKit");
        check(t.mpcKitSamples[0] == "/tmp/kick.wav" && t.mpcKitSamples[3] == "/tmp/snare.wav",
              "round-trip keeps the MPC pad samples");
        check(t.isMpcTuneMode && t.mpcTuneSample == "/tmp/lead.wav" && t.mpcTuneRoot == 39,
              "round-trip keeps MPC tune mode");
        check(t.clips.size() == 1 && t.clips.front().midiNotes.size() == 2,
              "round-trip keeps the clip and its notes");
    }
    check(std::abs(loaded.getTempoBpm() - 140.0) < 1.0e-9, "round-trip keeps the tempo");

    file.deleteFile();
}

// ---------------------------------------------------------------------------------------------
// 3. A kit clip renders audible audio. Guards the "recorded pads play back silently" class of bug.
void testKitClipMakesSound()
{
    // A short, loud sine so "did anything render" is unambiguous.
    const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("orion-smoke-pad.wav");
    wav.deleteFile();
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer(
            format.createWriterFor(stream.get(), 44100.0, 1, 16, {}, 0));
        if (writer == nullptr)
        {
            check(false, "could not write the test sample");
            return;
        }
        stream.release();   // the writer owns it now

        juce::AudioBuffer<float> tone(1, 44100);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            tone.setSample(0, i, 0.8f * std::sin(juce::MathConstants<float>::twoPi * 220.0f
                                                 * static_cast<float>(i) / 44100.0f));
        writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
    }

    orion::TrackState track;
    track.isMidiTrack = true;
    track.isMpcKit = true;
    track.mpcKitSamples[0] = wav.getFullPathName();   // pad 0 == MIDI note 36

    orion::TimelineClip clip;
    clip.type = orion::ClipType::midi;
    clip.startBeat = 0.0;
    clip.lengthInBeats = 4.0;
    clip.midiNotes.push_back({ 36, 0.0, 1.0, 127 });

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    orion::SamplerEngine engine(formats);

    juce::AudioBuffer<float> out(2, 512);
    out.clear();
    engine.renderMpcKitClip(out, 0, out.getNumSamples(),
                            /*blockStartBeat*/ 0.0, /*renderSampleRate*/ 44100.0,
                            /*beatsPerSecond*/ 2.0,
                            /*loopStartBeat*/ 0.0, /*loopEndBeat*/ 4.0, /*repeatEndBeat*/ 4.0,
                            /*wrapToLoop*/ false, /*wrapToProjectEnd*/ false,
                            track, clip);

    check(out.getMagnitude(0, out.getNumSamples()) > 0.01f,
          "MPC kit clip renders audible audio for a note on pad 0");

    // ---- Loop retrigger: a note at the loop start must re-fire on EVERY repeat, not just once.
    // Guards the "plays once, then silent on every loop repeat" class of bug. We render the block that
    // straddles the loop boundary (playhead past loopEnd, wrapping back to loopStart) and assert the
    // pad's audio comes through again — i.e. the note-on was re-emitted for the second pass.
    {
        const double loopEnd = 2.0;
        const double bps = 2.0;
        // Block sitting right on the wrap: starts just before loopEnd and spills past it, so the
        // wrapped beats cover the note at loop-start (beat 0).
        const double blockStart = loopEnd - (128.0 / 44100.0) * bps;   // ~128 samples before the boundary
        juce::AudioBuffer<float> pass2(2, 512);
        pass2.clear();
        engine.renderMpcKitClip(pass2, 0, pass2.getNumSamples(),
                                blockStart, /*renderSampleRate*/ 44100.0, bps,
                                /*loopStartBeat*/ 0.0, /*loopEndBeat*/ loopEnd, /*repeatEndBeat*/ loopEnd,
                                /*wrapToLoop*/ true, /*wrapToProjectEnd*/ false,
                                track, clip);
        check(pass2.getMagnitude(0, pass2.getNumSamples()) > 0.01f,
              "MPC kit clip re-triggers the loop-start note on a loop repeat (not silent after pass 1)");
    }

    wav.deleteFile();
}
}   // namespace

int main()
{
    std::printf("Orion smoke tests\n");
    testInputRecorderSilencesOutput();
    testProjectRoundTrip();
    testKitClipMakesSound();

    if (failures > 0)
        std::printf("\n%d check(s) FAILED\n", failures);
    else
        std::printf("\nall checks passed\n");
    return failures > 0 ? 1 : 0;
}
