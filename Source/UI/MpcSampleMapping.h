#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <functional>

namespace orion
{
// Device-agnostic first layer for MPC Sample MIDI. The hardware can be connected through
// a TRS MIDI interface or any class-compliant MIDI path; the UI does not depend on the
// transport being USB.
class MpcSampleMapping final
{
public:
    std::function<void(int, int)> onPadNote;
    std::function<void(int, float)> onControlChange;
    std::function<void(const juce::MidiMessage&)> onRawMessage;

    void handleMessage(const juce::MidiMessage& message) const;
    static int padIndexForNote(int midiNote) noexcept;
    static bool looksLikeMpcSample(const juce::String& deviceName);
};
} // namespace orion
