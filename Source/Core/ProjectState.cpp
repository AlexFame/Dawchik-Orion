#include "ProjectState.h"

namespace orion
{
ProjectState::ProjectState()
{
    tracks = {
        {
            "Drums",
            false,
            juce::Colour(0xffd97a2b),
            false,
            false,
            false,
            0.0,
            {
                { "Main Loop", ClipType::audio, 0.0, 8.0, juce::Colour(0xffd97a2b), {}, {}, 0.0, false, false, 0.0, 0.0, 0, false },
                { "Fill", ClipType::audio, 12.0, 4.0, juce::Colour(0xffc76622), {}, {}, 0.0, false, false, 0.0, 0.0, 0, false },
            },
        },
        {
            "Bass",
            true,
            juce::Colour(0xffca5d54),
            false,
            false,
            false,
            0.0,
            {
                {
                    "Bassline",
                    ClipType::midi,
                    4.0,
                    8.0,
                    juce::Colour(0xffca5d54),
                    {
                        { 48, 0.0, 1.0, 110 },
                        { 51, 1.0, 1.0, 102 },
                        { 48, 2.0, 1.0, 108 },
                        { 55, 3.0, 1.0, 104 },
                        { 53, 4.0, 1.0, 106 },
                        { 51, 5.0, 1.0, 98 },
                        { 48, 6.0, 1.0, 110 },
                        { 43, 7.0, 1.0, 112 },
                    },
                    {},
                    0.0,
                    false,
                    false,
                    0.0,
                    0.0,
                    0,
                    false
                },
            },
        },
        {
            "Lead",
            true,
            juce::Colour(0xff5b84d6),
            false,
            false,
            false,
            0.0,
            {
                {
                    "Hook",
                    ClipType::midi,
                    8.0,
                    8.0,
                    juce::Colour(0xff5b84d6),
                    {
                        { 60, 0.0, 0.5, 100 },
                        { 63, 1.0, 1.0, 104 },
                        { 67, 2.0, 0.5, 106 },
                        { 70, 3.0, 1.0, 110 },
                        { 68, 4.0, 0.5, 102 },
                        { 67, 5.0, 1.0, 106 },
                        { 63, 6.0, 0.5, 101 },
                        { 60, 7.0, 0.75, 98 },
                    },
                    {},
                    0.0,
                    false,
                    false,
                    0.0,
                    0.0,
                    0,
                    false
                },
                {
                    "Counter",
                    ClipType::midi,
                    18.0,
                    6.0,
                    juce::Colour(0xff7ba3f0),
                    {
                        { 72, 0.0, 0.5, 96 },
                        { 70, 1.0, 0.5, 92 },
                        { 68, 2.0, 0.5, 94 },
                        { 67, 3.0, 0.5, 96 },
                        { 65, 4.0, 0.5, 90 },
                        { 63, 5.0, 0.5, 88 },
                    },
                    {},
                    0.0,
                    false,
                    false,
                    0.0,
                    0.0,
                    0,
                    false
                },
            },
        },
        {
            "Textures",
            false,
            juce::Colour(0xff7b6db5),
            false,
            false,
            false,
            0.0,
            {
                { "Atmos", ClipType::audio, 0.0, 16.0, juce::Colour(0xff7b6db5), {}, {}, 0.0, false, false, 0.0, 0.0, 0, false },
            },
        },
    };
}

double ProjectState::getTempoBpm() const noexcept
{
    return tempoBpm;
}

void ProjectState::setTempoBpm(double newTempoBpm) noexcept
{
    tempoBpm = juce::jlimit(60.0, 200.0, newTempoBpm);
}

int ProjectState::getKeyRoot() const noexcept
{
    return projectKeyRoot;
}

bool ProjectState::isKeyMinor() const noexcept
{
    return projectKeyIsMinor;
}

void ProjectState::setKey(int rootSemitones, bool minor) noexcept
{
    projectKeyRoot = ((rootSemitones % 12) + 12) % 12;
    projectKeyIsMinor = minor;
}

int ProjectState::getNumerator() const noexcept
{
    return timeSigNumerator;
}

int ProjectState::getDenominator() const noexcept
{
    return timeSigDenominator;
}

double ProjectState::getLoopLengthInBeats() const noexcept
{
    return loopLengthInBeats;
}

double ProjectState::getProjectLengthInBeats() const noexcept
{
    auto maxBeat = loopLengthInBeats;

    for (const auto& track : tracks)
    {
        for (const auto& clip : track.clips)
            maxBeat = juce::jmax(maxBeat, clip.startBeat + clip.lengthInBeats);
    }

    return maxBeat;
}

double ProjectState::getContentEndInBeats() const noexcept
{
    double maxBeat = 0.0;

    for (const auto& track : tracks)
    {
        for (const auto& clip : track.clips)
            maxBeat = juce::jmax(maxBeat, clip.startBeat + clip.lengthInBeats);
    }

    return maxBeat;
}

bool ProjectState::hasLoopRange() const noexcept
{
    return loopRangeActive;
}

double ProjectState::getLoopStartBeat() const noexcept
{
    return loopStartBeat;
}

double ProjectState::getLoopEndBeat() const noexcept
{
    return loopEndBeat;
}

void ProjectState::setLoopRange(double startBeat, double endBeat) noexcept
{
    const auto loopRangeLimit = juce::jmax(loopLengthInBeats, getProjectLengthInBeats(), endBeat);
    const auto clampedStart = juce::jlimit(0.0, loopRangeLimit - 1.0, startBeat);
    const auto clampedEnd = juce::jlimit(clampedStart + 1.0, loopRangeLimit, endBeat);
    loopRangeActive = true;
    loopStartBeat = clampedStart;
    loopEndBeat = clampedEnd;
}

void ProjectState::clearLoopRange() noexcept
{
    loopRangeActive = false;
    loopStartBeat = 0.0;
    loopEndBeat = 8.0;
}

const std::vector<TrackState>& ProjectState::getTracks() const noexcept
{
    return tracks;
}

std::vector<TrackState>& ProjectState::getTracks() noexcept
{
    return tracks;
}
}  // namespace orion
