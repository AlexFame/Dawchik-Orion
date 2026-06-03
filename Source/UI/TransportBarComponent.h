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
    bool playing { false };
    bool recording { false };
    bool loop { false };
    bool metronome { false };
    bool countIn { false };
    bool scanVisible { false };
    double scanProgress { 0.0 };
    juce::String scanName;
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

    void setState(const TransportBarState& newState);
    juce::Rectangle<int> getTempoEditorBounds() const noexcept;
    juce::Rectangle<int> getKeyBounds() const noexcept;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;

private:
    void buttonClicked(juce::Button* button) override;
    void syncButtons();
    void drawButtonFrame(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour colour, bool active) const;

    TransportBarState state;
    bool syncingButtons { false };

    juce::TextButton playButton { ">" };
    juce::TextButton stopButton { "■" };
    juce::TextButton recordButton { "●" };
    juce::TextButton metronomeButton { "△" };
    juce::TextButton loopButton { "↻" };
    juce::Rectangle<int> tempoCardBounds;
    juce::Rectangle<int> keyCardBounds;
    juce::Rectangle<int> positionCardBounds;
};
}  // namespace orion
