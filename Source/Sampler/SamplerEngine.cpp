#include "SamplerEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace orion
{
namespace
{
constexpr int kSamplerAttackFadeSamples = 384;
constexpr int kSamplerSliceAttackFadeSamples = 24;
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

// [start,end] sample positions for a slice. With explicit slice points (start ratios,
// size = slice count) the boundaries follow detected transients; otherwise they are
// equal divisions of the sample by sliceCount.
std::pair<double, double> sliceBoundsSamples(int sliceIndex, int sliceCount, int totalSamples,
                                             const std::vector<double>* points) noexcept
{
    const auto total = static_cast<double>(juce::jmax(1, totalSamples));
    if (points != nullptr && points->size() >= 2)
    {
        const auto n = static_cast<int>(points->size());
        const auto i = juce::jlimit(0, n - 1, sliceIndex);
        const auto startR = juce::jlimit(0.0, 1.0, (*points)[static_cast<std::size_t>(i)]);
        const auto endR   = i + 1 < n ? juce::jlimit(0.0, 1.0, (*points)[static_cast<std::size_t>(i + 1)]) : 1.0;
        const auto s = std::floor(startR * total);
        const auto e = std::floor(endR * total);
        return { juce::jlimit(0.0, total - 1.0, s), juce::jlimit(s + 1.0, total, e) };
    }

    const auto sc = juce::jlimit(1, 64, sliceCount);
    const auto i  = juce::jlimit(0, sc - 1, sliceIndex);
    const auto s = std::floor(static_cast<double>(i) / static_cast<double>(sc) * total);
    const auto e = i == sc - 1 ? total : std::floor(static_cast<double>(i + 1) / static_cast<double>(sc) * total);
    return { juce::jlimit(0.0, total - 1.0, s), juce::jlimit(s + 1.0, total, e) };
}

double nextTriggerBeatAfter(const std::vector<MidiNote>& notes, const MidiNote& note, double fallbackBeat) noexcept
{
    auto nextTriggerBeat = fallbackBeat;

    for (const auto& nextNote : notes)
    {
        if (nextNote.startBeat > note.startBeat + 0.0001)
            nextTriggerBeat = juce::jmin(nextTriggerBeat, nextNote.startBeat);
    }

    return nextTriggerBeat;
}

// Glide source pitch for a note (polyphonic portamento). Finds the previous chord
// (the notes at the most recent earlier trigger beat), pairs voices by pitch rank
// (low→low, high→high), and returns the paired previous pitch this note glides FROM.
// Extra top voices in a larger chord clamp to the highest previous voice. Single-note
// lines fall out of this naturally (a "chord" of one). Returns nullopt for the very
// first trigger (nothing to glide from).
std::optional<double> glideSourcePitchForNote(const std::vector<MidiNote>& notes, const MidiNote& note) noexcept
{
    bool found = false;
    double prevBeat = 0.0;
    for (const auto& other : notes)
        if (other.startBeat < note.startBeat - 0.0001 && (! found || other.startBeat > prevBeat))
        {
            prevBeat = other.startBeat;
            found = true;
        }
    if (! found)
        return std::nullopt;

    std::vector<int> current, previous;
    for (const auto& other : notes)
    {
        if (std::abs(other.startBeat - note.startBeat) < 0.0001)
            current.push_back(other.pitch);
        else if (std::abs(other.startBeat - prevBeat) < 0.0001)
            previous.push_back(other.pitch);
    }
    if (previous.empty())
        return std::nullopt;

    std::sort(current.begin(), current.end());
    std::sort(previous.begin(), previous.end());

    const auto it = std::lower_bound(current.begin(), current.end(), note.pitch);
    auto rank = static_cast<int>(std::distance(current.begin(), it));
    rank = juce::jlimit(0, static_cast<int>(previous.size()) - 1, rank);
    return static_cast<double>(previous[static_cast<std::size_t>(rank)]);
}

// Analytic glide evaluation (no per-sample allocation). Returns the effective pitch and
// the integrated source-sample offset at beatBeyondStart beats into the note, for a
// glide that ramps prevPitch→notePitch over glideBeats (linear in semitones) then holds.
struct GlideEval
{
    double pitch { 0.0 };
    double sourceSamples { 0.0 };
};

GlideEval evalGlide(double prevPitch, double notePitch, int rootMidiNote, double glideBeats,
                    double beatBeyondStart, double sourceSampleRate, double beatsPerSecond) noexcept
{
    const auto rootD = static_cast<double>(rootMidiNote);
    const auto k = std::log(2.0) / 12.0;
    const auto r0 = std::pow(2.0, (prevPitch - rootD) / 12.0);
    const auto slopePerBeat = (notePitch - prevPitch) / glideBeats;  // semitones per beat
    const auto expo = k * slopePerBeat;                              // ratio growth rate per beat
    const auto tau = juce::jmin(beatBeyondStart, glideBeats);

    // ∫ ratio dBeat over the ramp portion [0, tau].
    const auto rampIntegral = std::abs(expo) < 1.0e-12
        ? r0 * tau
        : r0 * (std::exp(expo * tau) - 1.0) / expo;

    GlideEval out;
    auto sourceBeats = rampIntegral;
    if (beatBeyondStart <= glideBeats)
    {
        out.pitch = prevPitch + slopePerBeat * beatBeyondStart;
    }
    else
    {
        out.pitch = notePitch;
        const auto rNote = std::pow(2.0, (notePitch - rootD) / 12.0);
        sourceBeats += rNote * (beatBeyondStart - glideBeats);
    }
    out.sourceSamples = sourceBeats * sourceSampleRate / beatsPerSecond;
    return out;
}

const PitchSlide* findSlideForNote(const TimelineClip& clip, const MidiNote& note)
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

        return &slide;
    }

    return nullptr;
}

double pitchForSlideAtBeat(const PitchSlide& slide, double beat)
{
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
        return pitchSlideSegmentPitch(a, b, beat - a.beat, t);
    }

    return slide.points.front().pitch;
}

double pitchForNoteAtBeat(const PitchSlide* slide, const MidiNote& note, double beat)
{
    if (slide == nullptr || beat < slide->points.front().beat)
        return static_cast<double>(note.pitch);

    return pitchForSlideAtBeat(*slide, beat);
}

double sourceSamplesForNoteAtBeat(const PitchSlide* slide,
                                  const MidiNote& note,
                                  double beat,
                                  int rootMidiNote,
                                  double sourceSampleRate,
                                  double beatsPerSecond)
{
    if (beat <= note.startBeat || beatsPerSecond <= 0.0 || sourceSampleRate <= 0.0)
        return 0.0;

    if (slide == nullptr)
    {
        return ((beat - note.startBeat) / beatsPerSecond)
            * sourceSampleRate
            * pitchRatioForPitch(static_cast<double>(note.pitch), rootMidiNote);
    }

    std::vector<double> boundaries;
    boundaries.reserve(slide->points.size() + 2);
    boundaries.push_back(note.startBeat);
    boundaries.push_back(beat);

    for (const auto& point : slide->points)
    {
        if (point.beat > note.startBeat + 0.000001 && point.beat < beat - 0.000001)
            boundaries.push_back(point.beat);
    }

    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end(), [](double a, double b)
    {
        return std::abs(a - b) < 0.000001;
    }), boundaries.end());

    auto sourceSamples = 0.0;
    for (std::size_t i = 1; i < boundaries.size(); ++i)
    {
        const auto segmentStart = boundaries[i - 1];
        const auto segmentEnd = boundaries[i];
        if (segmentEnd <= segmentStart)
            continue;

        const auto midpointBeat = (segmentStart + segmentEnd) * 0.5;
        const auto segmentPitch = pitchForNoteAtBeat(slide, note, midpointBeat);
        sourceSamples += ((segmentEnd - segmentStart) / beatsPerSecond)
            * sourceSampleRate
            * pitchRatioForPitch(segmentPitch, rootMidiNote);
    }

    return sourceSamples;
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
                                                               projectTempoBpm,
                                                               /*allowBlocking*/ false))  // audio thread
            sampleData = warpedSampleData;
    }

    const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);
    const auto wrapToProject = wrapToProjectEnd && ! wrapToLoop && repeatEndBeat > 0.0;
    const auto beatAdvancePerSample = beatsPerSecond / renderSampleRate;
    const auto clipStartBeat = clip.startBeat;
    const auto clipEndBeat = clip.startBeat + clip.lengthInBeats;
    const auto trackGain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb + track.trackGainDb));
    const auto clipGain = juce::Decibels::decibelsToGain(static_cast<float>(clip.gainDb));
    const auto safeSliceCount = juce::jlimit(1, 64, track.samplerSliceCount);

    // Global transpose (Simpler "Transpose" knob): raising pitch by N semitones is the
    // same as lowering the root by N, so all pitched ratios pick it up via effectiveRoot.
    // Slice-index mapping deliberately keeps the untransposed root.
    const auto effectiveRoot = track.samplerRootMidiNote - track.samplerTransposeSemitones;

    // Per-channel amp envelope (ADSR). Tiny floors on attack/release keep edges click-free even
    // at zero (this also reproduces the previous fixed anti-click ramps for default settings).
    const double envAttack  = juce::jmax(0.002, track.samplerAmpAttackSeconds);
    const double envDecay   = juce::jmax(0.0,   track.samplerAmpDecaySeconds);
    const double envSustain = juce::jlimit(0.0, 1.0, track.samplerAmpSustain);
    const double envRelease = juce::jmax(0.004, track.samplerAmpReleaseSeconds);
    const double envReleaseBeats = envRelease * beatsPerSecond;
    // Level during the held phase (attack → decay → sustain) at time t seconds since trigger.
    const auto envHeldLevel = [envAttack, envDecay, envSustain](double t) -> double
    {
        if (t < envAttack)
            return t / envAttack;
        const auto td = t - envAttack;
        if (envDecay > 0.0 && td < envDecay)
            return 1.0 - (1.0 - envSustain) * (td / envDecay);
        return envSustain;
    };

    // Glide (portamento): precompute, once per block, the pitch each note glides FROM
    // (NaN = no glide). Done here so the per-sample inner loop allocates nothing.
    const bool glideActive = glideEnabled.load(std::memory_order_relaxed)
                          && track.samplerMode != SamplerPlaybackMode::slice;
    const auto glideBeats = juce::jmax(1.0e-4, glideTimeSeconds.load(std::memory_order_relaxed) * beatsPerSecond);
    std::vector<double> glideSourcePitch;
    if (glideActive)
    {
        glideSourcePitch.assign(clip.midiNotes.size(), std::numeric_limits<double>::quiet_NaN());
        for (std::size_t i = 0; i < clip.midiNotes.size(); ++i)
            if (const auto src = glideSourcePitchForNote(clip.midiNotes, clip.midiNotes[i]);
                src.has_value() && std::abs(*src - static_cast<double>(clip.midiNotes[i].pitch)) > 1.0e-6)
                glideSourcePitch[i] = *src;
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

        if (timelineBeat < clipStartBeat || timelineBeat >= clipEndBeat)
            continue;

        const auto clipLocalBeat = timelineBeat - clipStartBeat;
        for (const auto& note : clip.midiNotes)
        {
            const auto* noteSlide = findSlideForNote(clip, note);
            auto noteEndBeat = note.startBeat + juce::jmax(0.01, note.lengthInBeats);
            const auto nextTriggerBeat = nextTriggerBeatAfter(clip.midiNotes, note, clip.lengthInBeats);

            // Classic is polyphonic. One-Shot/Slice retrigger, so the next MIDI note
            // must choke the previous rendered voice even when that voice has a slide.
            if (track.samplerMode == SamplerPlaybackMode::classic)
            {
                if (noteSlide != nullptr)
                {
                    noteEndBeat = clip.lengthInBeats;
                }
                else
                {
                    const auto sourceDurationBeats = (static_cast<double>(sampleData->buffer.getNumSamples()) / sampleData->sampleRate)
                        * beatsPerSecond / getPitchRatio(note.pitch, effectiveRoot);
                    noteEndBeat = juce::jmin(clip.lengthInBeats, note.startBeat + sourceDurationBeats);
                }
            }
            else if (track.samplerMode == SamplerPlaybackMode::oneShot)
            {
                if (noteSlide != nullptr)
                {
                    noteEndBeat = juce::jmin(clip.lengthInBeats, nextTriggerBeat);
                }
                else
                {
                    const auto sourceDurationBeats = (static_cast<double>(sampleData->buffer.getNumSamples()) / sampleData->sampleRate)
                        * beatsPerSecond / getPitchRatio(note.pitch, effectiveRoot);
                    noteEndBeat = juce::jmin(nextTriggerBeat, note.startBeat + sourceDurationBeats);
                }
            }
            else if (track.samplerMode == SamplerPlaybackMode::slice)
            {
                const auto* slicePts = track.samplerSlicePoints.empty() ? nullptr : &track.samplerSlicePoints;
                const auto bounds = sliceBoundsSamples(note.pitch - track.samplerRootMidiNote,
                                                       safeSliceCount,
                                                       sampleData->buffer.getNumSamples(),
                                                       slicePts);
                const auto sliceDurationBeats = ((bounds.second - bounds.first) / sampleData->sampleRate)
                    * beatsPerSecond;
                noteEndBeat = juce::jmin(nextTriggerBeat, note.startBeat + sliceDurationBeats);
            }

            // Per-channel step gate: shorten the note so a long sample (e.g. an 808) can be
            // cut to a fixed length. 0 = play the full sample (default). The existing release
            // ramp below smooths the cut so there's no click.
            if (track.samplerStepGateBeats > 0.0)
                noteEndBeat = juce::jmin(noteEndBeat, note.startBeat + track.samplerStepGateBeats);

            // Amp-envelope note-off point. Classic is a sustaining/melodic voice, so the env is
            // gated by the MIDI NOTE LENGTH (note ends → release) — Release and step length both
            // matter. One-Shot/Slice play the whole sample (note-off ignored, drum-style), so the
            // env is gated by the playback end instead; there A/D/S shape it and Decay→Sustain
            // shortens an 808.
            const auto envHoldEnd = (track.samplerMode == SamplerPlaybackMode::classic)
                ? note.startBeat + juce::jmax(0.01, note.lengthInBeats)
                : noteEndBeat;

            // Render through the hold AND the release tail, so the release is audible past note-off.
            if (clipLocalBeat < note.startBeat || clipLocalBeat >= envHoldEnd + envReleaseBeats)
                continue;

            const auto noteSeconds = (clipLocalBeat - note.startBeat) / beatsPerSecond;
            const auto heldSeconds = juce::jmax(0.0, (envHoldEnd - note.startBeat) / beatsPerSecond);
            double sourceStartPosition = 0.0;
            double sourceEndPosition = static_cast<double>(sampleData->buffer.getNumSamples());
            // Glide (polyphonic portamento): if this note has a precomputed glide source
            // and no manual slide, evaluate the analytic glide for both pitch and source
            // position. The note's natural duration above is left unchanged.
            const auto noteIndex = static_cast<std::size_t>(&note - clip.midiNotes.data());
            const bool noteGlides = glideActive
                                 && noteSlide == nullptr
                                 && noteIndex < glideSourcePitch.size()
                                 && ! std::isnan(glideSourcePitch[noteIndex]);
            GlideEval glideEval;
            if (noteGlides)
                glideEval = evalGlide(glideSourcePitch[noteIndex],
                                      static_cast<double>(note.pitch),
                                      effectiveRoot,
                                      glideBeats,
                                      clipLocalBeat - note.startBeat,
                                      sampleData->sampleRate,
                                      beatsPerSecond);

            double playbackRatio = noteGlides
                ? pitchRatioForPitch(glideEval.pitch, effectiveRoot)
                : pitchRatioForPitch(pitchForNoteAtBeat(noteSlide, note, clipLocalBeat), effectiveRoot);

            if (track.samplerMode == SamplerPlaybackMode::slice)
            {
                const auto* slicePts = track.samplerSlicePoints.empty() ? nullptr : &track.samplerSlicePoints;
                const auto bounds = sliceBoundsSamples(note.pitch - track.samplerRootMidiNote,
                                                       safeSliceCount,
                                                       sampleData->buffer.getNumSamples(),
                                                       slicePts);
                sourceStartPosition = bounds.first;
                sourceEndPosition = bounds.second;
                playbackRatio = 1.0;
            }

            const auto sourceSamplePosition = sourceStartPosition
                + (track.samplerMode == SamplerPlaybackMode::slice
                       ? noteSeconds * sampleData->sampleRate * playbackRatio
                       : noteGlides
                           ? glideEval.sourceSamples
                           : sourceSamplesForNoteAtBeat(noteSlide,
                                                        note,
                                                        clipLocalBeat,
                                                        effectiveRoot,
                                                        sampleData->sampleRate,
                                                        beatsPerSecond));
            const auto sourceIndex = static_cast<int>(sourceSamplePosition);
            if (sourceIndex < 0 || sourceSamplePosition >= sourceEndPosition || sourceIndex >= sampleData->buffer.getNumSamples())
                continue;

            const auto velocityGain = static_cast<float>(juce::jlimit(1, 127, note.velocity)) / 127.0f;
            auto edgeGain = 1.0f;

            // Amp envelope (ADSR): attack → decay → sustain while held, then release.
            double envLevel;
            if (noteSeconds < heldSeconds)
            {
                envLevel = envHeldLevel(noteSeconds);
            }
            else
            {
                const auto offLevel = envHeldLevel(heldSeconds);
                const auto rt = noteSeconds - heldSeconds;
                envLevel = rt < envRelease ? offLevel * (1.0 - rt / envRelease) : 0.0;
            }
            edgeGain *= static_cast<float>(juce::jlimit(0.0, 1.0, envLevel));

            // Independent of the envelope: fade out when the SAMPLE DATA itself runs out, so a
            // sample shorter than the note doesn't click at its end.
            const auto sourceAdvancePerOutputSample = (sampleData->sampleRate * playbackRatio) / renderSampleRate;
            const auto outputSamplesToSourceEnd = (sourceEndPosition - sourceSamplePosition)
                / juce::jmax(0.000001, sourceAdvancePerOutputSample);
            if (outputSamplesToSourceEnd < static_cast<double>(kSamplerMidiTriggerReleaseSamples))
            {
                edgeGain *= smoothRamp(static_cast<float>(outputSamplesToSourceEnd)
                                       / static_cast<float>(kSamplerMidiTriggerReleaseSamples));
            }

            const auto linearGain = trackGain * clipGain * velocityGain * edgeGain;

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
        if (const auto* warpedSampleData = getWarpedSampleData(sourcePath, *sampleData, sourceBpm, projectTempoBpm,
                                                               /*allowBlocking*/ true))  // live note, message thread
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
    // Glide forces mono regardless of mode (one voice glides like a mono synth).
    for (auto& note : liveNotes)
    {
        if (glideEnabled || (playbackMode == SamplerPlaybackMode::classic ? note.midiNote == midiNote : true))
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
        // liveSlicePoints is read under liveNotesMutex (held here) — transient chop.
        const auto* slicePts = liveSlicePoints.empty() ? nullptr : &liveSlicePoints;
        const auto bounds = sliceBoundsSamples(sliceIndex, sliceCount,
                                               sampleData->buffer.getNumSamples(), slicePts);
        sourceStartPosition = bounds.first;
        sourceEndPosition = bounds.second;
        playbackRatio = 1.0;
    }

    LiveNote nextNote;
    nextNote.sample            = sampleData;
    nextNote.midiNote          = midiNote;
    nextNote.sourcePosition    = sourceStartPosition;
    nextNote.sourceEndPosition = sourceEndPosition;
    nextNote.playbackRatio     = playbackRatio;
    nextNote.attackSamplesTotal = playbackMode == SamplerPlaybackMode::slice
        ? kSamplerSliceAttackFadeSamples
        : kSamplerAttackFadeSamples;
    // Only velocity is baked into the voice. The track/sampler gain is applied LIVE in
    // renderLiveNotes via liveGainLinear, so turning the Gain knob is heard immediately
    // (not only on the next trigger). Seed it from this trigger's gain.
    nextNote.gain              = static_cast<float>(juce::jlimit(1, 127, velocity)) / 127.0f;
    nextNote.voiceId           = nextLiveVoiceId++;
    liveGainLinear.store(juce::Decibels::decibelsToGain(static_cast<float>(gainDb)), std::memory_order_relaxed);

    // Arm glide: start at the previous note's pitch ratio and slide to this note's.
    // Skipped for slice mode (slices ignore pitch) and when there is no previous note.
    const bool doGlide = glideEnabled
                      && lastLivePitch >= 0
                      && lastLivePitch != midiNote
                      && playbackMode != SamplerPlaybackMode::slice;
    nextNote.currentRatio     = doGlide ? getPitchRatio(lastLivePitch, rootMidiNote) : playbackRatio;
    nextNote.glideTimeSeconds = doGlide ? glideTimeSeconds.load() : 0.0;

    if (glideEnabled)
        lastLivePitch = midiNote;

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
    lastLivePitch = -1;
}

void SamplerEngine::setGlide(bool enabled, double timeSeconds)
{
    std::scoped_lock lock(liveNotesMutex);
    glideEnabled = enabled;
    glideTimeSeconds = juce::jlimit(0.0, 2.0, timeSeconds);
    if (! enabled)
        lastLivePitch = -1;  // start fresh next time glide is turned on
}

void SamplerEngine::renderLiveNotes(juce::AudioBuffer<float>& targetBuffer, int startSample, int numSamples, double renderSampleRate)
{
    if (renderSampleRate <= 0.0 || numSamples <= 0)
        return;

    std::scoped_lock lock(liveNotesMutex);
    const auto liveGain = liveGainLinear.load(std::memory_order_relaxed);  // applied live (see noteOn)
    for (auto& note : liveNotes)
    {
        if (note.sample == nullptr || note.sample->buffer.getNumSamples() <= 0 || note.sample->sampleRate <= 0.0)
            continue;

        // Lazily arm the glide step now that we know the render sample rate. Geometric
        // per-sample multiply on the ratio == linear movement in semitone space.
        if (note.glideTimeSeconds > 0.0 && ! note.glideInitialised)
        {
            note.glideInitialised = true;
            const auto glideSamples = juce::jmax(1, static_cast<int>(note.glideTimeSeconds * renderSampleRate));
            if (note.currentRatio > 0.0 && note.playbackRatio > 0.0
                && std::abs(note.currentRatio - note.playbackRatio) > 1.0e-9)
            {
                note.glideSamplesRemaining = glideSamples;
                note.glideRatioMul = std::pow(note.playbackRatio / note.currentRatio,
                                              1.0 / static_cast<double>(glideSamples));
            }
            else
            {
                note.currentRatio = note.playbackRatio;
            }
        }
        else if (note.glideTimeSeconds <= 0.0)
        {
            note.currentRatio = note.playbackRatio;
        }

        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
        {
            const auto sourceAdvancePerOutputSample = note.currentRatio * note.sample->sampleRate / renderSampleRate;
            const auto sourceIndex = static_cast<int>(note.sourcePosition);
            if (sourceIndex < 0
                || sourceIndex >= note.sample->buffer.getNumSamples()
                || note.sourcePosition >= note.sourceEndPosition)
                break;

            for (int channel = 0; channel < targetBuffer.getNumChannels(); ++channel)
            {
                const auto sourceChannel = juce::jmin(channel, note.sample->buffer.getNumChannels() - 1);
                auto noteGain = note.gain * liveGain;
                if (note.attackSamplesElapsed < note.attackSamplesTotal)
                    noteGain *= smoothRamp(static_cast<float>(note.attackSamplesElapsed + 1)
                                           / static_cast<float>(note.attackSamplesTotal));

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

            if (note.attackSamplesElapsed < note.attackSamplesTotal)
                ++note.attackSamplesElapsed;

            if (note.releaseSamplesRemaining > 0)
                --note.releaseSamplesRemaining;

            note.sourcePosition += sourceAdvancePerOutputSample;

            // Advance the glide toward the target ratio (one step per output sample).
            if (note.glideSamplesRemaining > 0)
            {
                note.currentRatio *= note.glideRatioMul;
                if (--note.glideSamplesRemaining == 0)
                    note.currentRatio = note.playbackRatio;
            }
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
                                                                    double projectTempoBpm,
                                                                    bool allowBlocking)
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

    if (allowBlocking)
    {
        // Live note (message thread). The HQ render is heavy, so we must not double it up
        // with the pre-warm that load() kicks off: if a build for this key is already in
        // flight, WAIT for it (poll the cache) instead of rendering a second copy. With
        // pre-warm-on-load this is usually already a cache hit, so there's no wait at all.
        const auto deadlineMs = juce::Time::getMillisecondCounter() + 8000;
        for (;;)
        {
            {
                std::scoped_lock lock(cacheMutex);
                if (const auto it = warpedSampleCache.find(key); it != warpedSampleCache.end())
                    return it->second.get();
                if (! warpInFlight.contains(key))
                {
                    warpInFlight.insert(key);   // claim it; we build it ourselves below
                    break;
                }
            }
            if (juce::Time::getMillisecondCounter() >= deadlineMs)
                break;                          // in-flight job stalled; build it ourselves
            juce::Thread::sleep(4);
        }

        auto data = std::make_unique<SampleData>();
        data->sampleRate = sourceData.sampleRate;
        data->buffer = stretchBuffer(sourceData.buffer, targetSamples, sourceData.sampleRate, path);

        std::scoped_lock lock(cacheMutex);
        const auto [it, inserted] = warpedSampleCache.emplace(key, std::move(data));
        juce::ignoreUnused(inserted);          // emplace is a no-op if the in-flight job won the race
        warpInFlight.erase(key);
        return it->second.get();
    }

    // Audio thread (renderMidiClip): never block. Queue a background build and return nullptr;
    // the caller plays the original until the warped buffer is ready. `src` points at a
    // sampleCache entry, which is never erased, so the pointer stays valid for the job.
    std::scoped_lock lock(cacheMutex);
    if (! warpInFlight.contains(key))
    {
        warpInFlight.insert(key);
        const auto* src = &sourceData;
        const auto srcSampleRate = sourceData.sampleRate;
        warpPool.addJob([this, key, src, srcSampleRate, targetSamples, path]
        {
            auto data = std::make_unique<SampleData>();
            data->sampleRate = srcSampleRate;
            data->buffer = stretchBuffer(src->buffer, targetSamples, srcSampleRate, path);

            std::scoped_lock jobLock(cacheMutex);
            warpedSampleCache.emplace(key, std::move(data));
            warpInFlight.erase(key);
        });
    }
    return nullptr;
}

void SamplerEngine::prewarmWarp(const juce::String& path, double sourceBpm, double projectTempoBpm)
{
    if (path.isEmpty() || sourceBpm <= 0.0 || projectTempoBpm <= 0.0)
        return;

    // Decode + stretch entirely on the background pool so the load gesture never blocks, but
    // the warped buffer is cached before the user can trigger a note.
    warpPool.addJob([this, path, sourceBpm, projectTempoBpm]
    {
        if (const auto* sd = getSampleData(path); sd != nullptr)
            getWarpedSampleData(path, *sd, sourceBpm, projectTempoBpm, /*allowBlocking*/ true);
    });
}

double SamplerEngine::getPitchRatio(int midiNote, int rootMidiNote) noexcept
{
    return std::pow(2.0, static_cast<double>(midiNote - rootMidiNote) / 12.0);
}
}  // namespace orion
