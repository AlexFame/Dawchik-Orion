#include "IndependentAudioInput.h"

namespace orion
{
IndependentAudioInput::IndependentAudioInput() = default;

IndependentAudioInput::~IndependentAudioInput()
{
    stop();
}

bool IndependentAudioInput::start(const juce::String& inputDeviceName, double sampleRate, int bufferSize,
                                  juce::AudioIODeviceCallback* callback, juce::String& error)
{
    stop();

    if (callback == nullptr)
    {
        error = "no input callback";
        return false;
    }

    if (deviceType == nullptr)
    {
        deviceType.reset(juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
        if (deviceType == nullptr)
        {
            error = "CoreAudio device type unavailable";
            return false;
        }
    }
    deviceType->scanForDevices();

    // Resolve the input device name (empty = system default input).
    auto name = inputDeviceName;
    const auto inputNames = deviceType->getDeviceNames(true);
    if (inputNames.isEmpty())
    {
        error = "no audio input device found";
        return false;
    }
    if (name.isEmpty() || ! inputNames.contains(name))
    {
        const auto defaultIndex = juce::jlimit(0, inputNames.size() - 1, deviceType->getDefaultDeviceIndex(true));
        name = inputNames[defaultIndex];
    }

    // Input-only device: no output name.
    device.reset(deviceType->createDevice({}, name));
    if (device == nullptr)
    {
        error = "could not open input device: " + name;
        return false;
    }

    juce::BigInteger inputChannels;
    inputChannels.setRange(0, juce::jmax(1, juce::jmin(2, device->getInputChannelNames().size())), true);
    const juce::BigInteger noOutputs;   // input-only

    // Match the requested rate/buffer when the device supports them, so the recorded file lines up
    // with the project; otherwise fall back to the device's own defaults.
    const auto rates = device->getAvailableSampleRates();
    const double rate = (sampleRate > 0.0 && rates.contains(sampleRate))
                            ? sampleRate
                            : (rates.isEmpty() ? 44100.0 : rates[rates.size() - 1]);

    const auto sizes = device->getAvailableBufferSizes();
    const int buffer = (bufferSize > 0 && sizes.contains(bufferSize))
                           ? bufferSize
                           : device->getDefaultBufferSize();

    error = device->open(inputChannels, noOutputs, rate, buffer);
    if (error.isNotEmpty())
    {
        device.reset();
        return false;
    }

    device->start(callback);
    activeCallback = callback;
    return true;
}

void IndependentAudioInput::stop()
{
    if (device != nullptr)
    {
        device->stop();
        device->close();
    }
    device.reset();
    activeCallback = nullptr;
}

double IndependentAudioInput::getSampleRate() const noexcept
{
    return device != nullptr ? device->getCurrentSampleRate() : 0.0;
}

juce::String IndependentAudioInput::getDeviceName() const
{
    return device != nullptr ? device->getName() : juce::String();
}
} // namespace orion
