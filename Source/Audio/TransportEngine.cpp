#include "TransportEngine.h"

namespace orion
{
TransportEngine::TransportEngine(ProjectState& projectState)
    : project(projectState)
{
}

TransportEngine::~TransportEngine() = default;

bool TransportEngine::isPlaying() const noexcept
{
    return playing;
}

bool TransportEngine::isPaused() const noexcept
{
    return paused;
}

bool TransportEngine::isCountInActive() const noexcept
{
    return countInActive;
}

bool TransportEngine::isLoopEnabled() const noexcept
{
    return loopEnabled;
}

bool TransportEngine::isRecordArmed() const noexcept
{
    return recordArmed;
}

double TransportEngine::getPlayheadBeat() const noexcept
{
    // During recording, ignore the loop range too — the user is laying down a take
    // that should run forward freely until they hit Stop.
    const auto loopActive = loopEnabled && project.hasLoopRange() && ! recordArmed;
    const auto loopStart = project.getLoopStartBeat();
    const auto loopEnd = project.getLoopEndBeat();
    const auto loopSpan = juce::jmax(1.0, loopEnd - loopStart);

    if (playing)
    {
        const auto nowMs = juce::Time::currentTimeMillis();
        const auto elapsedMs = static_cast<double>(nowMs - lastUpdateMs);
        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        auto currentBeat = playheadBeat + (elapsedMs / 1000.0) * beatsPerSecond;
        if (loopActive)
        {
            while (currentBeat >= loopEnd)
                currentBeat = loopStart + std::fmod(currentBeat - loopStart, loopSpan);

            return currentBeat;
        }

        // While recording, do NOT wrap at the content end. The "content end" equals the
        // recording clip's end (since it's often the only clip), so wrapping would dump
        // the playhead back to 0 and the next note-on would land on top of an earlier one.
        // Recording must run forward freely until the user presses Stop.
        if (! recordArmed)
        {
            const auto repeatEndBeat = project.getPlaybackEndInBeats();
            if (repeatEndBeat > 0.0)
            {
                while (currentBeat >= repeatEndBeat)
                    currentBeat = std::fmod(currentBeat, repeatEndBeat);
            }
        }

        return juce::jlimit(0.0, project.getProjectLengthInBeats(), currentBeat);
    }
    return playheadBeat;
}

double TransportEngine::getClickBeat() const noexcept
{
    if (countInActive)
    {
        const auto nowMs = juce::Time::currentTimeMillis();
        const auto elapsedMs = static_cast<double>(nowMs - lastUpdateMs);
        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        return countInBeat + (elapsedMs / 1000.0) * beatsPerSecond;
    }

    return getPlayheadBeat();
}

void TransportEngine::play(bool withCountIn)
{
    if (playing || countInActive)
        return;

    const auto projectEndBeat = project.getPlaybackEndInBeats();
    if (! (loopEnabled && project.hasLoopRange()) && playheadBeat >= juce::jmax(0.0, projectEndBeat - 0.0001))
        rewindToStart();

    paused = false;
    lastUpdateMs = juce::Time::currentTimeMillis();

    if (withCountIn)
    {
        countInActive = true;
        countInBeat = 0.0;
    }
    else
    {
        playing = true;
    }

    startTimerHz(120);
}

void TransportEngine::pause()
{
    if (! playing && ! countInActive)
        return;

    if (playing)
        playheadBeat = getPlayheadBeat();
    else if (countInActive)
        countInBeat = getClickBeat();

    playing = false;
    countInActive = false;
    paused = true;
    stopTimer();
}

void TransportEngine::stop()
{
    pause();
    rewindToStart();
    paused = false;
}

void TransportEngine::rewindToStart()
{
    playheadBeat = 0.0;
    countInBeat = 0.0;
}

void TransportEngine::setPlayheadBeat(double beat) noexcept
{
    playheadBeat = juce::jlimit(0.0, project.getProjectLengthInBeats(), beat);
    if (countInActive)
        countInBeat = 0.0;

    lastUpdateMs = juce::Time::currentTimeMillis();
}

void TransportEngine::togglePlayback(bool withCountIn)
{
    if (playing || countInActive)
        pause();
    else
        play(withCountIn);
}

void TransportEngine::setLoopEnabled(bool shouldLoop) noexcept
{
    loopEnabled = shouldLoop;
}

void TransportEngine::setRecordArmed(bool shouldArm) noexcept
{
    recordArmed = shouldArm;
}

void TransportEngine::timerCallback()
{
    const auto nowMs = juce::Time::currentTimeMillis();
    const auto elapsedMs = static_cast<double>(nowMs - lastUpdateMs);
    lastUpdateMs = nowMs;

    const auto beatsPerSecond = project.getTempoBpm() / 60.0;
    const auto beatAdvance = (elapsedMs / 1000.0) * beatsPerSecond;
    // Loop range is ignored while record-armed — see comment in getPlayheadBeat().
    const auto loopActive = loopEnabled && project.hasLoopRange() && ! recordArmed;
    const auto loopStart = project.getLoopStartBeat();
    const auto loopEnd = project.getLoopEndBeat();
    const auto loopSpan = juce::jmax(1.0, loopEnd - loopStart);

    if (countInActive)
    {
        countInBeat += beatAdvance;

        if (countInBeat >= static_cast<double>(project.getNumerator()))
        {
            countInActive = false;
            countInBeat = 0.0;
            playing = true;
            paused = false;
        }

        return;
    }

    if (! playing)
        return;

    playheadBeat += beatAdvance;

    if (loopActive)
    {
        while (playheadBeat >= loopEnd)
            playheadBeat = loopStart + std::fmod(playheadBeat - loopStart, loopSpan);
    }
    else if (! recordArmed)
    {
        // Same rule as getPlayheadBeat(): no content-end wraparound while record-armed,
        // otherwise an empty project's recording would overwrite itself in a 1-clip loop.
        const auto repeatEndBeat = project.getPlaybackEndInBeats();
        if (repeatEndBeat > 0.0)
        {
            // There is content — wrap the playhead at the end of the last clip.
            if (playheadBeat >= repeatEndBeat)
                playheadBeat = std::fmod(playheadBeat, repeatEndBeat);
        }
        else
        {
            // Empty project (no clips): let the playhead run forward freely up to the
            // project length instead of snapping back to 0 every tick (which froze it).
            playheadBeat = juce::jmin(playheadBeat, project.getProjectLengthInBeats());
        }
        paused = false;
    }
}
}  // namespace orion
