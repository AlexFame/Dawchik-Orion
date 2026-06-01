#include "BottomStatusBarComponent.h"

namespace orion
{
namespace
{
const auto background = juce::Colour(0xff2b2b2a);
const auto panelDark = juce::Colour(0xff191817);
const auto coral = juce::Colour(0xffff533f);
const auto peach = juce::Colour(0xffffb3a9);
const auto dim = juce::Colour(0xff6f6967);

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

    auto bounds = getLocalBounds();
    g.setColour(juce::Colours::black.withAlpha(0.32f));
    g.drawLine(0.0f, 0.0f, static_cast<float>(getWidth()), 0.0f, 1.0f);

    auto master = bounds.removeFromLeft(420).reduced(28, 12);
    g.setColour(coral);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText("MASTER OUT", master.removeFromTop(18), juce::Justification::centredLeft);

    auto meterRow = master.removeFromTop(34);
    auto meter = meterRow.removeFromLeft(156).withHeight(18).withY(meterRow.getY() + 8);
    g.setColour(panelDark);
    g.fillRect(meter);
    auto fill = meter.toFloat();
    fill.setWidth(fill.getWidth() * juce::jlimit(0.0f, 1.0f, state.masterLevel));
    juce::ColourGradient meterGradient(coral, fill.getX(), fill.getCentreY(),
                                       coral.darker(0.45f), fill.getRight(), fill.getCentreY(), false);
    g.setGradientFill(meterGradient);
    g.fillRect(fill);
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.drawRect(meter);

    g.setColour(juce::Colours::white.withAlpha(0.90f));
    g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    g.drawText(formatDb(state.masterGainDb), meterRow.removeFromLeft(96), juce::Justification::centredRight);

    const auto itemWidth = 118;
    auto center = getLocalBounds().withSizeKeepingCentre(itemWidth * 5 + 32, getHeight());
    drawNavItem(g, Item::mixer, "MIXER", center.removeFromLeft(itemWidth));
    drawNavItem(g, Item::master, "MASTER", center.removeFromLeft(itemWidth));
    drawNavItem(g, Item::fxRack, "FX RACK", center.removeFromLeft(itemWidth));
    drawNavItem(g, Item::routing, "ROUTING", center.removeFromLeft(itemWidth));
    drawNavItem(g, Item::clipEditor, "CLIP EDITOR", center.removeFromLeft(itemWidth));

    auto right = getLocalBounds().removeFromRight(360).reduced(24, 12);
    auto save = right.removeFromRight(160);
    g.setColour(peach);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText(state.projectSaved ? "PROJECT_SAVED" : "PROJECT_DIRTY", save.removeFromTop(22), juce::Justification::centredRight);
    g.setColour(dim.withAlpha(0.45f));
    g.setFont(juce::FontOptions(10.0f, juce::Font::plain));
    g.drawText("SYNCED_12:44:09", save, juce::Justification::centredRight);

    auto engine = right.removeFromRight(132);
    g.setColour(dim.withAlpha(0.35f));
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText("ENGINE_LOAD", engine.removeFromTop(20), juce::Justification::centred);
    auto loadRow = engine.removeFromTop(22);
    auto bars = loadRow.removeFromLeft(34).withSizeKeepingCentre(26, 14);
    const auto activeBars = juce::roundToInt(juce::jlimit(0.0f, 1.0f, state.engineLoad) * 4.0f);
    for (int i = 0; i < 4; ++i)
    {
        auto bar = bars.removeFromLeft(5);
        bars.removeFromLeft(2);
        g.setColour(i < activeBars ? coral : dim.withAlpha(0.25f));
        g.fillRect(bar.withTop(bar.getBottom() - 4 - i * 3));
    }
    g.setColour(juce::Colours::white.withAlpha(0.88f));
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(juce::String(juce::roundToInt(state.engineLoad * 100.0f)) + "%", loadRow, juce::Justification::centredLeft);
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
        case Item::master:
            if (onMaster)
                onMaster();
            break;
        case Item::fxRack:
            if (onFxRack)
                onFxRack();
            break;
        case Item::routing:
            if (onRouting)
                onRouting();
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
    auto center = getLocalBounds().withSizeKeepingCentre(itemWidth * 5 + 32, getHeight());
    const auto index = static_cast<int>(item) - 1;
    return center.removeFromLeft((index + 1) * itemWidth).removeFromRight(itemWidth);
}

BottomStatusBarComponent::Item BottomStatusBarComponent::hitTestItem(juce::Point<int> point) const noexcept
{
    for (const auto item : { Item::mixer, Item::master, Item::fxRack, Item::routing, Item::clipEditor })
        if (getItemBounds(item).contains(point))
            return item;

    return Item::none;
}

void BottomStatusBarComponent::drawNavItem(juce::Graphics& g, Item item, const juce::String& label, juce::Rectangle<int> bounds) const
{
    const auto active = (item == Item::mixer && state.mixerOpen)
                     || (item == Item::clipEditor && state.clipEditorOpen);
    const auto hovered = item == hoveredItem;
    const auto colour = active ? peach : (hovered ? juce::Colours::white.withAlpha(0.58f) : dim.withAlpha(0.58f));

    if (active || hovered)
    {
        g.setColour(juce::Colours::black.withAlpha(active ? 0.16f : 0.08f));
        g.fillRoundedRectangle(bounds.reduced(10, 8).toFloat(), 6.0f);
    }

    auto icon = bounds.removeFromTop(32).withSizeKeepingCentre(28, 22);
    drawItemIcon(g, item, icon.toFloat(), colour);

    g.setColour(colour);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText(label, bounds.removeFromTop(24), juce::Justification::centred);
}

void BottomStatusBarComponent::drawItemIcon(juce::Graphics& g, Item item, juce::Rectangle<float> bounds, juce::Colour colour) const
{
    g.setColour(colour);

    if (item == Item::mixer)
    {
        const auto w = bounds.getWidth() / 7.0f;
        for (int i = 0; i < 3; ++i)
        {
            const auto index = static_cast<float>(i);
            g.fillRect(bounds.getX() + index * w * 2.0f, bounds.getBottom() - (8.0f + index * 5.0f), w, 8.0f + index * 5.0f);
        }
    }
    else if (item == Item::master)
    {
        for (int i = 0; i < 3; ++i)
        {
            const auto index = static_cast<float>(i);
            const auto x = bounds.getX() + 5.0f + index * 8.0f;
            g.drawLine(x, bounds.getY(), x, bounds.getBottom(), 1.5f);
            g.fillEllipse(x - 3.0f, bounds.getY() + 4.0f + index * 5.0f, 6.0f, 6.0f);
        }
    }
    else if (item == Item::fxRack)
    {
        for (int i = 0; i < 4; ++i)
            g.fillEllipse(bounds.getX() + static_cast<float>(i) * 6.5f, bounds.getCentreY() - 3.0f, 5.0f, 5.0f);
    }
    else if (item == Item::routing)
    {
        juce::Path path;
        path.startNewSubPath(bounds.getX() + 4.0f, bounds.getBottom() - 4.0f);
        path.lineTo(bounds.getCentreX(), bounds.getCentreY());
        path.lineTo(bounds.getRight() - 4.0f, bounds.getY() + 4.0f);
        g.strokePath(path, juce::PathStrokeType(1.7f));
        g.fillEllipse(bounds.getX() + 1.0f, bounds.getBottom() - 7.0f, 6.0f, 6.0f);
        g.fillEllipse(bounds.getCentreX() - 3.0f, bounds.getCentreY() - 3.0f, 6.0f, 6.0f);
        g.fillEllipse(bounds.getRight() - 7.0f, bounds.getY() + 1.0f, 6.0f, 6.0f);
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
