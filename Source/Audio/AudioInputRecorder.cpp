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
    writerChannels.store(channels);

    // Prepare live-waveform capture (~100 buckets/sec).
    liveSamplesPerBucket = juce::jmax(64, static_cast<int>(sampleRate / 100.0));
    if (static_cast<int>(liveMinBuckets.size()) != liveBucketCapacity)
    {
        liveMinBuckets.assign(static_cast<std::size_t>(liveBucketCapacity), 0.0f);
        liveMaxBuckets.assign(static_cast<std::size_t>(liveBucketCapacity), 0.0f);
    }
    liveBucketFill = 0;
    liveCurMin = 0.0f;
    liveCurMax = 0.0f;
    liveBucketCount.store(0, std::memory_order_release);

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

void AudioInputRecorder::fetchAndResetInputPeak(float& outL, float& outR) noexcept
{
    outL = inputPeakL.exchange(0.0f, std::memory_order_relaxed);
    outR = inputPeakR.exchange(0.0f, std::memory_order_relaxed);
}

int AudioInputRecorder::copyLiveWaveform(std::vector<float>& minsOut, std::vector<float>& maxsOut) const
{
    const auto count = liveBucketCount.load(std::memory_order_acquire);
    if (count <= 0 || static_cast<int>(liveMinBuckets.size()) < count)
    {
        minsOut.clear();
        maxsOut.clear();
        return 0;
    }
    minsOut.assign(liveMinBuckets.begin(), liveMinBuckets.begin() + count);
    maxsOut.assign(liveMaxBuckets.begin(), liveMaxBuckets.begin() + count);
    return count;
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
    juce::ignoreUnused(context);

    // MUST come first, before any early return. This is an input-only consumer, but the JUCE
    // contract is that every AudioIODeviceCallback fills the output block it is handed:
    // AudioDeviceManager sums each callback's output, and the scratch buffer it passes to the
    // secondary callbacks is NOT cleared between blocks. Leaving it untouched summed stale
    // memory into the output — which came out of the speakers as loud garbage the moment a
    // track was armed.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData != nullptr && outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    if (inputChannelData == nullptr || numInputChannels <= 0 || numSamples <= 0)
        return;

    // The active input channel is not necessarily at index 0, and some entries may be
    // null. Compact the valid (non-null) channel pointers so we never dereference null.
    const float* valid[2] = { nullptr, nullptr };
    int validCount = 0;
    for (int ch = 0; ch < numInputChannels && validCount < 2; ++ch)
        if (inputChannelData[ch] != nullptr)
            valid[validCount++] = inputChannelData[ch];
    if (validCount == 0)
        return;

    // Live input metering (per channel) runs whenever the callback is attached — even
    // when not recording — so an armed track shows the incoming mic/line level.
    const auto storePeak = [](std::atomic<float>& slot, float value)
    {
        float prev = slot.load(std::memory_order_relaxed);
        while (value > prev && ! slot.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {}
    };
    const auto channelMagnitude = [numSamples](const float* data)
    {
        auto range = juce::FloatVectorOperations::findMinAndMax(data, numSamples);
        return juce::jmax(std::abs(range.getStart()), std::abs(range.getEnd()));
    };
    storePeak(inputPeakL, channelMagnitude(valid[0]));
    storePeak(inputPeakR, channelMagnitude(valid[validCount > 1 ? 1 : 0]));

    if (! recording.load())
        return;

    const juce::ScopedLock lock(writerLock);
    if (activeWriter == nullptr)
        return;

    // Provide exactly the number of channels the writer expects, reusing the last valid
    // channel if the device has fewer (e.g. mono input into a stereo file).
    const auto wc = juce::jlimit(1, 2, writerChannels.load());
    const float* toWrite[2];
    for (int c = 0; c < wc; ++c)
        toWrite[c] = valid[juce::jmin(c, validCount - 1)];

    activeWriter->write(toWrite, numSamples);
    samplesWritten.fetch_add(numSamples);

    // Accumulate the live waveform from the first valid channel.
    const float* wave = valid[0];
    int idx = liveBucketCount.load(std::memory_order_relaxed);
    for (int s = 0; s < numSamples; ++s)
    {
        const auto v = wave[s];
        liveCurMin = juce::jmin(liveCurMin, v);
        liveCurMax = juce::jmax(liveCurMax, v);
        if (++liveBucketFill >= liveSamplesPerBucket)
        {
            if (idx < liveBucketCapacity)
            {
                liveMinBuckets[static_cast<std::size_t>(idx)] = liveCurMin;
                liveMaxBuckets[static_cast<std::size_t>(idx)] = liveCurMax;
                ++idx;
                liveBucketCount.store(idx, std::memory_order_release);
            }
            liveBucketFill = 0;
            liveCurMin = 0.0f;
            liveCurMax = 0.0f;
        }
    }
}
}  // namespace orion
