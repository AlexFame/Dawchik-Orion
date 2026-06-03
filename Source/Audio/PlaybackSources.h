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

    // ---- Insert FX chain hosting (per track) --------------------------------
    struct InsertSlot
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        std::atomic<bool> bypassed { false };
    };

    // Appends an effect to a track's insert chain (message thread). Returns its index.
    int addInsert(int trackIndex, std::unique_ptr<juce::AudioPluginInstance> instance, bool bypassed)
    {
        if (instance == nullptr)
            return -1;
        instance->setRateAndBufferSizeDetails(outputSampleRate, preparedBlockSize);
        instance->setPlayConfigDetails(2, 2, outputSampleRate, preparedBlockSize);
        instance->prepareToPlay(outputSampleRate, preparedBlockSize);

        auto slot = std::make_unique<InsertSlot>();
        slot->instance = std::move(instance);
        slot->bypassed.store(bypassed, std::memory_order_relaxed);

        const juce::ScopedLock sl(insertLock);
        auto& chain = trackInserts[trackIndex];
        chain.push_back(std::move(slot));
        return static_cast<int>(chain.size()) - 1;
    }

    void removeInsert(int trackIndex, int index)
    {
        std::unique_ptr<InsertSlot> removed;
        {
            const juce::ScopedLock sl(insertLock);
            const auto it = trackInserts.find(trackIndex);
            if (it == trackInserts.end() || index < 0 || index >= static_cast<int>(it->second.size()))
                return;
            removed = std::move(it->second[static_cast<std::size_t>(index)]);
            it->second.erase(it->second.begin() + index);
        }
        if (removed != nullptr && removed->instance != nullptr)
            removed->instance->releaseResources();
    }

    void clearTrackInserts(int trackIndex)
    {
        std::vector<std::unique_ptr<InsertSlot>> removed;
        {
            const juce::ScopedLock sl(insertLock);
            if (const auto it = trackInserts.find(trackIndex); it != trackInserts.end())
            {
                removed = std::move(it->second);
                trackInserts.erase(it);
            }
        }
        for (auto& slot : removed)
            if (slot != nullptr && slot->instance != nullptr)
                slot->instance->releaseResources();
    }

    // Moves an insert slot (the live plugin instance) between/within tracks.
    void moveInsert(int fromTrack, int fromIndex, int toTrack, int toIndex)
    {
        const juce::ScopedLock sl(insertLock);
        const auto fromIt = trackInserts.find(fromTrack);
        if (fromIt == trackInserts.end() || fromIndex < 0 || fromIndex >= static_cast<int>(fromIt->second.size()))
            return;
        auto slot = std::move(fromIt->second[static_cast<std::size_t>(fromIndex)]);
        fromIt->second.erase(fromIt->second.begin() + fromIndex);

        auto& dest = trackInserts[toTrack];
        const auto clamped = juce::jlimit(0, static_cast<int>(dest.size()), toIndex);
        dest.insert(dest.begin() + clamped, std::move(slot));
    }

    void setInsertBypass(int trackIndex, int index, bool bypassed)
    {
        const juce::ScopedLock sl(insertLock);
        const auto it = trackInserts.find(trackIndex);
        if (it != trackInserts.end() && index >= 0 && index < static_cast<int>(it->second.size())
            && it->second[static_cast<std::size_t>(index)] != nullptr)
            it->second[static_cast<std::size_t>(index)]->bypassed.store(bypassed, std::memory_order_relaxed);
    }

    juce::AudioPluginInstance* getInsertInstance(int trackIndex, int index) noexcept
    {
        const juce::ScopedLock sl(insertLock);
        const auto it = trackInserts.find(trackIndex);
        if (it != trackInserts.end() && index >= 0 && index < static_cast<int>(it->second.size())
            && it->second[static_cast<std::size_t>(index)] != nullptr)
            return it->second[static_cast<std::size_t>(index)]->instance.get();
        return nullptr;
    }

    int getInsertCount(int trackIndex) noexcept
    {
        const juce::ScopedLock sl(insertLock);
        const auto it = trackInserts.find(trackIndex);
        return it != trackInserts.end() ? static_cast<int>(it->second.size()) : 0;
    }

    // Insert chains for aux buses are stored in the same map under a high key offset,
    // so the whole insert API (add/remove/bypass/editor) is reused for buses too.
    static constexpr int kBusInsertKeyBase = 1000000;
    static int busInsertKey(int busIndex) noexcept { return kBusInsertKeyBase + busIndex; }
    // Master-bus insert chain id (processed on the final arrangement mix, pre-fader).
    static constexpr int kMasterInsertKey = 2000000;

    static constexpr int maxBuses = 64;
    void accumulateBusPeakStereo(int busIndex, float l, float r) noexcept
    {
        if (busIndex < 0 || busIndex >= maxBuses) return;
        if (l > 0.0f) accumulateAtomicMax(busPeaksL[static_cast<std::size_t>(busIndex)], l);
        if (r > 0.0f) accumulateAtomicMax(busPeaksR[static_cast<std::size_t>(busIndex)], r);
    }
    void fetchAndResetBusPeakStereo(int busIndex, float& outL, float& outR) noexcept
    {
        if (busIndex < 0 || busIndex >= maxBuses) { outL = outR = 0.0f; return; }
        outL = busPeaksL[static_cast<std::size_t>(busIndex)].exchange(0.0f, std::memory_order_relaxed);
        outR = busPeaksR[static_cast<std::size_t>(busIndex)].exchange(0.0f, std::memory_order_relaxed);
    }

    // Audio thread: run a track's buffer through its (non-bypassed) inserts in order.
    void processTrackInserts(int trackIndex, juce::AudioBuffer<float>& buf)
    {
        const juce::ScopedTryLock stl(insertLock);
        if (! stl.isLocked())
            return;
        const auto it = trackInserts.find(trackIndex);
        if (it == trackInserts.end())
            return;
        juce::MidiBuffer noMidi;
        for (auto& slot : it->second)
            if (slot != nullptr && slot->instance != nullptr && ! slot->bypassed.load(std::memory_order_relaxed))
                slot->instance->processBlock(buf, noMidi);
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
        const auto l = trackPeaksL[static_cast<std::size_t>(trackIndex)].exchange(0.0f, std::memory_order_relaxed);
        const auto r = trackPeaksR[static_cast<std::size_t>(trackIndex)].exchange(0.0f, std::memory_order_relaxed);
        return juce::jmax(l, r);
    }

    // Stereo variant: reads and resets the left/right peaks for a track.
    void fetchAndResetTrackPeakStereo(int trackIndex, float& outL, float& outR) noexcept
    {
        if (trackIndex < 0 || trackIndex >= maxMeterTracks)
        {
            outL = outR = 0.0f;
            return;
        }
        outL = trackPeaksL[static_cast<std::size_t>(trackIndex)].exchange(0.0f, std::memory_order_relaxed);
        outR = trackPeaksR[static_cast<std::size_t>(trackIndex)].exchange(0.0f, std::memory_order_relaxed);
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

        // Prepare aux-bus accumulation buffers for this block (cleared each time).
        const auto& buses = project.getBuses();
        const auto numBuses = juce::jmin(maxBuses, static_cast<int>(buses.size()));
        const auto busChannels = juce::jmax(2, targetBuffer.getNumChannels());
        if (static_cast<int>(busBuffers.size()) != numBuses)
            busBuffers.resize(static_cast<std::size_t>(numBuses));
        for (auto& bb : busBuffers)
        {
            bb.setSize(busChannels, numSamples, false, false, true);
            bb.clear();
        }

        for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex)
        {
            const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
            if (track.muted)
                continue;

            const auto trackGain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb));
            float panLeftGain = 1.0f, panRightGain = 1.0f;
            panToGains(track.pan, panLeftGain, panRightGain);
            // Per-output-channel gain (left=0, right=1; mono/others unaffected).
            const auto panForChannel = [panLeftGain, panRightGain](int ch) -> float
            {
                return ch == 0 ? panLeftGain : (ch == 1 ? panRightGain : 1.0f);
            };
            const auto hasInstrument = trackHasInstrument(trackIndex);
            const auto hasInserts = ! track.inserts.empty();
            const auto hasSends = ! track.sends.empty() && ! busBuffers.empty();
            // Main output routing: -1 = master (default), >=0 = aux bus.
            const auto routedToBus = track.outputBus >= 0 && track.outputBus < numBuses;
            const auto needIsolation = hasInserts || hasSends || routedToBus;
            // Snapshot this track's slice of the mix so we can (a) measure the track's TRUE
            // summed contribution, (b) isolate it for the insert FX chain, and (c) feed aux
            // sends. Needed for metering (realtime) and for FX/sends (realtime + export).
            const auto meterChannels = juce::jmax(1, targetBuffer.getNumChannels());
            if (isRealtime || needIsolation)
            {
                trackMeterBefore.setSize(meterChannels, numSamples, false, false, true);
                for (int ch = 0; ch < meterChannels; ++ch)
                    trackMeterBefore.copyFrom(ch, 0, targetBuffer, ch, startSample, numSamples);
            }
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
                            const auto pg = panForChannel(channel);
                            const auto srcChannel = juce::jmin(channel, samplerMeterScratch.getNumChannels() - 1);
                            targetBuffer.addFrom(channel, startSample, samplerMeterScratch, srcChannel, 0, numSamples, pg);
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

                    // Fade in/out envelope (linear), based on position within the clip.
                    float fadeGain = 1.0f;
                    if (! useClipEditorPreview)
                    {
                        if (clip.fadeInBeats > 0.0 && timelineBeat < clipStartBeat + clip.fadeInBeats)
                            fadeGain *= static_cast<float>(fadeCurveGain((timelineBeat - clipStartBeat) / clip.fadeInBeats, clip.fadeInCurve));
                        if (clip.fadeOutBeats > 0.0 && timelineBeat > clipEndBeat - clip.fadeOutBeats)
                            fadeGain *= static_cast<float>(fadeCurveGain((clipEndBeat - timelineBeat) / clip.fadeOutBeats, clip.fadeOutCurve));
                    }

                    for (int channel = 0; channel < targetBuffer.getNumChannels(); ++channel)
                    {
                        const auto sourceChannel = juce::jmin(channel, audioData->buffer.getNumChannels() - 1);
                        const auto y0 = audioData->buffer.getSample(sourceChannel, i0);
                        const auto y1 = audioData->buffer.getSample(sourceChannel, i1);
                        const auto y2 = audioData->buffer.getSample(sourceChannel, i2);
                        const auto y3 = audioData->buffer.getSample(sourceChannel, i3);
                        const auto sampleValue = cubicHermite(sourceFraction, y0, y1, y2, y3) * linearGain;
                        const auto outputValue = sampleValue * 0.75f * fadeGain * panForChannel(channel);
                        targetBuffer.addSample(channel, startSample + sampleIndex, outputValue);
                    }
                }
            }

            if (needIsolation)
            {
                // Isolate the track's dry contribution (after − before), run it through the
                // insert chain, feed aux sends, then write the processed signal back in
                // place. Tracks with no inserts/sends are untouched (byte-identical).
                trackFxScratch.setSize(meterChannels, numSamples, false, false, true);
                for (int ch = 0; ch < meterChannels; ++ch)
                    for (int s = 0; s < numSamples; ++s)
                        trackFxScratch.setSample(ch, s, targetBuffer.getSample(ch, startSample + s)
                                                        - trackMeterBefore.getSample(ch, s));

                if (hasInserts)
                    processTrackInserts(trackIndex, trackFxScratch);

                // Aux sends (post-insert): post-fader adds the processed signal as-is;
                // pre-fader undoes the channel fader (so the send is independent of volume).
                if (hasSends)
                {
                    for (const auto& send : track.sends)
                    {
                        const auto level = static_cast<float>(juce::jlimit(0.0, 1.0, send.level));
                        if (level <= 0.0001f || send.busIndex < 0 || send.busIndex >= static_cast<int>(busBuffers.size()))
                            continue;
                        const auto sendGain = send.prefader
                            ? level / juce::jmax(1.0e-4f, trackGain) : level;
                        auto& busBuf = busBuffers[static_cast<std::size_t>(send.busIndex)];
                        const auto bc = juce::jmin(meterChannels, busBuf.getNumChannels());
                        for (int ch = 0; ch < bc; ++ch)
                            busBuf.addFrom(ch, 0, trackFxScratch, ch, 0, numSamples, sendGain);
                    }
                }

                if (routedToBus)
                {
                    // Output goes to an aux bus instead of the master: restore the master to
                    // its pre-track state and add the processed contribution to the bus.
                    auto& outBuf = busBuffers[static_cast<std::size_t>(track.outputBus)];
                    const auto bc = juce::jmin(meterChannels, outBuf.getNumChannels());
                    for (int ch = 0; ch < bc; ++ch)
                        outBuf.addFrom(ch, 0, trackFxScratch, ch, 0, numSamples, 1.0f);
                    for (int ch = 0; ch < meterChannels; ++ch)
                        for (int s = 0; s < numSamples; ++s)
                            targetBuffer.setSample(ch, startSample + s, trackMeterBefore.getSample(ch, s));
                }
                else
                {
                    for (int ch = 0; ch < meterChannels; ++ch)
                        for (int s = 0; s < numSamples; ++s)
                            targetBuffer.setSample(ch, startSample + s,
                                                   trackMeterBefore.getSample(ch, s) + trackFxScratch.getSample(ch, s));
                }

                if (isRealtime)
                {
                    float peakL = 0.0f, peakR = 0.0f;
                    for (int ch = 0; ch < meterChannels; ++ch)
                    {
                        const auto m = trackFxScratch.getMagnitude(ch, 0, numSamples);
                        if (ch == 0)      peakL = m;
                        else if (ch == 1) peakR = m;
                    }
                    accumulateTrackPeakStereo(trackIndex, peakL, peakR);
                }
            }
            else if (isRealtime)
            {
                // Measure the track's contribution as (buffer after − buffer before).
                float peakL = 0.0f, peakR = 0.0f;
                for (int ch = 0; ch < meterChannels; ++ch)
                {
                    float chPeak = 0.0f;
                    for (int s = 0; s < numSamples; ++s)
                        chPeak = juce::jmax(chPeak, std::abs(targetBuffer.getSample(ch, startSample + s)
                                                            - trackMeterBefore.getSample(ch, s)));
                    if (ch == 0)      peakL = chPeak;
                    else if (ch == 1) peakR = chPeak;
                }
                accumulateTrackPeakStereo(trackIndex, peakL, peakR);
            }
        }

        // Mix the aux buses (through their own insert chains + gain/pan) into the master.
        for (int b = 0; b < numBuses; ++b)
        {
            const auto& bus = buses[static_cast<std::size_t>(b)];
            if (bus.muted)
                continue;
            auto& busBuf = busBuffers[static_cast<std::size_t>(b)];
            processTrackInserts(busInsertKey(b), busBuf);

            float gL = 1.0f, gR = 1.0f;
            panToGains(bus.pan, gL, gR);
            const auto g = juce::Decibels::decibelsToGain(static_cast<float>(bus.volumeDb));
            const auto bc = juce::jmin(targetBuffer.getNumChannels(), busBuf.getNumChannels());
            float pkL = 0.0f, pkR = 0.0f;
            for (int ch = 0; ch < bc; ++ch)
            {
                const auto cg = g * (ch == 0 ? gL : (ch == 1 ? gR : 1.0f));
                if (isRealtime)
                {
                    const auto m = busBuf.getMagnitude(ch, 0, numSamples) * cg;
                    if (ch == 0)      pkL = m;
                    else if (ch == 1) pkR = m;
                }
                targetBuffer.addFrom(ch, startSample, busBuf, ch, 0, numSamples, cg);
            }
            if (isRealtime)
                accumulateBusPeakStereo(b, pkL, pkR);
        }

        // Render any hosted VST instruments for this block (clip notes + live keyboard).
        processInstruments(targetBuffer, startSample, numSamples, blockStartBeat, beatsPerSecond,
                           true, isRealtime, anySoloActive);

        // Master insert chain: process the full arrangement mix (tracks + buses + instruments)
        // through the master inserts, pre-fader (the master gain/meter live downstream).
        {
            const juce::ScopedTryLock stl(insertLock);
            if (stl.isLocked() && trackInserts.find(kMasterInsertKey) != trackInserts.end())
            {
                juce::AudioBuffer<float> sub(targetBuffer.getArrayOfWritePointers(),
                                             targetBuffer.getNumChannels(), startSample, numSamples);
                juce::MidiBuffer noMidi;
                for (auto& slot : trackInserts[kMasterInsertKey])
                    if (slot != nullptr && slot->instance != nullptr
                        && ! slot->bypassed.load(std::memory_order_relaxed))
                        slot->instance->processBlock(sub, noMidi);
            }
        }
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

        // The warp cache is read from the audio thread (allowBuild=false) and written from
        // the message thread (allowBuild=true, on Play / tempo change). Without
        // synchronization the audio thread's std::map::find races the build's emplace, so
        // the first playback after a drop doesn't see the just-built entry (plays unwarped),
        // while the second does. A lock fixes both correctness and visibility. Entries are
        // never erased, so a pointer returned under the lock stays valid afterwards.
        if (! allowBuild)
        {
            const juce::ScopedTryLock stl(warpCacheLock);
            if (! stl.isLocked())
                return nullptr;   // a build is in progress — this block plays the fallback
            const auto it = warpedAudioCache.find(key);
            return it != warpedAudioCache.end() ? it->second.get() : nullptr;
        }

        const juce::ScopedLock sl(warpCacheLock);
        if (const auto it = warpedAudioCache.find(key); it != warpedAudioCache.end())
            return it->second.get();

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
            float panL = 1.0f, panR = 1.0f;
            panToGains(track.pan, panL, panR);

            float instPeakL = 0.0f, instPeakR = 0.0f;
            for (int ch = 0; ch < targetBuffer.getNumChannels(); ++ch)
            {
                const auto pg = (ch == 0 ? panL : (ch == 1 ? panR : 1.0f)) * trackGain;
                const auto srcCh = juce::jmin(ch, slot->scratch.getNumChannels() - 1);
                const auto mag = slot->scratch.getMagnitude(srcCh, 0, numSamples) * pg;
                if (ch == 0)      instPeakL = juce::jmax(instPeakL, mag);
                else if (ch == 1) instPeakR = juce::jmax(instPeakR, mag);
                targetBuffer.addFrom(ch, startSample, slot->scratch, srcCh, 0, numSamples, pg);
            }
            accumulateTrackPeakStereo(trackIndex, instPeakL, instPeakR);
        }
    }

    static void accumulateAtomicMax(std::atomic<float>& slot, float peak) noexcept
    {
        float prev = slot.load(std::memory_order_relaxed);
        while (peak > prev && ! slot.compare_exchange_weak(prev, peak, std::memory_order_relaxed))
        {
        }
    }

    // Record a track's loudest magnitude this block (keeps the max until the UI reads it).
    // The mono variant feeds both channels equally.
    void accumulateTrackPeak(int trackIndex, float peak) noexcept
    {
        accumulateTrackPeakStereo(trackIndex, peak, peak);
    }

    void accumulateTrackPeakStereo(int trackIndex, float peakL, float peakR) noexcept
    {
        if (trackIndex < 0 || trackIndex >= maxMeterTracks)
            return;
        if (peakL > 0.0f)
            accumulateAtomicMax(trackPeaksL[static_cast<std::size_t>(trackIndex)], peakL);
        if (peakR > 0.0f)
            accumulateAtomicMax(trackPeaksR[static_cast<std::size_t>(trackIndex)], peakR);
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
    juce::CriticalSection warpCacheLock;   // guards warpedAudioCache (audio vs message thread)
    juce::CriticalSection instrumentLock;
    std::map<int, std::unique_ptr<InstrumentSlot>> instruments;

    static constexpr int maxMeterTracks = 512;
    std::array<std::atomic<float>, maxMeterTracks> trackPeaksL {};
    std::array<std::atomic<float>, maxMeterTracks> trackPeaksR {};
    juce::AudioBuffer<float> samplerMeterScratch;
    juce::AudioBuffer<float> trackMeterBefore;   // per-track mix snapshot for accurate metering
    juce::AudioBuffer<float> trackFxScratch;     // isolated per-track buffer for insert FX
    std::map<int, std::vector<std::unique_ptr<InsertSlot>>> trackInserts;
    juce::CriticalSection insertLock;
    std::vector<juce::AudioBuffer<float>> busBuffers;   // per-block aux-bus accumulation
    std::array<std::atomic<float>, maxBuses> busPeaksL {};
    std::array<std::atomic<float>, maxBuses> busPeaksR {};
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

        // CRITICAL: clear our slice first. MixerAudioSource reuses a single temp buffer
        // across inputs, so if we leave it untouched (e.g. metronome off) it still holds
        // the PREVIOUS input's audio (the arrangement) — which the mixer would then add a
        // second time, doubling the level on the master. Always start from silence.
        info.clearActiveBufferRegion();

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

        const auto accumulate = [](std::atomic<float>& slot, float value)
        {
            float prev = slot.load(std::memory_order_relaxed);
            while (value > prev && ! slot.compare_exchange_weak(prev, value, std::memory_order_relaxed))
            {
            }
        };

        const auto gain = gainLinear.load(std::memory_order_relaxed);
        if (std::abs(gain - 1.0f) > 1.0e-4f)
            info.buffer->applyGain(info.startSample, info.numSamples, gain);

        const auto numCh = info.buffer->getNumChannels();
        const auto peakL = numCh > 0 ? info.buffer->getMagnitude(0, info.startSample, info.numSamples) : 0.0f;
        const auto peakR = numCh > 1 ? info.buffer->getMagnitude(1, info.startSample, info.numSamples) : peakL;

        accumulate(meterPeakL, peakL);
        accumulate(meterPeakR, peakR);
    }

    void setGainDb(double db) noexcept
    {
        gainLinear.store(juce::Decibels::decibelsToGain(static_cast<float>(db)), std::memory_order_relaxed);
    }

    // Returns the peak magnitude observed since the previous call, then resets it.
    float fetchAndResetPeak() noexcept
    {
        float l = 0.0f, r = 0.0f;
        fetchAndResetPeakStereo(l, r);
        return juce::jmax(l, r);
    }

    void fetchAndResetPeakStereo(float& outL, float& outR) noexcept
    {
        outL = meterPeakL.exchange(0.0f, std::memory_order_relaxed);
        outR = meterPeakR.exchange(0.0f, std::memory_order_relaxed);
    }

private:
    juce::AudioSource& downstream;
    std::atomic<float> gainLinear { 1.0f };
    std::atomic<float> meterPeakL { 0.0f };
    std::atomic<float> meterPeakR { 0.0f };
};
}  // namespace orion
