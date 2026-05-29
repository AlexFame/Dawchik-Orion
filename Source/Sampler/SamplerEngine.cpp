#include "SamplerEngine.h"

#include <algorithm>
#include <cmath>

namespace orion
{
namespace
{
constexpr int kSamplerAttackFadeSamples = 384;
constexpr int kSamplerSliceEndFadeSamples = 384;
constexpr int kSamplerRetriggerFadeSamples = 1536;
constexpr int kSamplerMaxLiveVoices = 24;
constexpr int kSamplerMidiTriggerAttackSamples = 96;
constexpr int kSamplerMidiTriggerReleaseSamples = 384;

float smoothRamp(float value) noexcept
{
    const auto x = juce::jlimit(0.0f, 1.0f, value);
    return x * x * (3.0f - 2.0f * x);
}

float readCubicSample(const juce::AudioBuffer<float>& buffer, int channel, double position) noexcept
{
    const auto sampleCount = buffer.getNumSamples();
    if (sampleCount <= 0)
        return 0.0f;

    const auto index = static_cast<int>(position);
    const auto fraction = static_cast<float>(position - static_cast<double>(index));
    const auto i0 = juce::jlimit(0, sampleCount - 1, index - 1);
    const auto i1 = juce::jlimit(0, sampleCount - 1, index);
    const auto i2 = juce::jlimit(0, sampleCount - 1, index + 1);
    const auto i3 = juce::jlimit(0, sampleCount - 1, index + 2);

    const auto y0 = buffer.getSample(channel, i0);
    const auto y1 = buffer.getSample(channel, i1);
    const auto y2 = buffer.getSample(channel, i2);
    const auto y3 = buffer.getSample(channel, i3);

    const auto a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const auto a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const auto a2 = -0.5f * y0 + 0.5f * y2;
    const auto a3 = y1;
    return ((a0 * fraction + a1) * fraction + a2) * fraction + a3;
}

double pitchRatioForPitch(double midiPitch, int rootMidiNote) noexcept
{
    return std::pow(2.0, (midiPitch - static_cast<double>(rootMidiNote)) / 12.0);
}

std::optional<double> slidePitchForNoteAtBeat(const TimelineClip& clip, const MidiNote& note, double beat)
{
    for (const auto& slide : clip.pitchSlides)
    {
        if (slide.points.size() < 2)
            continue;

        const auto slideStart = slide.points.front().beat;
        const auto noteEnd = note.startBeat + juce::jmax(0.01, note.lengthInBeats);
        const auto originalSource = slide.sourcePitch == note.pitch
            && std::abs(slide.sourceNoteStartBeat - note.startBeat) < 0.0001;
        const auto startsInsideNote = slideStart >= note.startBeat && slideStart <= noteEnd;
        if (! originalSource && ! startsInsideNote)
            continue;

        if (beat < slide.points.front().beat)
            continue;
        if (beat >= slide.points.back().beat)
            return slide.points.back().pitch;

        for (std::size_t i = 1; i < slide.points.size(); ++i)
        {
            const auto& a = slide.points[i - 1];
            const auto& b = slide.points[i];
            if (beat < a.beat || beat > b.beat)
                continue;

            const auto span = juce::jmax(0.0001, b.beat - a.beat);
            const auto t = juce::jlimit(0.0, 1.0, (beat - a.beat) / span);
            return a.pitch + (b.pitch - a.pitch) * t;
        }
    }

    return std::nullopt;
}
}

SamplerEngine::SamplerEngine(juce::AudioFormatManager& formatManager)
    : audioFormatManager(formatManager)
{
}

SamplerEngine::SamplerEngine(juce::AudioFormatManager& formatManager, StretchBufferCallback stretchBufferCallback)
    : audioFormatManager(formatManager),
      stretchBuffer(std::move(stretchBufferCallback))
{
}

void SamplerEngine::renderMidiClip(juce::AudioBuffer<float>& targetBuffer,
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
                                   const TimelineClip& clip)
{
    if (track.samplerSourcePath.isEmpty() || clip.type != ClipType::midi || clip.muted || clip.recording || clip.midiNotes.empty())
        return;

    const auto* sampleData = getSampleData(track.samplerSourcePath);
    if (sampleData == nullptr || sampleData->buffer.getNumSamples() <= 0 || sampleData->sampleRate <= 0.0)
        return;

    if (track.samplerWarpEnabled)
    {
        const auto projectTempoBpm = beatsPerSecond * 60.0;
        if (const auto* warpedSampleData = getWarpedSampleData(track.samplerSourcePath,
                                                               *sampleData,
                                                               track.samplerSourceBpm,
                                                               projectTempoBpm))
            sampleData = warpedSampleData;
    }

    const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);
    const auto wrapToProject = wrapToProjectEnd && ! wrapToLoop && repeatEndBeat > 0.0;
    const auto beatAdvancePerSample = beatsPerSecond / renderSampleRate;
    const auto clipStartBeat = clip.startBeat;
    const auto clipEndBeat = clip.startBeat + clip.lengthInBeats;
    const auto trackGain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb));
    const auto clipGain = juce::Decibels::decibelsToGain(static_cast<float>(clip.gainDb));
    const auto safeSliceCount = juce::jlimit(1, 64, track.samplerSliceCount);

    for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
    {
        auto timelineBeat = blockStartBeat + static_cast<double>(sampleIndex) * beatAdvancePerSample;

        if (wrapToLoop)
        {
            while (timelineBeat >= loopEndBeat)
                timelineBeat = loopStartBeat + std::fmod(timelineBeat - loopStartBeat, loopSpanBeats);
        }
        else if (wrapToProject)
        {
            while (timelineBeat >= repeatEndBeat)
                timelineBeat = std::fmod(timelineBeat, repeatEndBeat);
        }

        if (! wrapToLoop && ! wrapToProject && timelineBeat >= repeatEndBeat)
            continue;

        if (timelineBeat < clipStartBeat || timelineBeat >= clipEndBeat)
            continue;

        const auto clipLocalBeat = timelineBeat - clipStartBeat;
        for (const auto& note : clip.midiNotes)
        {
            auto noteEndBeat = note.startBeat + juce::jmax(0.01, note.lengthInBeats);
            // In Classic and OneShot modes the sample plays through to its natural end
            // regardless of the MIDI note duration — matches the keyboard-live behaviour
            // (noteOff is intentionally a no-op for those modes). Only Slice mode keeps
            // the note bounded by the slice / next-trigger.
            if (track.samplerMode == SamplerPlaybackMode::classic
                || track.samplerMode == SamplerPlaybackMode::oneShot)
            {
                const auto sourceDurationBeats = (static_cast<double>(sampleData->buffer.getNumSamples()) / sampleData->sampleRate)
                    * beatsPerSecond / getPitchRatio(note.pitch, track.samplerRootMidiNote);
                noteEndBeat = juce::jmin(clip.lengthInBeats, note.startBeat + sourceDurationBeats);
            }
            else if (track.samplerMode == SamplerPlaybackMode::slice)
            {
                const auto sliceDurationBeats = (static_cast<double>(sampleData->buffer.getNumSamples()) / sampleData->sampleRate)
                    * beatsPerSecond / static_cast<double>(safeSliceCount);
                auto nextTriggerBeat = clip.lengthInBeats;
                for (const auto& nextNote : clip.midiNotes)
                {
                    if (nextNote.startBeat > note.startBeat + 0.0001)
                        nextTriggerBeat = juce::jmin(nextTriggerBeat, nextNote.startBeat);
                }

                noteEndBeat = juce::jmin(nextTriggerBeat, note.startBeat + sliceDurationBeats);
            }

            if (clipLocalBeat < note.startBeat || clipLocalBeat >= noteEndBeat)
                continue;

            const auto noteSeconds = (clipLocalBeat - note.startBeat) / beatsPerSecond;
            double sourceStartPosition = 0.0;
            double sourceEndPosition = static_cast<double>(sampleData->buffer.getNumSamples());
            const auto slidePitch = slidePitchForNoteAtBeat(clip, note, clipLocalBeat);
            double playbackRatio = pitchRatioForPitch(slidePitch.value_or(static_cast<double>(note.pitch)), track.samplerRootMidiNote);

            if (track.samplerMode == SamplerPlaybackMode::slice)
            {
                const auto sliceIndex = juce::jlimit(0, safeSliceCount - 1, note.pitch - track.samplerRootMidiNote);
                sourceStartPosition = std::floor((static_cast<double>(sliceIndex) / static_cast<double>(safeSliceCount))
                                                 * static_cast<double>(sampleData->buffer.getNumSamples()));
                sourceEndPosition = sliceIndex == safeSliceCount - 1
                    ? static_cast<double>(sampleData->buffer.getNumSamples())
                    : std::floor((static_cast<double>(sliceIndex + 1) / static_cast<double>(safeSliceCount))
                                 * static_cast<double>(sampleData->buffer.getNumSamples()));
                playbackRatio = 1.0;
            }

            const auto sourceSamplePosition = sourceStartPosition + noteSeconds * sampleData->sampleRate * playbackRatio;
            const auto sourceIndex = static_cast<int>(sourceSamplePosition);
            if (sourceIndex < 0 || sourceSamplePosition >= sourceEndPosition || sourceIndex >= sampleData->buffer.getNumSamples())
                continue;

            const auto velocityGain = static_cast<float>(juce::jlimit(1, 127, note.velocity)) / 127.0f;
            auto edgeGain = 1.0f;

            const auto sourceAdvancePerOutputSample = (sampleData->sampleRate * playbackRatio) / renderSampleRate;
            const auto outputSamplesFromTrigger = noteSeconds * renderSampleRate;
            const auto outputSamplesToNoteEnd = ((noteEndBeat - clipLocalBeat) / beatsPerSecond) * renderSampleRate;
            const auto outputSamplesToSourceEnd = (sourceEndPosition - sourceSamplePosition)
                / juce::jmax(0.000001, sourceAdvancePerOutputSample);
            const auto outputSamplesToEnd = juce::jmax(0.0, juce::jmin(outputSamplesToNoteEnd, outputSamplesToSourceEnd));

            if (outputSamplesFromTrigger < static_cast<double>(kSamplerMidiTriggerAttackSamples))
            {
                edgeGain *= smoothRamp(static_cast<float>(outputSamplesFromTrigger + 1.0)
                                       / static_cast<float>(kSamplerMidiTriggerAttackSamples));
            }

            if (outputSamplesToEnd < static_cast<double>(kSamplerMidiTriggerReleaseSamples))
            {
                edgeGain *= smoothRamp(static_cast<float>(outputSamplesToEnd)
                                       / static_cast<float>(kSamplerMidiTriggerReleaseSamples));
            }

            const auto linearGain = trackGain * clipGain * velocityGain * edgeGain * 0.75f;

            for (int channel = 0; channel < targetBuffer.getNumChannels(); ++channel)
            {
                const auto sourceChannel = juce::jmin(channel, sampleData->buffer.getNumChannels() - 1);
                targetBuffer.addSample(channel,
                                       startSample + sampleIndex,
                                       readCubicSample(sampleData->buffer, sourceChannel, sourceSamplePosition) * linearGain);
            }
        }
    }
}

void SamplerEngine::noteOn(const juce::String& sourcePath,
                           int midiNote,
                           int velocity,
                           int rootMidiNote,
                           double gainDb,
                           SamplerPlaybackMode playbackMode,
                           int sliceIndex,
                           int sliceCount,
                           bool warpEnabled,
                           double sourceBpm,
                           double projectTempoBpm)
{
    if (sourcePath.isEmpty())
        return;

    const auto* sampleData = getSampleData(sourcePath);
    if (sampleData == nullptr || sampleData->buffer.getNumSamples() <= 0)
        return;

    if (warpEnabled)
    {
        if (const auto* warpedSampleData = getWarpedSampleData(sourcePath, *sampleData, sourceBpm, projectTempoBpm))
            sampleData = warpedSampleData;
    }

    std::scoped_lock lock(liveNotesMutex);

    auto startRelease = [](LiveNote& note)
    {
        if (note.releaseSamplesRemaining <= 0)
        {
            note.releaseSamplesRemaining = kSamplerRetriggerFadeSamples;
            note.releaseSamplesTotal = kSamplerRetriggerFadeSamples;
        }
    };

    // Classic is polyphonic. One-Shot/Slice behave as mono retrigger for 808-style playing.
    for (auto& note : liveNotes)
    {
        if (playbackMode == SamplerPlaybackMode::classic ? note.midiNote == midiNote : true)
            startRelease(note);
    }

    while (static_cast<int>(liveNotes.size()) >= kSamplerMaxLiveVoices)
    {
        auto oldestActive = std::min_element(liveNotes.begin(), liveNotes.end(), [](const auto& left, const auto& right)
        {
            const auto leftIsReleasing = left.releaseSamplesRemaining > 0;
            const auto rightIsReleasing = right.releaseSamplesRemaining > 0;
            if (leftIsReleasing != rightIsReleasing)
                return ! leftIsReleasing;

            return left.voiceId < right.voiceId;
        });

        if (oldestActive == liveNotes.end())
            break;

        if (oldestActive->releaseSamplesRemaining > 0)
            break;

        startRelease(*oldestActive);
        break;
    }

    auto sourceStartPosition = 0.0;
    auto sourceEndPosition = static_cast<double>(sampleData->buffer.getNumSamples());
    auto playbackRatio = getPitchRatio(midiNote, rootMidiNote);

    if (playbackMode == SamplerPlaybackMode::slice)
    {
        const auto safeSliceCount = juce::jlimit(1, 64, sliceCount);
        const auto safeSliceIndex = juce::jlimit(0, safeSliceCount - 1, sliceIndex);
        const auto totalSamples = sampleData->buffer.getNumSamples();
        const auto sliceStartSample = static_cast<int>(std::floor((static_cast<double>(safeSliceIndex) / static_cast<double>(safeSliceCount))
                                                                  * static_cast<double>(totalSamples)));
        const auto sliceEndSample = safeSliceIndex == safeSliceCount - 1
            ? totalSamples
            : static_cast<int>(std::floor((static_cast<double>(safeSliceIndex + 1) / static_cast<double>(safeSliceCount))
                                          * static_cast<double>(totalSamples)));
        sourceStartPosition = static_cast<double>(juce::jlimit(0, totalSamples - 1, sliceStartSample));
        sourceEndPosition = static_cast<double>(juce::jlimit(sliceStartSample + 1, totalSamples, sliceEndSample));
        playbackRatio = 1.0;
    }

    auto nextNote = LiveNote {
        sampleData,
        midiNote,
        sourceStartPosition,
        sourceEndPosition,
        playbackRatio,
        juce::Decibels::decibelsToGain(static_cast<float>(gainDb)) * (static_cast<float>(juce::jlimit(1, 127, velocity)) / 127.0f) * 0.75f,
        0,
        0,
        0,
        nextLiveVoiceId++
    };

    liveNotes.push_back(nextNote);
}

void SamplerEngine::noteOff(int midiNote, SamplerPlaybackMode playbackMode)
{
    juce::ignoreUnused(midiNote, playbackMode);
}

void SamplerEngine::allNotesOff()
{
    std::scoped_lock lock(liveNotesMutex);
    liveNotes.clear();
}

void SamplerEngine::renderLiveNotes(juce::AudioBuffer<float>& targetBuffer, int startSample, int numSamples, double renderSampleRate)
{
    if (renderSampleRate <= 0.0 || numSamples <= 0)
        return;

    std::scoped_lock lock(liveNotesMutex);
    for (auto& note : liveNotes)
    {
        if (note.sample == nullptr || note.sample->buffer.getNumSamples() <= 0 || note.sample->sampleRate <= 0.0)
            continue;

        const auto sourceAdvancePerOutputSample = note.playbackRatio * note.sample->sampleRate / renderSampleRate;
        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            const auto sourceIndex = static_cast<int>(note.sourcePosition);
            if (sourceIndex < 0
                || sourceIndex >= note.sample->buffer.getNumSamples()
                || note.sourcePosition >= note.sourceEndPosition)
                break;

            for (int channel = 0; channel < targetBuffer.getNumChannels(); ++channel)
            {
                const auto sourceChannel = juce::jmin(channel, note.sample->buffer.getNumChannels() - 1);
                auto noteGain = note.gain;
                if (note.attackSamplesElapsed < kSamplerAttackFadeSamples)
                    noteGain *= smoothRamp(static_cast<float>(note.attackSamplesElapsed + 1)
                                           / static_cast<float>(kSamplerAttackFadeSamples));

                const auto samplesUntilSliceEnd = static_cast<int>(std::floor((note.sourceEndPosition - note.sourcePosition)
                                                                              / juce::jmax(0.000001, sourceAdvancePerOutputSample)));
                if (samplesUntilSliceEnd < kSamplerSliceEndFadeSamples)
                    noteGain *= smoothRamp(static_cast<float>(juce::jmax(0, samplesUntilSliceEnd))
                                           / static_cast<float>(kSamplerSliceEndFadeSamples));

                if (note.releaseSamplesRemaining > 0 && note.releaseSamplesTotal > 0)
                    noteGain *= smoothRamp(static_cast<float>(note.releaseSamplesRemaining)
                                           / static_cast<float>(note.releaseSamplesTotal));

                targetBuffer.addSample(channel,
                                       startSample + sampleIndex,
                                       readCubicSample(note.sample->buffer, sourceChannel, note.sourcePosition) * noteGain);
            }

            if (note.attackSamplesElapsed < kSamplerAttackFadeSamples)
                ++note.attackSamplesElapsed;

            if (note.releaseSamplesRemaining > 0)
                --note.releaseSamplesRemaining;

            note.sourcePosition += sourceAdvancePerOutputSample;
        }
    }

    std::erase_if(liveNotes, [](const auto& note)
    {
        return note.sample == nullptr
            || note.sourcePosition >= static_cast<double>(note.sample->buffer.getNumSamples())
            || note.sourcePosition >= note.sourceEndPosition
            || (note.releaseSamplesTotal > 0 && note.releaseSamplesRemaining <= 0);
    });
}

const SamplerEngine::SampleData* SamplerEngine::getSampleData(const juce::String& path)
{
    const auto key = path.toStdString();
    {
        std::scoped_lock lock(cacheMutex);
        if (const auto it = sampleCache.find(key); it != sampleCache.end())
            return it->second.get();
    }

    juce::File file(path);
    if (! file.existsAsFile())
        return nullptr;

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0 || reader->numChannels <= 0)
        return nullptr;

    auto data = std::make_unique<SampleData>();
    data->sampleRate = reader->sampleRate;
    data->buffer.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&data->buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

    std::scoped_lock lock(cacheMutex);
    const auto [it, inserted] = sampleCache.emplace(key, std::move(data));
    juce::ignoreUnused(inserted);
    return it->second.get();
}

const SamplerEngine::SampleData* SamplerEngine::getWarpedSampleData(const juce::String& path,
                                                                    const SampleData& sourceData,
                                                                    double sourceBpm,
                                                                    double projectTempoBpm)
{
    if (stretchBuffer == nullptr
        || sourceData.buffer.getNumSamples() <= 1
        || sourceData.sampleRate <= 0.0
        || sourceBpm <= 0.0
        || projectTempoBpm <= 0.0)
        return nullptr;

    const auto tempoRatio = sourceBpm / projectTempoBpm;
    if (std::abs(tempoRatio - 1.0) < 0.001)
        return &sourceData;

    const auto targetSamples = juce::jmax(1,
                                          static_cast<int>(std::round(static_cast<double>(sourceData.buffer.getNumSamples())
                                                                      * tempoRatio)));
    const auto key = path.toStdString()
        + "|samplerWarp|"
        + std::to_string(targetSamples)
        + "|"
        + std::to_string(static_cast<int>(std::round(projectTempoBpm * 100.0)));

    {
        std::scoped_lock lock(cacheMutex);
        if (const auto it = warpedSampleCache.find(key); it != warpedSampleCache.end())
            return it->second.get();
    }

    auto data = std::make_unique<SampleData>();
    data->sampleRate = sourceData.sampleRate;
    data->buffer = stretchBuffer(sourceData.buffer, targetSamples, sourceData.sampleRate, path);

    std::scoped_lock lock(cacheMutex);
    const auto [it, inserted] = warpedSampleCache.emplace(key, std::move(data));
    juce::ignoreUnused(inserted);
    return it->second.get();
}

double SamplerEngine::getPitchRatio(int midiNote, int rootMidiNote) noexcept
{
    return std::pow(2.0, static_cast<double>(midiNote - rootMidiNote) / 12.0);
}
}  // namespace orion
