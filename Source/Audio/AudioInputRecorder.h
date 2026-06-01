#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

namespace orion
{
class AudioInputRecorder final : public juce::AudioIODeviceCallback
{
public:
    AudioInputRecorder();
    ~AudioInputRecorder() override;

    bool start(const juce::File& file, double requestedSampleRate, int requestedChannels, juce::String& error);
    juce::int64 stop();

    bool isRecording() const noexcept;
    juce::int64 getSamplesWritten() const noexcept;
    double getCurrentSampleRate() const noexcept;
    int getCurrentInputChannels() const noexcept;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;

private:
    juce::TimeSliceThread writerThread { "Orion Audio Recorder" };
    juce::CriticalSection writerLock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    juce::AudioFormatWriter::ThreadedWriter* activeWriter { nullptr };
    std::atomic<bool> recording { false };
    std::atomic<juce::int64> samplesWritten { 0 };
    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<int> currentInputChannels { 0 };
};
}  // namespace orion
