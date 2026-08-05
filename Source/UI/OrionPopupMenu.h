#pragma once

#include "OrionTheme.h"

namespace orion::ui
{
// Shared popup styling for menus created outside MainComponent's menu bar.
class PopupMenuLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    PopupMenuLookAndFeel()
    {
        setColour(juce::PopupMenu::backgroundColourId, theme::surface::elevated);
        setColour(juce::PopupMenu::textColourId, theme::text::primary);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, theme::accent::activeCoral);
        setColour(juce::PopupMenu::highlightedTextColourId, theme::text::inverse);
    }

    void drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                            const juce::PopupMenu::Options&) override
    {
        const auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width),
                                                   static_cast<float>(height)).reduced(1.0f);
        g.setColour(juce::Colours::black.withAlpha(0.30f));
        g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f), 10.0f);
        g.setColour(findColour(juce::PopupMenu::backgroundColourId));
        g.fillRoundedRectangle(bounds, 10.0f);
        g.setColour(theme::line::normal.withAlpha(0.85f));
        g.drawRoundedRectangle(bounds, 10.0f, 1.0f);
    }

    void getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool isSeparator,
                                              int standardMenuItemHeight, int& idealWidth,
                                              int& idealHeight,
                                              const juce::PopupMenu::Options& options) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSizeWithOptions(text, isSeparator,
                                                                   standardMenuItemHeight,
                                                                   idealWidth, idealHeight, options);
        if (! isSeparator)
        {
            idealHeight = juce::jmax(idealHeight, 32);
            idealWidth += 28;
        }
    }

    void drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                      bool isHighlighted, const juce::PopupMenu::Item& item,
                                      const juce::PopupMenu::Options&) override
    {
        if (item.isSeparator)
        {
            g.setColour(theme::line::normal.withAlpha(0.35f));
            g.fillRect(area.reduced(12, 0).removeFromTop(1).translated(0, area.getHeight() / 2));
            return;
        }

        const auto itemArea = area.reduced(5, 2);
        if (isHighlighted && item.isEnabled)
        {
            g.setColour(theme::accent::activeCoral.withAlpha(0.90f));
            g.fillRoundedRectangle(itemArea.toFloat(), 6.0f);
        }

        auto textArea = itemArea.reduced(12, 0);
        if (item.isTicked)
        {
            g.setColour(isHighlighted ? theme::text::inverse : theme::accent::activeCoral);
            g.fillEllipse(static_cast<float>(textArea.getX()),
                          static_cast<float>(textArea.getCentreY() - 3), 6.0f, 6.0f);
            textArea.removeFromLeft(16);
        }

        g.setColour(item.isEnabled
                        ? (isHighlighted ? theme::text::inverse : theme::text::primary)
                        : theme::text::muted);
        g.setFont(juce::FontOptions("Avenir Next", 13.0f,
                                    isHighlighted ? juce::Font::bold : juce::Font::plain));
        g.drawText(item.text, textArea, juce::Justification::centredLeft, true);

        if (item.subMenu != nullptr)
        {
            g.setColour(isHighlighted ? theme::text::inverse : theme::text::secondary);
            const auto x = static_cast<float>(itemArea.getRight() - 12);
            const auto y = static_cast<float>(itemArea.getCentreY());
            g.drawLine(x - 3.0f, y - 4.0f, x + 1.0f, y, 1.4f);
            g.drawLine(x + 1.0f, y, x - 3.0f, y + 4.0f, 1.4f);
        }
    }

    int getPopupMenuBorderSize() override { return 6; }
    juce::Font getPopupMenuFont() override { return juce::FontOptions("Avenir Next", 13.0f, juce::Font::plain); }
};

inline juce::LookAndFeel& popupMenuLookAndFeel()
{
    static PopupMenuLookAndFeel lookAndFeel;
    return lookAndFeel;
}

inline void stylePopupMenu(juce::PopupMenu& menu)
{
    menu.setLookAndFeel(&popupMenuLookAndFeel());
}
} // namespace orion::ui
