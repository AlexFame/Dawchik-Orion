#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_video/juce_video.h>
#include "../ChordWheel/ChordWheelComponent.h"
#include "HandPoseTracker.h"

namespace orion
{
class CameraChordWheelComponent final : public juce::Component,
                                        private juce::CameraDevice::Listener,
                                        private juce::Timer
{
public:
    CameraChordWheelComponent();
    ~CameraChordWheelComponent() override;

    void setProjectKey (int rootPc, const std::array<int, 7>& pattern, const juce::String& keyName);
    void setChord (const chords::ChordSpec& chord, bool audition);
    bool startCamera();
    void stopCamera();

    std::function<void (const chords::ChordSpec&)> onChordPointed;
    std::function<void (const chords::ChordSpec&)> onChordSelected;
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void imageReceived (const juce::Image&) override;
    void timerCallback() override;
    void cameraOpened (juce::CameraDevice*, const juce::String& error);
    void updatePointingFromFrame();

    juce::TextButton closeButton { "Close" };
    juce::Label titleLabel;
    juce::Label statusLabel;
    std::unique_ptr<chordwheel::Component> wheel;
    std::unique_ptr<juce::CameraDevice> camera;
    juce::CriticalSection frameLock;
    juce::Image latestFrame;
    int keyRootPc { 0 };
    std::array<int, 7> keyPattern { { 0, 2, 3, 5, 7, 8, 10 } };
    juce::String keyName { "C Minor" };
    bool cameraOpening { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CameraChordWheelComponent)
};
}
