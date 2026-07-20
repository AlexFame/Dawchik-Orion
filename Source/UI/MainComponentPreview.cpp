// Audio preview for MainComponent: browser sample preview, the clip-editor preview, and the
// global space-bar preview.
//
// Split out of MainComponent.cpp purely to keep that file manageable — this is the SAME
// class, same members, identical logic; only the definitions live here. See MainComponent.h
// for the declarations.
#include "MainComponent.h"

#include "../Audio/PlaybackSources.h"
#include "../Audio/WarpEngine.h"
#include "../Sampler/SamplerEngine.h"
#include "OrionTheme.h"

#include <memory>
#include <vector>

namespace orion
{
namespace
{
// Browser previews play this far below unity, so a sample gets louder the moment it is dropped
// into the playlist (where it plays at its true level).
//
// This matches Live, but not because Live has a rule about preview headroom — it doesn't. Live
// routes previews through a separate Preview/Cue Volume control on the Main track, bypassing the
// track fader and the main fader entirely, and that control ships set to -6.0 dB (verified in
// Live 12: Main row shows main volume 0.0 and cue volume -6.0 out of the box). The famous
// "louder when you drop it" jump is just those two independent gain paths, 6 dB apart.
//
// We hard-code the same 6 dB. If it ever needs to be user-adjustable, a Preview Volume control
// mirroring Live's Cue Volume is the honest way to do it.
constexpr double browserPreviewHeadroomDb = -6.0;
}

void MainComponent::playBrowserPreview(const BrowserItem& item)
{
    if (! item.file.existsAsFile())
    {
        statusLabel.setText("Preview failed: file missing", juce::dontSendNotification);
        return;
    }

    // Same file already loaded at the current tempo and sync mode → just restart instantly.
    if ((previewBufferSource != nullptr || previewFileSource != nullptr)
        && item.file == currentPreviewFile
        && std::abs(currentPreviewTempoBpm - projectState.getTempoBpm()) < 0.001
        && currentPreviewBpmSync == browserPanel.isPreviewBpmSyncEnabled())
    {
        pendingBrowserPreviewStart = false;
        previewTransportSource.stop();
        armOrStartBrowserPreview();
        return;
    }

    statusLabel.setText("Previewing: " + item.file.getFileName(), juce::dontSendNotification);

    // Ableton's browser treats a one-bar preview as a one-shot and loops longer musical
    // material. BrowserPanelComponent estimates the musical length from its metadata.
    currentPreviewLooping = item.defaultClipLengthInBeats
                            > static_cast<double>(juce::jmax(1, projectState.getNumerator()));

    // Read + decode on a background thread so flipping through samples on a slow / external
    // drive never freezes the UI. A generation token discards stale loads when the user has
    // already moved on to another sample.
    const auto generation = ++previewRequestGeneration;
    pendingBrowserPreviewStart = false;
    const auto file = item.file;
    const auto displayName = item.file.getFileNameWithoutExtension();
    const auto tempoNow = projectState.getTempoBpm();
    const auto numerator = projectState.getNumerator();
    const auto fitToTempo = browserPanel.isPreviewBpmSyncEnabled();
    currentPreviewBpmSync = fitToTempo;
    currentPreviewLooping = item.defaultClipLengthInBeats
                            > static_cast<double>(juce::jmax(1, projectState.getNumerator()));

    // Stop the previous preview immediately. The new source below owns one continuous
    // tempo-fitted stream, so there is no raw-then-warped handoff and no double start.
    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();
    previewFileSource.reset();
    browserPanel.setPreviewPlayback(false, 0.0f);
    juce::Component::SafePointer<MainComponent> safeThis(this);

    previewLoadPool.addJob([this, safeThis, generation, file, displayName, tempoNow, numerator, fitToTempo]
    {
        std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return;

        const auto sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
        const auto sourceSamples = juce::jmax<juce::int64>(1, reader->lengthInSamples);
        const auto analysis = fitToTempo ? analyzeAudioWarpMetadata(file, tempoNow, numerator, false)
                                         : AudioWarpAnalysis {};
        const auto tempoRatio = fitToTempo && analysis.sourceBpm > 0.0 && tempoNow > 0.0
                              ? analysis.sourceBpm / tempoNow : 1.0;
        const auto outputSamples64 = juce::jmax<juce::int64>(1, static_cast<juce::int64>(std::llround(
            static_cast<double>(sourceSamples) * tempoRatio)));
        const auto outputSamples = static_cast<int>(juce::jmin<juce::int64>(
            outputSamples64, static_cast<juce::int64>(std::numeric_limits<int>::max())));
        if (outputSamples <= 0)
            return;

        // If a newer preview was requested while we were reading, drop this one.
        if (generation != previewRequestGeneration.load())
            return;

        juce::MessageManager::callAsync([safeThis, generation, reader = std::move(reader), outputSamples,
                                         sampleRate, file, displayName]() mutable
        {
            if (safeThis == nullptr || generation != safeThis->previewRequestGeneration.load())
                return;
            safeThis->startStreamingPreviewPlayback(std::move(reader), outputSamples, sampleRate, file, displayName);
        });

        // Waveform analysis is deliberately decoupled from audio start. It can finish later
        // without affecting the already-playing stream.
        std::unique_ptr<juce::AudioFormatReader> waveformReader(audioFormatManager.createReaderFor(file));
        if (waveformReader != nullptr && waveformReader->lengthInSamples > 0)
        {
            const auto waveformSamples = static_cast<int>(juce::jmin<juce::int64>(
                waveformReader->lengthInSamples, static_cast<juce::int64>(std::numeric_limits<int>::max())));
            juce::AudioBuffer<float> waveformBuffer(static_cast<int>(waveformReader->numChannels), waveformSamples);
            waveformReader->read(&waveformBuffer, 0, waveformSamples, 0, true, true);
            constexpr int columns = 480;
            std::vector<float> peaks(columns, 0.0f);
            float globalMax = 1.0e-6f;
            for (int c = 0; c < columns; ++c)
            {
                const auto s0 = static_cast<juce::int64>(c) * waveformSamples / columns;
                const auto s1 = juce::jmax(s0 + 1, static_cast<juce::int64>(c + 1) * waveformSamples / columns);
                for (int ch = 0; ch < waveformBuffer.getNumChannels(); ++ch)
                    for (auto s = s0; s < s1 && s < waveformSamples; ++s)
                        peaks[static_cast<std::size_t>(c)] = juce::jmax(peaks[static_cast<std::size_t>(c)],
                                                                       std::abs(waveformBuffer.getSample(ch, static_cast<int>(s))));
                globalMax = juce::jmax(globalMax, peaks[static_cast<std::size_t>(c)]);
            }
            for (auto& p : peaks) p /= globalMax;
            juce::MessageManager::callAsync([safeThis, generation, displayName, peaks = std::move(peaks)]() mutable
            {
                if (safeThis != nullptr && generation == safeThis->previewRequestGeneration.load())
                    safeThis->browserPanel.setPreviewWaveform(displayName, std::move(peaks));
            });
        }
    });
}

void MainComponent::armOrStartBrowserPreview()
{
    previewTransportSource.setPosition(0.0);

    const auto synced = browserPanel.isPreviewBpmSyncEnabled();

    // Preview loop is decided from the item's musical length; tempo sync is independent.
    if (previewBufferSource != nullptr)
        previewBufferSource->setLooping(currentPreviewLooping);
    if (previewFileSource != nullptr)
        previewFileSource->setLooping(currentPreviewLooping);

    // Launch quantize: only when synced to project tempo AND the transport is actually
    // running — without a moving playhead there's no beat to lock onto.
    if (synced && transportEngine.isPlaying())
    {
        const auto currentBeat = transportEngine.getPlayheadBeat();
        // Ableton starts the preview "at the beginning of the next bar", not the next beat, so
        // the loop lands on the downbeat and you hear it against the arrangement in phase.
        const auto beatsPerBar = juce::jmax(1.0, static_cast<double>(projectState.getNumerator()));
        const auto barIndex = std::floor((currentBeat + 1.0e-4) / beatsPerBar);

        pendingBrowserPreviewStart = true;
        pendingBrowserPreviewGeneration = previewRequestGeneration.load();
        pendingBrowserPreviewStartBeat = (barIndex + 1.0) * beatsPerBar;
        pendingBrowserPreviewLastBeat = currentBeat;
        browserPanel.setPreviewArmed(true);
    }
    else
    {
        pendingBrowserPreviewStart = false;
        browserPanel.setPreviewArmed(false);
        previewTransportSource.start();
    }
}

void MainComponent::startPreviewPlayback(juce::AudioBuffer<float> previewBuffer, double sampleRate,
                                         const juce::File& file, const juce::String& displayName)
{
    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();
    previewFileSource.reset();

    // Compact waveform (normalised abs peaks) for the browser's preview bar.
    {
        constexpr int columns = 480;
        std::vector<float> peaks(columns, 0.0f);
        const auto total = previewBuffer.getNumSamples();
        const auto chans = previewBuffer.getNumChannels();
        if (total > 0 && chans > 0)
        {
            float globalMax = 1.0e-6f;
            for (int c = 0; c < columns; ++c)
            {
                const auto s0 = static_cast<juce::int64>(c) * total / columns;
                const auto s1 = juce::jmax(s0 + 1, static_cast<juce::int64>(c + 1) * total / columns);
                float m = 0.0f;
                for (int ch = 0; ch < chans; ++ch)
                {
                    const auto* d = previewBuffer.getReadPointer(ch);
                    for (auto s = s0; s < s1 && s < total; ++s)
                        m = juce::jmax(m, std::abs(d[s]));
                }
                peaks[static_cast<std::size_t>(c)] = m;
                globalMax = juce::jmax(globalMax, m);
            }
            for (auto& p : peaks) p /= globalMax;
        }
        browserPanel.setPreviewWaveform(displayName, std::move(peaks));
    }

    previewBufferSource = std::make_unique<BufferPreviewSource>(std::move(previewBuffer), sampleRate);
    previewTransportSource.setSource(previewBufferSource.get(), 0, nullptr, sampleRate);
    // Browser preview plays with headroom (quieter than unity), like Ableton — so a
    // sample audibly "opens up" / gets louder the moment you drop it into the playlist,
    // where it plays at its true level.
    previewTransportSource.setGain(juce::Decibels::decibelsToGain(browserPreviewHeadroomDb));
    currentPreviewFile = file;
    currentPreviewTempoBpm = projectState.getTempoBpm();
    currentPreviewBpmSync = browserPanel.isPreviewBpmSyncEnabled();
    armOrStartBrowserPreview();
}

void MainComponent::startStreamingPreviewPlayback(std::unique_ptr<juce::AudioFormatReader> reader,
                                                   int outputSamples, double sampleRate,
                                                   const juce::File& file, const juce::String& displayName)
{
    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();
    previewFileSource.reset();

    previewFileSource = std::make_unique<StreamingFilePreviewSource>(std::move(reader), outputSamples,
                                                                       sampleRate, currentPreviewLooping);
    previewTransportSource.setSource(previewFileSource.get(), 0, nullptr, sampleRate);
    previewTransportSource.setGain(juce::Decibels::decibelsToGain(browserPreviewHeadroomDb));
    currentPreviewFile = file;
    currentPreviewTempoBpm = projectState.getTempoBpm();
    currentPreviewBpmSync = browserPanel.isPreviewBpmSyncEnabled();
    armOrStartBrowserPreview();
}

void MainComponent::stopBrowserPreview(bool resetPosition)
{
    previewTransportSource.stop();
    if (resetPosition)
        previewTransportSource.setPosition(0.0);
}

bool MainComponent::startClipEditorPreview()
{
    auto* clip = getSelectedTimelineClip();
    if (clip == nullptr || clip->type != ClipType::audio || clip->sourcePath.isEmpty())
        return false;

    juce::File sourceFile(clip->sourcePath);
    if (! sourceFile.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(sourceFile));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
        return false;

    const auto startRatio = juce::jlimit(0.0, 0.999, clipEditorPreviewStartRatio);
    const auto endRatio = juce::jlimit(startRatio + 0.001, 1.0, clipEditorPreviewEndRatio);
    const auto totalSamples64 = reader->lengthInSamples;
    if (totalSamples64 > static_cast<juce::int64>(std::numeric_limits<int>::max()))
        return false;
    const int fullSourceSamples = static_cast<int>(totalSamples64);

    // Total pitch shift to match the arrangement (manual transpose + auto key-match).
    // Auto key-match is skipped entirely when the project has no key.
    int semitones = clip->transposeSemitones;
    if (clip->keyShiftEnabled && clip->sourceKeyRoot >= 0 && projectState.isKeyEnabled())
    {
        int keyDiff = projectState.getKeyRoot() - clip->sourceKeyRoot;
        while (keyDiff > 6)  keyDiff -= 12;
        while (keyDiff < -6) keyDiff += 12;
        semitones += keyDiff;
    }
    const auto pitchScale = std::pow(2.0, static_cast<double>(semitones) / 12.0);

    // AKAI MPC-style: warp the WHOLE source to project tempo (not just the selected
    // region), then loop the [startRatio, endRatio] selection. Because the whole source is
    // rendered, the START/END markers can be dragged anywhere during playback and the loop
    // follows live (see setLoopBounds in onSampleRangeChanged).
    const double projectBps = projectState.getTempoBpm() / 60.0;
    int fullTargetSamples = fullSourceSamples;
    double fullWarpBeats = 0.0;
    if (clip->warpEnabled && projectBps > 0.0)
    {
        const auto clipTrimStart = juce::jlimit(0.0, 0.999, clip->sampleStartRatio);
        const auto clipTrimEnd   = juce::jlimit(clipTrimStart + 0.001, 1.0, clip->sampleEndRatio);
        const auto clipTrimSpan  = juce::jmax(0.001, clipTrimEnd - clipTrimStart);
        fullWarpBeats = clip->warpTargetLengthInBeats > 0.0
            ? clip->warpTargetLengthInBeats
            : clip->lengthInBeats / clipTrimSpan;
        const double tgt = (fullWarpBeats / projectBps) * reader->sampleRate;
        fullTargetSamples = juce::jlimit(1, std::numeric_limits<int>::max(), static_cast<int>(std::llround(tgt)));
    }

    // Piecewise warp control points for the streamer: (outputRatio, sourceRatio) across the whole
    // warped source. Only when warp is on and the user has placed markers; otherwise empty = linear.
    std::vector<std::pair<double, double>> warpPts;
    juce::String warpSig;
    if (clip->warpEnabled && ! clip->warpMarkers.empty() && fullWarpBeats > 0.0)
    {
        for (const auto& p : warpControlPoints(clip->warpMarkers, fullWarpBeats))
        {
            warpPts.push_back({ p.beat / fullWarpBeats, p.sourceRatio });
            warpSig << juce::String(p.sourceRatio, 4) << ":" << juce::String(p.beat, 4) << ";";
        }
    }

    clipEditorPreviewResumeSeconds = -1.0;

    const double fullDurationSeconds = static_cast<double>(fullTargetSamples) / reader->sampleRate;
    const auto streamKey = clip->sourcePath.toStdString()
                         + "|" + std::to_string(semitones)
                         + "|t" + std::to_string(fullTargetSamples)
                         + "|w" + warpSig.toStdString();

    // Reuse the existing rendered source when nothing that affects the buffer changed
    // (same file/pitch/tempo). Its producer fills the whole source even while stopped, so
    // re-pressing Play is instant from ANY loop start — no re-render, no fill wait.
    const bool canReuse = clipEditorPreviewStreamSource != nullptr
                          && clipEditorPreviewStreamKey == streamKey;

    if (transportEngine.isPlaying() || transportEngine.isCountInActive())
    {
        transportEngine.pause();
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allInstrumentNotesOff();
            arrangementPlaybackSource->clearClipEditorPreviewTrim();
            arrangementPlaybackSource->syncToTransportPosition();
        }
    }
    stopBrowserPreview(true);
    clipEditorPreviewTransportSource.stop();

    if (! canReuse)
    {
        // Read the full source and build a fresh looping streamer.
        juce::AudioBuffer<float> region(juce::jmax(1, static_cast<int>(reader->numChannels)),
                                        juce::jmax(1, fullSourceSamples));
        region.clear();
        reader->read(&region, 0, fullSourceSamples, 0, true, true);

        clipEditorPreviewTransportSource.setSource(nullptr);
        clipEditorPreviewBufferSource.reset();
        clipEditorPreviewStreamSource.reset();
        clipEditorPreviewStreamSource = std::make_unique<StreamingWarpPreviewSource>(
            std::move(region), reader->sampleRate, juce::jmax(1, fullTargetSamples), pitchScale, std::move(warpPts));
        clipEditorPreviewStreamKey = streamKey;
        clipEditorPreviewTransportSource.setSource(clipEditorPreviewStreamSource.get(), 0, nullptr, reader->sampleRate);
    }

    clipEditorPreviewStreamSource->setLoopBounds(startRatio, endRatio);

    // The playhead maps over the FULL warped source (0..1); the loop keeps it inside the
    // selection. Start the transport AT the loop start so the playhead doesn't flash at 0.
    clipEditorLocalPreviewStartRatio = 0.0;
    clipEditorLocalPreviewEndRatio = 1.0;
    clipEditorLocalPreviewDurationSeconds = fullDurationSeconds;
    clipEditorPreviewTransportSource.setPosition(startRatio * fullDurationSeconds);
    setClipEditorLocalPreviewPosition(startRatio);
    clipEditorPreviewTransportSource.start();
    return true;
}

void MainComponent::playClipEditorPreviewBuffer(juce::AudioBuffer<float> buffer, double sampleRate,
                                                double startRatio, double endRatio,
                                                double rawDurationSeconds, double resumeSeconds)
{
    if (transportEngine.isPlaying() || transportEngine.isCountInActive())
    {
        transportEngine.pause();
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allInstrumentNotesOff();
            arrangementPlaybackSource->clearClipEditorPreviewTrim();
            arrangementPlaybackSource->syncToTransportPosition();
        }
    }

    stopBrowserPreview(true);
    clipEditorPreviewTransportSource.stop();
    clipEditorPreviewTransportSource.setSource(nullptr);
    clipEditorPreviewStreamSource.reset();   // drop the streaming stand-in, if any
    clipEditorPreviewBufferSource = std::make_unique<BufferPreviewSource>(std::move(buffer), sampleRate);
    clipEditorPreviewTransportSource.setSource(clipEditorPreviewBufferSource.get(), 0, nullptr, sampleRate);
    clipEditorPreviewTransportSource.setPosition(0.0);
    clipEditorLocalPreviewStartRatio = startRatio;
    clipEditorLocalPreviewEndRatio = endRatio;
    clipEditorLocalPreviewDurationSeconds = rawDurationSeconds;
    setClipEditorLocalPreviewPosition(startRatio);

    if (resumeSeconds >= 0.0)
        clipEditorPreviewTransportSource.setPosition(juce::jlimit(0.0, clipEditorLocalPreviewDurationSeconds, resumeSeconds));

    clipEditorPreviewTransportSource.start();
}

void MainComponent::stopClipEditorPreview(bool resetToStart)
{
    // Keep the streaming source alive and attached so re-pressing Play reuses its already-
    // rendered buffer (instant). Its producer keeps filling even while stopped. The source
    // is only torn down when the clip/pitch/tempo changes (key mismatch) or on shutdown.
    clipEditorPreviewTransportSource.stop();
    if (clipEditorPreviewStreamSource != nullptr)
        clipEditorPreviewTransportSource.setPosition(clipEditorPreviewStartRatio * clipEditorLocalPreviewDurationSeconds);
    if (resetToStart)
        setClipEditorLocalPreviewPosition(clipEditorPreviewStartRatio);
}

void MainComponent::updateClipEditorPreviewPlayhead()
{
    if (! clipEditorPreviewTransportSource.isPlaying())
        return;

    if (clipEditorLocalPreviewDurationSeconds <= 0.0)
    {
        stopClipEditorPreview(true);
        return;
    }

    // The preview loops the selection (MPC-style), so it doesn't auto-stop. The streaming
    // source's read position maps directly onto the full warped source: ratio = position /
    // full-output-duration, which lands inside the live loop region.
    const auto position = clipEditorPreviewTransportSource.getCurrentPosition();
    clipEditorPreviewPlayheadRatio = juce::jlimit(0.0, 1.0, position / clipEditorLocalPreviewDurationSeconds);
}

void MainComponent::startGlobalSpacePreview(double startBeat)
{
    if (! globalSpacePreviewRestoreBeat.has_value())
    {
        globalSpacePreviewRestoreBeat = transportEngine.getPlayheadBeat();
        globalSpacePreviewWasRecordArmed = transportEngine.isRecordArmed();
    }

    stopBrowserPreview(true);
    stopClipEditorPreview(true);

    // NB: do NOT panic the instruments here. allInstrumentNotesOff() starts a 3-block panic
    // that skips clip note-ons, which dropped the very first note when the preview started
    // right before it. (Stop still panics to silence the tail.)

    transportEngine.pause();
    transportController.setRecordArmed(false);
    transportEngine.setPlayheadBeat(startBeat);

    if (arrangementPlaybackSource != nullptr)
    {
        if (arrangementPlaybackSource->isRealtimeWarpEnabled())
            arrangementPlaybackSource->prepareWarpStreams(/*blockForLead*/ true, /*allowSyncDecode*/ true);
        else
            arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();

        arrangementPlaybackSource->syncToTransportPosition();
    }

    transportEngine.play(false);
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::commitGlobalSpacePreview()
{
    // A space TAP turns the momentary preview into normal playback: keep playing and drop
    // the rewind anchor so it doesn't snap back.
    globalSpacePreviewRestoreBeat.reset();
    updateTransportLabels();
}

void MainComponent::stopGlobalSpacePreview()
{
    if (! globalSpacePreviewRestoreBeat.has_value())
        return;

    transportEngine.pause();

    if (arrangementPlaybackSource != nullptr)
    {
        arrangementPlaybackSource->allSamplerNotesOff();
        arrangementPlaybackSource->allInstrumentNotesOff();
    }

    const auto restoreBeat = *globalSpacePreviewRestoreBeat;
    globalSpacePreviewRestoreBeat.reset();
    transportController.setRecordArmed(globalSpacePreviewWasRecordArmed);
    transportEngine.setPlayheadBeat(restoreBeat);

    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->syncToTransportPosition();

    updateTransportLabels();
    arrangementTimeline.repaint();
}
} // namespace orion
