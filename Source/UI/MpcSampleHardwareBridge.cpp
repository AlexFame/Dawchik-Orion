#include "MpcSampleHardwareBridge.h"

#include <algorithm>
#include <array>

namespace orion
{
namespace
{
constexpr std::array<int, 2> padOutputChannels { 1, 10 };

juce::String joinDeviceNames(const juce::Array<juce::MidiDeviceInfo>& devices,
                             const std::vector<juce::String>& ids)
{
    juce::StringArray names;
    for (const auto& id : ids)
        for (const auto& device : devices)
            if (device.identifier == id)
                names.addIfNotAlreadyThere(device.name);
    return names.joinIntoString(", ");
}
} // namespace

MpcSampleHardwareBridge::DeviceState MpcSampleHardwareBridge::refreshDevices(
    const juce::Array<juce::MidiDeviceInfo>& inputDevices,
    const juce::Array<juce::MidiDeviceInfo>& outputDevices)
{
    DeviceState nextState;
    for (const auto& device : inputDevices)
        if (MpcSampleMapping::looksLikeMpcSample(device.name))
        {
            nextState.inputConnected = true;
            nextState.inputName = device.name;
            break;
        }

    std::vector<juce::String> wantedOutputIds;
    for (const auto& device : outputDevices)
        if (MpcSampleMapping::looksLikeMpcSample(device.name))
            wantedOutputIds.push_back(device.identifier);

    // A lot of small MIDI interfaces expose themselves as generic "USB MIDI Device".
    // When the dedicated MPC name is not visible, fan out from the MPC shell to available
    // hardware outputs so the physical unit still has a chance to receive the pad note.
    if (wantedOutputIds.empty() && outputDevices.size() > 0)
        for (const auto& device : outputDevices)
            wantedOutputIds.push_back(device.identifier);

    if (wantedOutputIds != outputIds)
    {
        outputs.clear();
        outputIds = wantedOutputIds;
        for (const auto& device : outputDevices)
            if (std::find(outputIds.begin(), outputIds.end(), device.identifier) != outputIds.end())
                if (auto output = juce::MidiOutput::openDevice(device.identifier))
                {
                    output->startBackgroundThread();
                    outputs.push_back(std::move(output));
                }
    }

    nextState.outputConnected = ! outputs.empty();
    nextState.outputName = joinDeviceNames(outputDevices, outputIds);
    deviceState = nextState;
    return deviceState;
}

bool MpcSampleHardwareBridge::isUsefulMidiForMpcPanel(const juce::MidiMessage& message) noexcept
{
    return message.isNoteOnOrOff()
        || message.isController()
        || message.isPitchWheel()
        || message.isAftertouch()
        || message.isChannelPressure()
        || message.isProgramChange();
}

bool MpcSampleHardwareBridge::shouldHandleInput(const juce::MidiMessage& message,
                                                const juce::String& sourceName,
                                                bool mpcPanelVisible) const
{
    if (! isUsefulMidiForMpcPanel(message))
        return false;

    return MpcSampleMapping::looksLikeMpcSample(sourceName)
        || (deviceState.inputConnected && sourceName == deviceState.inputName)
        || mpcPanelVisible;
}

std::optional<MpcSampleHardwareBridge::PadEvent> MpcSampleHardwareBridge::handleIncomingMessage(
    const juce::MidiMessage& message,
    const juce::String& sourceName)
{
    lastMidiDescription = describeMessage(message, sourceName);

    if (! message.isNoteOnOrOff())
        return std::nullopt;

    const auto pad = MpcSampleMapping::padIndexForNote(message.getNoteNumber());
    if (pad < 0)
        return std::nullopt;

    return PadEvent { pad, message.isNoteOn() ? juce::jlimit(1, 127, static_cast<int>(message.getVelocity())) : 0 };
}

void MpcSampleHardwareBridge::sendPadToHardware(int padIndex, int velocity)
{
    if (outputs.empty())
        return;

    const auto note = juce::jlimit(0, 127, 36 + juce::jlimit(0, 15, padIndex));
    for (auto& output : outputs)
    {
        if (output == nullptr)
            continue;

        for (const auto channel : padOutputChannels)
        {
            const auto message = velocity > 0
                ? juce::MidiMessage::noteOn(channel, note, static_cast<juce::uint8>(juce::jlimit(1, 127, velocity)))
                : juce::MidiMessage::noteOff(channel, note);
            output->sendMessageNow(message);
        }
    }
}

juce::String MpcSampleHardwareBridge::describeMessage(const juce::MidiMessage& message, const juce::String& sourceName)
{
    const auto source = sourceName.isNotEmpty() ? sourceName : juce::String("unknown");
    if (message.isNoteOnOrOff())
        return "MIDI IN " + source + " ch " + juce::String(message.getChannel())
            + " note " + juce::String(message.getNoteNumber())
            + " vel " + juce::String(message.isNoteOn() ? static_cast<int>(message.getVelocity()) : 0);

    if (message.isController())
        return "MIDI IN " + source + " ch " + juce::String(message.getChannel())
            + " cc " + juce::String(message.getControllerNumber())
            + " value " + juce::String(message.getControllerValue());

    return "MIDI IN " + source + " ch " + juce::String(message.getChannel());
}
} // namespace orion
