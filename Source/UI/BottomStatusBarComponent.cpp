#include "BottomStatusBarComponent.h"

#include "OrionTheme.h"

namespace orion
{
namespace
{
const auto background = orion::theme::core::deepSpace;
const auto peach      = orion::theme::text::secondary;
}  // namespace

BottomStatusBarComponent::BottomStatusBarComponent()
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void BottomStatusBarComponent::setState(const BottomStatusBarState& newState)
{
    state = newState;
    repaint();
}

void BottomStatusBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(background);

    g.setColour(juce::Colours::black.withAlpha(0.32f));
    g.drawLine(0.0f, 0.0f, static_cast<float>(getWidth()), 0.0f, 1.0f);

    auto right = getLocalBounds().removeFromRight(220).reduced(24, 0).withSizeKeepingCentre(172, 52);
    auto save = right.removeFromRight(160);
    g.setColour(peach);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    auto saveTitle = save.removeFromTop(22);
    g.drawText(state.projectSaved ? "PROJECT_SAVED" : "PROJECT_DIRTY", saveTitle, juce::Justification::centredRight);
    g.setColour(juce::Colours::white.withAlpha(0.66f));
    g.setFont(juce::FontOptions(10.0f, juce::Font::plain));
    g.drawText("SYNCED_12:44:09", save.removeFromTop(18), juce::Justification::centredRight);
}

void BottomStatusBarComponent::mouseMove(const juce::MouseEvent& event)
{
    const auto item = hitTestItem(event.getPosition());
    if (hoveredItem == item)
        return;

    hoveredItem = item;
    setMouseCursor(item == Item::none ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
    repaint();
}

void BottomStatusBarComponent::mouseExit(const juce::MouseEvent&)
{
    hoveredItem = Item::none;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

void BottomStatusBarComponent::mouseDown(const juce::MouseEvent& event)
{
    switch (hitTestItem(event.getPosition()))
    {
        case Item::mixer:
            if (onMixer)
                onMixer();
            break;
        case Item::clipEditor:
            if (onClipEditor)
                onClipEditor();
            break;
        case Item::none:
            break;
    }
}

juce::Rectangle<int> BottomStatusBarComponent::getItemBounds(Item item) const noexcept
{
    juce::ignoreUnused(item);
    return {};
}

BottomStatusBarComponent::Item BottomStatusBarComponent::hitTestItem(juce::Point<int> point) const noexcept
{
    for (const auto item : { Item::mixer, Item::clipEditor })
        if (getItemBounds(item).contains(point))
            return item;

    return Item::none;
}

}  // namespace orion
