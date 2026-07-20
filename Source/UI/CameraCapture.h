#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace orion
{
// Native AVFoundation camera capture. Replaces juce::CameraDevice, whose macOS path
// mis-handles the camera's BGRA/YUV frames and produces a brown / washed-out image.
// We request 32BGRA explicitly (byte-identical to JUCE's little-endian ARGB pixel
// layout, so the copy is a straight memcpy) and deliver colour-correct frames on the
// message thread. NB: system camera effects (Studio Light / Portrait) are applied by
// macOS to every app and cannot be disabled from here — toggle them in Control Center.
class CameraCapture
{
public:
    CameraCapture();
    ~CameraCapture();

    // Called on the message thread with each new frame (opaque ARGB, colour-correct).
    std::function<void(const juce::Image&)> onFrame;

    // Requests camera authorization (async) and starts the default video camera.
    // onStarted(true) once frames are flowing, onStarted(false) if unavailable/denied.
    void start(std::function<void(bool)> onStarted = {});
    void stop();
    bool isRunning() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CameraCapture)
};
} // namespace orion
