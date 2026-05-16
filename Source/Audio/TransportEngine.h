#pragma once

#include <juce_events/juce_events.h>

#include "../Core/ProjectState.h"

namespace orion
{
class TransportEngine final : private juce::Timer
{
public:
    explicit TransportEngine(ProjectState& projectState);
    ~TransportEngine() override;

    bool isPlaying() const noexcept;
    bool isPaused() const noexcept;
    bool isCountInActive() const noexcept;
    bool isLoopEnabled() const noexcept;
    bool isRecordArmed() const noexcept;
    double getPlayheadBeat() const noexcept;
    double getClickBeat() const noexcept;

    void play(bool withCountIn = false);
    void pause();
    void stop();
    void togglePlayback(bool withCountIn = false);
    void rewindToStart();
    void setPlayheadBeat(double beat) noexcept;
    void setLoopEnabled(bool shouldLoop) noexcept;
    void setRecordArmed(bool shouldArm) noexcept;

private:
    void timerCallback() override;

    ProjectState& project;
    double playheadBeat { 0.0 };
    double countInBeat { 0.0 };
    juce::int64 lastUpdateMs { 0 };
    bool playing { false };
    bool paused { false };
    bool countInActive { false };
    bool loopEnabled { false };
    bool recordArmed { false };
};
}  // namespace orion
