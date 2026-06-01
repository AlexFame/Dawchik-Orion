#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace orion
{
enum class SidebarNavItem
{
    project,
    files,
    vst,
    samples,
    history,
    cloud,
    add
};

class SidebarNavComponent final : public juce::Component
{
public:
    SidebarNavComponent();

    static constexpr int preferredWidth = 72;

    std::function<void(SidebarNavItem)> onItemSelected;

    void setActiveItem(SidebarNavItem item);

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    struct NavEntry
    {
        SidebarNavItem item;
        juce::String label;
    };

    juce::Rectangle<int> getItemBounds(SidebarNavItem item) const noexcept;
    std::optional<SidebarNavItem> hitTestNavItem(juce::Point<int> position) const noexcept;
    void drawIcon(juce::Graphics& g, SidebarNavItem item, juce::Rectangle<float> bounds, juce::Colour colour) const;

    SidebarNavItem activeItem { SidebarNavItem::files };
    std::optional<SidebarNavItem> hoveredItem;
    std::vector<NavEntry> navEntries;
};
}  // namespace orion
