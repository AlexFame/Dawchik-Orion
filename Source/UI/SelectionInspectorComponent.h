#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "OrionTheme.h"

namespace orion
{
struct SelectionInspectorModel
{
    juce::String title;
    juce::String subtitle;
    juce::String detail;
    juce::Colour accent { theme::warm::red };
    double gainDb { 0.0 };
    bool muted { false };
    bool solo { false };
    bool warpEnabled { false };
    bool showWarp { false };
    bool hasSelection { false };
};

class SelectionInspectorComponent final : public juce::Component
{
public:
    SelectionInspectorComponent();

    std::function<void(double)> onGainChanged;
    std::function<void(bool)> onMuteChanged;
    std::function<void(bool)> onSoloChanged;
    std::function<void(bool)> onWarpChanged;
    std::function<float()> onRequestLiveLevel;
    std::function<float()> onRequestLiveLevelDb;

    void setModel(const SelectionInspectorModel& newModel);
    const SelectionInspectorModel& getModel() const noexcept { return model; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    juce::Rectangle<int> getGainTrackBounds() const noexcept;
    void applyGainFromPoint(juce::Point<int> point);
    void syncButtonStates();

    SelectionInspectorModel model;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };
    juce::TextButton warpButton { "WARP" };
    bool updatingButtons { false };
    bool draggingGain { false };
};
}  // namespace orion
