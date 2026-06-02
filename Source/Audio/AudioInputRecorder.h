#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>
#include <vector>

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
    // Reads and resets the loudest input magnitude per channel since the last call
    // (for live metering of the incoming signal while recording).
    void fetchAndResetInputPeak(float& outL, float& outR) noexcept;
    // Copies the live min/max waveform buckets captured so far (for drawing the clip
    // waveform while recording, before the file is readable). Returns the bucket count.
    int copyLiveWaveform(std::vector<float>& minsOut, std::vector<float>& maxsOut) const;
    int getLiveSamplesPerBucket() const noexcept { return liveSamplesPerBucket; }
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
    std::atomic<float> inputPeakL { 0.0f };
    std::atomic<float> inputPeakR { 0.0f };
    std::atomic<int> writerChannels { 0 };

    // Live waveform capture (audio thread writes, UI thread reads via atomic count).
    static constexpr int liveBucketCapacity = 300000;   // ~38 min at 512 samples/bucket@44.1k
    std::vector<float> liveMinBuckets;
    std::vector<float> liveMaxBuckets;
    std::atomic<int> liveBucketCount { 0 };
    int liveSamplesPerBucket { 512 };
    int liveBucketFill { 0 };
    float liveCurMin { 0.0f };
    float liveCurMax { 0.0f };
};
}  // namespace orion
