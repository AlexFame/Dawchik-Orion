#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../Audio/TransportEngine.h"
#include "../Audio/WarpEngine.h"
#include "../Core/ProjectState.h"
#include "../Sampler/SamplerEngine.h"

// Audio render sources extracted from MainComponent. These used to be private
// nested classes of MainComponent; they are pure audio-thread render units with
// no UI dependency, so they live in Audio/ and will host per-track plugins later.
namespace orion
{
constexpr double vstPitchBendRangeSemitones = 12.0;

class BufferPreviewSource final : public juce::PositionableAudioSource
{
public:
    BufferPreviewSource(juce::AudioBuffer<float> previewBuffer, double sourceSampleRate)
        : buffer(std::move(previewBuffer)),
          sampleRate(sourceSampleRate)
    {
    }

    void prepareToPlay(int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        if (buffer.getNumSamples() == 0 || info.numSamples <= 0)
            return;

        const auto remaining = juce::jmax(0, buffer.getNumSamples() - static_cast<int>(positionSamples));
        const auto samplesToCopy = juce::jmin(info.numSamples, remaining);
        if (samplesToCopy <= 0)
            return;

        for (int channel = 0; channel < info.buffer->getNumChannels(); ++channel)
        {
            const auto sourceChannel = juce::jmin(channel, buffer.getNumChannels() - 1);
            info.buffer->copyFrom(channel, info.startSample, buffer, sourceChannel, static_cast<int>(positionSamples), samplesToCopy);
        }

        positionSamples += samplesToCopy;
    }

    void setNextReadPosition(juce::int64 newPosition) override
    {
        positionSamples = juce::jlimit<juce::int64>(0, buffer.getNumSamples(), newPosition);
    }

    juce::int64 getNextReadPosition() const override
    {
        return positionSamples;
    }

    juce::int64 getTotalLength() const override
    {
        return buffer.getNumSamples();
    }

    bool isLooping() const override
    {
        return false;
    }

    double getSampleRate() const noexcept
    {
        return sampleRate;
    }

private:
    juce::AudioBuffer<float> buffer;
    double sampleRate { 44100.0 };
    juce::int64 positionSamples { 0 };
};

class ArrangementPlaybackSource final : public juce::AudioSource
{
public:
    ArrangementPlaybackSource(ProjectState& state, TransportEngine& engine, juce::AudioFormatManager& formatManager)
        : project(state),
          transport(engine),
          audioFormatManager(formatManager),
          samplerEngine(audioFormatManager,
                        [](const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate, const juce::String& sourcePath)
                        {
                            return stretchBufferToLengthWithExperimentalBackend(source, outputSamples, sampleRate, sourcePath);
                        })
    {
    }

    void prepareToPlay(int samplesPerBlockExpected, double newSampleRate) override
    {
        outputSampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        preparedBlockSize = samplesPerBlockExpected > 0 ? samplesPerBlockExpected : 512;

        // Pre-allocate the metering scratch buffer so the audio callback never reallocates.
        samplerMeterScratch.setSize(2, preparedBlockSize, false, false, true);

        const juce::ScopedLock sl(instrumentLock);
        for (auto& [trackIndex, slot] : instruments)
        {
            juce::ignoreUnused(trackIndex);
            if (slot != nullptr && slot->instance != nullptr)
            {
                slot->instance->setRateAndBufferSizeDetails(outputSampleRate, preparedBlockSize);
                slot->instance->prepareToPlay(outputSampleRate, preparedBlockSize);
                slot->liveCollector.reset(outputSampleRate);
            }
        }
    }

    void releaseResources() override
    {
        const juce::ScopedLock sl(instrumentLock);
        for (auto& [trackIndex, slot] : instruments)
        {
            juce::ignoreUnused(trackIndex);
            if (slot != nullptr && slot->instance != nullptr)
                slot->instance->releaseResources();
        }
    }

    void syncToTransportPosition() noexcept
    {
        currentTimelineBeat = transport.getPlayheadBeat();
        wasPlaying = false;
    }

    void setClipEditorPreviewTrim(int trackIndex, int clipIndex, double anchorBeat, double startRatio, double endRatio) noexcept
    {
        clipEditorPreviewAnchorBeat.store(anchorBeat, std::memory_order_relaxed);
        clipEditorPreviewStartRatio.store(juce::jlimit(0.0, 0.999, startRatio), std::memory_order_relaxed);
        clipEditorPreviewEndRatio.store(juce::jlimit(startRatio + 0.001, 1.0, endRatio), std::memory_order_relaxed);
        clipEditorPreviewTrackIndex.store(trackIndex, std::memory_order_release);
        clipEditorPreviewClipIndex.store(clipIndex, std::memory_order_release);
    }

    void clearClipEditorPreviewTrim() noexcept
    {
        clipEditorPreviewTrackIndex.store(-1, std::memory_order_release);
        clipEditorPreviewClipIndex.store(-1, std::memory_order_release);
    }

    void samplerNoteOn(const juce::String& sourcePath,
                       int midiNote,
                       int velocity,
                       int rootMidiNote,
                       double gainDb,
                       SamplerPlaybackMode playbackMode,
                       int sliceIndex,
                       int sliceCount,
                       bool warpEnabled,
                       double sourceBpm)
    {
        samplerEngine.noteOn(sourcePath,
                             midiNote,
                             velocity,
                             rootMidiNote,
                             gainDb,
                             playbackMode,
                             sliceIndex,
                             sliceCount,
                             warpEnabled,
                             sourceBpm,
                             project.getTempoBpm());
    }

    void samplerNoteOff(int midiNote, SamplerPlaybackMode playbackMode)
    {
        samplerEngine.noteOff(midiNote, playbackMode);
    }

    void allSamplerNotesOff()
    {
        samplerEngine.allNotesOff();
    }

    //==========================================================================
    // Per-track VST instrument hosting.
    //
    // Lifecycle calls (set/clear/get) must happen on the message thread. The
    // instance is fully prepared here before being published to the audio
    // thread under instrumentLock, so the audio callback never sees a
    // half-constructed plugin.

    void setTrackInstrument(int trackIndex, std::unique_ptr<juce::AudioPluginInstance> instance)
    {
        if (instance == nullptr)
        {
            clearTrackInstrument(trackIndex);
            return;
        }

        auto slot = std::make_unique<InstrumentSlot>();
        slot->instance = std::move(instance);
        slot->instance->setRateAndBufferSizeDetails(outputSampleRate, preparedBlockSize);
        slot->instance->prepareToPlay(outputSampleRate, preparedBlockSize);
        slot->liveCollector.reset(outputSampleRate);

        std::unique_ptr<InstrumentSlot> previous;
        {
            const juce::ScopedLock sl(instrumentLock);
            auto& current = instruments[trackIndex];
            previous = std::move(current);
            current = std::move(slot);
        }
        // 'previous' is destroyed here, outside the audio lock.
    }

    void clearTrackInstrument(int trackIndex)
    {
        std::unique_ptr<InstrumentSlot> previous;
        {
            const juce::ScopedLock sl(instrumentLock);
            if (const auto it = instruments.find(trackIndex); it != instruments.end())
            {
                previous = std::move(it->second);
                instruments.erase(it);
            }
        }
        if (previous != nullptr && previous->instance != nullptr)
            previous->instance->releaseResources();
    }

    // Message-thread accessor for editor windows / state save & restore.
    juce::AudioPluginInstance* getTrackInstrument(int trackIndex) noexcept
    {
        const juce::ScopedLock sl(instrumentLock);
        if (const auto it = instruments.find(trackIndex); it != instruments.end() && it->second != nullptr)
            return it->second->instance.get();
        return nullptr;
    }

    bool hasTrackInstrument(int trackIndex) noexcept
    {
        return getTrackInstrument(trackIndex) != nullptr;
    }

    // Live monitoring: queue a note from the keyboard for the instrument on a
    // given track. Thread-safe (MidiMessageCollector handles cross-thread enqueue).
    void instrumentLiveNoteOn(int trackIndex, int midiNote, int velocity)
    {
        const juce::ScopedLock sl(instrumentLock);
        if (const auto it = instruments.find(trackIndex); it != instruments.end() && it->second != nullptr)
            it->second->pendingLiveMidi.addEvent(
                juce::MidiMessage::noteOn(1, midiNote, static_cast<juce::uint8>(juce::jlimit(1, 127, velocity))), 0);
    }

    void instrumentLiveNoteOff(int trackIndex, int midiNote)
    {
        const juce::ScopedLock sl(instrumentLock);
        if (const auto it = instruments.find(trackIndex); it != instruments.end() && it->second != nullptr)
            it->second->pendingLiveMidi.addEvent(juce::MidiMessage::noteOff(1, midiNote), 0);
    }

    void allInstrumentNotesOff()
    {
        const juce::ScopedLock sl(instrumentLock);
        for (auto& [trackIndex, slot] : instruments)
        {
            juce::ignoreUnused(trackIndex);
            if (slot != nullptr)
                slot->pendingAllNotesOff = true;
        }
    }

    // Per-track output metering. Pure observation: the audio thread records the
    // loudest post-fader sample magnitude seen for each track; the UI thread reads
    // and resets it. This does NOT change the mix in any way.
    float fetchAndResetTrackPeak(int trackIndex) noexcept
    {
        if (trackIndex < 0 || trackIndex >= maxMeterTracks)
            return 0.0f;
        return trackPeaks[static_cast<std::size_t>(trackIndex)].exchange(0.0f, std::memory_order_relaxed);
    }

    void prepareWarpCacheForCurrentTempo()
    {
        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0)
            return;

        for (const auto& track : project.getTracks())
        {
            for (const auto& clip : track.clips)
            {
                if (clip.type != ClipType::audio || clip.sourcePath.isEmpty() || clip.lengthInBeats <= 0.0)
                    continue;
                // Pre-build for warped clips AND for clips that only need a pitch
                // shift (manual transpose / key-shift) — otherwise the realtime read
                // (allowBuild=false) misses the cache and the clip goes silent.
                if (! clip.warpEnabled && computeKeyShiftSemitones(clip) == 0)
                    continue;

                if (const auto* originalAudioData = getAudioFileData(clip.sourcePath))
                    juce::ignoreUnused(getWarpedAudioFileData(clip, *originalAudioData, beatsPerSecond, true));
            }
        }
    }

    void renderOfflineBlock(juce::AudioBuffer<float>& outputBuffer,
                            int startSample,
                            int numSamples,
                            double blockStartBeat,
                            double renderSampleRate)
    {
        if (startSample < 0 || numSamples <= 0 || renderSampleRate <= 0.0)
            return;

        const auto availableSamples = outputBuffer.getNumSamples() - startSample;
        if (availableSamples <= 0)
            return;

        const auto samplesToRender = juce::jmin(numSamples, availableSamples);
        renderAudioIntoBuffer(outputBuffer, startSample, samplesToRender, blockStartBeat, renderSampleRate, false, false, false);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        if (outputSampleRate <= 0.0 || info.numSamples <= 0)
            return;

        if (! transport.isPlaying())
        {
            currentTimelineBeat = transport.getPlayheadBeat();
            wasPlaying = false;
            samplerEngine.renderLiveNotes(*info.buffer, info.startSample, info.numSamples, outputSampleRate);
            processInstruments(*info.buffer, info.startSample, info.numSamples,
                               currentTimelineBeat, project.getTempoBpm() / 60.0,
                               false, true, false);
            return;
        }

        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0)
            return;

        const auto loopActive = transport.isLoopEnabled() && project.hasLoopRange();
        const auto loopStartBeat = project.getLoopStartBeat();
        const auto loopEndBeat = project.getLoopEndBeat();
        const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);
        if (! loopActive && project.getContentEndInBeats() <= 0.0)
        {
            samplerEngine.renderLiveNotes(*info.buffer, info.startSample, info.numSamples, outputSampleRate);
            processInstruments(*info.buffer, info.startSample, info.numSamples,
                               currentTimelineBeat, beatsPerSecond, false, true, false);
            return;
        }

        if (! wasPlaying)
        {
            // Keep the position captured by syncToTransportPosition() before
            // playback starts. Re-reading the transport here can be a few ms
            // late and skip note-ons placed exactly at the clip start.
            wasPlaying = true;
        }

        const auto beatAdvancePerSample = beatsPerSecond / outputSampleRate;
        renderAudioIntoBuffer(*info.buffer, info.startSample, info.numSamples, currentTimelineBeat, outputSampleRate, loopActive, ! loopActive, true);
        samplerEngine.renderLiveNotes(*info.buffer, info.startSample, info.numSamples, outputSampleRate);

        currentTimelineBeat += static_cast<double>(info.numSamples) * beatAdvancePerSample;
        while (loopActive && currentTimelineBeat >= loopEndBeat)
            currentTimelineBeat = loopStartBeat + std::fmod(currentTimelineBeat - loopStartBeat, loopSpanBeats);
        if (! loopActive)
        {
            const auto repeatEndBeat = project.getContentEndInBeats();
            while (repeatEndBeat > 0.0 && currentTimelineBeat >= repeatEndBeat)
                currentTimelineBeat = std::fmod(currentTimelineBeat, repeatEndBeat);
        }
    }

private:
    struct AudioFileData
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate { 44100.0 };
    };

    void renderAudioIntoBuffer(juce::AudioBuffer<float>& targetBuffer,
                               int startSample,
                               int numSamples,
                               double blockStartBeat,
                               double renderSampleRate,
                               bool wrapToLoop,
                               bool wrapToProjectEnd,
                               bool isRealtime)
    {
        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0 || renderSampleRate <= 0.0 || numSamples <= 0)
            return;

        std::vector<int> instrumentTracks;
        snapshotInstrumentTracks(instrumentTracks);
        const auto trackHasInstrument = [&instrumentTracks](int idx)
        {
            return std::find(instrumentTracks.begin(), instrumentTracks.end(), idx) != instrumentTracks.end();
        };

        const auto loopStartBeat = project.getLoopStartBeat();
        const auto loopEndBeat = project.getLoopEndBeat();
        const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);
        const auto repeatEndBeat = project.getContentEndInBeats();
        const auto wrapToProject = wrapToProjectEnd && ! wrapToLoop && repeatEndBeat > 0.0;
        const auto beatAdvancePerSample = beatsPerSecond / renderSampleRate;
        const auto& tracks = project.getTracks();

        bool anySoloActive = false;
        for (const auto& soloTrack : tracks)
        {
            if (soloTrack.solo)
            {
                anySoloActive = true;
                break;
            }

            for (const auto& soloClip : soloTrack.clips)
            {
                if (soloClip.solo)
                {
                    anySoloActive = true;
                    break;
                }
            }

            if (anySoloActive)
                break;
        }

        for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex)
        {
            const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
            if (track.muted)
                continue;

            const auto trackGain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb));
            const auto hasInstrument = trackHasInstrument(trackIndex);
            float trackOutputPeak = 0.0f; // loudest post-fader magnitude for this track (metering only)
            for (int clipIndex = 0; clipIndex < static_cast<int>(track.clips.size()); ++clipIndex)
            {
                const auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
                if (clip.muted || clip.recording)
                    continue;
                if (anySoloActive && ! track.solo && ! clip.solo)
                    continue;

                if (clip.type == ClipType::midi)
                {
                    // Tracks with a hosted VST instrument render via processInstruments();
                    // only fall back to the built-in sampler when no instrument is loaded.
                    if (hasInstrument)
                        continue;

                    if (isRealtime)
                    {
                        // Render into an isolated scratch buffer so we can measure this
                        // track's level, then mix it into the output unchanged.
                        const auto numCh = juce::jmax(1, targetBuffer.getNumChannels());
                        samplerMeterScratch.setSize(numCh, numSamples, false, false, true);
                        samplerMeterScratch.clear();
                        samplerEngine.renderMidiClip(samplerMeterScratch,
                                                     0,
                                                     numSamples,
                                                     blockStartBeat,
                                                     renderSampleRate,
                                                     beatsPerSecond,
                                                     loopStartBeat,
                                                     loopEndBeat,
                                                     repeatEndBeat,
                                                     wrapToLoop,
                                                     wrapToProject,
                                                     track,
                                                     clip);
                        for (int channel = 0; channel < numCh; ++channel)
                        {
                            trackOutputPeak = juce::jmax(trackOutputPeak,
                                                         samplerMeterScratch.getMagnitude(channel, 0, numSamples));
                            const auto srcChannel = juce::jmin(channel, samplerMeterScratch.getNumChannels() - 1);
                            targetBuffer.addFrom(channel, startSample, samplerMeterScratch, srcChannel, 0, numSamples);
                        }
                    }
                    else
                    {
                        samplerEngine.renderMidiClip(targetBuffer,
                                                     startSample,
                                                     numSamples,
                                                     blockStartBeat,
                                                     renderSampleRate,
                                                     beatsPerSecond,
                                                     loopStartBeat,
                                                     loopEndBeat,
                                                     repeatEndBeat,
                                                     wrapToLoop,
                                                     wrapToProject,
                                                     track,
                                                     clip);
                    }
                    continue;
                }

                if (clip.type != ClipType::audio || clip.sourcePath.isEmpty())
                    continue;

                const auto* originalAudioData = getAudioFileData(clip.sourcePath);
                if (originalAudioData == nullptr || originalAudioData->buffer.getNumSamples() <= 0 || originalAudioData->sampleRate <= 0.0)
                    continue;

                const auto clipStartBeat = clip.startBeat;
                const auto clipEndBeat = clip.startBeat + clip.lengthInBeats;
                const auto linearGain = juce::Decibels::decibelsToGain(static_cast<float>(clip.gainDb)) * trackGain;
                const auto* audioData = originalAudioData;
                const auto trimStart = juce::jlimit(0.0, 0.999, clip.sampleStartRatio);
                const auto trimEnd = juce::jlimit(trimStart + 0.001, 1.0, clip.sampleEndRatio);
                const auto trimSpan = juce::jmax(0.001, trimEnd - trimStart);
                const auto fullSourceLengthInBeats = clip.warpTargetLengthInBeats > 0.0
                    ? clip.warpTargetLengthInBeats
                    : clip.lengthInBeats / trimSpan;
                const auto useClipEditorPreview = isRealtime
                                                && clipEditorPreviewTrackIndex.load(std::memory_order_acquire) == trackIndex
                                                && clipEditorPreviewClipIndex.load(std::memory_order_acquire) == clipIndex;
                const auto previewAnchorBeat = clipEditorPreviewAnchorBeat.load(std::memory_order_relaxed);
                const auto previewStartRatio = juce::jlimit(0.0, 0.999, clipEditorPreviewStartRatio.load(std::memory_order_relaxed));
                const auto previewEndRatio = juce::jlimit(previewStartRatio + 0.001,
                                                          1.0,
                                                          clipEditorPreviewEndRatio.load(std::memory_order_relaxed));
                // Use the pitch-capable warp render when the clip is warped OR when a
                // pitch shift (manual transpose / key-shift) is requested — so that
                // transpose works even on un-warped clips.
                const bool needsPitchRender = (clip.warpEnabled || computeKeyShiftSemitones(clip) != 0)
                                              && clip.lengthInBeats > 0.0;
                if (needsPitchRender)
                {
                    if (const auto* warpedAudioData = getWarpedAudioFileData(clip, *originalAudioData, beatsPerSecond, false))
                        audioData = warpedAudioData;
                    else
                        continue;
                }

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

                    if (useClipEditorPreview)
                    {
                        const auto previewEndBeat = previewAnchorBeat
                            + juce::jmax(0.001, previewEndRatio - previewStartRatio) * fullSourceLengthInBeats;
                        if (timelineBeat < previewAnchorBeat || timelineBeat >= previewEndBeat)
                            continue;
                    }
                    else if (timelineBeat < clipStartBeat || timelineBeat >= clipEndBeat)
                    {
                        continue;
                    }

                    double sourceRatio = 0.0;
                    if (useClipEditorPreview)
                    {
                        const auto previewBeatOffset = timelineBeat - previewAnchorBeat;
                        sourceRatio = previewStartRatio + previewBeatOffset / juce::jmax(0.001, fullSourceLengthInBeats);
                        if (sourceRatio < previewStartRatio || sourceRatio >= previewEndRatio)
                            continue;
                    }
                    else
                    {
                        const auto clipBeatOffset = timelineBeat - clipStartBeat;
                        const auto clipProgress = clip.lengthInBeats > 0.0 ? juce::jlimit(0.0, 1.0, clipBeatOffset / clip.lengthInBeats) : 0.0;
                        sourceRatio = trimStart + clipProgress * trimSpan;
                    }

                    const auto sourceSamplePosition = juce::jlimit(0.0, 1.0, sourceRatio)
                                                    * static_cast<double>(audioData->buffer.getNumSamples() - 1);

                    const auto sourceIndex = static_cast<int>(sourceSamplePosition);
                    if (sourceIndex < 0 || sourceIndex >= audioData->buffer.getNumSamples())
                        continue;

                    const auto sourceFraction = static_cast<float>(sourceSamplePosition - static_cast<double>(sourceIndex));
                    const auto lastSample = audioData->buffer.getNumSamples() - 1;
                    const auto i0 = juce::jmax(0, sourceIndex - 1);
                    const auto i1 = sourceIndex;
                    const auto i2 = juce::jmin(sourceIndex + 1, lastSample);
                    const auto i3 = juce::jmin(sourceIndex + 2, lastSample);
                    for (int channel = 0; channel < targetBuffer.getNumChannels(); ++channel)
                    {
                        const auto sourceChannel = juce::jmin(channel, audioData->buffer.getNumChannels() - 1);
                        const auto y0 = audioData->buffer.getSample(sourceChannel, i0);
                        const auto y1 = audioData->buffer.getSample(sourceChannel, i1);
                        const auto y2 = audioData->buffer.getSample(sourceChannel, i2);
                        const auto y3 = audioData->buffer.getSample(sourceChannel, i3);
                        const auto sampleValue = cubicHermite(sourceFraction, y0, y1, y2, y3) * linearGain;
                        const auto outputValue = sampleValue * 0.75f;
                        targetBuffer.addSample(channel, startSample + sampleIndex, outputValue);
                        if (isRealtime)
                            trackOutputPeak = juce::jmax(trackOutputPeak, std::abs(outputValue));
                    }
                }
            }

            if (isRealtime)
                accumulateTrackPeak(trackIndex, trackOutputPeak);
        }

        // Render any hosted VST instruments for this block (clip notes + live keyboard).
        processInstruments(targetBuffer, startSample, numSamples, blockStartBeat, beatsPerSecond,
                           true, isRealtime, anySoloActive);
    }

    const AudioFileData* getAudioFileData(const juce::String& path)
    {
        const auto key = path.toStdString();
        if (const auto it = audioCache.find(key); it != audioCache.end())
            return it->second.get();

        juce::File file(path);
        if (! file.existsAsFile())
            return nullptr;

        std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0 || reader->numChannels <= 0)
            return nullptr;

        auto data = std::make_unique<AudioFileData>();
        data->sampleRate = reader->sampleRate;
        data->buffer.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
        reader->read(&data->buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

        const auto* dataPtr = data.get();
        audioCache.emplace(key, std::move(data));
        return dataPtr;
    }

    // Semitone offset to transpose the clip from its detected source key into the
    // current project key. Picks the shortest direction (max 6 semitones either way).
    int computeKeyShiftSemitones(const TimelineClip& clip) const noexcept
    {
        int diff = clip.transposeSemitones;
        if (! clip.keyShiftEnabled || clip.sourceKeyRoot < 0)
            return diff;

        int keyDiff = project.getKeyRoot() - clip.sourceKeyRoot;
        while (keyDiff > 6)  keyDiff -= 12;
        while (keyDiff < -6) keyDiff += 12;
        diff += keyDiff;
        return diff;
    }

    const AudioFileData* getWarpedAudioFileData(const TimelineClip& clip,
                                                const AudioFileData& originalData,
                                                double beatsPerSecond,
                                                bool allowBuild)
    {
        const auto warpLengthInBeats = clip.warpTargetLengthInBeats > 0.0 ? clip.warpTargetLengthInBeats : clip.lengthInBeats;
        if (beatsPerSecond <= 0.0 || originalData.sampleRate <= 0.0 || warpLengthInBeats <= 0.0)
            return nullptr;

        const auto targetSamples   = juce::jmax(1, static_cast<int>(std::round((warpLengthInBeats / beatsPerSecond) * originalData.sampleRate)));
        const auto semitonesShift  = computeKeyShiftSemitones(clip);
        const auto pitchScale      = std::pow(2.0, static_cast<double>(semitonesShift) / 12.0);
        const auto key = clip.sourcePath.toStdString() + "|" + std::to_string(targetSamples)
                       + "|p" + std::to_string(semitonesShift)
                       + "|" + currentWarpBackendTag().toStdString();
        if (const auto it = warpedAudioCache.find(key); it != warpedAudioCache.end())
        {
            DBG("[Warp-Cache] HIT key=" + juce::String(key));
            return it->second.get();
        }

        if (! allowBuild)
            return nullptr;

        DBG("[Warp-Cache] MISS key=" + juce::String(key) + " building... pitchSemi=" + juce::String(semitonesShift));

        auto data = std::make_unique<AudioFileData>();
        data->sampleRate = originalData.sampleRate;
        data->buffer = stretchBufferToLengthWithExperimentalBackend(originalData.buffer, targetSamples, originalData.sampleRate, clip.sourcePath, pitchScale);

        const auto* dataPtr = data.get();
        warpedAudioCache.emplace(key, std::move(data));
        return dataPtr;
    }

    // One hosted VST instrument plus its live-note queue and process scratch.
    struct InstrumentSlot
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::MidiMessageCollector liveCollector;
        juce::MidiBuffer pendingLiveMidi;
        juce::AudioBuffer<float> scratch;
        bool pendingAllNotesOff { false };
    };

    // Snapshot the set of track indices that currently host an instrument, so
    // the hot clip loop can skip the sampler path for those tracks without
    // taking the lock per-iteration. Returns false if the lock was contended.
    bool snapshotInstrumentTracks(std::vector<int>& out) noexcept
    {
        out.clear();
        const juce::ScopedTryLock stl(instrumentLock);
        if (! stl.isLocked())
            return false;
        for (auto& [trackIndex, slot] : instruments)
            if (slot != nullptr && slot->instance != nullptr)
                out.push_back(trackIndex);
        return true;
    }

    void processInstruments(juce::AudioBuffer<float>& targetBuffer,
                            int startSample,
                            int numSamples,
                            double blockStartBeat,
                            double beatsPerSecond,
                            bool includeClipNotes,
                            bool includeLiveNotes,
                            bool anySoloActive)
    {
        const juce::ScopedTryLock stl(instrumentLock);
        if (! stl.isLocked() || instruments.empty())
            return;

        const auto& tracks = project.getTracks();
        const auto beatAdvancePerSample = (beatsPerSecond > 0.0 && outputSampleRate > 0.0)
                                              ? beatsPerSecond / outputSampleRate
                                              : 0.0;
        const auto blockEndBeat = blockStartBeat + beatAdvancePerSample * static_cast<double>(numSamples);

        for (auto& [trackIndex, slot] : instruments)
        {
            if (slot == nullptr || slot->instance == nullptr)
                continue;
            if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
                continue;

            const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
            auto* inst = slot->instance.get();

            juce::MidiBuffer midi;

            if (slot->pendingAllNotesOff)
            {
                midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                slot->pendingAllNotesOff = false;
            }

            if (includeLiveNotes)
            {
                for (const auto metadata : slot->pendingLiveMidi)
                    midi.addEvent(metadata.getMessage(), 0);
                slot->pendingLiveMidi.clear();
            }

            const bool trackAudible = ! track.muted && (! anySoloActive || track.solo);

            if (includeClipNotes && trackAudible && beatAdvancePerSample > 0.0)
            {
                for (const auto& clip : track.clips)
                {
                    if (clip.type != ClipType::midi || clip.muted || clip.recording)
                        continue;
                    if (anySoloActive && ! track.solo && ! clip.solo)
                        continue;

                    for (const auto& note : clip.midiNotes)
                    {
                        const auto onBeat  = clip.startBeat + note.startBeat;
                        const auto offBeat = onBeat + juce::jmax(0.01, note.lengthInBeats);

                        if (onBeat >= blockStartBeat && onBeat < blockEndBeat)
                        {
                            const auto offset = juce::jlimit(0, numSamples - 1,
                                static_cast<int>(std::round((onBeat - blockStartBeat) / beatAdvancePerSample)));
                            midi.addEvent(juce::MidiMessage::pitchWheel(1, 8192), offset);
                            midi.addEvent(juce::MidiMessage::noteOn(1, note.pitch,
                                static_cast<juce::uint8>(juce::jlimit(1, 127, note.velocity))), offset);
                        }
                        if (offBeat >= blockStartBeat && offBeat < blockEndBeat)
                        {
                            const auto offset = juce::jlimit(0, numSamples - 1,
                                static_cast<int>(std::round((offBeat - blockStartBeat) / beatAdvancePerSample)));
                            midi.addEvent(juce::MidiMessage::noteOff(1, note.pitch), offset);
                        }
                    }

                    for (const auto& slide : clip.pitchSlides)
                    {
                        if (slide.points.size() < 2)
                            continue;

                        const auto slideStartBeat = clip.startBeat + slide.points.front().beat;
                        const auto slideEndBeat = clip.startBeat + slide.points.back().beat;

                        double slideHoldEndBeat = slideEndBeat;
                        int slideSourcePitch = slide.sourcePitch;
                        bool foundDynamicSource = false;

                        for (const auto& note : clip.midiNotes)
                        {
                            if (note.pitch != slide.sourcePitch
                                || std::abs(note.startBeat - slide.sourceNoteStartBeat) > 0.0001)
                                continue;

                            slideHoldEndBeat = clip.startBeat + note.startBeat + juce::jmax(0.01, note.lengthInBeats);
                            slideSourcePitch = note.pitch;
                            foundDynamicSource = true;
                            break;
                        }

                        if (! foundDynamicSource)
                        {
                            for (const auto& note : clip.midiNotes)
                            {
                                const auto noteEnd = note.startBeat + juce::jmax(0.01, note.lengthInBeats);
                                if (slide.points.front().beat < note.startBeat || slide.points.front().beat > noteEnd)
                                    continue;

                                slideHoldEndBeat = clip.startBeat + noteEnd;
                                slideSourcePitch = note.pitch;
                                foundDynamicSource = true;
                                break;
                            }
                        }

                        if (slideHoldEndBeat < blockStartBeat || slideStartBeat >= blockEndBeat)
                            continue;

                        auto pitchAt = [&slide](double clipBeat)
                        {
                            if (clipBeat <= slide.points.front().beat)
                                return slide.points.front().pitch;
                            if (clipBeat >= slide.points.back().beat)
                                return slide.points.back().pitch;

                            for (std::size_t i = 1; i < slide.points.size(); ++i)
                            {
                                const auto& a = slide.points[i - 1];
                                const auto& b = slide.points[i];
                                if (clipBeat < a.beat || clipBeat > b.beat)
                                    continue;

                                const auto span = juce::jmax(0.0001, b.beat - a.beat);
                                const auto t = juce::jlimit(0.0, 1.0, (clipBeat - a.beat) / span);
                                return a.pitch + (b.pitch - a.pitch) * t;
                            }

                            return slide.points.back().pitch;
                        };

                        const auto firstSample = juce::jlimit(0, numSamples - 1,
                            static_cast<int>(std::floor((juce::jmax(slideStartBeat, blockStartBeat) - blockStartBeat) / beatAdvancePerSample)));
                        const auto lastSample = juce::jlimit(0, numSamples - 1,
                            static_cast<int>(std::ceil((juce::jmin(slideHoldEndBeat, blockEndBeat) - blockStartBeat) / beatAdvancePerSample)));

                        for (int offset = firstSample; offset <= lastSample; offset += 64)
                        {
                            const auto clipBeat = (blockStartBeat + static_cast<double>(offset) * beatAdvancePerSample) - clip.startBeat;
                            const auto semitones = juce::jlimit(-vstPitchBendRangeSemitones,
                                                                 vstPitchBendRangeSemitones,
                                                                 pitchAt(clipBeat) - static_cast<double>(slideSourcePitch));
                            const auto wheel = juce::jlimit(0, 16383,
                                static_cast<int>(std::round(8192.0 + (semitones / vstPitchBendRangeSemitones) * 8191.0)));
                            midi.addEvent(juce::MidiMessage::pitchWheel(1, wheel), offset);
                        }

                    }
                }
            }

            const auto numInCh  = inst->getTotalNumInputChannels();
            const auto numOutCh = inst->getTotalNumOutputChannels();
            const auto chans    = juce::jmax(2, juce::jmax(numInCh, numOutCh));
            slot->scratch.setSize(chans, numSamples, false, false, true);
            slot->scratch.clear();

            inst->processBlock(slot->scratch, midi);

            if (! trackAudible)
                continue;

            const auto trackGain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb));
            float instrumentPeak = 0.0f;
            for (int ch = 0; ch < slot->scratch.getNumChannels(); ++ch)
                instrumentPeak = juce::jmax(instrumentPeak, slot->scratch.getMagnitude(ch, 0, numSamples));
            accumulateTrackPeak(trackIndex, instrumentPeak * trackGain);

            for (int ch = 0; ch < targetBuffer.getNumChannels(); ++ch)
            {
                const auto srcCh = juce::jmin(ch, slot->scratch.getNumChannels() - 1);
                targetBuffer.addFrom(ch, startSample, slot->scratch, srcCh, 0, numSamples, trackGain);
            }
        }
    }

    // Record a track's loudest magnitude this block (keeps the max until the UI reads it).
    void accumulateTrackPeak(int trackIndex, float peak) noexcept
    {
        if (trackIndex < 0 || trackIndex >= maxMeterTracks || peak <= 0.0f)
            return;
        auto& slot = trackPeaks[static_cast<std::size_t>(trackIndex)];
        float prev = slot.load(std::memory_order_relaxed);
        while (peak > prev && ! slot.compare_exchange_weak(prev, peak, std::memory_order_relaxed))
        {
        }
    }

    ProjectState& project;
    TransportEngine& transport;
    juce::AudioFormatManager& audioFormatManager;
    SamplerEngine samplerEngine;
    double outputSampleRate { 44100.0 };
    int preparedBlockSize { 512 };
    double currentTimelineBeat { 0.0 };
    bool wasPlaying { false };
    std::map<std::string, std::unique_ptr<AudioFileData>> audioCache;
    std::map<std::string, std::unique_ptr<AudioFileData>> warpedAudioCache;
    juce::CriticalSection instrumentLock;
    std::map<int, std::unique_ptr<InstrumentSlot>> instruments;

    static constexpr int maxMeterTracks = 512;
    std::array<std::atomic<float>, maxMeterTracks> trackPeaks {};
    juce::AudioBuffer<float> samplerMeterScratch;
    std::atomic<int> clipEditorPreviewTrackIndex { -1 };
    std::atomic<int> clipEditorPreviewClipIndex { -1 };
    std::atomic<double> clipEditorPreviewAnchorBeat { 0.0 };
    std::atomic<double> clipEditorPreviewStartRatio { 0.0 };
    std::atomic<double> clipEditorPreviewEndRatio { 1.0 };
};

class ClickTrackSource final : public juce::AudioSource
{
public:
    ClickTrackSource(ProjectState& state,
                     TransportEngine& engine,
                     std::function<bool()> isMetronomeEnabledFn)
        : project(state),
          transport(engine),
          isMetronomeEnabled(std::move(isMetronomeEnabledFn))
    {
    }

    void prepareToPlay(int, double newSampleRate) override
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    }

    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (info.buffer == nullptr || info.numSamples <= 0)
            return;

        const auto metronomeOn = isMetronomeEnabled && isMetronomeEnabled();
        const auto shouldTick = metronomeOn || transport.isCountInActive();
        if (! shouldTick || (! transport.isPlaying() && ! transport.isCountInActive()) || sampleRate <= 0.0)
        {
            // Reset to -1 so the very first beat after entering tick mode (count-in
            // start or metronome turn-on) always triggers a click. Previously
            // lastBeatIndex stayed at 0 from idle, masking the first downbeat.
            lastBeatIndex = -1;
            currentAmplitude = 0.0f;
            return;
        }

        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0)
            return;

        const auto beatAdvancePerSample = beatsPerSecond / sampleRate;
        auto beatPosition = transport.getClickBeat();
        const auto loopActive = transport.isLoopEnabled() && project.hasLoopRange() && ! transport.isCountInActive();
        const auto loopStartBeat = project.getLoopStartBeat();
        const auto loopEndBeat = project.getLoopEndBeat();
        const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);

        for (int sampleIndex = 0; sampleIndex < info.numSamples; ++sampleIndex)
        {
            if (loopActive)
            {
                while (beatPosition >= loopEndBeat)
                    beatPosition = loopStartBeat + std::fmod(beatPosition - loopStartBeat, loopSpanBeats);
            }

            const auto beatIndex = static_cast<int>(std::floor(beatPosition + 0.0001));
            if (beatIndex != lastBeatIndex)
            {
                lastBeatIndex = beatIndex;
                clickPhase = 0.0;
                const auto beatInBar = beatIndex % juce::jmax(1, project.getNumerator());
                clickFrequency = (beatInBar == 0) ? 1760.0 : 1320.0;
                currentAmplitude = (beatInBar == 0) ? 0.42f : 0.28f;
            }

            float clickSample = 0.0f;
            if (currentAmplitude > 0.0005f)
            {
                clickSample = static_cast<float>(std::sin(clickPhase) * currentAmplitude);
                clickPhase += juce::MathConstants<double>::twoPi * clickFrequency / sampleRate;
                currentAmplitude *= 0.9962f;
            }

            for (int channel = 0; channel < info.buffer->getNumChannels(); ++channel)
                info.buffer->addSample(channel, info.startSample + sampleIndex, clickSample);

            beatPosition += beatAdvancePerSample;
        }
    }

private:
    ProjectState& project;
    TransportEngine& transport;
    std::function<bool()> isMetronomeEnabled;
    double sampleRate { 44100.0 };
    double clickPhase { 0.0 };
    double clickFrequency { 1320.0 };
    float currentAmplitude { 0.0f };
    int lastBeatIndex { -1 };
};

// Master output stage. Sits between the master mixer and the audio device:
// applies a master gain and captures the output peak level for the mixer's
// master meter. All audio runs through here, so it stays deliberately cheap.
class MasterStripSource final : public juce::AudioSource
{
public:
    explicit MasterStripSource(juce::AudioSource& downstreamSource)
        : downstream(downstreamSource)
    {
    }

    void prepareToPlay(int samplesPerBlockExpected, double newSampleRate) override
    {
        downstream.prepareToPlay(samplesPerBlockExpected, newSampleRate);
    }

    void releaseResources() override
    {
        downstream.releaseResources();
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        downstream.getNextAudioBlock(info);

        if (info.buffer == nullptr || info.numSamples <= 0)
            return;

        const auto gain = gainLinear.load(std::memory_order_relaxed);
        if (std::abs(gain - 1.0f) > 1.0e-4f)
            info.buffer->applyGain(info.startSample, info.numSamples, gain);

        float blockPeak = 0.0f;
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            blockPeak = juce::jmax(blockPeak, info.buffer->getMagnitude(ch, info.startSample, info.numSamples));

        // Keep the loudest peak seen since the UI last read it.
        float prev = meterPeak.load(std::memory_order_relaxed);
        while (blockPeak > prev
               && ! meterPeak.compare_exchange_weak(prev, blockPeak, std::memory_order_relaxed))
        {
        }
    }

    void setGainDb(double db) noexcept
    {
        gainLinear.store(juce::Decibels::decibelsToGain(static_cast<float>(db)), std::memory_order_relaxed);
    }

    // Returns the peak magnitude observed since the previous call, then resets it.
    float fetchAndResetPeak() noexcept
    {
        return meterPeak.exchange(0.0f, std::memory_order_relaxed);
    }

private:
    juce::AudioSource& downstream;
    std::atomic<float> gainLinear { 1.0f };
    std::atomic<float> meterPeak { 0.0f };
};
}  // namespace orion
