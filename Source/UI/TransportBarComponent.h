#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace orion
{
struct TransportBarState
{
    double tempoBpm { 126.0 };
    juce::String keyText { "Cm" };
    juce::String positionText { "0:00.0" };
    juce::String projectName { "Untitled" };
    bool playing { false };
    bool recording { false };
    bool loop { false };
    bool metronome { false };
    bool countIn { false };
    bool scanVisible { false };
    double scanProgress { 0.0 };
    juce::String scanName;
    float engineLoad { 0.0f };
    double masterGainDb { 0.0 };
    float masterLevel { 0.0f };
    float masterLevelDb { -100.0f };
    bool mixerOpen { false };
    bool clipEditorOpen { false };
};

class TransportBarComponent final : public juce::Component,
                                    private juce::Button::Listener
{
public:
    TransportBarComponent();
    ~TransportBarComponent() override;

    static constexpr int preferredHeight = 84;

    std::function<void()> onPlay;
    std::function<void()> onStop;
    std::function<void(bool)> onRecordChanged;
    std::function<void()> onRecordOptions;
    std::function<void(bool)> onMetronomeChanged;
    std::function<void(bool)> onLoopChanged;
    std::function<void()> onTempoEdit;
    std::function<void()> onKeySelect;
    std::function<void()> onMixer;
    std::function<void()> onClipEditor;

    void setState(const TransportBarState& newState);
    juce::Rectangle<int> getTempoEditorBounds() const noexcept;
    juce::Rectangle<int> getKeyBounds() const noexcept;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;

private:
    enum class UtilityItem
    {
        none,
        mixer,
        clipEditor
    };

    void buttonClicked(juce::Button* button) override;
    void syncButtons();
    void drawButtonFrame(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour colour, bool active) const;
    juce::Rectangle<int> getUtilityItemBounds(UtilityItem item) const noexcept;
    UtilityItem hitTestUtilityItem(juce::Point<int> point) const noexcept;
    void drawUtilityItem(juce::Graphics& g, UtilityItem item, const juce::String& label, juce::Rectangle<int> bounds) const;
    void drawUtilityIcon(juce::Graphics& g, UtilityItem item, juce::Rectangle<float> bounds, juce::Colour colour) const;
    void drawMasterMeter(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawBrandCluster(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawCpuMeter(juce::Graphics& g, juce::Rectangle<int> bounds) const;

    TransportBarState state;
    bool syncingButtons { false };
    UtilityItem hoveredUtilityItem { UtilityItem::none };

    juce::TextButton playButton { ">" };
    juce::TextButton stopButton { "■" };
    juce::TextButton recordButton { "●" };
    juce::TextButton metronomeButton { "△" };
    juce::TextButton loopButton { "↻" };
    juce::Rectangle<int> tempoCardBounds;
    juce::Rectangle<int> keyCardBounds;
    juce::Rectangle<int> positionCardBounds;
    juce::Rectangle<int> brandClusterBounds;
    juce::Rectangle<int> utilityClusterBounds;
    juce::Rectangle<int> monitorClusterBounds;
    juce::Rectangle<int> masterMeterBounds;
    juce::Rectangle<int> cpuMeterBounds;
};
}  // namespace orion
