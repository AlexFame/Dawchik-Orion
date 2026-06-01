#include "AudioInputRecorder.h"

namespace orion
{
AudioInputRecorder::AudioInputRecorder()
{
    writerThread.startThread();
}

AudioInputRecorder::~AudioInputRecorder()
{
    stop();
    writerThread.stopThread(1000);
}

bool AudioInputRecorder::start(const juce::File& file, double requestedSampleRate, int requestedChannels, juce::String& error)
{
    stop();

    const auto sampleRate = requestedSampleRate > 0.0 ? requestedSampleRate : currentSampleRate.load();
    const auto channels = juce::jlimit(1, 2, requestedChannels > 0 ? requestedChannels : currentInputChannels.load());
    if (sampleRate <= 0.0)
    {
        error = "invalid input sample rate";
        return false;
    }

    file.getParentDirectory().createDirectory();
    auto stream = file.createOutputStream();
    if (stream == nullptr)
    {
        error = "could not create recording file";
        return false;
    }

    juce::WavAudioFormat wavFormat;
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(
        wavFormat.createWriterFor(stream.get(),
                                  sampleRate,
                                  static_cast<unsigned int>(channels),
                                  24,
                                  {},
                                  0));
    if (writer == nullptr)
    {
        error = "could not create WAV writer";
        return false;
    }

    stream.release();
    samplesWritten.store(0);

    auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), writerThread, 32768);
    {
        const juce::ScopedLock lock(writerLock);
        threadedWriter = std::move(threaded);
        activeWriter = threadedWriter.get();
    }

    recording.store(true);
    return true;
}

juce::int64 AudioInputRecorder::stop()
{
    recording.store(false);
    {
        const juce::ScopedLock lock(writerLock);
        activeWriter = nullptr;
        threadedWriter.reset();
    }
    return samplesWritten.load();
}

bool AudioInputRecorder::isRecording() const noexcept
{
    return recording.load();
}

juce::int64 AudioInputRecorder::getSamplesWritten() const noexcept
{
    return samplesWritten.load();
}

double AudioInputRecorder::getCurrentSampleRate() const noexcept
{
    return currentSampleRate.load();
}

int AudioInputRecorder::getCurrentInputChannels() const noexcept
{
    return currentInputChannels.load();
}

void AudioInputRecorder::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    currentSampleRate.store(device->getCurrentSampleRate());
    currentInputChannels.store(juce::jmax(1, juce::jmin(2, device->getActiveInputChannels().countNumberOfSetBits())));
}

void AudioInputRecorder::audioDeviceStopped()
{
    currentInputChannels.store(0);
}

void AudioInputRecorder::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                          int numInputChannels,
                                                          float* const* outputChannelData,
                                                          int numOutputChannels,
                                                          int numSamples,
                                                          const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(outputChannelData, numOutputChannels, context);
    if (! recording.load() || inputChannelData == nullptr || numInputChannels <= 0 || numSamples <= 0)
        return;

    const juce::ScopedLock lock(writerLock);
    if (activeWriter == nullptr)
        return;

    activeWriter->write(inputChannelData, numSamples);
    samplesWritten.fetch_add(numSamples);
}
}  // namespace orion
