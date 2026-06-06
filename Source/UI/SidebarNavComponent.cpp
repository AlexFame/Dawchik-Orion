#include "SidebarNavComponent.h"

namespace
{
const auto railBackground = juce::Colour(0xff141414);
const auto railStroke = juce::Colours::white.withAlpha(0.08f);
const auto activeColour = juce::Colour(0xffff543d);
const auto cyanColour = juce::Colour(0xff18e5ef);
const auto textColour = juce::Colours::white.withAlpha(0.72f);
const auto mutedText = juce::Colours::white.withAlpha(0.52f);
constexpr int topPadding = 22;
constexpr int itemHeight = 72;
constexpr int itemGap = 10;
}  // namespace

namespace orion
{
SidebarNavComponent::SidebarNavComponent()
{
    setWantsKeyboardFocus(false);
    rebuildNavEntries();
}

void SidebarNavComponent::rebuildNavEntries()
{
    // SAMPLES/HISTORY/CLOUD hidden for now — SAMPLES duplicated FILES (no dedicated sample
    // library yet); the others are placeholders. Easy to re-add when they do something real.
    navEntries = {
        { SidebarNavItem::addFolder, "ADD", {} },
        { SidebarNavItem::files, "FILES", {} },
        { SidebarNavItem::vst, "VST", {} }
    };

    for (const auto& folder : customFolders)
    {
        if (! folder.isDirectory())
            continue;

        auto label = folder.getFileName().toUpperCase();
        if (label.isEmpty())
            label = "FOLDER";
        navEntries.push_back({ SidebarNavItem::customFolder, label.substring(0, 8), folder });
    }
}

void SidebarNavComponent::setActiveItem(SidebarNavItem item)
{
    if (activeItem == item)
        return;

    activeItem = item;
    if (item != SidebarNavItem::customFolder)
        activeFolderPath.clear();
    repaint();
}

void SidebarNavComponent::setActiveFolder(const juce::File& folder)
{
    activeItem = SidebarNavItem::customFolder;
    activeFolderPath = folder.getFullPathName();
    repaint();
}

void SidebarNavComponent::setCustomFolders(const std::vector<juce::File>& folders)
{
    customFolders = folders;
    rebuildNavEntries();
    repaint();
}

void SidebarNavComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    g.setColour(railBackground);
    g.fillRect(bounds);

    g.setColour(railStroke);
    g.drawLine(static_cast<float>(bounds.getRight() - 1), 0.0f,
               static_cast<float>(bounds.getRight() - 1), static_cast<float>(bounds.getBottom()), 1.0f);

    auto projectBounds = getItemBounds(SidebarNavItem::project);
    g.setColour(juce::Colours::black.withAlpha(0.22f));
    g.fillRoundedRectangle(projectBounds.withSizeKeepingCentre(42, 42).toFloat(), 6.0f);
    drawIcon(g, SidebarNavItem::project, projectBounds.withSizeKeepingCentre(34, 34).toFloat(), cyanColour);
    g.setColour(cyanColour);
    g.setFont(juce::FontOptions(7.0f, juce::Font::bold));
    g.drawText("ORION", projectBounds.withY(projectBounds.getBottom() - 12).withHeight(10),
               juce::Justification::centred, true);

    for (int i = 0; i < static_cast<int>(navEntries.size()); ++i)
    {
        const auto& entry = navEntries[static_cast<std::size_t>(i)];
        const auto itemBounds = getEntryBounds(i);
        const auto active = entry.item == SidebarNavItem::customFolder
            ? activeItem == SidebarNavItem::customFolder && entry.folder.getFullPathName() == activeFolderPath
            : entry.item == activeItem;
        const auto hovered = hoveredEntryIndex == i;

        if (active)
        {
            g.setColour(activeColour);
            g.fillRoundedRectangle(itemBounds.toFloat().reduced(4.0f, 3.0f), 5.0f);
        }
        else if (hovered)
        {
            g.setColour(juce::Colours::white.withAlpha(0.055f));
            g.fillRoundedRectangle(itemBounds.toFloat().reduced(5.0f, 3.0f), 5.0f);
        }

        const auto colour = active ? juce::Colours::black.withAlpha(0.86f)
                                   : (hovered ? textColour : mutedText);
        drawIcon(g, entry.item, itemBounds.withSizeKeepingCentre(28, 28).toFloat().translated(0.0f, -8.0f), colour);

        g.setColour(colour);
        g.setFont(juce::FontOptions(8.2f, juce::Font::bold));
        g.drawText(entry.label, itemBounds.withY(itemBounds.getBottom() - 22).withHeight(14),
                   juce::Justification::centred, true);
    }

    const auto addBounds = getItemBounds(SidebarNavItem::add).withSizeKeepingCentre(44, 44);
    const auto addHovered = hoveredItem.has_value() && *hoveredItem == SidebarNavItem::add;
    g.setColour(juce::Colour(0xff242424));
    g.fillRoundedRectangle(addBounds.toFloat(), 10.0f);
    g.setColour((addHovered ? activeColour : activeColour.withAlpha(0.52f)));
    g.drawRoundedRectangle(addBounds.toFloat().reduced(0.5f), 10.0f, 1.4f);
    drawIcon(g, SidebarNavItem::add, addBounds.reduced(12).toFloat(), juce::Colours::white.withAlpha(addHovered ? 0.96f : 0.78f));
}

void SidebarNavComponent::mouseMove(const juce::MouseEvent& event)
{
    const auto oldHoveredItem = hoveredItem;
    const auto oldHoveredEntryIndex = hoveredEntryIndex;

    hoveredItem = std::nullopt;
    hoveredEntryIndex = -1;

    if (getItemBounds(SidebarNavItem::project).contains(event.getPosition()))
        hoveredItem = SidebarNavItem::project;
    else if (getItemBounds(SidebarNavItem::add).contains(event.getPosition()))
        hoveredItem = SidebarNavItem::add;
    else
    {
        for (int i = 0; i < static_cast<int>(navEntries.size()); ++i)
        {
            if (getEntryBounds(i).contains(event.getPosition()))
            {
                hoveredEntryIndex = i;
                break;
            }
        }
    }

    if (oldHoveredItem == hoveredItem && oldHoveredEntryIndex == hoveredEntryIndex)
        return;

    repaint();
}

void SidebarNavComponent::mouseExit(const juce::MouseEvent&)
{
    hoveredItem.reset();
    hoveredEntryIndex = -1;
    repaint();
}

void SidebarNavComponent::mouseDown(const juce::MouseEvent& event)
{
    for (int i = 0; i < static_cast<int>(navEntries.size()); ++i)
    {
        if (! getEntryBounds(i).contains(event.getPosition()))
            continue;

        const auto& entry = navEntries[static_cast<std::size_t>(i)];
        if (entry.item == SidebarNavItem::customFolder)
        {
            setActiveFolder(entry.folder);
            if (onFolderSelected)
                onFolderSelected(entry.folder);
            return;
        }

        if (entry.item != SidebarNavItem::addFolder)
            setActiveItem(entry.item);

        if (onItemSelected)
            onItemSelected(entry.item);
        return;
    }

    if (const auto item = hitTestNavItem(event.getPosition()))
    {
        if (*item != SidebarNavItem::add && *item != SidebarNavItem::project)
            setActiveItem(*item);

        if (onItemSelected)
            onItemSelected(*item);
    }
}

juce::Rectangle<int> SidebarNavComponent::getItemBounds(SidebarNavItem item) const noexcept
{
    const auto w = getWidth();
    if (item == SidebarNavItem::project)
        return { 0, topPadding, w, 62 };
    if (item == SidebarNavItem::add)
        return { 0, juce::jmax(topPadding + 62, getHeight() - 68), w, 56 };

    auto y = topPadding + 106;
    for (const auto& entry : navEntries)
    {
        if (entry.item == item)
            return { 0, y, w, itemHeight };
        y += itemHeight + itemGap;
    }

    return {};
}

juce::Rectangle<int> SidebarNavComponent::getEntryBounds(int index) const noexcept
{
    return { 0, topPadding + 106 + index * (itemHeight + itemGap), getWidth(), itemHeight };
}

std::optional<SidebarNavItem> SidebarNavComponent::hitTestNavItem(juce::Point<int> position) const noexcept
{
    if (getItemBounds(SidebarNavItem::project).contains(position))
        return SidebarNavItem::project;
    if (getItemBounds(SidebarNavItem::add).contains(position))
        return SidebarNavItem::add;

    return std::nullopt;
}

void SidebarNavComponent::drawIcon(juce::Graphics& g, SidebarNavItem item, juce::Rectangle<float> bounds, juce::Colour colour) const
{
    g.setColour(colour);

    if (item == SidebarNavItem::project)
    {
        g.drawRoundedRectangle(bounds.reduced(3.0f), 5.0f, 1.4f);
        g.fillEllipse(bounds.getCentreX() - 4.0f, bounds.getCentreY() - 4.0f, 8.0f, 8.0f);
        return;
    }

    if (item == SidebarNavItem::files || item == SidebarNavItem::customFolder)
    {
        auto folder = bounds.reduced(2.0f, 5.0f);
        juce::Path p;
        p.startNewSubPath(folder.getX(), folder.getY() + 7.0f);
        p.lineTo(folder.getX() + folder.getWidth() * 0.34f, folder.getY() + 7.0f);
        p.lineTo(folder.getX() + folder.getWidth() * 0.42f, folder.getY() + 2.0f);
        p.lineTo(folder.getRight(), folder.getY() + 2.0f);
        p.lineTo(folder.getRight(), folder.getBottom());
        p.lineTo(folder.getX(), folder.getBottom());
        p.closeSubPath();
        g.strokePath(p, juce::PathStrokeType(1.8f));
        return;
    }

    if (item == SidebarNavItem::vst)
    {
        g.drawRoundedRectangle(bounds.reduced(2.0f, 5.0f), 3.0f, 1.7f);
        for (int i = 0; i < 4; ++i)
            g.drawLine(bounds.getX() + 6.0f + static_cast<float>(i) * 4.0f, bounds.getY() + 9.0f,
                       bounds.getX() + 6.0f + static_cast<float>(i) * 4.0f, bounds.getBottom() - 9.0f, 1.4f);
        return;
    }

    if (item == SidebarNavItem::samples)
    {
        const auto cx = bounds.getCentreX();
        const auto cy = bounds.getCentreY();
        for (int i = -2; i <= 2; ++i)
        {
            const auto h = 7.0f + static_cast<float>(2 - std::abs(i)) * 5.0f;
            g.drawLine(cx + static_cast<float>(i) * 5.0f, cy - h * 0.5f,
                       cx + static_cast<float>(i) * 5.0f, cy + h * 0.5f, 2.0f);
        }
        return;
    }

    if (item == SidebarNavItem::history)
    {
        g.drawEllipse(bounds.reduced(4.0f), 1.8f);
        g.drawLine(bounds.getCentreX(), bounds.getCentreY(), bounds.getCentreX(), bounds.getY() + 7.0f, 1.8f);
        g.drawLine(bounds.getCentreX(), bounds.getCentreY(), bounds.getX() + 8.0f, bounds.getCentreY(), 1.8f);
        return;
    }

    if (item == SidebarNavItem::cloud)
    {
        juce::Path cloud;
        cloud.startNewSubPath(bounds.getX() + 7.0f, bounds.getBottom() - 8.0f);
        cloud.cubicTo(bounds.getX() + 2.0f, bounds.getBottom() - 8.0f, bounds.getX() + 2.0f, bounds.getCentreY(),
                      bounds.getX() + 9.0f, bounds.getCentreY());
        cloud.cubicTo(bounds.getX() + 10.0f, bounds.getY() + 5.0f, bounds.getCentreX() + 5.0f, bounds.getY() + 4.0f,
                      bounds.getCentreX() + 7.0f, bounds.getCentreY() - 2.0f);
        cloud.cubicTo(bounds.getRight() - 3.0f, bounds.getCentreY() - 1.0f, bounds.getRight() - 3.0f, bounds.getBottom() - 8.0f,
                      bounds.getRight() - 9.0f, bounds.getBottom() - 8.0f);
        cloud.closeSubPath();
        g.strokePath(cloud, juce::PathStrokeType(1.8f));
        return;
    }

    if (item == SidebarNavItem::add || item == SidebarNavItem::addFolder)
    {
        g.drawLine(bounds.getCentreX(), bounds.getY(), bounds.getCentreX(), bounds.getBottom(), 2.0f);
        g.drawLine(bounds.getX(), bounds.getCentreY(), bounds.getRight(), bounds.getCentreY(), 2.0f);
    }
}
}  // namespace orion
