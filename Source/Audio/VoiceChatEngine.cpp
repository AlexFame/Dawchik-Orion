#include "VoiceChatEngine.h"

namespace orion
{
// ---- Real-time audio callbacks. They only touch the SPSC FIFOs; never allocate or lock. ----

struct VoiceChatEngine::CaptureCallback final : juce::AudioIODeviceCallback
{
    explicit CaptureCallback(VoiceChatEngine& o) : owner(o) {}

    void audioDeviceIOCallbackWithContext(const float* const* input, int numInputChannels,
                                          float* const* output, int numOutputChannels,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOutputChannels; ++c)
            if (output[c] != nullptr)
                juce::FloatVectorOperations::clear(output[c], numSamples);

        if (numInputChannels > 0 && input[0] != nullptr)
            owner.pushCaptured(input[0], numSamples);   // mono: first channel
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        owner.captureRate = static_cast<int>(device->getCurrentSampleRate());
    }

    void audioDeviceStopped() override {}

    VoiceChatEngine& owner;
};

struct VoiceChatEngine::PlaybackCallback final : juce::AudioIODeviceCallback
{
    explicit PlaybackCallback(VoiceChatEngine& o) : owner(o) {}

    void audioDeviceIOCallbackWithContext(const float* const*, int,
                                          float* const* output, int numOutputChannels,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        owner.pullPlayback(output, numOutputChannels, numSamples);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        owner.playbackRate = static_cast<int>(device->getCurrentSampleRate());
    }

    void audioDeviceStopped() override {}

    VoiceChatEngine& owner;
};

VoiceChatEngine::VoiceChatEngine()
{
    captureBuf.resize(static_cast<size_t>(captureFifo.getTotalSize()));
    playbackBuf.resize(static_cast<size_t>(playbackFifo.getTotalSize()));
}

VoiceChatEngine::~VoiceChatEngine()
{
    stop();
}

bool VoiceChatEngine::startPlayback(juce::String& error)
{
    if (playing)
        return true;

    // An output-only device manager, separate from the main engine. Output-only never triggers the
    // input≠output combiner that froze CoreAudio, and macOS mixes multiple output clients on one device.
    playbackCb = std::make_unique<PlaybackCallback>(*this);
    const auto playErr = playbackDevices.initialise(0, 2, nullptr, true);
    if (playErr.isNotEmpty())
    {
        error = "Voice output: " + playErr;
        playbackCb.reset();
        return false;
    }
    playbackDevices.addAudioCallback(playbackCb.get());
    playing = true;
    return true;
}

void VoiceChatEngine::stopPlayback()
{
    if (! playing)
        return;
    if (playbackCb != nullptr)
    {
        playbackDevices.removeAudioCallback(playbackCb.get());
        playbackDevices.closeAudioDevice();
        playbackCb.reset();
    }
    playbackFifo.reset();
    playing = false;
}

bool VoiceChatEngine::startCapture(juce::String& error)
{
    if (capturing)
        return true;

    // A dedicated input-only device (the recording pattern) — never combined with an output.
    captureCb = std::make_unique<CaptureCallback>(*this);
    juce::String micErr;
    if (! micInput.start({}, 0.0, 0, captureCb.get(), micErr))
    {
        error = "Voice mic: " + micErr;
        captureCb.reset();
        return false;
    }
    capturing = true;
    startTimerHz(50);   // drain captured audio into ~20 ms network chunks
    return true;
}

void VoiceChatEngine::stopCapture()
{
    if (! capturing)
        return;
    stopTimer();
    micInput.stop();
    captureCb.reset();
    captureFifo.reset();
    capturing = false;
}

void VoiceChatEngine::pushCaptured(const float* mono, int numSamples)
{
    int start1, size1, start2, size2;
    captureFifo.prepareToWrite(numSamples, start1, size1, start2, size2);   // drops overflow
    if (size1 > 0) juce::FloatVectorOperations::copy(captureBuf.data() + start1, mono, size1);
    if (size2 > 0) juce::FloatVectorOperations::copy(captureBuf.data() + start2, mono + size1, size2);
    captureFifo.finishedWrite(size1 + size2);
}

void VoiceChatEngine::timerCallback()
{
    const int ready = captureFifo.getNumReady();
    if (ready <= 0 || ! onCaptured)
        return;

    std::vector<float> mono(static_cast<size_t>(ready));
    int start1, size1, start2, size2;
    captureFifo.prepareToRead(ready, start1, size1, start2, size2);
    if (size1 > 0) juce::FloatVectorOperations::copy(mono.data(), captureBuf.data() + start1, size1);
    if (size2 > 0) juce::FloatVectorOperations::copy(mono.data() + size1, captureBuf.data() + start2, size2);
    captureFifo.finishedRead(size1 + size2);

    // Float -> int16 PCM for the wire.
    juce::MemoryBlock pcm;
    pcm.setSize(static_cast<size_t>(ready) * sizeof(juce::int16));
    auto* dst = static_cast<juce::int16*>(pcm.getData());
    for (int i = 0; i < ready; ++i)
        dst[i] = static_cast<juce::int16>(juce::jlimit(-1.0f, 1.0f, mono[static_cast<size_t>(i)]) * 32767.0f);

    onCaptured(captureRate.load(), pcm);
}

void VoiceChatEngine::pushRemoteVoice(int sampleRate, const juce::MemoryBlock& pcm)
{
    const int numIn = static_cast<int>(pcm.getSize() / sizeof(juce::int16));
    if (numIn <= 0)
        return;

    const auto* src = static_cast<const juce::int16*>(pcm.getData());
    const int outRate = playbackRate.load();

    // int16 -> float, resampled (linear) from the sender's rate to our output rate.
    std::vector<float> resampled;
    if (sampleRate == outRate)
    {
        resampled.resize(static_cast<size_t>(numIn));
        for (int i = 0; i < numIn; ++i)
            resampled[static_cast<size_t>(i)] = src[i] / 32768.0f;
    }
    else
    {
        const double ratio = static_cast<double>(outRate) / juce::jmax(1, sampleRate);
        const int numOut = juce::jmax(1, static_cast<int>(numIn * ratio));
        resampled.resize(static_cast<size_t>(numOut));
        for (int i = 0; i < numOut; ++i)
        {
            const double srcPos = i / ratio;
            const int i0 = static_cast<int>(srcPos);
            const int i1 = juce::jmin(numIn - 1, i0 + 1);
            const float frac = static_cast<float>(srcPos - i0);
            resampled[static_cast<size_t>(i)] = juce::jmap(frac, src[i0] / 32768.0f, src[i1] / 32768.0f);
        }
    }

    const int n = static_cast<int>(resampled.size());
    int start1, size1, start2, size2;
    playbackFifo.prepareToWrite(n, start1, size1, start2, size2);   // drops overflow if we fall behind
    if (size1 > 0) juce::FloatVectorOperations::copy(playbackBuf.data() + start1, resampled.data(), size1);
    if (size2 > 0) juce::FloatVectorOperations::copy(playbackBuf.data() + start2, resampled.data() + size1, size2);
    playbackFifo.finishedWrite(size1 + size2);
}

void VoiceChatEngine::pullPlayback(float* const* out, int numOut, int numSamples)
{
    const int ready = juce::jmin(numSamples, playbackFifo.getNumReady());

    std::array<float, 4096> mono {};
    const int take = juce::jmin(ready, static_cast<int>(mono.size()));
    if (take > 0)
    {
        int start1, size1, start2, size2;
        playbackFifo.prepareToRead(take, start1, size1, start2, size2);
        if (size1 > 0) juce::FloatVectorOperations::copy(mono.data(), playbackBuf.data() + start1, size1);
        if (size2 > 0) juce::FloatVectorOperations::copy(mono.data() + size1, playbackBuf.data() + start2, size2);
        playbackFifo.finishedRead(size1 + size2);
    }

    for (int c = 0; c < numOut; ++c)
    {
        if (out[c] == nullptr)
            continue;
        for (int i = 0; i < numSamples; ++i)
            out[c][i] = i < take ? mono[static_cast<size_t>(i)] : 0.0f;
    }
}
} // namespace orion
