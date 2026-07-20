#include "MpcSampleMapping.h"

namespace orion
{
void MpcSampleMapping::handleMessage(const juce::MidiMessage& message) const
{
    if (onRawMessage)
        onRawMessage(message);

    if (message.isNoteOnOrOff())
    {
        const auto pad = padIndexForNote(message.getNoteNumber());
        if (pad >= 0 && onPadNote)
            onPadNote(pad, message.isNoteOn() ? juce::jlimit(1, 127, static_cast<int>(message.getVelocity())) : 0);
        return;
    }

    if (message.isController() && onControlChange)
        onControlChange(message.getControllerNumber(), static_cast<float>(message.getControllerValue()) / 127.0f);
}

int MpcSampleMapping::padIndexForNote(int midiNote) noexcept
{
    // Hardware presets differ: MPC/MPD units commonly start pads at C1, C#1, C2 or C3.
    if (midiNote >= 36 && midiNote <= 51) return midiNote - 36;
    if (midiNote >= 37 && midiNote <= 52) return midiNote - 37;
    if (midiNote >= 48 && midiNote <= 63) return midiNote - 48;
    if (midiNote >= 60 && midiNote <= 75) return midiNote - 60;
    return midiNote >= 0 && midiNote <= 127 ? midiNote % 16 : -1;
}

bool MpcSampleMapping::looksLikeMpcSample(const juce::String& deviceName)
{
    const auto name = deviceName.toLowerCase();
    return name.contains("mpc sample")
        || name.contains("mpc-sample")
        || name.contains("mpc_sample")
        || name.contains("akai")
        || name.contains("mpc")
        || name.contains("mpd")
        || name.contains("mpk")
        || name.contains("force");
}
} // namespace orion
