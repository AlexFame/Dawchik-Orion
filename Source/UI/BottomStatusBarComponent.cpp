#include "BottomStatusBarComponent.h"

#include "OrionTheme.h"

namespace orion
{
namespace
{
const auto background = juce::Colour(0xff2b2b2a);
const auto panelDark = juce::Colour(0xff191817);
const auto coral = theme::warm::red;
const auto peach = juce::Colour(0xffffb3a9);
const auto dim = juce::Colour(0xff6f6967);
constexpr int navOpticalCenterNudgeY = 3;

juce::String formatDb(double db)
{
    return db <= -59.0 ? "-inf" : juce::String(db, 1) + " dB";
}
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
    g.setColour(background);
    g.fillRect(0, getHeight() - 14, 18, 14);

    auto bounds = getLocalBounds();
    g.setColour(juce::Colours::black.withAlpha(0.32f));
    g.drawLine(0.0f, 0.0f, static_cast<float>(getWidth()), 0.0f, 1.0f);

    auto master = bounds.removeFromLeft(420).reduced(28, 0).withSizeKeepingCentre(364, 52);
    g.setColour(coral);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText("MASTER OUT", master.removeFromTop(18), juce::Justification::centredLeft);

    master.removeFromTop(8);
    auto meterRow = master.removeFromTop(22);
    auto meter = meterRow.removeFromLeft(156).withHeight(18).withY(meterRow.getCentreY() - 9);
    g.setColour(panelDark);
    g.fillRect(meter);
    auto fill = meter.toFloat();
    fill.setWidth(fill.getWidth() * juce::jlimit(0.0f, 1.0f, state.masterLevel));
    juce::ColourGradient meterGradient(juce::Colour(0xff39d36b), fill.getX(), fill.getCentreY(),
                                       orion::theme::warm::red, fill.getRight(), fill.getCentreY(), false);
    meterGradient.addColour(0.72, juce::Colour(0xffe7c93a));
    g.setGradientFill(meterGradient);
    g.fillRect(fill);
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.drawRect(meter);

    g.setColour(juce::Colours::white.withAlpha(0.90f));
    g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    g.drawText(formatDb(state.masterLevelDb), meterRow.removeFromLeft(108), juce::Justification::centredRight);

    const auto itemWidth = 118;
    auto center = getLocalBounds().withSizeKeepingCentre(itemWidth * 2 + 16, getHeight());
    drawNavItem(g, Item::mixer, "MIXER", center.removeFromLeft(itemWidth));
    drawNavItem(g, Item::clipEditor, "CLIP EDITOR", center.removeFromLeft(itemWidth));

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
    if (item == Item::none)
        return {};

    constexpr int itemWidth = 118;
    auto center = getLocalBounds().withSizeKeepingCentre(itemWidth * 2 + 16, getHeight());
    const auto index = static_cast<int>(item) - 1;
    return center.removeFromLeft((index + 1) * itemWidth).removeFromRight(itemWidth);
}

BottomStatusBarComponent::Item BottomStatusBarComponent::hitTestItem(juce::Point<int> point) const noexcept
{
    for (const auto item : { Item::mixer, Item::clipEditor })
        if (getItemBounds(item).contains(point))
            return item;

    return Item::none;
}

void BottomStatusBarComponent::drawNavItem(juce::Graphics& g, Item item, const juce::String& label, juce::Rectangle<int> bounds) const
{
    const auto active = (item == Item::mixer && state.mixerOpen)
                     || (item == Item::clipEditor && state.clipEditorOpen);
    const auto hovered = item == hoveredItem;
    const auto colour = active ? peach : (hovered ? juce::Colours::white.withAlpha(0.78f) : juce::Colours::white.withAlpha(0.66f));

    if (active || hovered)
    {
        g.setColour(juce::Colours::black.withAlpha(active ? 0.16f : 0.08f));
        g.fillRoundedRectangle(bounds.reduced(10, 10).toFloat(), 6.0f);
    }

    auto content = bounds.withSizeKeepingCentre(bounds.getWidth(), 46).translated(0, navOpticalCenterNudgeY);
    auto icon = content.removeFromTop(24).withSizeKeepingCentre(28, 22);
    drawItemIcon(g, item, icon.toFloat(), colour);

    content.removeFromTop(4);
    g.setColour(colour);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText(label, content.removeFromTop(18), juce::Justification::centred);
}

void BottomStatusBarComponent::drawItemIcon(juce::Graphics& g, Item item, juce::Rectangle<float> bounds, juce::Colour colour) const
{
    g.setColour(colour);

    if (item == Item::mixer)
    {
        for (int i = 0; i < 3; ++i)
        {
            const auto index = static_cast<float>(i);
            const auto x = bounds.getX() + 5.0f + index * 8.0f;
            g.drawLine(x, bounds.getY(), x, bounds.getBottom(), 1.5f);
            g.fillEllipse(x - 3.0f, bounds.getY() + 4.0f + index * 5.0f, 6.0f, 6.0f);
        }
    }
    else if (item == Item::clipEditor)
    {
        auto body = bounds.reduced(3.0f, 5.0f);
        g.drawRoundedRectangle(body, 2.0f, 1.6f);
        g.drawLine(body.getX() + 4.0f, body.getCentreY(), body.getRight() - 4.0f, body.getCentreY(), 1.4f);
        juce::Path wave;
        wave.startNewSubPath(body.getX() + 5.0f, body.getCentreY() + 5.0f);
        wave.lineTo(body.getX() + 9.0f, body.getCentreY() + 1.0f);
        wave.lineTo(body.getX() + 13.0f, body.getCentreY() + 7.0f);
        wave.lineTo(body.getX() + 17.0f, body.getCentreY() + 2.0f);
        wave.lineTo(body.getRight() - 5.0f, body.getCentreY() + 5.0f);
        g.strokePath(wave, juce::PathStrokeType(1.5f));
    }
}
}  // namespace orion
