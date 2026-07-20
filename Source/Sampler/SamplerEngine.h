#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../Core/ProjectState.h"

namespace orion
{
class SamplerEngine final
{
public:
    using StretchBufferCallback = std::function<juce::AudioBuffer<float>(const juce::AudioBuffer<float>&, int, double, const juce::String&)>;

    explicit SamplerEngine(juce::AudioFormatManager& formatManager);
    SamplerEngine(juce::AudioFormatManager& formatManager, StretchBufferCallback stretchBuffer);

    void renderMidiClip(juce::AudioBuffer<float>& targetBuffer,
                        int startSample,
                        int numSamples,
                        double blockStartBeat,
                        double renderSampleRate,
                        double beatsPerSecond,
                        double loopStartBeat,
                        double loopEndBeat,
                        double repeatEndBeat,
                        bool wrapToLoop,
                        bool wrapToProjectEnd,
                        const TrackState& track,
                        const TimelineClip& clip);

    // MPC drum-kit playback: for a track with isMpcKit, each MIDI note 36+pad plays
    // mpcKitSamples[pad] as a one-shot at native pitch. Reuses renderMidiClip per pad
    // (filtered to that pad's notes) so the proven, click-free voice rendering is shared.
    void renderMpcKitClip(juce::AudioBuffer<float>& targetBuffer,
                          int startSample,
                          int numSamples,
                          double blockStartBeat,
                          double renderSampleRate,
                          double beatsPerSecond,
                          double loopStartBeat,
                          double loopEndBeat,
                          double repeatEndBeat,
                          bool wrapToLoop,
                          bool wrapToProjectEnd,
                          const TrackState& track,
                          const TimelineClip& clip);

    void noteOn(const juce::String& sourcePath,
                int midiNote,
                int velocity,
                int rootMidiNote,
                double gainDb,
                SamplerPlaybackMode playbackMode,
                int sliceIndex,
                int sliceCount,
                bool warpEnabled,
                double sourceBpm,
                double projectTempoBpm,
                bool fullSampleTrigger = false);
    void noteOff(int midiNote, SamplerPlaybackMode playbackMode, bool gateByNoteLength = false);
    void allNotesOff();
    void renderLiveNotes(juce::AudioBuffer<float>& targetBuffer, int startSample, int numSamples, double renderSampleRate);

    // Build the warped buffer ahead of time (called on load) so the first note is a cache hit.
    void prewarmWarp(const juce::String& path, double sourceBpm, double projectTempoBpm);

    // Live portamento ("Glide"): when enabled, each new live note starts at the
    // previously played pitch and slides to its own pitch over glideTimeSeconds.
    // Mono behaviour (one voice glides) so it reads like a classic mono synth.
    void setGlide(bool enabled, double glideTimeSeconds);

    // Live gain for the audition/live voices (the simpler Gain knob / armed track volume).
    // Applied per-block in renderLiveNotes so changes are heard during playback.
    void setLiveGainDb(double db) noexcept
    {
        liveGainLinear.store(juce::Decibels::decibelsToGain(static_cast<float>(db)), std::memory_order_relaxed);
    }

    // Slice points (start ratios, size = slice count) for the armed track's live/audition
    // slices. Empty = equal divisions. Read in noteOn under liveNotesMutex.
    void setLiveSlicePoints(const std::vector<double>& points)
    {
        std::scoped_lock lock(liveNotesMutex);
        liveSlicePoints = points;
    }

private:
    struct SampleData
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate { 44100.0 };
    };

    struct LiveNote
    {
        const SampleData* sample { nullptr };
        int midiNote { 60 };
        double sourcePosition { 0.0 };
        double sourceEndPosition { 0.0 };
        double playbackRatio { 1.0 };      // target (final) resampling ratio for this note
        float gain { 1.0f };
        int attackSamplesElapsed { 0 };
        int attackSamplesTotal { 384 };
        int releaseSamplesRemaining { 0 };
        int releaseSamplesTotal { 0 };
        std::uint64_t voiceId { 0 };

        // Glide (portamento): currentRatio slides geometrically toward playbackRatio,
        // which is linear in semitone space. glideTimeSeconds > 0 arms the slide; the
        // per-sample step is computed lazily in renderLiveNotes (needs the sample rate).
        double currentRatio { 1.0 };
        double glideRatioMul { 1.0 };
        int    glideSamplesRemaining { 0 };
        double glideTimeSeconds { 0.0 };
        bool   glideInitialised { false };
    };

    const SampleData* getSampleData(const juce::String& path);
    // allowBlocking=true (live noteOn, message thread): if the warped buffer isn't cached yet,
    // compute it synchronously (fast backend → no audible wait) and return it, so warped audio
    // plays from the first note. allowBlocking=false (audio thread, renderMidiClip): never
    // blocks — returns the cache or nullptr (and queues a background build).
    const SampleData* getWarpedSampleData(const juce::String& path, const SampleData& sourceData,
                                          double sourceBpm, double projectTempoBpm, bool allowBlocking);
    static double getPitchRatio(int midiNote, int rootMidiNote) noexcept;

    juce::AudioFormatManager& audioFormatManager;
    StretchBufferCallback stretchBuffer;

    // Reused per-block by renderMpcKitClip so it allocates nothing on the audio thread
    // after warm-up (vector capacities are retained across clear()).
    TrackState kitScratchTrack;
    TimelineClip kitScratchClip;
    std::mutex cacheMutex;
    std::map<std::string, std::unique_ptr<SampleData>> sampleCache;
    std::map<std::string, std::unique_ptr<SampleData>> warpedSampleCache;
    // The time-stretch is heavy (~seconds). It runs on this background pool so neither the
    // audio thread (renderMidiClip) nor the message thread (live noteOn) ever blocks on it;
    // until the warped buffer is ready the caller plays the original sample. `warpInFlight`
    // stops the same key being queued twice. Guarded by cacheMutex.
    std::set<std::string> warpInFlight;
    juce::ThreadPool warpPool { 1 };
    std::mutex liveNotesMutex;
    std::vector<LiveNote> liveNotes;
    juce::AudioBuffer<float> liveMixScratch;   // live voices sum here, soft-clipped, then mixed out
    std::uint64_t nextLiveVoiceId { 1 };

    // Glide state. glideEnabled/glideTimeSeconds are read from the audio thread (clip
    // render) so they are atomic; lastLivePitch is touched only under liveNotesMutex.
    std::atomic<bool>   glideEnabled { false };
    std::atomic<double> glideTimeSeconds { 0.08 };
    int    lastLivePitch { -1 };
    // One-Shot/Slice choke: notes struck within this window of each other count as ONE chord and
    // don't choke each other; a later hit (a new chord/key) chokes the previous. Touched only under
    // liveNotesMutex. Akai-style: press a chord → rings full → next chord chokes it.
    double lastLiveNoteOnMs { -1.0e12 };
    static constexpr double kChordChokeWindowMs = 40.0;
    std::atomic<float>  liveGainLinear { 1.0f };  // live audition/voice gain (see setLiveGainDb)
    std::vector<double> liveSlicePoints;          // transient slice ratios (guarded by liveNotesMutex)
};
}  // namespace orion
