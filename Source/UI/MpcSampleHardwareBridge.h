#pragma once

#include "MpcSampleMapping.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <optional>
#include <vector>

namespace orion
{
class MpcSampleHardwareBridge final
{
public:
    struct PadEvent
    {
        int padIndex { -1 };
        int velocity { 0 };
    };

    struct DeviceState
    {
        bool inputConnected { false };
        juce::String inputName;
        bool outputConnected { false };
        juce::String outputName;
    };

    DeviceState refreshDevices(const juce::Array<juce::MidiDeviceInfo>& inputDevices,
                               const juce::Array<juce::MidiDeviceInfo>& outputDevices);

    bool shouldHandleInput(const juce::MidiMessage& message,
                           const juce::String& sourceName,
                           bool mpcPanelVisible) const;

    std::optional<PadEvent> handleIncomingMessage(const juce::MidiMessage& message,
                                                  const juce::String& sourceName);

    void sendPadToHardware(int padIndex, int velocity);

    const DeviceState& getDeviceState() const noexcept { return deviceState; }
    juce::String getLastMidiDescription() const { return lastMidiDescription; }

private:
    static juce::String describeMessage(const juce::MidiMessage& message, const juce::String& sourceName);
    static bool isUsefulMidiForMpcPanel(const juce::MidiMessage& message) noexcept;

    MpcSampleMapping mapping;
    DeviceState deviceState;
    std::vector<juce::String> outputIds;
    std::vector<std::unique_ptr<juce::MidiOutput>> outputs;
    juce::String lastMidiDescription { "MPC MIDI: waiting for pads" };
};
} // namespace orion
