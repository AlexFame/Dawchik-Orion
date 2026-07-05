#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <signalsmith-stretch/signalsmith-stretch.h>

#if JUCE_MAC || JUCE_IOS
 #include <pthread.h>
 #include <sys/qos.h>
#endif

#include "../Audio/TransportEngine.h"
#include "../Audio/WarpEngine.h"
#include "../Core/ProjectState.h"
#include "../Sampler/SamplerEngine.h"

// Audio render sources extracted from MainComponent. These used to be private
// nested classes of MainComponent; they are pure audio-thread render units with
// no UI dependency, so they live in Audio/ and will host per-track plugins later.
namespace orion
{
constexpr double vstPitchBendRangeSemitones = 48.0;  // wide for MPE glides (set on the synth via RPN)

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

// Instant, length-independent clip-editor preview: time-stretches (and optionally
// pitch-shifts) a region to a target length with signalsmith, but starts playing from
// sample 0 IMMEDIATELY while a background producer thread fills the rest ahead of the
// playhead — so Play never waits on a full render, no matter how long the loop is. A
// high-quality RubberBand buffer can swap in afterwards (handled by the host).
class StreamingWarpPreviewSource final : public juce::PositionableAudioSource,
                                         private juce::Thread
{
public:
    StreamingWarpPreviewSource(juce::AudioBuffer<float> sourceRegion, double sr,
                               int targetSamples, double pitchScale)
        : juce::Thread("ClipPreviewWarp"),
          input(std::move(sourceRegion)),
          sampleRate(sr > 0.0 ? sr : 44100.0),
          totalOut(juce::jmax(1, targetSamples))
    {
        channels = juce::jlimit(1, 2, input.getNumChannels());
        out.setSize(channels, totalOut);
        out.clear();
        loopStartSample.store(0, std::memory_order_relaxed);
        loopEndSample.store(totalOut, std::memory_order_relaxed);
        stretch.presetDefault(channels, static_cast<float>(sampleRate));
        stretch.setTransposeFactor(static_cast<float>(juce::jlimit(0.25, 4.0, pitchScale)));
        startThread();

        // Wait briefly (capped) for a small playback lead so the very first transient
        // isn't clipped. The producer runs far faster than real time, so in practice this
        // returns within a few milliseconds — playback still feels instant.
        const int lead = juce::jmin(totalOut, 8192);
        const auto deadlineMs = juce::Time::getMillisecondCounter() + 250;
        while (producedSamples.load(std::memory_order_acquire) < lead
               && juce::Time::getMillisecondCounter() < deadlineMs)
            juce::Thread::sleep(2);
    }

    ~StreamingWarpPreviewSource() override
    {
        stopThread(2000);
    }

    void prepareToPlay(int, double) override {}
    void releaseResources() override {}

    void run() override
    {
        const double inRatio = static_cast<double>(input.getNumSamples()) / juce::jmax(1, totalOut);

        int inputPos = 0;
        const int seekLength = juce::jmin(input.getNumSamples(), stretch.outputSeekLength(static_cast<float>(inRatio)));
        if (seekLength > 0)
        {
            const float* inPtrs[2] = { input.getReadPointer(0),
                                       channels > 1 ? input.getReadPointer(juce::jmin(1, input.getNumChannels() - 1)) : nullptr };
            stretch.outputSeek(inPtrs, seekLength);
            inputPos = seekLength;
        }

        juce::AudioBuffer<float> scratch(channels, 4096);
        const int scratchCap = scratch.getNumSamples();
        int produced = 0;
        while (produced < totalOut && ! threadShouldExit())
        {
            const int outChunk = juce::jmin(2048, totalOut - produced);
            const int inChunk = juce::jlimit(1, scratchCap, static_cast<int>(std::llround(outChunk * inRatio)));
            for (int c = 0; c < channels; ++c)
            {
                auto* dst = scratch.getWritePointer(c);
                const int sc = juce::jmin(c, input.getNumChannels() - 1);
                for (int i = 0; i < inChunk; ++i)
                {
                    const int idx = inputPos + i;
                    dst[i] = idx < input.getNumSamples() ? input.getSample(sc, idx) : 0.0f;
                }
            }
            inputPos += inChunk;
            const float* inPtrs[2]  = { scratch.getReadPointer(0), channels > 1 ? scratch.getReadPointer(1) : nullptr };
            float*       outPtrs[2] = { out.getWritePointer(0) + produced, channels > 1 ? out.getWritePointer(1) + produced : nullptr };
            stretch.process(inPtrs, inChunk, outPtrs, outChunk);
            produced += outChunk;
            producedSamples.store(produced, std::memory_order_release);
        }
    }

    // AKAI MPC-style: continuously loop the [loopStart, loopEnd] region. The bounds can be
    // moved live (setLoopBounds) while playing and the loop follows immediately — the whole
    // source is rendered, so the markers can be dragged anywhere.
    void setLoopBounds(double startRatio, double endRatio) noexcept
    {
        const int ls = juce::jlimit(0, juce::jmax(0, totalOut - 1), static_cast<int>(std::llround(startRatio * totalOut)));
        const int le = juce::jlimit(ls + 1, totalOut, static_cast<int>(std::llround(endRatio * totalOut)));
        loopStartSample.store(ls, std::memory_order_relaxed);
        loopEndSample.store(le, std::memory_order_relaxed);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        if (info.numSamples <= 0)
            return;

        const int ls = loopStartSample.load(std::memory_order_relaxed);
        const int le = juce::jmax(ls + 1, loopEndSample.load(std::memory_order_relaxed));
        const int ready = juce::jmin(totalOut, producedSamples.load(std::memory_order_acquire));

        int pos = static_cast<int>(positionSamples);
        if (pos >= le || pos < 0 || pos >= totalOut)
            pos = ls;   // wrap at the loop end. NB: don't snap when pos < ls — if START is
                        // dragged past the playhead we let the current pass finish and wrap
                        // naturally; snapping every block was what crackled while dragging.

        // Short S-shaped fade in at the loop start and out at the loop end so the seam doesn't click
        // when it wraps. Timing is preserved (no samples skipped), so a tempo-locked loop stays in sync.
        const int loopLen = le - ls;
        const int fadeLen = (loopLen > 8) ? juce::jmin(loopLen / 4, static_cast<int>(sampleRate * 0.004)) : 0;
        const auto smoothstep = [](float x) { x = juce::jlimit(0.0f, 1.0f, x); return x * x * (3.0f - 2.0f * x); };
        const int nch = info.buffer->getNumChannels();

        int written = 0;
        while (written < info.numSamples)
        {
            if (pos >= le)
                pos = ls;                 // wrap at the loop end
            const int cap = juce::jmin(le, ready);
            if (pos >= cap)
                break;                    // producer hasn't filled this far yet → silence rest
            const int n = juce::jmin(info.numSamples - written, cap - pos);
            for (int i = 0; i < n; ++i)
            {
                const int p = pos + i;
                float g = 1.0f;
                if (fadeLen > 0)
                {
                    if (p - ls < fadeLen) g *= smoothstep(static_cast<float>(p - ls) / static_cast<float>(fadeLen));
                    if (le - p <= fadeLen) g *= smoothstep(static_cast<float>(le - p) / static_cast<float>(fadeLen));
                }
                for (int c = 0; c < nch; ++c)
                {
                    const int sc = juce::jmin(c, out.getNumChannels() - 1);
                    info.buffer->setSample(c, info.startSample + written + i, out.getSample(sc, p) * g);
                }
            }
            pos += n;
            written += n;
        }

        if (pos >= le)
            pos = ls;   // keep the stored position inside the loop so the transport never
                        // sees "stream finished" (which would stop playback at loopEnd)
        positionSamples = pos;
    }

    void setNextReadPosition(juce::int64 newPosition) override
    {
        positionSamples = juce::jlimit<juce::int64>(0, totalOut, newPosition);
    }

    juce::int64 getNextReadPosition() const override { return positionSamples; }
    juce::int64 getTotalLength() const override       { return totalOut; }
    bool isLooping() const override                   { return false; }
    double getSampleRate() const noexcept             { return sampleRate; }

private:
    juce::AudioBuffer<float> input;
    double sampleRate { 44100.0 };
    int totalOut { 1 };
    int channels { 1 };
    juce::AudioBuffer<float> out;
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    std::atomic<int> producedSamples { 0 };
    std::atomic<int> loopStartSample { 0 };
    std::atomic<int> loopEndSample { 1 };
    juce::int64 positionSamples { 0 };
};

class ArrangementPlaybackSource final : public juce::AudioSource
{
public:
    ArrangementPlaybackSource(ProjectState& state, TransportEngine& engine, juce::AudioFormatManager& formatManager)
        : project(state),
          transport(engine),
          audioFormatManager(formatManager),
          // Sampler warp uses the HIGH-QUALITY (RubberBand) backend — the fast signalsmith
          // preview thinned out the low end. To keep playback instant despite the heavier
          // render, the buffer is pre-warped in the background on load (prewarmWarp), so by
          // the time a note is played it's already a cache hit. A live note that arrives
          // before the pre-warm finishes waits for that in-flight job (see getWarpedSampleData)
          // rather than re-rendering — no quality loss, no dry-then-swap.
          samplerEngine(audioFormatManager,
                        [](const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate, const juce::String& sourcePath)
                        {
                            return stretchBufferToLengthWithExperimentalBackend(source, outputSamples, sampleRate, sourcePath);
                        })
    {
    }

    ~ArrangementPlaybackSource() override
    {
        stopWarpProducer();   // join the producer before its data (streams/caches) is destroyed
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
                       double sourceBpm,
                       bool fullSampleTrigger = false)
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
                             project.getTempoBpm(),
                             fullSampleTrigger);
    }

    void samplerNoteOff(int midiNote, SamplerPlaybackMode playbackMode, bool gateByNoteLength = false)
    {
        samplerEngine.noteOff(midiNote, playbackMode, gateByNoteLength);
    }

    // Pre-build the warped sampler buffer in the background (called when a sample is loaded /
    // warp is enabled) so the first note plays warped instantly instead of computing on demand.
    void prewarmSamplerWarp(const juce::String& sourcePath, double sourceBpm)
    {
        samplerEngine.prewarmWarp(sourcePath, sourceBpm, project.getTempoBpm());
    }

    void allSamplerNotesOff()
    {
        samplerEngine.allNotesOff();
    }

    // Live portamento for the internal sampler. (VST glide is handled separately.)
    void setSamplerGlide(bool enabled, double glideTimeSeconds)
    {
        samplerEngine.setGlide(enabled, glideTimeSeconds);
    }

    // Live gain for sampler audition voices (heard immediately, not only on next trigger).
    void setSamplerLiveGainDb(double db)
    {
        samplerEngine.setLiveGainDb(db);
    }

    // Transient slice points for the armed sampler track's live/pad playback.
    void setSamplerLiveSlicePoints(const std::vector<double>& points)
    {
        samplerEngine.setLiveSlicePoints(points);
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

    // A track was removed from the project at `removedTrackIndex`. Drop that track's hosted
    // instrument + insert chain (releasing them so any held voice stops), and shift every
    // higher-indexed slot down by one so the engine's per-track maps stay aligned with the
    // project's track vector. Without this, deleting a track left every lower instrument keyed
    // to the wrong project index — a currently-held note never received its note-off and rang
    // forever. Surviving instruments keep their LIVE instances (no reinstantiation, no state
    // loss, no audio flush). The removed track's plugin is kept ALIVE in the instrument stash
    // (see below) so an undo can re-home the exact same instance instantly.
    void removeTrackAndReindex(int removedTrackIndex)
    {
        std::vector<std::unique_ptr<InsertSlot>> removedInserts;
        {
            const juce::ScopedLock sl(instrumentLock);

            std::map<int, std::unique_ptr<InstrumentSlot>> remappedInstruments;
            for (auto& [idx, slot] : instruments)
            {
                if (idx == removedTrackIndex)
                {
                    if (slot != nullptr)
                        instrumentStash.push_back(std::move(slot));   // keep alive for undo
                }
                else
                {
                    const auto newIdx = idx > removedTrackIndex ? idx - 1 : idx;
                    remappedInstruments[newIdx] = std::move(slot);
                }
            }
            instruments = std::move(remappedInstruments);

            std::map<int, std::vector<std::unique_ptr<InsertSlot>>> remappedInserts;
            for (auto& [idx, chain] : trackInserts)
            {
                if (idx == kMasterInsertKey)
                    remappedInserts[idx] = std::move(chain);        // master chain is not per-track
                else if (idx == removedTrackIndex)
                    removedInserts = std::move(chain);
                else
                {
                    const auto newIdx = idx > removedTrackIndex ? idx - 1 : idx;
                    remappedInserts[newIdx] = std::move(chain);
                }
            }
            trackInserts = std::move(remappedInserts);
        }

        // Release the removed inserts outside the audio lock.
        for (auto& ins : removedInserts)
            if (ins != nullptr && ins->instance != nullptr)
                ins->instance->releaseResources();

        trimInstrumentStash(kInstrumentStashLimit);
    }

    // ---- Instrument stash: instant, lossless track delete + undo ----------------
    // Deleting a track parks its (live) plugin here instead of destroying it; undo re-homes
    // live instances by plugin id. So delete+undo is just pointer moves — instant, with the
    // exact same instance and state — instead of reinstantiating every plugin (which froze).
    //
    // Atomically (under a single lock, so the audio thread never sees a half-empty map) park
    // every mounted instrument and re-home them onto `wanted` (track index -> plugin id) by
    // reusing matching LIVE instances from the stash. Returns the wanted entries that had no
    // live match, so the caller can reinstantiate just those from saved state (rare).
    std::vector<std::pair<int, juce::String>> rehomeInstrumentsFromStash(
        const std::vector<std::pair<int, juce::String>>& wanted)
    {
        std::vector<std::pair<int, juce::String>> unsatisfied;
        const juce::ScopedLock sl(instrumentLock);

        for (auto& [idx, slot] : instruments)
        {
            juce::ignoreUnused(idx);
            if (slot != nullptr)
                instrumentStash.push_back(std::move(slot));
        }
        instruments.clear();

        for (const auto& [index, pluginId] : wanted)
        {
            bool found = false;
            for (auto it = instrumentStash.begin(); it != instrumentStash.end(); ++it)
            {
                auto& s = *it;
                if (s != nullptr && s->instance != nullptr
                    && s->instance->getPluginDescription().createIdentifierString() == pluginId)
                {
                    instruments[index] = std::move(s);
                    instrumentStash.erase(it);
                    found = true;
                    break;
                }
            }
            if (! found)
                unsatisfied.push_back({ index, pluginId });
        }
        return unsatisfied;
    }

    // Bound the stash's memory: release all but the `keep` most-recent parked plugins.
    void trimInstrumentStash(std::size_t keep)
    {
        std::vector<std::unique_ptr<InstrumentSlot>> toRelease;
        {
            const juce::ScopedLock sl(instrumentLock);
            while (instrumentStash.size() > keep)
            {
                toRelease.push_back(std::move(instrumentStash.front()));
                instrumentStash.erase(instrumentStash.begin());
            }
        }
        for (auto& s : toRelease)
            if (s != nullptr && s->instance != nullptr)
                s->instance->releaseResources();
    }

    static constexpr std::size_t kInstrumentStashLimit = 24;

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

    // Live monitoring: forward an arbitrary controller message (CC, pitch bend,
    // aftertouch, channel pressure) from a hardware MIDI keyboard straight to the
    // instrument on a track. Note on/off go through the dedicated helpers above so
    // they pick up the engine's channel-1 convention; everything else passes through
    // verbatim. Thread-safe.
    void instrumentLiveMidiMessage(int trackIndex, const juce::MidiMessage& message)
    {
        const juce::ScopedLock sl(instrumentLock);
        if (const auto it = instruments.find(trackIndex); it != instruments.end() && it->second != nullptr)
            it->second->pendingLiveMidi.addEvent(message, 0);
    }

    void allInstrumentNotesOff()
    {
        const juce::ScopedLock sl(instrumentLock);
        for (auto& [trackIndex, slot] : instruments)
        {
            juce::ignoreUnused(trackIndex);
            if (slot != nullptr)
            {
                slot->pendingLiveMidi.clear();
                slot->pendingPanicBlocks = 3;   // a few blocks is enough to release every voice
            }
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
            const auto beatsPerSecondStopped = project.getTempoBpm() / 60.0;

            // Falling edge: start a short fade-out tail from the last playing position so the
            // arrangement audio doesn't cut with a click.
            if (wasPlaying)
            {
                declickRemaining = juce::jmax(1, static_cast<int>(outputSampleRate * 0.006));
                declickTotal = declickRemaining;
                wasPlaying = false;
            }

            if (declickRemaining > 0 && beatsPerSecondStopped > 0.0)
            {
                const int n = juce::jmin(info.numSamples, declickRemaining);
                const auto loopActive = transport.isLoopEnabled() && project.hasLoopRange();
                // Audio clips only (includeInstruments=false) — re-triggering MIDI here would
                // fire note-ons with no matching note-off and leave stuck/ringing notes.
                renderAudioIntoBuffer(*info.buffer, info.startSample, n, currentTimelineBeat, outputSampleRate, loopActive, ! loopActive, true, false);
                const auto g0 = static_cast<float>(declickRemaining) / static_cast<float>(declickTotal);
                const auto g1 = static_cast<float>(declickRemaining - n) / static_cast<float>(declickTotal);
                for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
                    info.buffer->applyGainRamp(ch, info.startSample, n, g0, g1);
                currentTimelineBeat += static_cast<double>(n) * beatsPerSecondStopped / outputSampleRate;
                declickRemaining -= n;
            }
            else
            {
                currentTimelineBeat = transport.getPlayheadBeat();
            }

            samplerEngine.renderLiveNotes(*info.buffer, info.startSample, info.numSamples, outputSampleRate);
            processInstruments(*info.buffer, info.startSample, info.numSamples,
                               currentTimelineBeat, beatsPerSecondStopped,
                               false, true, false);
            return;
        }
        declickRemaining = 0;   // playing again — cancel any pending fade tail

        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0)
            return;

        const auto loopActive = transport.isLoopEnabled() && project.hasLoopRange();
        const auto loopStartBeat = project.getLoopStartBeat();
        const auto loopEndBeat = project.getLoopEndBeat();
        const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);
        if (! loopActive && project.getPlaybackEndInBeats() <= 0.0)
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
            // First playing block: chase notes sitting under the start playhead so the
            // very first note (e.g. one exactly at the part start) is always articulated.
            chaseNotesAtStart = true;
        }

        const auto beatAdvancePerSample = beatsPerSecond / outputSampleRate;
        renderAudioIntoBuffer(*info.buffer, info.startSample, info.numSamples, currentTimelineBeat, outputSampleRate, loopActive, ! loopActive, true);
        samplerEngine.renderLiveNotes(*info.buffer, info.startSample, info.numSamples, outputSampleRate);

        currentTimelineBeat += static_cast<double>(info.numSamples) * beatAdvancePerSample;
        while (loopActive && currentTimelineBeat >= loopEndBeat)
            currentTimelineBeat = loopStartBeat + std::fmod(currentTimelineBeat - loopStartBeat, loopSpanBeats);
        if (! loopActive)
        {
            const auto repeatEndBeat = project.getPlaybackEndInBeats();
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

public:
    // ===== Real-time streaming warp (Ableton-style) ==============================
    // A background PRODUCER thread fills each warp clip's stretched output buffer ahead
    // of the playhead; the audio thread only READS it (cheap → no dropouts), and Play is
    // instant (no upfront pre-render). Output buffers are filled once (never erased), so
    // a pointer handed to the audio thread stays valid.
    struct WarpStream
    {
        signalsmith::stretch::SignalsmithStretch<float> stretch;
        AudioFileData out;                       // out.buffer (targetSamples): producer writes, audio reads
        const AudioFileData* original { nullptr };
        int targetSamples { 0 };
        int originalSamples { 0 };
        int channels { 1 };
        int inputPos { 0 };                      // producer-only
        bool primed { false };                   // producer-only
        std::atomic<int> producedSamples { 0 };  // producer (release) → audio (acquire)
        std::atomic<int> requestedSamples { 0 }; // audio → producer (how far to fill)

        // High-quality offline (RubberBand) render, built lazily on the producer
        // thread. Once ready, playback swaps up from the streaming stand-in to this.
        juce::String sourcePath;
        int semis { 0 };
        std::atomic<bool> offlineBuilt { false };
    };

    static juce::String warpStreamKey(const juce::String& path, int targetSamples, int semis)
    {
        return path + "|" + juce::String(targetSamples) + "|p" + juce::String(semis);
    }

    void setRealtimeWarpEnabled(bool e) noexcept { realtimeWarpEnabled.store(e, std::memory_order_relaxed); }
    bool isRealtimeWarpEnabled() const noexcept { return realtimeWarpEnabled.load(std::memory_order_relaxed); }

    // Message thread: configure (cheap) a streaming stretcher per warp/pitch clip, capture
    // the decoded original, and ensure the producer thread is running. No stretching here.
    // blockForLead: briefly wait for a playback lead before returning (used at Play start so the
    // first block has audio). Pass false to re-prep WHILE PLAYING (e.g. live time-stretch drag) —
    // the new-length stream builds in the background and the render swaps to it when ready, so a
    // drag changes speed on the fly without stopping/replaying and without stalling the UI thread.
    // allowSyncDecode: if a source isn't decoded yet, decode it synchronously HERE (message thread
    // only) so Play is guaranteed to have audio — a strict fallback that preserves the old
    // "plays immediately" behaviour. Off elsewhere (timer / live drag), which stay freeze-free via
    // the background prewarm.
    void prepareWarpStreams(bool blockForLead = true, bool allowSyncDecode = false)
    {
        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0)
            return;

        for (const auto& track : project.getTracks())
            for (const auto& clip : track.clips)
            {
                if (clip.type != ClipType::audio || clip.sourcePath.isEmpty() || clip.lengthInBeats <= 0.0)
                    continue;
                const auto semis = computeKeyShiftSemitones(clip);
                if (! clip.warpEnabled && semis == 0)
                    continue;
                const auto* original = getAudioFileDataCached(clip.sourcePath);
                if (original == nullptr)
                {
                    prewarmAudioFile(clip.sourcePath);   // always kick a background decode
                    // Fallback (Play only): decode synchronously on this message thread so the
                    // stream is ready immediately — never regress "plays right away". This never
                    // runs on the audio thread and is skipped for the timer / live-drag paths.
                    if (allowSyncDecode)
                        original = getAudioFileData(clip.sourcePath);
                }
                if (original == nullptr || original->sampleRate <= 0.0 || original->buffer.getNumSamples() <= 0)
                    continue;
                const auto warpLengthInBeats = clip.warpTargetLengthInBeats > 0.0 ? clip.warpTargetLengthInBeats : clip.lengthInBeats;
                const auto targetSamples = juce::jmax(1, static_cast<int>(std::round((warpLengthInBeats / beatsPerSecond) * original->sampleRate)));
                const auto key = warpStreamKey(clip.sourcePath, targetSamples, semis).toStdString();
                {
                    const juce::ScopedLock sl(warpStreamLock);
                    if (warpStreams.find(key) != warpStreams.end())
                        continue;
                }
                auto s = std::make_unique<WarpStream>();
                s->original = original;
                s->channels = juce::jlimit(1, 2, original->buffer.getNumChannels());
                s->originalSamples = original->buffer.getNumSamples();
                s->targetSamples = targetSamples;
                s->sourcePath = clip.sourcePath;
                s->semis = semis;
                s->stretch.presetDefault(s->channels, static_cast<float>(original->sampleRate));
                s->stretch.setTransposeSemitones(static_cast<float>(semis));
                s->out.sampleRate = original->sampleRate;
                s->out.buffer.setSize(s->channels, targetSamples);
                s->out.buffer.clear();
                // Pre-fill the ENTIRE loop while stopped (loops are short), so the first Play
                // never reads samples the producer is still writing — that race was the crackle.
                s->requestedSamples.store(targetSamples, std::memory_order_relaxed);
                {
                    const juce::ScopedLock sl(warpStreamLock);
                    warpStreams[key] = std::move(s);
                }
            }

        // Make sure every existing stream is also targeted for a full fill (e.g. created
        // before this clip's request) so nothing is left half-stretched at Play time.
        {
            const juce::ScopedLock sl(warpStreamLock);
            for (auto& kv : warpStreams)
                kv.second->requestedSamples.store(kv.second->targetSamples, std::memory_order_relaxed);
        }

        startWarpProducer();
        warpProducerWake.signal();
        warpOfflineWake.signal();

        // Block briefly (capped) until every stream has a playback lead ready, so the
        // FIRST Play has samples to read instead of silence. Without this wait the
        // transport starts while producedSamples == 0 and streamWarpData() returns
        // nullptr for every block — that's why playback used to "warp in" only after a
        // few presses, each getting a little further as the producer caught up. The
        // producer runs far faster than real time, so short loops fully fill within a
        // few ms; long clips fill the lead here and stream the remainder ahead of the
        // playhead. The deadline guarantees the UI thread never freezes.
        std::vector<WarpStream*> pending;
        {
            const juce::ScopedLock sl(warpStreamLock);
            pending.reserve(warpStreams.size());
            for (auto& kv : warpStreams)
                pending.push_back(kv.second.get());
        }
        if (blockForLead && ! pending.empty())
        {
            const auto deadlineMs = juce::Time::getMillisecondCounter() + 400;
            for (;;)
            {
                bool allReady = true;
                for (auto* s : pending)
                {
                    // ~0.75s lead per stream (or the whole thing if shorter).
                    const int lead = juce::jmin(s->targetSamples,
                                                static_cast<int>(s->out.sampleRate * 0.75));
                    if (s->producedSamples.load(std::memory_order_acquire) < lead)
                    {
                        allReady = false;
                        break;
                    }
                }
                if (allReady || juce::Time::getMillisecondCounter() >= deadlineMs)
                    break;
                warpProducerWake.signal();
                juce::Thread::sleep(2);
            }
        }
    }

    // Audio thread: request the producer fill up to `neededOut` and return the output buffer.
    // `readyOut` receives the producer's high-water mark (samples actually written, acquired
    // so they pair with the producer's release) — the caller must NOT read at or beyond it,
    // because those indices may be cleared zeros or being written right now. We hand the
    // buffer back even when the producer is still catching up (instead of returning nullptr
    // for the whole block): the caller silences only the not-yet-filled tail per-sample. The
    // old all-or-nothing nullptr made a hard stretch pump and drop out whenever the producer
    // briefly fell behind the playhead — every such block went fully silent, and as the
    // playhead outran the producer the dropouts grew until the clip went silent.
    const AudioFileData* streamWarpData(const juce::String& path, int targetSamples, int semis,
                                        int neededOut, int& readyOut)
    {
        readyOut = 0;
        WarpStream* s = nullptr;
        WarpStream* fallback = nullptr;   // most-complete sibling of a different length
        int fallbackReady = 0;
        int fallbackLenDist = std::numeric_limits<int>::max();
        {
            const juce::ScopedTryLock stl(warpStreamLock);
            if (! stl.isLocked())
                return nullptr;

            const auto it = warpStreams.find(warpStreamKey(path, targetSamples, semis).toStdString());
            if (it != warpStreams.end())
            {
                s = it->second.get();
                if (neededOut > s->requestedSamples.load(std::memory_order_relaxed))
                {
                    s->requestedSamples.store(neededOut, std::memory_order_relaxed);
                    warpProducerWake.signal();
                }
                const int ready = s->producedSamples.load(std::memory_order_acquire);
                // Primary stream already covers what this block needs → use the exact new length.
                if (ready >= neededOut)
                {
                    readyOut = ready;
                    return &s->out;
                }
            }

            // Primary isn't filled up to the playhead yet (a freshly-created live-stretch stream
            // whose producer is still catching up from sample 0). Rather than emit SILENCE for the
            // gap, keep the PREVIOUS stretch of the same source/pitch audible — identical pitch, only
            // a hair of tempo drift — until the new length catches up, then we swap to it seamlessly.
            // Streams are never erased, so this fallback pointer stays valid after the lock is released.
            for (auto& kv : warpStreams)
            {
                auto* c = kv.second.get();
                if (c == s || c->semis != semis || c->sourcePath != path)
                    continue;
                if (c->producedSamples.load(std::memory_order_acquire) < c->targetSamples)
                    continue;   // only fully-produced siblings are safe to read end-to-end
                const int dist = std::abs(c->targetSamples - targetSamples);
                if (dist < fallbackLenDist)
                {
                    fallbackLenDist = dist;
                    fallback = c;
                    fallbackReady = c->producedSamples.load(std::memory_order_acquire);
                }
            }
        }

        if (fallback != nullptr)
        {
            readyOut = fallbackReady;
            return &fallback->out;
        }

        // No continuity fallback available: hand back the primary with whatever it has produced so
        // far (its filled head plays; the unfilled tail stays silent per-sample, as before).
        if (s != nullptr)
        {
            readyOut = s->producedSamples.load(std::memory_order_acquire);
            return &s->out;
        }
        return nullptr;
    }

    void startWarpProducer()
    {
        if (warpProducerRunning.load(std::memory_order_acquire))
            return;
        warpProducerRunning.store(true, std::memory_order_release);
        warpProducerThread = std::thread([this] { warpProducerLoop(); });
        warpOfflineThread  = std::thread([this] { warpOfflineLoop(); });
    }

    void stopWarpProducer()
    {
        warpProducerRunning.store(false, std::memory_order_release);
        warpProducerWake.signal();
        warpOfflineWake.signal();
        if (warpProducerThread.joinable())
            warpProducerThread.join();
        if (warpOfflineThread.joinable())
            warpOfflineThread.join();
    }

    // Dedicated low-traffic thread that renders the high-quality OFFLINE (RubberBand)
    // buffer for each warp clip and publishes it to the cache. Kept SEPARATE from the
    // streaming producer so a heavy multi-second render never blocks the real-time
    // stream fill (that blocking was what made Play stutter right after a drop).
    // Playback swaps up from the streaming stand-in the moment the buffer appears.
    void warpOfflineLoop()
    {
       #if JUCE_MAC || JUCE_IOS
        // Background QoS: the OS deprioritises this thread under load, so even a
        // long render already in progress yields CPU to the audio + streaming
        // threads instead of starving them (no dropouts during playback).
        pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
       #endif
        while (warpProducerRunning.load(std::memory_order_acquire))
        {
            // The RubberBand render is CPU-heavy. While STOPPED we pre-render any pending stream so
            // the next Play is instantly high-quality. While PLAYING we still build the HQ buffer —
            // but ONLY for a stream whose real-time (signalsmith) stand-in is ALREADY fully produced,
            // so we never compete with the producer (that contention was the dropout risk). This lets
            // the HQ low-end swap in a couple seconds after a live time-stretch WITHOUT a stop/play —
            // previously the degraded stand-in's low end played until you stopped long enough.
            const bool playing = transport.isPlaying() || transport.isCountInActive();

            WarpStream* pending = nullptr;
            {
                const juce::ScopedLock sl(warpStreamLock);
                for (auto& kv : warpStreams)
                {
                    auto* s = kv.second.get();
                    if (s->offlineBuilt.load(std::memory_order_acquire) || s->original == nullptr)
                        continue;
                    // While playing, wait until the producer has fully filled this stream's stand-in
                    // (no CPU contention) before spending cycles on its offline render.
                    if (playing && s->producedSamples.load(std::memory_order_acquire) < s->targetSamples)
                        continue;
                    pending = s;
                    break;
                }
            }

            if (pending == nullptr)
            {
                warpOfflineWake.wait(playing ? 60 : 50);
                continue;
            }

            buildOfflineWarpBuffer(*pending);
            pending->offlineBuilt.store(true, std::memory_order_release);
        }
    }

    // Background thread: keep every stream's output filled ahead of the playhead.
    void warpProducerLoop()
    {
       #if JUCE_MAC || JUCE_IOS
        // This thread feeds the real-time audio callback, so it must NOT be deprioritised
        // like the offline (RubberBand) thread. USER_INITIATED keeps it scheduled ahead of
        // ordinary background work so it stays ahead of the playhead even on a hard stretch
        // under load — otherwise it falls behind and the clip pumps/drops out on first play.
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
       #endif
        while (warpProducerRunning.load(std::memory_order_acquire))
        {
            bool didWork = false;
            std::vector<WarpStream*> snapshot;
            {
                const juce::ScopedLock sl(warpStreamLock);
                snapshot.reserve(warpStreams.size());
                for (auto& kv : warpStreams)
                    snapshot.push_back(kv.second.get());
            }
            for (auto* s : snapshot)
            {
                const auto lookahead = static_cast<int>(s->out.sampleRate * 2.0);   // stay ~2s ahead
                const auto want = juce::jmin(s->targetSamples, s->requestedSamples.load(std::memory_order_relaxed) + lookahead);
                if (s->producedSamples.load(std::memory_order_relaxed) < want)
                {
                    produceWarpChunk(*s, want);
                    didWork = true;
                }
            }
            if (! didWork)
                warpProducerWake.wait(20);
        }
    }

    // Background thread: extend a stream's stretched output toward `want` (bounded step).
    void produceWarpChunk(WarpStream& s, int want)
    {
        if (s.original == nullptr)
            return;
        want = juce::jmin(want, s.targetSamples);
        const int ch = s.channels;
        const double inRatio = static_cast<double>(s.originalSamples) / juce::jmax(1, s.targetSamples);

        if (warpProducerScratch.getNumSamples() < 4096)
            warpProducerScratch.setSize(2, 4096, false, false, true);
        const int scratchCap = warpProducerScratch.getNumSamples();

        const auto feed = [&](int inChunk)
        {
            for (int c = 0; c < ch; ++c)
            {
                auto* dst = warpProducerScratch.getWritePointer(c);
                const auto srcCh = juce::jmin(c, s.original->buffer.getNumChannels() - 1);
                for (int i = 0; i < inChunk; ++i)
                {
                    const auto idx = s.inputPos + i;
                    dst[i] = idx < s.originalSamples ? s.original->buffer.getSample(srcCh, idx) : 0.0f;
                }
            }
            s.inputPos += inChunk;
        };

        // One-time priming so the output aligns to the clip start (mirrors exact()).
        if (! s.primed)
        {
            s.primed = true;
            const int seekLength = juce::jmin(s.originalSamples, s.stretch.outputSeekLength(static_cast<float>(inRatio)));
            if (seekLength > 0)
            {
                const float* inPtrs[2] = { s.original->buffer.getReadPointer(0),
                                           ch > 1 ? s.original->buffer.getReadPointer(juce::jmin(1, s.original->buffer.getNumChannels() - 1)) : nullptr };
                s.stretch.outputSeek(inPtrs, seekLength);
                s.inputPos = seekLength;
            }
        }

        int produced = s.producedSamples.load(std::memory_order_relaxed);
        const int stepTarget = juce::jmin(want, produced + 32768);   // bound per call so the loop stays responsive
        while (produced < stepTarget)
        {
            const int outChunk = juce::jmin(2048, stepTarget - produced);
            const int inChunk = juce::jlimit(1, scratchCap, static_cast<int>(std::llround(outChunk * inRatio)));
            feed(inChunk);
            const float* inPtrs[2]  = { warpProducerScratch.getReadPointer(0), ch > 1 ? warpProducerScratch.getReadPointer(1) : nullptr };
            float*       outPtrs[2] = { s.out.buffer.getWritePointer(0) + produced, ch > 1 ? s.out.buffer.getWritePointer(1) + produced : nullptr };
            s.stretch.process(inPtrs, inChunk, outPtrs, outChunk);
            produced += outChunk;
            s.producedSamples.store(produced, std::memory_order_release);
        }
    }

    // Producer thread: render the full high-quality offline buffer for a stream and
    // publish it under the same key getWarpedAudioFileData() reads. The expensive
    // stretch runs OUTSIDE the cache lock; only the insert is locked.
    void buildOfflineWarpBuffer(WarpStream& s)
    {
        if (s.original == nullptr || s.targetSamples <= 0)
            return;

        const auto key = s.sourcePath.toStdString() + "|" + std::to_string(s.targetSamples)
                       + "|p" + std::to_string(s.semis)
                       + "|" + currentWarpBackendTag().toStdString();
        {
            const juce::ScopedLock sl(warpCacheLock);
            if (warpedAudioCache.find(key) != warpedAudioCache.end())
                return;   // already built (e.g. by the clip editor)
        }

        const double pitchScale = std::pow(2.0, static_cast<double>(s.semis) / 12.0);
        auto data = std::make_unique<AudioFileData>();
        data->sampleRate = s.original->sampleRate;
        data->buffer = stretchBufferToLengthWithExperimentalBackend(
            s.original->buffer, s.targetSamples, s.original->sampleRate, s.sourcePath, pitchScale);

        const juce::ScopedLock sl(warpCacheLock);
        warpedAudioCache.emplace(key, std::move(data));
    }

private:
    void renderAudioIntoBuffer(juce::AudioBuffer<float>& targetBuffer,
                               int startSample,
                               int numSamples,
                               double blockStartBeat,
                               double renderSampleRate,
                               bool wrapToLoop,
                               bool wrapToProjectEnd,
                               bool isRealtime,
                               bool includeInstruments = true)
    {
        // Flush-to-zero: stretched/reverb tails produce denormal floats which are ~100x
        // slower to process on x86. Without this, routing through an extra bus buffer (e.g.
        // group/folder tracks) can spike CPU and crackle. Standard in every DAW.
        juce::ScopedNoDenormals noDenormals;

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
        const auto repeatEndBeat = project.getPlaybackEndInBeats();
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

            const auto trackGain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb + track.trackGainDb));
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

                // Realtime (audio thread): never decode here — a big file would stall the callback.
                // Use the cache only and prewarm off-thread. Offline render (export) must have the
                // data, so it decodes synchronously.
                const auto* originalAudioData = isRealtime ? getAudioFileDataCached(clip.sourcePath)
                                                           : getAudioFileData(clip.sourcePath);
                if (originalAudioData == nullptr || originalAudioData->buffer.getNumSamples() <= 0 || originalAudioData->sampleRate <= 0.0)
                {
                    if (isRealtime)
                        prewarmAudioFile(clip.sourcePath);
                    continue;
                }

                const auto clipStartBeat = clip.startBeat;
                const auto clipEndBeat = clip.startBeat + clip.lengthInBeats;
                const auto linearGain = juce::Decibels::decibelsToGain(static_cast<float>(clip.gainDb)) * trackGain;
                const auto* audioData = originalAudioData;
                // Highest sample index that is safe to read from `audioData`. For the
                // streaming warp stand-in this is the producer's high-water mark (the rest
                // of the buffer isn't filled yet); for fully-resident buffers it's the whole
                // buffer. Reads at/after this are silenced per-sample so a producer that's
                // momentarily behind only mutes the unfilled tail, never the whole block.
                int streamReadyLimit = std::numeric_limits<int>::max();
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
                    bool resolved = false;

                    // Prefer the high-quality OFFLINE (RubberBand) buffer the moment the
                    // background producer publishes it — clean low end, same as the clip
                    // editor. Until it's ready we stream as an instant stand-in, so Play
                    // never waits and the audio swaps up seamlessly when the render lands.
                    if (const auto* warpedAudioData = getWarpedAudioFileData(clip, *originalAudioData, beatsPerSecond, false))
                    {
                        audioData = warpedAudioData;
                        resolved = true;
                    }

                    if (! resolved && isRealtime && ! useClipEditorPreview && realtimeWarpEnabled.load(std::memory_order_relaxed))
                    {
                        const auto warpLen = clip.warpTargetLengthInBeats > 0.0 ? clip.warpTargetLengthInBeats : clip.lengthInBeats;
                        const int targetSamples = juce::jmax(1, static_cast<int>(std::round((warpLen / beatsPerSecond) * originalAudioData->sampleRate)));
                        const int semis = computeKeyShiftSemitones(clip);
                        const auto blockEndBeat = blockStartBeat + static_cast<double>(numSamples) * beatAdvancePerSample;
                        const auto endProgress = juce::jlimit(0.0, 1.0, (blockEndBeat - clipStartBeat) / clip.lengthInBeats);
                        const auto srcRatioEnd = trimStart + endProgress * trimSpan;
                        const int neededOut = juce::jmin(targetSamples, static_cast<int>(std::ceil(srcRatioEnd * targetSamples)) + 8);
                        int ready = 0;
                        if (const auto* sd = streamWarpData(clip.sourcePath, targetSamples, semis, neededOut, ready))
                        {
                            audioData = sd;
                            streamReadyLimit = ready;
                            resolved = true;
                        }
                    }

                    if (! resolved)
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
                    else if (needsPitchRender)
                    {
                        // Warped buffer is pre-stretched to the clip length → map 1:1.
                        const auto clipBeatOffset = timelineBeat - clipStartBeat;
                        const auto clipProgress = clip.lengthInBeats > 0.0 ? juce::jlimit(0.0, 1.0, clipBeatOffset / clip.lengthInBeats) : 0.0;
                        sourceRatio = trimStart + clipProgress * trimSpan;
                    }
                    else
                    {
                        // Unwarped clip (one-shot / full track): play at the file's NATURAL
                        // rate so pitch is original — never stretch the sample to the clip
                        // length. Past the trimmed end → silent.
                        const auto elapsedSeconds = (timelineBeat - clipStartBeat) / juce::jmax(1.0e-6, beatsPerSecond);
                        const auto totalSamples = juce::jmax(1, audioData->buffer.getNumSamples());
                        sourceRatio = trimStart + (elapsedSeconds * audioData->sampleRate) / static_cast<double>(totalSamples);
                        if (sourceRatio >= trimEnd)
                            continue;
                    }

                    const auto sourceSamplePosition = juce::jlimit(0.0, 1.0, sourceRatio)
                                                    * static_cast<double>(audioData->buffer.getNumSamples() - 1);

                    const auto sourceIndex = static_cast<int>(sourceSamplePosition);
                    if (sourceIndex < 0 || sourceIndex >= audioData->buffer.getNumSamples())
                        continue;
                    // Streaming warp: don't read past what the producer has written. The
                    // unfilled tail stays silent for this block and fills in on the next
                    // pass — far better than the whole clip pumping/dropping out.
                    if (sourceIndex >= streamReadyLimit)
                        continue;

                    const auto sourceFraction = static_cast<float>(sourceSamplePosition - static_cast<double>(sourceIndex));
                    const auto lastSample = juce::jmin(audioData->buffer.getNumSamples() - 1, streamReadyLimit - 1);
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
                        const auto outputValue = sampleValue * fadeGain * panForChannel(channel);
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
                {
                    // scratch = (targetBuffer slice) − before, via vectorised block ops.
                    trackFxScratch.copyFrom(ch, 0, targetBuffer, ch, startSample, numSamples);
                    trackFxScratch.addFrom(ch, 0, trackMeterBefore, ch, 0, numSamples, -1.0f);
                }

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
                    // Restore the master to its pre-track state (block copy).
                    for (int ch = 0; ch < meterChannels; ++ch)
                        targetBuffer.copyFrom(ch, startSample, trackMeterBefore, ch, 0, numSamples);
                }
                else
                {
                    // master = before + processed contribution (block ops).
                    for (int ch = 0; ch < meterChannels; ++ch)
                    {
                        targetBuffer.copyFrom(ch, startSample, trackMeterBefore, ch, 0, numSamples);
                        targetBuffer.addFrom(ch, startSample, trackFxScratch, ch, 0, numSamples, 1.0f);
                    }
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
        // Pass the loop/project wrap context so notes at the loop start fire on every repeat.
        if (includeInstruments)
            processInstruments(targetBuffer, startSample, numSamples, blockStartBeat, beatsPerSecond,
                               true, isRealtime, anySoloActive,
                               wrapToLoop, wrapToProject, loopStartBeat, loopEndBeat, repeatEndBeat);

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

        // Thread-safe: read from the audio thread AND the message/producer threads. Entries
        // are never erased, so a pointer returned under the lock stays valid. The slow file
        // decode happens outside the lock (build-outside-lock); the lock only guards find/emplace.
        {
            const juce::ScopedLock sl(audioCacheLock);
            if (const auto it = audioCache.find(key); it != audioCache.end())
                return it->second.get();
        }

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

        const juce::ScopedLock sl(audioCacheLock);
        if (const auto it = audioCache.find(key); it != audioCache.end())
            return it->second.get();   // another thread won the race
        const auto* dataPtr = data.get();
        audioCache.emplace(key, std::move(data));
        return dataPtr;
    }

    // Cache-only lookup — safe to call from the audio thread (never decodes). Returns nullptr if
    // the file hasn't been decoded yet; pair with prewarmAudioFile() to trigger a background load.
    const AudioFileData* getAudioFileDataCached(const juce::String& path)
    {
        const juce::ScopedLock sl(audioCacheLock);
        const auto it = audioCache.find(path.toStdString());
        return it != audioCache.end() ? it->second.get() : nullptr;
    }

public:
    // Length of a decoded sample in seconds (0 if unknown). Message-thread only: uses the cache and
    // falls back to a synchronous decode. Used e.g. to size a recorded one-shot note to the sample.
    double getSampleLengthSeconds(const juce::String& path)
    {
        const auto* data = getAudioFileDataCached(path);
        if (data == nullptr)
            data = getAudioFileData(path);
        if (data == nullptr || data->sampleRate <= 0.0)
            return 0.0;
        return static_cast<double>(data->buffer.getNumSamples()) / data->sampleRate;
    }

    // Decode + cache a file in the background (message-thread callers), so nothing blocks on a
    // large decode. No-op if already cached or a decode is already queued for it.
    void prewarmAudioFile(const juce::String& path)
    {
        if (path.isEmpty())
            return;
        const auto key = path.toStdString();
        {
            const juce::ScopedLock sl(audioCacheLock);
            if (audioCache.find(key) != audioCache.end())
                return;
            if (! audioDecodePending.insert(key).second)
                return;   // a decode is already in flight
        }
        audioDecodePool.addJob([this, path, key]
        {
            getAudioFileData(path);   // heavy decode, off the audio/message thread
            const juce::ScopedLock sl(audioCacheLock);
            audioDecodePending.erase(key);
        });
    }

private:

    // Semitone offset to transpose the clip from its detected source key into the
    // current project key. Picks the shortest direction (max 6 semitones either way).
    int computeKeyShiftSemitones(const TimelineClip& clip) const noexcept
    {
        int diff = clip.transposeSemitones;
        // When the project has no key, samples keep their original pitch (time-stretch
        // still applies elsewhere). Manual transpose is honoured regardless.
        if (! clip.keyShiftEnabled || clip.sourceKeyRoot < 0 || ! project.isKeyEnabled())
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
        int pendingPanicBlocks { 0 };
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
                            bool anySoloActive,
                            bool wrapToLoop = false,
                            bool wrapToProject = false,
                            double loopStartBeat = 0.0,
                            double loopEndBeat = 0.0,
                            double repeatEndBeat = 0.0)
    {
        const juce::ScopedTryLock stl(instrumentLock);
        if (! stl.isLocked() || instruments.empty())
            return;

        const auto& tracks = project.getTracks();
        const auto beatAdvancePerSample = (beatsPerSecond > 0.0 && outputSampleRate > 0.0)
                                              ? beatsPerSecond / outputSampleRate
                                              : 0.0;
        const auto blockEndBeat = blockStartBeat + beatAdvancePerSample * static_cast<double>(numSamples);

        // Maps a note's absolute beat to an in-block sample offset, or -1 if it doesn't fall in
        // this block. Crucially this is wrap-aware: when the block crosses the loop / project end
        // the playhead wraps back to the start, so a note at (or just after) the loop start must
        // still fire. Without this, audio clips (which wrap per-sample) kept playing but the very
        // first note of a looped MIDI part was dropped on every repeat.
        const double wrapPoint = wrapToLoop ? loopEndBeat : (wrapToProject ? repeatEndBeat : 0.0);
        const double wrapStart = wrapToLoop ? loopStartBeat : 0.0;
        const bool   wrapActive = (wrapToLoop || wrapToProject) && wrapPoint > 0.0 && blockEndBeat > wrapPoint;
        const double wrapOver   = wrapActive ? (blockEndBeat - wrapPoint) : 0.0;   // beats past the wrap
        const auto offsetForBeat = [&](double b) -> int
        {
            if (b >= blockStartBeat && b < blockEndBeat)
                return juce::jlimit(0, numSamples - 1, static_cast<int>(std::round((b - blockStartBeat) / beatAdvancePerSample)));
            if (wrapActive && b >= wrapStart && b < wrapStart + wrapOver)
                return juce::jlimit(0, numSamples - 1,
                    static_cast<int>(std::round(((wrapPoint - blockStartBeat) + (b - wrapStart)) / beatAdvancePerSample)));
            return -1;
        };

        // First block after Play: re-articulate (at offset 0) any clip note whose span already
        // contains the start playhead. This covers a note sitting exactly at the part start,
        // which float jitter in blockStartBeat can otherwise push just past offsetForBeat().
        const bool chaseAtStart = chaseNotesAtStart && includeClipNotes;
        if (includeClipNotes)
            chaseNotesAtStart = false;

        for (auto& [trackIndex, slot] : instruments)
        {
            if (slot == nullptr || slot->instance == nullptr)
                continue;
            if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
                continue;

            const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
            auto* inst = slot->instance.get();

            juce::MidiBuffer midi;
            const bool panicActive = slot->pendingPanicBlocks > 0;

            if (panicActive)
            {
                slot->pendingLiveMidi.clear();

                // Gentle silence: note-offs + allNotesOff trigger each voice's RELEASE (no
                // click), unlike allSoundOff which hard-cuts mid-waveform (that was the crackle).
                // Clip/live notes are emitted on channel 1, so panic only needs channel 1.
                midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);   // sustain pedal off
                for (int note = 0; note <= 127; ++note)
                    midi.addEvent(juce::MidiMessage::noteOff(1, note), 0);
                midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                --slot->pendingPanicBlocks;
            }

            if (! panicActive && includeLiveNotes)
            {
                for (const auto metadata : slot->pendingLiveMidi)
                    midi.addEvent(metadata.getMessage(), 0);
                slot->pendingLiveMidi.clear();
            }

            const bool trackAudible = ! track.muted && (! anySoloActive || track.solo);

            if (! panicActive && includeClipNotes && trackAudible && beatAdvancePerSample > 0.0)
            {
                for (const auto& clip : track.clips)
                {
                    if (clip.type != ClipType::midi || clip.muted || clip.recording)
                        continue;
                    if (anySoloActive && ! track.solo && ! clip.solo)
                        continue;

                    // MPE-style glide: give each pitch-slide voice its own MIDI channel
                    // (2..16) so voices bend independently; plain notes stay on channel 1.
                    const auto noteCount = static_cast<int>(clip.midiNotes.size());
                    std::vector<int> noteChannel(static_cast<std::size_t>(juce::jmax(0, noteCount)), 1);
                    std::vector<int> slideChannel(clip.pitchSlides.size(), 0);
                    {
                        int nextCh = 2;
                        for (std::size_t s = 0; s < clip.pitchSlides.size(); ++s)
                        {
                            const auto& sl = clip.pitchSlides[s];
                            if (sl.points.size() < 2)
                                continue;
                            int src = -1;
                            for (int i = 0; i < noteCount; ++i)
                            {
                                const auto& n = clip.midiNotes[static_cast<std::size_t>(i)];
                                if (n.pitch == sl.sourcePitch && std::abs(n.startBeat - sl.sourceNoteStartBeat) < 0.0001) { src = i; break; }
                            }
                            if (src < 0)
                                for (int i = 0; i < noteCount; ++i)
                                {
                                    const auto& n = clip.midiNotes[static_cast<std::size_t>(i)];
                                    const auto ne = n.startBeat + juce::jmax(0.01, n.lengthInBeats);
                                    if (sl.points.front().beat >= n.startBeat && sl.points.front().beat <= ne) { src = i; break; }
                                }
                            if (src >= 0)
                            {
                                slideChannel[s] = nextCh;
                                noteChannel[static_cast<std::size_t>(src)] = nextCh;
                                nextCh = nextCh >= 16 ? 2 : nextCh + 1;
                            }
                        }
                    }

                    const auto sendBendRange = [&](int ch, int offset)
                    {
                        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 101, 0), offset);  // RPN MSB
                        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 100, 0), offset);  // RPN LSB = pitch-bend sensitivity
                        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 6, static_cast<int>(vstPitchBendRangeSemitones)), offset);
                        midi.addEvent(juce::MidiMessage::controllerEvent(ch, 38, 0), offset);
                    };

                    for (int ni = 0; ni < noteCount; ++ni)
                    {
                        const auto& note = clip.midiNotes[static_cast<std::size_t>(ni)];
                        const auto ch = noteChannel[static_cast<std::size_t>(ni)];
                        const auto onBeat  = clip.startBeat + note.startBeat;
                        const auto offBeat = onBeat + juce::jmax(0.01, note.lengthInBeats);

                        auto offset = offsetForBeat(onBeat);
                        // Chase: if the note didn't start in this block but is already sounding
                        // under the start playhead, articulate it at offset 0 on the first block.
                        if (offset < 0 && chaseAtStart && onBeat < blockStartBeat && offBeat > blockStartBeat)
                            offset = 0;
                        if (offset >= 0)
                        {
                            if (ch != 1)
                                sendBendRange(ch, offset);  // widen the glide voice's bend range
                            midi.addEvent(juce::MidiMessage::pitchWheel(ch, 8192), offset);
                            midi.addEvent(juce::MidiMessage::noteOn(ch, note.pitch,
                                static_cast<juce::uint8>(juce::jlimit(1, 127, note.velocity))), offset);
                        }
                        if (const auto offOffset = offsetForBeat(offBeat); offOffset >= 0)
                            midi.addEvent(juce::MidiMessage::noteOff(ch, note.pitch), offOffset);
                    }

                    for (std::size_t si = 0; si < clip.pitchSlides.size(); ++si)
                    {
                        const auto& slide = clip.pitchSlides[si];
                        const int ch = slideChannel[si] > 0 ? slideChannel[si] : 1;
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
                                return pitchSlideSegmentPitch(a, b, clipBeat - a.beat, t);
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
                            midi.addEvent(juce::MidiMessage::pitchWheel(ch, wheel), offset);
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

            if (panicActive || ! trackAudible)
                continue;

            const auto trackGain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb + track.trackGainDb));
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
    // Set on the first block after playback starts: re-articulate any clip note whose span
    // contains the start playhead, so the note you press Play on top of (or exactly at the
    // part start, where float jitter can otherwise drop the note-on) is heard. Consumed by
    // processInstruments / renderMidiClip on that first block.
    bool chaseNotesAtStart { false };
    // Short fade-out tail rendered when playback stops, so the arrangement doesn't click.
    int declickRemaining { 0 };
    int declickTotal { 1 };
    std::map<std::string, std::unique_ptr<AudioFileData>> audioCache;
    std::map<std::string, std::unique_ptr<AudioFileData>> warpedAudioCache;
    juce::CriticalSection warpCacheLock;   // guards warpedAudioCache (audio vs message thread)
    juce::CriticalSection audioCacheLock;  // guards audioCache (audio vs producer/message threads)
    // Background file decode so a big source (a 90 s clip decodes in ~1.5 s) never blocks the
    // audio or message thread — that block was why a long warped clip started late / silent.
    juce::ThreadPool audioDecodePool { 1 };
    std::set<std::string> audioDecodePending;   // files with a decode job queued (guarded by audioCacheLock)

    // Real-time streaming warp: streams + background producer thread.
    std::map<std::string, std::unique_ptr<WarpStream>> warpStreams;
    juce::CriticalSection warpStreamLock;
    std::thread warpProducerThread;
    std::atomic<bool> warpProducerRunning { false };
    juce::WaitableEvent warpProducerWake;
    std::thread warpOfflineThread;            // separate thread: heavy RubberBand renders
    juce::WaitableEvent warpOfflineWake;
    juce::AudioBuffer<float> warpProducerScratch;   // producer-thread input scratch
    std::atomic<bool> realtimeWarpEnabled { true };

    juce::CriticalSection instrumentLock;
    std::map<int, std::unique_ptr<InstrumentSlot>> instruments;
    // Parked (still-alive) instruments from deleted tracks, kept so undo can re-home the exact
    // same instance instantly instead of reinstantiating. Bounded by trimInstrumentStash().
    std::vector<std::unique_ptr<InstrumentSlot>> instrumentStash;

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
                // Moderate level (~-14 / -18 dBFS): audible as a reference but doesn't
                // dominate the mix the way the old hot default (0.42/0.28) did.
                currentAmplitude = (beatInBar == 0) ? 0.20f : 0.13f;
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

    // A monitor-only source (the metronome click) added AFTER the master meter is taken,
    // so it's audible but never shows on the master meter or lands in an export — matching
    // how Logic/Ableton treat the click.
    void setMonitorSource(juce::AudioSource* source) noexcept { monitorSource = source; }

    void prepareToPlay(int samplesPerBlockExpected, double newSampleRate) override
    {
        downstream.prepareToPlay(samplesPerBlockExpected, newSampleRate);
        if (monitorSource != nullptr)
            monitorSource->prepareToPlay(samplesPerBlockExpected, newSampleRate);
        monitorScratch.setSize(2, juce::jmax(1, samplesPerBlockExpected), false, false, true);
    }

    void releaseResources() override
    {
        downstream.releaseResources();
        if (monitorSource != nullptr)
            monitorSource->releaseResources();
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        juce::ScopedNoDenormals noDenormals;   // flush denormals across the whole render chain
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

        // Mix the monitor click on top, AFTER metering (so it doesn't inflate the meter).
        if (monitorSource != nullptr)
        {
            if (monitorScratch.getNumSamples() < info.numSamples)
                monitorScratch.setSize(2, info.numSamples, false, false, true);
            juce::AudioSourceChannelInfo mi(&monitorScratch, 0, info.numSamples);
            mi.clearActiveBufferRegion();
            monitorSource->getNextAudioBlock(mi);
            const auto chn = juce::jmin(numCh, monitorScratch.getNumChannels());
            for (int ch = 0; ch < chn; ++ch)
                info.buffer->addFrom(ch, info.startSample, monitorScratch, ch, 0, info.numSamples);
        }
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
    juce::AudioSource* monitorSource { nullptr };   // metronome click (post-meter monitor)
    juce::AudioBuffer<float> monitorScratch;
    std::atomic<float> gainLinear { 1.0f };
    std::atomic<float> meterPeakL { 0.0f };
    std::atomic<float> meterPeakR { 0.0f };
};
}  // namespace orion
