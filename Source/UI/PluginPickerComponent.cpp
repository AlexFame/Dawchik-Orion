#include "PluginPickerComponent.h"

namespace
{
const auto panelColour   = juce::Colour(0xff15191f);
const auto panelStroke   = juce::Colour(0xff2b3640);
const auto rowFill       = juce::Colour(0xff1b2027);
const auto rowSelected   = juce::Colour(0xff2f6df0);
const auto accent        = juce::Colour(0xffe8401f);
const auto cyan          = juce::Colour(0xff35c9d6);
const auto textColour    = juce::Colours::white.withAlpha(0.92f);
const auto mutedText     = juce::Colours::white.withAlpha(0.55f);

constexpr int panelW = 520;
constexpr int panelH = 560;
constexpr int pad    = 18;
constexpr int rowH   = 40;
}  // namespace

namespace orion
{
PluginPickerComponent::PluginPickerComponent()
{
    setVisible(false);
    setAlwaysOnTop(true);

    searchBox.setTextToShowWhenEmpty("Search instruments...", mutedText);
    searchBox.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0c0e10));
    searchBox.setColour(juce::TextEditor::textColourId, textColour);
    searchBox.setColour(juce::TextEditor::outlineColourId, panelStroke);
    searchBox.setColour(juce::TextEditor::focusedOutlineColourId, cyan);
    searchBox.setFont(juce::FontOptions(15.0f));
    searchBox.addListener(this);
    addAndMakeVisible(searchBox);

    list.setModel(this);
    list.setRowHeight(rowH);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(list);
}

void PluginPickerComponent::show(const juce::String& title, juce::Array<juce::PluginDescription> plugins, bool scanning)
{
    titleText = title;
    allPlugins = std::move(plugins);
    isScanning = scanning;
    searchBox.setText({}, juce::dontSendNotification);
    rebuildFiltered();
    anim = 0.0f;
    setAlpha(0.0f);
    setVisible(true);
    toFront(true);
    resized();
    startTimerHz(60);
    searchBox.grabKeyboardFocus();
    repaint();
}

void PluginPickerComponent::closePicker()
{
    stopTimer();
    setTransform({});
    setAlpha(1.0f);
    setVisible(false);
    if (onClose)
        onClose();
}

void PluginPickerComponent::timerCallback()
{
    anim = juce::jmin(1.0f, anim + 0.08f);
    const auto t = 1.0f - anim;
    const auto eased = 1.0f - t * t * t;
    setAlpha(eased);
    const auto scale = 0.96f + 0.04f * eased;
    setTransform(juce::AffineTransform::scale(scale, scale, getWidth() * 0.5f, getHeight() * 0.5f));
    repaint();
    if (anim >= 1.0f)
        stopTimer();
}

void PluginPickerComponent::rebuildFiltered()
{
    const auto query = searchBox.getText().trim().toLowerCase();
    filtered.clear();
    for (int i = 0; i < allPlugins.size(); ++i)
    {
        if (query.isEmpty())
        {
            filtered.push_back(i);
            continue;
        }
        const auto& d = allPlugins.getReference(i);
        const auto hay = (d.name + " " + d.manufacturerName + " " + d.pluginFormatName).toLowerCase();
        if (hay.contains(query))
            filtered.push_back(i);
    }
    list.updateContent();
    list.repaint();
}

int PluginPickerComponent::getNumRows()
{
    return static_cast<int>(filtered.size());
}

void PluginPickerComponent::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(filtered.size()))
        return;
    const auto& d = allPlugins.getReference(filtered[static_cast<std::size_t>(row)]);

    auto r = juce::Rectangle<int>(0, 0, width, height).reduced(2, 2);
    g.setColour(selected ? rowSelected.withAlpha(0.85f) : rowFill);
    g.fillRoundedRectangle(r.toFloat(), 6.0f);

    // Coloured pill with the plugin's initial.
    auto pill = r.removeFromLeft(height).reduced(7);
    g.setColour((selected ? juce::Colours::white : cyan).withAlpha(0.9f));
    g.fillRoundedRectangle(pill.toFloat(), 5.0f);
    g.setColour(juce::Colours::black.withAlpha(0.8f));
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText(d.name.substring(0, 1).toUpperCase(), pill, juce::Justification::centred);

    r.removeFromLeft(10);
    auto textArea = r;
    auto nameRow = textArea.removeFromTop(height / 2 + 2);
    g.setColour(selected ? juce::Colours::white : textColour);
    g.setFont(juce::FontOptions(14.5f, juce::Font::bold));
    g.drawText(d.name, nameRow, juce::Justification::bottomLeft, true);

    g.setColour((selected ? juce::Colours::white : mutedText).withAlpha(selected ? 0.85f : 0.7f));
    g.setFont(juce::FontOptions(11.5f));
    juce::String sub = d.manufacturerName;
    if (d.pluginFormatName.isNotEmpty())
        sub += (sub.isNotEmpty() ? "  ·  " : "") + d.pluginFormatName;
    g.drawText(sub, textArea, juce::Justification::topLeft, true);
}

void PluginPickerComponent::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    pickRow(row);
}

void PluginPickerComponent::returnKeyPressed(int lastRowSelected)
{
    pickRow(lastRowSelected);
}

void PluginPickerComponent::pickRow(int row)
{
    if (row < 0 || row >= static_cast<int>(filtered.size()))
        return;
    const auto picked = allPlugins.getReference(filtered[static_cast<std::size_t>(row)]);
    closePicker();
    if (onPick)
        onPick(picked);
}

void PluginPickerComponent::textEditorTextChanged(juce::TextEditor&)
{
    rebuildFiltered();
}

void PluginPickerComponent::paint(juce::Graphics& g)
{
    // No background dim — floating panel only (soft shadow gives separation).
    const auto panel = getPanelBounds();
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    g.fillRoundedRectangle(panel.toFloat().translated(0.0f, 3.0f), 12.0f);
    g.setColour(panelColour);
    g.fillRoundedRectangle(panel.toFloat(), 12.0f);
    g.setColour(panelStroke);
    g.drawRoundedRectangle(panel.toFloat(), 12.0f, 1.0f);

    auto inner = panel.reduced(pad);
    auto titleRow = inner.removeFromTop(30);
    g.setColour(textColour);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText(titleText, titleRow, juce::Justification::centredLeft, false);

    // Close "x".
    const auto closeB = getCloseBounds();
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(20.0f));
    g.drawText("x", closeB, juce::Justification::centred);

    // Footer.
    auto footer = panel.reduced(pad, 0).removeFromBottom(46);
    g.setColour(panelStroke.withAlpha(0.5f));
    g.drawHorizontalLine(footer.getY(), static_cast<float>(footer.getX()), static_cast<float>(footer.getRight()));

    const auto rescanB = getRescanBounds();
    g.setColour(juce::Colour(0xff262a30));
    g.fillRoundedRectangle(rescanB.toFloat(), 6.0f);
    g.setColour(cyan.withAlpha(0.9f));
    g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
    g.drawText(isScanning ? "Scanning..." : "Rescan VST3", rescanB, juce::Justification::centred);

    if (filtered.empty())
    {
        g.setColour(mutedText);
        g.setFont(juce::FontOptions(14.0f));
        g.drawText(isScanning ? "Scanning plugins..."
                              : (allPlugins.isEmpty() ? "No plugins found — try Rescan." : "No matches."),
                   list.getBounds(), juce::Justification::centred, false);
    }
}

void PluginPickerComponent::resized()
{
    const auto panel = getPanelBounds();
    auto inner = panel.reduced(pad);
    inner.removeFromTop(30);            // title
    inner.removeFromTop(8);
    searchBox.setBounds(inner.removeFromTop(36));
    inner.removeFromTop(10);
    inner.removeFromBottom(46);         // footer
    list.setBounds(inner);
}

juce::Rectangle<int> PluginPickerComponent::getPanelBounds() const
{
    return getLocalBounds().withSizeKeepingCentre(juce::jmin(panelW, getWidth() - 40),
                                                  juce::jmin(panelH, getHeight() - 40));
}

juce::Rectangle<int> PluginPickerComponent::getCloseBounds() const
{
    const auto panel = getPanelBounds();
    return { panel.getRight() - pad - 24, panel.getY() + pad, 24, 24 };
}

juce::Rectangle<int> PluginPickerComponent::getRescanBounds() const
{
    const auto panel = getPanelBounds();
    return { panel.getX() + pad, panel.getBottom() - 38, 110, 26 };
}

bool PluginPickerComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        closePicker();
        return true;
    }
    return false;
}

void PluginPickerComponent::mouseDown(const juce::MouseEvent& event)
{
    const auto pos = event.getPosition();
    if (getCloseBounds().contains(pos)) { closePicker(); return; }
    if (getRescanBounds().contains(pos)) { if (onRescan) onRescan(); return; }
    if (! getPanelBounds().contains(pos)) closePicker();   // click outside dismisses
}
}  // namespace orion
