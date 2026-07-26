#pragma once

#include "IndependentAudioInput.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <vector>

namespace orion
{
// VoiceChatEngine — LAN voice talkback that stays OUT of the main audio engine.
//
// Capture uses IndependentAudioInput (a dedicated input-only CoreAudio device, the pattern we
// already use for recording), and playback uses its own output-only AudioDeviceManager. Neither
// touches the main output callback, so a voice hiccup can't disturb the arrangement's audio. The
// two audio callbacks are real-time: they only push/pull through lock-free SPSC FIFOs — no locks,
// no allocation. A timer on the message thread drains the capture FIFO into network chunks; peer
// chunks are pushed into the playback FIFO from the message thread.
//
// It knows nothing about collab: onCaptured hands out mic chunks, pushRemoteVoice feeds in received
// ones. The host wires those to the transport.
class VoiceChatEngine : private juce::Timer
{
public:
    VoiceChatEngine();
    ~VoiceChatEngine() override;

    // Fired ~50x/s on the message thread with a chunk of mono int16 mic PCM to ship to peers.
    std::function<void(int sampleRate, const juce::MemoryBlock& pcm)> onCaptured;

    // Playback (hearing peers) runs for the whole session; capture (your mic) is toggled by the
    // mic button. Both are idempotent and independently startable.
    bool startPlayback(juce::String& error);
    void stopPlayback();
    bool startCapture(juce::String& error);
    void stopCapture();
    void stop() { stopCapture(); stopPlayback(); }
    bool isCapturing() const noexcept { return capturing; }
    bool isPlaying() const noexcept { return playing; }

    // Feed a peer's received voice chunk (mono int16 PCM at `sampleRate`) into playback.
    void pushRemoteVoice(int sampleRate, const juce::MemoryBlock& pcm);

private:
    // Real-time producers/consumers over lock-free FIFOs.
    void pushCaptured(const float* mono, int numSamples);           // audio thread (capture)
    void pullPlayback(float* const* out, int numOut, int numSamples); // audio thread (playback)
    void timerCallback() override;                                  // message thread: drain capture

    struct CaptureCallback;
    struct PlaybackCallback;
    friend struct CaptureCallback;
    friend struct PlaybackCallback;

    IndependentAudioInput micInput;
    std::unique_ptr<CaptureCallback> captureCb;

    juce::AudioDeviceManager playbackDevices;
    std::unique_ptr<PlaybackCallback> playbackCb;

    // Capture ring (audio-in producer -> timer consumer).
    juce::AbstractFifo captureFifo { 1 << 16 };
    std::vector<float> captureBuf;
    std::atomic<int> captureRate { 24000 };

    // Playback ring (message-thread producer -> audio-out consumer).
    juce::AbstractFifo playbackFifo { 1 << 16 };
    std::vector<float> playbackBuf;
    std::atomic<int> playbackRate { 48000 };

    bool capturing { false };
    bool playing { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceChatEngine)
};
} // namespace orion
