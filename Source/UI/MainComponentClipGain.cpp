// Clip level work for MainComponent: peak/LUFS analysis, loudness matching, normalisation and
// clip gain.
//
// Split out of MainComponent.cpp purely to keep that file manageable — this is the SAME
// class, same members, identical logic; only the definitions live here. See MainComponent.h
// for the declarations.
#include "MainComponent.h"

#include "../Audio/LoudnessMeter.h"
#include "../Audio/PlaybackSources.h"
#include "OrionTheme.h"

#include <cmath>
#include <memory>
#include <vector>

namespace orion
{
namespace
{
// Peak target. Just under 0 dBFS: high enough that already-mastered loops (which peak around
// -0.1 dB) are left where they are instead of being pulled down, low enough to leave room for
// the inter-sample peaks that lossy encoders push above full scale. The old -3.0 quietly
// attenuated every commercial loop by ~3 dB.
constexpr double normalizeTargetDb = -0.3;
constexpr double clipGainMinDb = -24.0;
constexpr double clipGainMaxDb = 12.0;
}

float MainComponent::measureClipPeak(const TimelineClip& clip)
{
    return measureClipLevels(clip).peak;
}

MainComponent::ClipLevels MainComponent::measureClipLevels(const TimelineClip& clip)
{
    ClipLevels levels;
    levels.lufs = orion::LoudnessMeter::silence();

    if (clip.type != ClipType::audio || clip.sourcePath.isEmpty())
        return levels;

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(juce::File(clip.sourcePath)));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return levels;

    // Only the trimmed region is audible, so only it decides the peak. Scanning the whole file
    // let a loud transient outside the trim hold the clip down.
    const auto total = reader->lengthInSamples;
    const auto ratioToSample = [total] (double r)
    {
        return juce::jlimit<juce::int64>(0, total, static_cast<juce::int64>(std::llround(juce::jlimit(0.0, 1.0, r) * static_cast<double>(total))));
    };

    auto position = ratioToSample(clip.sampleStartRatio);
    const auto end = juce::jmax(position + 1, ratioToSample(clip.sampleEndRatio));

    constexpr int chunkSize = 16384;
    const auto numChannels = static_cast<int>(reader->numChannels);
    juce::AudioBuffer<float> buffer(numChannels, chunkSize);
    orion::LoudnessMeter meter(reader->sampleRate, numChannels);
    float peak = 0.0f;

    // One pass for both: the file read dominates, and the K-weighting is cheap next to it.
    while (position < end)
    {
        const auto samplesToRead = static_cast<int>(juce::jmin<juce::int64>(chunkSize, end - position));
        buffer.clear();
        reader->read(&buffer, 0, samplesToRead, position, true, true);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            peak = juce::jmax(peak, buffer.getMagnitude(channel, 0, samplesToRead));
        meter.process(buffer, 0, samplesToRead);
        position += samplesToRead;
    }

    levels.peak = peak;
    levels.lufs = meter.getIntegratedLufs();
    return levels;
}

void MainComponent::matchClipLoudness(const std::vector<ArrangementTimelineComponent::SelectedClip>& selection)
{
    if (selection.empty())
        return;
    arrangementTimeline.captureUndoSnapshot();   // so Cmd+Z undoes THIS gain change, not the previous edit
    auto& tracks = projectState.getTracks();

    struct Entry { TimelineClip* clip; ClipLevels levels; };
    std::vector<Entry> entries;

    for (const auto& sel : selection)
    {
        if (sel.trackIndex < 0 || sel.trackIndex >= static_cast<int>(tracks.size()))
            continue;

        auto& trackClips = tracks[static_cast<std::size_t>(sel.trackIndex)].clips;
        if (sel.clipIndex < 0 || sel.clipIndex >= static_cast<int>(trackClips.size()))
            continue;

        auto& clip = trackClips[static_cast<std::size_t>(sel.clipIndex)];
        if (clip.type != ClipType::audio || clip.sourcePath.isEmpty())
            continue;

        const auto levels = measureClipLevels(clip);
        if (levels.peak <= 0.000001f || ! std::isfinite(levels.lufs))
            continue;   // unreadable, silent, or shorter than one 400ms gating block

        entries.push_back({ &clip, levels });
    }

    if (entries.size() < 2)
    {
        statusLabel.setText(entries.empty() ? "Match loudness: no measurable audio clips"
                                            : "Match loudness needs two or more clips",
                            juce::dontSendNotification);
        return;
    }

    // The loudest clip is the reference and keeps its current gain; everything else moves to it.
    const auto reference = std::max_element(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) { return a.levels.lufs < b.levels.lufs; })->levels.lufs;

    int clipped = 0;

    for (auto& e : entries)
    {
        const auto wanted = reference - e.levels.lufs;

        // Loudness says how much to lift; the peak says how much we can lift before clipping.
        // Take the smaller. A dense loop can be quiet in LUFS yet already near full scale.
        const auto peakDb = static_cast<double>(juce::Decibels::gainToDecibels(e.levels.peak, -60.0f));
        const auto headroom = normalizeTargetDb - peakDb;

        const auto applied = juce::jlimit(clipGainMinDb, clipGainMaxDb, juce::jmin(wanted, headroom));
        if (applied < wanted - 0.05)
            ++clipped;

        e.clip->gainDb = applied;
    }

    auto message = "Matched " + juce::String(entries.size()) + " clips to "
                 + juce::String(reference, 1) + " LUFS";
    if (clipped > 0)
        message += " (" + juce::String(clipped) + " capped at the clipping ceiling)";

    statusLabel.setText(message, juce::dontSendNotification);
    refreshClipInspector();
    refreshClipEditor();
    arrangementTimeline.repaint();
}

void MainComponent::normalizeClips(const std::vector<ArrangementTimelineComponent::SelectedClip>& selection,
                                   bool relativeToLoudest)
{
    if (selection.empty())
        return;
    arrangementTimeline.captureUndoSnapshot();   // so Cmd+Z undoes THIS gain change, not the previous edit
    auto& tracks = projectState.getTracks();

    std::vector<TimelineClip*> clips;
    std::vector<float> peaks;

    for (const auto& sel : selection)
    {
        if (sel.trackIndex < 0 || sel.trackIndex >= static_cast<int>(tracks.size()))
            continue;

        auto& trackClips = tracks[static_cast<std::size_t>(sel.trackIndex)].clips;
        if (sel.clipIndex < 0 || sel.clipIndex >= static_cast<int>(trackClips.size()))
            continue;

        auto& clip = trackClips[static_cast<std::size_t>(sel.clipIndex)];
        if (clip.type != ClipType::audio || clip.sourcePath.isEmpty())
            continue;

        const auto peak = measureClipPeak(clip);
        if (peak <= 0.000001f)   // unreadable or silent: leave it alone
            continue;

        clips.push_back(&clip);
        peaks.push_back(peak);
    }

    if (clips.empty())
    {
        statusLabel.setText("Normalize skipped: no readable audio clips", juce::dontSendNotification);
        return;
    }

    if (relativeToLoudest)
    {
        // One offset for everyone, set by the loudest clip. Quiet clips stay quiet relative to
        // it — the point is to lift the group without flattening the balance inside it.
        const auto loudest = *std::max_element(peaks.begin(), peaks.end());
        const auto offsetDb = juce::jlimit(clipGainMinDb, clipGainMaxDb,
                                           normalizeTargetDb - static_cast<double>(juce::Decibels::gainToDecibels(loudest, -60.0f)));

        for (auto* clip : clips)
            clip->gainDb = offsetDb;

        statusLabel.setText("Normalized " + juce::String(clips.size()) + " clips to loudest: "
                                + juce::String(offsetDb, 1) + " dB",
                            juce::dontSendNotification);
    }
    else
    {
        for (std::size_t i = 0; i < clips.size(); ++i)
        {
            const auto peakDb = static_cast<double>(juce::Decibels::gainToDecibels(peaks[i], -60.0f));
            clips[i]->gainDb = juce::jlimit(clipGainMinDb, clipGainMaxDb, normalizeTargetDb - peakDb);
        }

        statusLabel.setText(clips.size() == 1
                                ? "Normalized clip: " + juce::String(clips.front()->gainDb, 1) + " dB"
                                : "Normalized " + juce::String(clips.size()) + " clips individually",
                            juce::dontSendNotification);
    }

    refreshClipInspector();
    refreshClipEditor();
    arrangementTimeline.repaint();
}

void MainComponent::setClipsGainDb(const std::vector<ArrangementTimelineComponent::SelectedClip>& selection, double gainDb)
{
    if (selection.empty())
        return;
    arrangementTimeline.captureUndoSnapshot();   // so Cmd+Z undoes THIS gain change, not the previous edit
    auto& tracks = projectState.getTracks();
    const auto clamped = juce::jlimit(clipGainMinDb, clipGainMaxDb, gainDb);
    int changed = 0;

    for (const auto& sel : selection)
    {
        if (sel.trackIndex < 0 || sel.trackIndex >= static_cast<int>(tracks.size()))
            continue;

        auto& trackClips = tracks[static_cast<std::size_t>(sel.trackIndex)].clips;
        if (sel.clipIndex < 0 || sel.clipIndex >= static_cast<int>(trackClips.size()))
            continue;

        auto& clip = trackClips[static_cast<std::size_t>(sel.clipIndex)];
        if (clip.type != ClipType::audio)
            continue;

        clip.gainDb = clamped;
        ++changed;
    }

    if (changed > 0)
    {
        statusLabel.setText("Clip gain: " + juce::String(clamped, 1) + " dB ("
                                + juce::String(changed) + (changed == 1 ? " clip)" : " clips)"),
                            juce::dontSendNotification);
        refreshClipInspector();
        refreshClipEditor();
        arrangementTimeline.repaint();
    }
}

void MainComponent::normalizeSelectedAudioClip()
{
    // Clip-editor button: the one clip the editor is showing, routed through normalizeClips()
    // so it picks up the trim-aware peak too.
    if (! selectedArrangementClip.has_value())
    {
        statusLabel.setText("Normalize skipped: no clip selected", juce::dontSendNotification);
        return;
    }

    const auto [trackIndex, clipIndex] = *selectedArrangementClip;
    normalizeClips({ { trackIndex, clipIndex } }, false);
}
} // namespace orion
