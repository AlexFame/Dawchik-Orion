#include "ProjectState.h"

namespace orion
{
ProjectState::ProjectState()
{
    // New projects start empty — users add tracks and drop samples themselves.
    // (Previous demo content was useful for development but distracting on real use.)
    tracks.clear();
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

bool ProjectState::isScaleLockEnabled() const noexcept
{
    return scaleLockEnabled;
}

void ProjectState::setScaleLockEnabled(bool enabled) noexcept
{
    scaleLockEnabled = enabled;
}

bool ProjectState::isRecordWithMetronome() const noexcept
{
    return recordWithMetronome;
}

void ProjectState::setRecordWithMetronome(bool enabled) noexcept
{
    recordWithMetronome = enabled;
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
