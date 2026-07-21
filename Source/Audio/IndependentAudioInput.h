#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>

namespace orion
{
// A standalone CoreAudio INPUT stream, opened on its own device independently of the main
// AudioDeviceManager (which owns the output).
//
// Why this exists: on macOS the built-in mic and the built-in speakers are two different
// CoreAudio devices, as are almost any mic/interface + separate monitors. When input and output
// differ, JUCE's AudioDeviceManager builds an AudioIODeviceCombiner, whose blocking device start
// can deadlock CoreAudio on some hardware (the freeze we chased). A dedicated input device sits
// beside the output device instead of being combined with it — the way Ableton records a mic
// while playing through separate monitors. Clock drift between the two devices doesn't matter for
// recording: we capture the input's own stream to a file at its own rate.
class IndependentAudioInput
{
public:
    IndependentAudioInput();
    ~IndependentAudioInput();

    // Opens `inputDeviceName` (input-only) and starts feeding `callback`. Empty name = the system
    // default input. Pass the output device's rate/buffer so the take lines up; <= 0 uses the
    // input device's own defaults. Returns true on success, else false with `error` set. The
    // caller owns `callback` and must keep it alive until stop().
    bool start(const juce::String& inputDeviceName, double sampleRate, int bufferSize,
               juce::AudioIODeviceCallback* callback, juce::String& error);

    void stop();

    bool isRunning() const noexcept { return device != nullptr; }
    double getSampleRate() const noexcept;
    juce::String getDeviceName() const;

private:
    std::unique_ptr<juce::AudioIODeviceType> deviceType;   // CoreAudio, scanned lazily
    std::unique_ptr<juce::AudioIODevice> device;
    juce::AudioIODeviceCallback* activeCallback { nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IndependentAudioInput)
};
} // namespace orion
