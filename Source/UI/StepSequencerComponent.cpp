#include "StepSequencerComponent.h"

#include <algorithm>
#include <cmath>

#include "OrionTheme.h"

namespace orion
{
namespace
{
constexpr int kHeaderHeight = 40;
constexpr int kRowHeight    = 44;
constexpr int kRowGap       = 6;
constexpr int kLeftPad      = 18;
constexpr int kMuteWidth    = 22;
constexpr int kNameWidth    = 148;
constexpr int kColGap       = 12;   // gap between the name block and the step grid
constexpr double kBeatEpsilonFraction = 0.5;
}  // namespace

StepSequencerComponent::StepSequencerComponent(ProjectState& projectState, TransportEngine& transportEngine)
    : project(projectState), transport(transportEngine)
{
    setWantsKeyboardFocus(false);
}

StepSequencerComponent::~StepSequencerComponent() = default;

int StepSequencerComponent::beatsPerBar() const noexcept
{
    return juce::jmax(1, project.getNumerator());
}

int StepSequencerComponent::patternSteps() const noexcept
{
    return kStepsPerBar * juce::jmax(1, bars);
}

double StepSequencerComponent::stepLengthBeats() const noexcept
{
    return static_cast<double>(beatsPerBar()) / static_cast<double>(kStepsPerBar);
}

double StepSequencerComponent::patternBeats() const noexcept
{
    return static_cast<double>(beatsPerBar()) * juce::jmax(1, bars);
}

int StepSequencerComponent::channelNotePitch(const TrackState& track) const noexcept
{
    // Sampler channels trigger their sample at its root note; instruments use middle C.
    return track.samplerSourcePath.isNotEmpty() ? track.samplerRootMidiNote : 60;
}

void StepSequencerComponent::rebuildChannels()
{
    channelTrackIndices.clear();
    auto& tracks = project.getTracks();
    const auto patBeats = patternBeats();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        auto& track = tracks[static_cast<std::size_t>(i)];
        if (! track.isMidiTrack)   // sampler + instrument tracks are channels
            continue;
        channelTrackIndices.push_back(i);

        // Keep a plain (non-melodic) drum step pattern exactly one grid long. Otherwise a stray
        // long note that was later deleted leaves the clip — and the playback loop — overlong.
        // Melodic channels keep their own length (handled by the piano roll).
        if (! rowIsMelodic(i))
            for (auto& clip : track.clips)
                if (clip.type == ClipType::midi && std::abs(clip.startBeat) < 1.0e-4
                    && std::abs(clip.lengthInBeats - patBeats) > 1.0e-4)
                    clip.lengthInBeats = patBeats;
    }
}

int StepSequencerComponent::findPatternClip(int trackIndex, bool createIfMissing)
{
    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return -1;

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    for (int i = 0; i < static_cast<int>(track.clips.size()); ++i)
    {
        const auto& clip = track.clips[static_cast<std::size_t>(i)];
        if (clip.type == ClipType::midi && std::abs(clip.startBeat) < 1.0e-4)
            return i;
    }

    if (! createIfMissing)
        return -1;

    TimelineClip clip;
    clip.name = "Pattern";
    clip.type = ClipType::midi;
    clip.startBeat = 0.0;
    clip.lengthInBeats = patternBeats();
    clip.colour = track.colour;
    track.clips.push_back(std::move(clip));
    return static_cast<int>(track.clips.size()) - 1;
}

bool StepSequencerComponent::stepActive(int trackIndex, int step) const
{
    const auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return false;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    const auto pitch = channelNotePitch(track);
    const auto stepLen = stepLengthBeats();
    const auto target = step * stepLen;

    for (const auto& clip : track.clips)
    {
        if (clip.type != ClipType::midi || std::abs(clip.startBeat) > 1.0e-4)
            continue;
        for (const auto& note : clip.midiNotes)
            if (note.pitch == pitch && std::abs(note.startBeat - target) < stepLen * kBeatEpsilonFraction)
                return true;
    }
    return false;
}

const TimelineClip* StepSequencerComponent::rowClip(int trackIndex) const
{
    const auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return nullptr;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    for (const auto& clip : track.clips)
        if (clip.type == ClipType::midi && std::abs(clip.startBeat) < 1.0e-4)
            return &clip;
    return nullptr;
}

bool StepSequencerComponent::rowIsMelodic(int trackIndex) const
{
    const auto* clip = rowClip(trackIndex);
    if (clip == nullptr || clip->midiNotes.empty())
        return false;

    const auto& tracks = project.getTracks();
    const auto pitch  = channelNotePitch(tracks[static_cast<std::size_t>(trackIndex)]);
    const auto stepLen = stepLengthBeats();

    for (const auto& note : clip->midiNotes)
    {
        if (note.pitch != pitch)
            return true;                                   // a different pitch → melody
        if (note.lengthInBeats > stepLen * 1.6)
            return true;                                   // sustained note → melody
        const auto offGrid = std::abs(note.startBeat - std::round(note.startBeat / stepLen) * stepLen);
        if (offGrid > stepLen * kBeatEpsilonFraction)
            return true;                                   // off the step grid → melody
    }
    return false;
}

bool StepSequencerComponent::stepIsOn(int trackIndex, int step) const
{
    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return false;

    const int clipIndex = const_cast<StepSequencerComponent*>(this)->findPatternClip(trackIndex, false);
    if (clipIndex < 0)
        return false;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    const auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
    const auto pitch = channelNotePitch(track);
    const auto stepLen = stepLengthBeats();
    const auto target = step * stepLen;
    for (const auto& n : clip.midiNotes)
        if (n.pitch == pitch && std::abs(n.startBeat - target) < stepLen * kBeatEpsilonFraction)
            return true;
    return false;
}

void StepSequencerComponent::setStep(int trackIndex, int step, bool on)
{
    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    const int clipIndex = findPatternClip(trackIndex, true);
    if (clipIndex < 0)
        return;

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
    const auto pitch = channelNotePitch(track);
    const auto stepLen = stepLengthBeats();
    const auto target = step * stepLen;

    // Guard the note vector against the audio render thread reading it mid-reallocation.
    const juce::ScopedLock audioLock(project.getAudioEditLock());

    for (auto it = clip.midiNotes.begin(); it != clip.midiNotes.end(); ++it)
    {
        if (it->pitch == pitch && std::abs(it->startBeat - target) < stepLen * kBeatEpsilonFraction)
        {
            if (on)
                return;   // already on — nothing to do
            clip.midiNotes.erase(it);
            if (onPatternEdited)
                onPatternEdited();
            repaint();
            return;
        }
    }

    if (! on)
        return;   // already off — nothing to do

    clip.midiNotes.push_back(MidiNote { pitch, target, stepLen, 100 });
    if (clip.lengthInBeats < patternBeats() - 1.0e-4)
        clip.lengthInBeats = patternBeats();
    if (onPatternEdited)
        onPatternEdited();
    repaint();
}

void StepSequencerComponent::toggleStep(int trackIndex, int step)
{
    setStep(trackIndex, step, ! stepIsOn(trackIndex, step));
}

//==============================================================================
std::pair<juce::Rectangle<int>, juce::Rectangle<int>> StepSequencerComponent::stepCountToggleBounds() const
{
    auto header = getLocalBounds().removeFromTop(kHeaderHeight).reduced(kLeftPad, 0);
    header.removeFromLeft(184);   // past the title
    auto area = header.withSizeKeepingCentre(112, 22);
    auto left = area.removeFromLeft(area.getWidth() / 2);
    return { left, area };
}

juce::Rectangle<int> StepSequencerComponent::gridArea() const
{
    return getLocalBounds().withTrimmedTop(kHeaderHeight).reduced(kLeftPad, 8);
}

juce::Rectangle<int> StepSequencerComponent::rowBounds(int row) const
{
    auto area = gridArea();
    return juce::Rectangle<int>(area.getX(), area.getY() + row * (kRowHeight + kRowGap),
                                area.getWidth(), kRowHeight);
}

juce::Rectangle<int> StepSequencerComponent::muteBounds(int row) const
{
    return rowBounds(row).removeFromLeft(kMuteWidth).withSizeKeepingCentre(12, 12);
}

juce::Rectangle<int> StepSequencerComponent::nameBounds(int row) const
{
    auto r = rowBounds(row);
    r.removeFromLeft(kMuteWidth);
    return r.removeFromLeft(kNameWidth);
}

juce::Rectangle<int> StepSequencerComponent::stepsBounds(int row) const
{
    auto r = rowBounds(row);
    r.removeFromLeft(kMuteWidth + kNameWidth + kColGap);
    return r;
}

juce::Rectangle<int> StepSequencerComponent::stepCellBounds(int row, int step) const
{
    const auto area = stepsBounds(row);
    const int n = patternSteps();
    const double w = static_cast<double>(area.getWidth()) / static_cast<double>(juce::jmax(1, n));
    const int x1 = area.getX() + static_cast<int>(std::round(step * w));
    const int x2 = area.getX() + static_cast<int>(std::round((step + 1) * w));
    return juce::Rectangle<int>(x1, area.getY(), x2 - x1, area.getHeight()).reduced(2, 7);
}

int StepSequencerComponent::currentPlayStep() const
{
    if (! transport.isPlaying())
        return -1;
    const auto pb = patternBeats();
    if (pb <= 0.0)
        return -1;
    auto local = std::fmod(transport.getPlayheadBeat(), pb);
    if (local < 0.0)
        local += pb;
    return juce::jlimit(0, patternSteps() - 1, static_cast<int>(local / stepLengthBeats()));
}

//==============================================================================
void StepSequencerComponent::paint(juce::Graphics& g)
{
    rebuildChannels();

    g.fillAll(theme::core::deepSpace);

    // Header.
    auto header = getLocalBounds().removeFromTop(kHeaderHeight).reduced(kLeftPad, 0);
    g.setColour(theme::text::primary.withAlpha(0.95f));
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText("STEP SEQUENCER", header.removeFromLeft(180), juce::Justification::centredLeft);
    g.setColour(theme::text::muted.withAlpha(0.8f));
    g.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    g.drawText("right-click a row for the piano roll", header, juce::Justification::centredRight);

    // "16 / 32" step-count toggle.
    {
        const auto [b16, b32] = stepCountToggleBounds();
        const bool is32 = bars >= 2;
        const auto drawSeg = [&](juce::Rectangle<int> b, const juce::String& label, bool active)
        {
            g.setColour(active ? theme::cool::cyan.withAlpha(0.85f) : theme::core::voidBlack.withAlpha(0.6f));
            g.fillRoundedRectangle(b.toFloat(), 5.0f);
            g.setColour(active ? theme::core::deepSpace : theme::text::muted);
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            g.drawText(label, b, juce::Justification::centred);
        };
        drawSeg(b16, "16", ! is32);
        drawSeg(b32, "32", is32);
    }

    if (channelTrackIndices.empty())
    {
        auto box = getLocalBounds().withTrimmedTop(kHeaderHeight).reduced(40, 24);
        g.setColour((dragOver ? theme::cool::cyan : theme::line::normal).withAlpha(dragOver ? 0.9f : 0.5f));
        const float dashes[] = { 6.0f, 5.0f };
        juce::Path border;
        border.addRoundedRectangle(box.toFloat(), 10.0f);
        juce::Path dashed;
        juce::PathStrokeType(1.4f).createDashedStroke(dashed, border, dashes, 2);
        g.fillPath(dashed);
        g.setColour(theme::text::secondary);
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawText("Drag drum samples here to add channels",
                   box.withTrimmedBottom(box.getHeight() / 2), juce::Justification::centred);
        g.setColour(theme::text::muted);
        g.setFont(juce::FontOptions(14.0f, juce::Font::plain));
        g.drawText("each sample becomes a channel — click steps to build the beat",
                   box.withTrimmedTop(box.getHeight() / 2), juce::Justification::centred);
        return;
    }

    const auto& tracks = project.getTracks();
    const int playStep = currentPlayStep();
    const int n = patternSteps();
    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    for (int row = 0; row < static_cast<int>(channelTrackIndices.size()); ++row)
    {
        const int ti = channelTrackIndices[static_cast<std::size_t>(row)];
        if (ti < 0 || ti >= static_cast<int>(tracks.size()))
            continue;
        const auto& track = tracks[static_cast<std::size_t>(ti)];

        // Name block.
        const bool selected = (ti == selectedTrackIndex);
        auto nameBox = nameBounds(row).toFloat();
        g.setColour(theme::surface::elevated.withAlpha(selected ? 1.0f : (hoveredRow == row ? 0.95f : 0.75f)));
        g.fillRoundedRectangle(nameBox, 6.0f);
        if (selected)
        {
            g.setColour(theme::accent::brandCyan.withAlpha(0.9f));
            g.drawRoundedRectangle(nameBox.reduced(0.5f), 6.0f, 1.4f);
        }
        g.setColour(track.colour.withAlpha(0.9f));
        g.fillRoundedRectangle(nameBox.removeFromLeft(4.0f), 2.0f);   // colour tab

        // Signal indicator (right of the name): a little meter bar that jumps when the playhead
        // hits an active step on this channel, then decays — so you see which channel is hitting.
        double activity = 0.0;
        if (! track.muted)
            if (const auto it = lastTriggerMsByTrack.find(ti); it != lastTriggerMsByTrack.end())
                activity = juce::jlimit(0.0, 1.0, 1.0 - (nowMs - it->second) / 220.0);
        auto meter = nameBox.removeFromRight(12).reduced(3.0f, 6.0f);
        g.setColour(theme::core::voidBlack.withAlpha(0.7f));
        g.fillRoundedRectangle(meter, 2.0f);
        if (activity > 0.01)
        {
            auto fill = meter.withTop(meter.getBottom() - meter.getHeight() * static_cast<float>(activity));
            g.setColour(theme::accent::successGreen.withAlpha(0.5f + 0.5f * static_cast<float>(activity)));
            g.fillRoundedRectangle(fill, 2.0f);
        }

        g.setColour(theme::text::primary.withAlpha(track.muted ? 0.4f : 0.92f));
        g.setFont(juce::FontOptions(14.5f, juce::Font::bold));
        g.drawText(track.name, nameBox.reduced(10, 0), juce::Justification::centredLeft, true);

        // Mute LED (green = playing, dim = muted).
        const auto led = muteBounds(row).toFloat();
        g.setColour((track.muted ? theme::text::disabled : theme::accent::successGreen).withAlpha(track.muted ? 0.5f : 0.95f));
        g.fillEllipse(led);

        // Melodic channel (notes drawn in the piano roll): show a mini note preview spanning the
        // row, like the playlist clip — instead of empty step cells that can't represent it.
        if (rowIsMelodic(ti))
        {
            const auto area = stepsBounds(row).toFloat().reduced(1.0f, 4.0f);
            g.setColour(theme::surface::primary.withAlpha(0.55f));
            g.fillRoundedRectangle(area, 4.0f);

            const auto* clip = rowClip(ti);
            if (clip != nullptr && ! clip->midiNotes.empty())
            {
                int minPitch = clip->midiNotes.front().pitch, maxPitch = minPitch;
                double maxBeat = 0.0;
                for (const auto& note : clip->midiNotes)
                {
                    minPitch = juce::jmin(minPitch, note.pitch);
                    maxPitch = juce::jmax(maxPitch, note.pitch);
                    maxBeat  = juce::jmax(maxBeat, note.startBeat + note.lengthInBeats);
                }
                const auto span = juce::jmax(1.0, juce::jmax(maxBeat, clip->lengthInBeats));
                const auto pitchRange = juce::jmax(1, maxPitch - minPitch + 1);
                const auto laneH = area.getHeight() / static_cast<float>(pitchRange);
                g.setColour(track.colour.withAlpha(track.muted ? 0.4f : 0.92f));
                for (const auto& note : clip->midiNotes)
                {
                    const auto x  = area.getX() + static_cast<float>(note.startBeat / span) * area.getWidth();
                    const auto w  = juce::jmax(2.0f, static_cast<float>(note.lengthInBeats / span) * area.getWidth());
                    const auto y  = area.getY() + static_cast<float>(maxPitch - note.pitch) * laneH;
                    g.fillRoundedRectangle(x, y + 0.5f, w, juce::jmax(1.5f, laneH - 1.0f), 1.5f);
                }
            }

            if (playStep >= 0)
            {
                const auto px = area.getX() + (static_cast<float>(playStep) / static_cast<float>(juce::jmax(1, n))) * area.getWidth();
                g.setColour(theme::cool::cyan.withAlpha(0.5f));
                g.drawLine(px, area.getY(), px, area.getBottom(), 1.4f);
            }
            continue;   // melodic rows don't show clickable steps
        }

        // Steps.
        for (int step = 0; step < n; ++step)
        {
            const auto cell = stepCellBounds(row, step).toFloat();
            const bool active = stepActive(ti, step);
            const bool beatGroup = (step / 4) % 2 == 0;   // alternate shading every 4 steps
            const bool onBeat = (step % 4) == 0;

            if (active)
            {
                g.setColour(theme::warm::red.withAlpha(track.muted ? 0.45f : 0.95f));
                g.fillRoundedRectangle(cell, 4.0f);
                g.setColour(theme::warm::salmon.withAlpha(track.muted ? 0.3f : 0.55f));
                g.drawRoundedRectangle(cell.reduced(0.5f), 4.0f, 1.0f);
            }
            else
            {
                g.setColour((beatGroup ? theme::surface::panel : theme::surface::primary)
                                .withAlpha(hoveredRow == row && hoveredStep == step ? 0.95f : 0.7f));
                g.fillRoundedRectangle(cell, 4.0f);
                if (onBeat)
                {
                    g.setColour(theme::line::normal.withAlpha(0.6f));
                    g.drawRoundedRectangle(cell.reduced(0.5f), 4.0f, 1.0f);
                }
            }

            if (step == playStep)
            {
                g.setColour(theme::cool::cyan.withAlpha(0.55f));
                g.drawRoundedRectangle(cell.reduced(0.5f), 4.0f, 1.6f);
            }
        }
    }
}

void StepSequencerComponent::resized()
{
    repaint();
}

void StepSequencerComponent::mouseDown(const juce::MouseEvent& event)
{
    rebuildChannels();
    const auto pos = event.getPosition();
    const bool popup = event.mods.isPopupMenu();

    // "16 / 32" step-count toggle in the header (32 steps = 2 bars of 16).
    if (const auto [b16, b32] = stepCountToggleBounds(); b16.contains(pos) || b32.contains(pos))
    {
        const int newBars = b32.contains(pos) ? 2 : 1;
        if (newBars != bars)
        {
            bars = newBars;
            rebuildChannels();           // resizes the pattern clips to the new length
            if (onPatternEdited)
                onPatternEdited();
            repaint();
        }
        return;
    }

    for (int row = 0; row < static_cast<int>(channelTrackIndices.size()); ++row)
    {
        if (! rowBounds(row).contains(pos))
            continue;

        const int ti = channelTrackIndices[static_cast<std::size_t>(row)];

        // Right-click anywhere on a row → open the piano roll for that channel's pattern clip.
        if (popup)
        {
            const int ci = findPatternClip(ti, true);
            if (ci >= 0 && onOpenPianoRoll)
                onOpenPianoRoll(ti, ci);
            return;
        }

        // Click the channel name → select this channel (host selects the track too, so
        // Enter in the browser replaces THIS channel's sample).
        if (nameBounds(row).contains(pos) && ! muteBounds(row).expanded(6).contains(pos))
        {
            selectedTrackIndex = ti;
            if (onChannelSelected)
                onChannelSelected(ti);
            // Clicking the channel/sample opens its manipulators (sampler panel).
            if (onOpenSampler)
                onOpenSampler(ti);
            repaint();
            return;
        }

        // Mute LED toggles the track mute.
        if (muteBounds(row).expanded(6).contains(pos))
        {
            auto& tracks = project.getTracks();
            if (ti >= 0 && ti < static_cast<int>(tracks.size()))
            {
                tracks[static_cast<std::size_t>(ti)].muted = ! tracks[static_cast<std::size_t>(ti)].muted;
                repaint();
            }
            return;
        }

        // Step area.
        const auto steps = stepsBounds(row);
        if (steps.contains(pos))
        {
            // Melodic channels show a note preview, not clickable steps — left-click just
            // selects it (edit via right-click → piano roll).
            if (rowIsMelodic(ti))
            {
                selectedTrackIndex = ti;
                if (onChannelSelected)
                    onChannelSelected(ti);
                repaint();
                return;
            }

            const int n = patternSteps();
            const double w = static_cast<double>(steps.getWidth()) / static_cast<double>(juce::jmax(1, n));
            const int step = juce::jlimit(0, n - 1, static_cast<int>((pos.x - steps.getX()) / w));

            // Begin a paint stroke: the first step's new state becomes the value dragged across the row.
            const bool nowOn = ! stepIsOn(ti, step);
            stepPaintActive = true;
            stepPaintValue = nowOn;
            stepPaintTrack = ti;
            stepPaintLastStep = step;
            setStep(ti, step, nowOn);
        }
        return;
    }
}

void StepSequencerComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (! stepPaintActive || stepPaintTrack < 0)
        return;

    // Paint across the row the stroke started on: map the pointer's x to a step and apply the
    // stroke value to every new step it crosses.
    for (int row = 0; row < static_cast<int>(channelTrackIndices.size()); ++row)
    {
        if (channelTrackIndices[static_cast<std::size_t>(row)] != stepPaintTrack)
            continue;

        const auto steps = stepsBounds(row);
        const int n = patternSteps();
        const double w = static_cast<double>(steps.getWidth()) / static_cast<double>(juce::jmax(1, n));
        const int step = juce::jlimit(0, n - 1, static_cast<int>((event.getPosition().x - steps.getX()) / w));
        if (step != stepPaintLastStep)
        {
            stepPaintLastStep = step;
            setStep(stepPaintTrack, step, stepPaintValue);
        }
        break;
    }
}

void StepSequencerComponent::mouseUp(const juce::MouseEvent&)
{
    stepPaintActive = false;
    stepPaintTrack = -1;
    stepPaintLastStep = -1;
}

void StepSequencerComponent::mouseMove(const juce::MouseEvent& event)
{
    rebuildChannels();
    const auto pos = event.getPosition();
    int newRow = -1, newStep = -1;
    for (int row = 0; row < static_cast<int>(channelTrackIndices.size()); ++row)
    {
        if (! rowBounds(row).contains(pos))
            continue;
        newRow = row;
        const auto steps = stepsBounds(row);
        if (steps.contains(pos))
        {
            const int n = patternSteps();
            const double w = static_cast<double>(steps.getWidth()) / static_cast<double>(juce::jmax(1, n));
            newStep = juce::jlimit(0, n - 1, static_cast<int>((pos.x - steps.getX()) / w));
        }
        break;
    }
    if (newRow != hoveredRow || newStep != hoveredStep)
    {
        hoveredRow = newRow;
        hoveredStep = newStep;
        repaint();
    }
}

void StepSequencerComponent::mouseExit(const juce::MouseEvent&)
{
    if (hoveredRow != -1 || hoveredStep != -1)
    {
        hoveredRow = -1;
        hoveredStep = -1;
        repaint();
    }
}

void StepSequencerComponent::visibilityChanged()
{
    if (isVisible())
        startTimerHz(30);   // animate the play-step cursor
    else
        stopTimer();
}

void StepSequencerComponent::timerCallback()
{
    if (! isVisible())
        return;

    if (transport.isPlaying())
    {
        const int s = currentPlayStep();
        const int previousStep = lastPlayStep;
        if (s != lastPlayStep)
        {
            lastPlayStep = s;
            rebuildChannels();
            const auto now = juce::Time::getMillisecondCounterHiRes();
            for (const int ti : channelTrackIndices)
                if (stepActive(ti, s))
                    lastTriggerMsByTrack[ti] = now;

            // The cursor only changes two cells per drum row. Melodic rows draw one
            // continuous cursor line, so their step strip is still a small repaint.
            for (int row = 0; row < static_cast<int>(channelTrackIndices.size()); ++row)
            {
                const auto trackIndex = channelTrackIndices[static_cast<std::size_t>(row)];
                if (rowIsMelodic(trackIndex))
                    repaint(stepsBounds(row));
                else
                {
                    if (previousStep >= 0)
                        repaint(stepCellBounds(row, previousStep).expanded(2, 2));
                    repaint(stepCellBounds(row, s).expanded(2, 2));
                }
            }
        }

        // Only active signal indicators need a decay redraw between step changes.
        const auto now = juce::Time::getMillisecondCounterHiRes();
        for (int row = 0; row < static_cast<int>(channelTrackIndices.size()); ++row)
        {
            const auto trackIndex = channelTrackIndices[static_cast<std::size_t>(row)];
            if (const auto it = lastTriggerMsByTrack.find(trackIndex);
                it != lastTriggerMsByTrack.end() && now - it->second < 220.0)
                repaint(nameBounds(row));
        }
    }
    else
    {
        if (lastPlayStep >= 0)
            repaint(gridArea());   // clear the final cursor frame once after Stop
        lastPlayStep = -1;
    }
}

//==============================================================================
namespace
{
juce::File fileFromDragPayload(const juce::var& payload)
{
    if (auto* obj = payload.getDynamicObject())
    {
        const auto type = obj->getProperty("type").toString();
        if (type == "browser-item" || obj->hasProperty("path"))
            return juce::File(obj->getProperty("path").toString());
    }
    return {};
}
}  // namespace

bool StepSequencerComponent::isInterestedInDragSource(const SourceDetails& dragSourceDetails)
{
    return fileFromDragPayload(dragSourceDetails.description).existsAsFile();
}

void StepSequencerComponent::itemDragEnter(const SourceDetails&)
{
    dragOver = true;
    repaint();
}

void StepSequencerComponent::itemDragMove(const SourceDetails&)
{
}

void StepSequencerComponent::itemDragExit(const SourceDetails&)
{
    dragOver = false;
    repaint();
}

void StepSequencerComponent::itemDropped(const SourceDetails& dragSourceDetails)
{
    dragOver = false;
    const auto file = fileFromDragPayload(dragSourceDetails.description);
    if (! file.existsAsFile())
    {
        repaint();
        return;
    }

    rebuildChannels();
    const auto pos = dragSourceDetails.localPosition;

    // Dropped on an existing channel row → load the sample into that track. Otherwise create a
    // brand-new channel (sampler track) from the sample.
    for (int row = 0; row < static_cast<int>(channelTrackIndices.size()); ++row)
    {
        if (rowBounds(row).contains(pos))
        {
            if (onAssignSampleToChannel)
                onAssignSampleToChannel(channelTrackIndices[static_cast<std::size_t>(row)], file);
            repaint();
            return;
        }
    }

    if (onAddChannelFromSample)
        onAddChannelFromSample(file);
    repaint();
}
}  // namespace orion
