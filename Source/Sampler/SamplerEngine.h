#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
                double projectTempoBpm);
    void noteOff(int midiNote, SamplerPlaybackMode playbackMode);
    void allNotesOff();
    void renderLiveNotes(juce::AudioBuffer<float>& targetBuffer, int startSample, int numSamples, double renderSampleRate);

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
        double playbackRatio { 1.0 };
        float gain { 1.0f };
        int attackSamplesElapsed { 0 };
        int releaseSamplesRemaining { 0 };
        int releaseSamplesTotal { 0 };
        std::uint64_t voiceId { 0 };
    };

    const SampleData* getSampleData(const juce::String& path);
    const SampleData* getWarpedSampleData(const juce::String& path, const SampleData& sourceData, double sourceBpm, double projectTempoBpm);
    static double getPitchRatio(int midiNote, int rootMidiNote) noexcept;

    juce::AudioFormatManager& audioFormatManager;
    StretchBufferCallback stretchBuffer;
    std::mutex cacheMutex;
    std::map<std::string, std::unique_ptr<SampleData>> sampleCache;
    std::map<std::string, std::unique_ptr<SampleData>> warpedSampleCache;
    std::mutex liveNotesMutex;
    std::vector<LiveNote> liveNotes;
    std::uint64_t nextLiveVoiceId { 1 };
};
}  // namespace orion
