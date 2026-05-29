#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "../Core/ProjectState.h"

namespace orion
{
// A floating mixer overlay: one channel strip per track (volume fader + mute /
// solo) plus a master strip with its own fader and an output level meter.
//
// The panel mutates TrackState::volumeDb / muted / solo directly (the audio
// engine reads those live) and reports edits through onTrackChanged so the
// timeline can repaint. Master gain + metering are delegated to the host via
// the onSetMasterGainDb / onRequestMasterGainDb / onRequestMasterPeak hooks.
class MixerPanelComponent final : public juce::Component,
                                  private juce::Timer
{
public:
    explicit MixerPanelComponent(ProjectState& projectState);

    std::function<void()> onClose;
    std::function<void()> onTrackChanged;
    std::function<void(double)> onSetMasterGainDb;
    std::function<double()> onRequestMasterGainDb;
    std::function<float()>  onRequestMasterPeak;
    // Returns the current 0..1 output level for a track (for the per-strip meter).
    std::function<float(int)> onRequestTrackLevel;
    // Returns the current live signal level in dB (-100 ≈ silent → "-inf").
    std::function<float(int)> onRequestTrackLevelDb;

    void open();
    void closePanel();

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    void timerCallback() override;
    void rebuildStrips();
    void syncControlsFromProject();
    juce::Rectangle<int> getPanelBounds() const;
    juce::Rectangle<int> getCloseButtonBounds() const;

    struct ChannelStrip
    {
        std::unique_ptr<juce::Slider> volume;
        std::unique_ptr<juce::TextButton> mute;
        std::unique_ptr<juce::TextButton> solo;
        int trackIndex { -1 };
        juce::Rectangle<int> meterBounds;
        float meterDisplay { 0.0f };
        juce::Rectangle<int> levelTextBounds;
        float levelDbDisplay { -100.0f };
    };

    ProjectState& project;
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    juce::Slider masterVolume;
    int builtTrackCount { -1 };
    float masterMeterDisplay { 0.0f };
    juce::Rectangle<int> masterMeterBounds;
    float masterLevelDb { -100.0f };
    int   masterPeakHoldFrames { 0 };
    juce::Rectangle<int> masterLevelTextBounds;
};
}  // namespace orion
