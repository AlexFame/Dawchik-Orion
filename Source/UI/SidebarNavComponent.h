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
    addFolder,
    files,
    vst,
    customFolder,
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
    std::function<void(const juce::File&)> onFolderSelected;

    void setActiveItem(SidebarNavItem item);
    void setActiveFolder(const juce::File& folder);
    void setCustomFolders(const std::vector<juce::File>& folders);

    // Bounds of a nav item in this component's coordinates (e.g. to anchor a popup menu).
    juce::Rectangle<int> getNavItemBounds(SidebarNavItem item) const noexcept { return getItemBounds(item); }

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    struct NavEntry
    {
        SidebarNavItem item;
        juce::String label;
        juce::File folder;
    };

    juce::Rectangle<int> getItemBounds(SidebarNavItem item) const noexcept;
    juce::Rectangle<int> getEntryBounds(int index) const noexcept;
    std::optional<SidebarNavItem> hitTestNavItem(juce::Point<int> position) const noexcept;
    void drawIcon(juce::Graphics& g, SidebarNavItem item, juce::Rectangle<float> bounds, juce::Colour colour) const;
    void rebuildNavEntries();

    SidebarNavItem activeItem { SidebarNavItem::files };
    juce::String activeFolderPath;
    std::optional<SidebarNavItem> hoveredItem;
    int hoveredEntryIndex { -1 };
    std::vector<juce::File> customFolders;
    std::vector<NavEntry> navEntries;
};
}  // namespace orion
