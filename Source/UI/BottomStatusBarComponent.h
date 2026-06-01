#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace orion
{
struct BottomStatusBarState
{
    double masterGainDb { 0.0 };
    float masterLevel { 0.0f };
    float engineLoad { 0.32f };
    bool projectSaved { true };
    bool mixerOpen { false };
    bool clipEditorOpen { false };
};

class BottomStatusBarComponent final : public juce::Component
{
public:
    BottomStatusBarComponent();

    static constexpr int preferredHeight = 76;

    std::function<void()> onMixer;
    std::function<void()> onMaster;
    std::function<void()> onFxRack;
    std::function<void()> onRouting;
    std::function<void()> onClipEditor;

    void setState(const BottomStatusBarState& newState);

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    enum class Item
    {
        none,
        mixer,
        master,
        fxRack,
        routing,
        clipEditor
    };

    juce::Rectangle<int> getItemBounds(Item item) const noexcept;
    Item hitTestItem(juce::Point<int> point) const noexcept;
    void drawNavItem(juce::Graphics& g, Item item, const juce::String& label, juce::Rectangle<int> bounds) const;
    void drawItemIcon(juce::Graphics& g, Item item, juce::Rectangle<float> bounds, juce::Colour colour) const;

    BottomStatusBarState state;
    Item hoveredItem { Item::none };
};
}  // namespace orion
