#include "SelectionInspectorComponent.h"

#include "OrionTheme.h"

#include <cmath>

namespace orion
{
namespace
{
constexpr double minGainDb = -24.0;
constexpr double maxGainDb = 12.0;

void styleInspectorButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff0b0d12));
    button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff252a31));
    button.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.88f));
    button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
}
}  // namespace

SelectionInspectorComponent::SelectionInspectorComponent()
{
    for (auto* button : { &muteButton, &soloButton, &warpButton })
    {
        button->setClickingTogglesState(true);
        styleInspectorButton(*button);
        addAndMakeVisible(*button);
    }

    muteButton.onClick = [this]
    {
        if (updatingButtons)
            return;

        model.muted = muteButton.getToggleState();
        if (onMuteChanged)
            onMuteChanged(model.muted);
        repaint();
    };

    soloButton.onClick = [this]
    {
        if (updatingButtons)
            return;

        model.solo = soloButton.getToggleState();
        if (onSoloChanged)
            onSoloChanged(model.solo);
        repaint();
    };

    warpButton.onClick = [this]
    {
        if (updatingButtons)
            return;

        model.warpEnabled = warpButton.getToggleState();
        if (onWarpChanged)
            onWarpChanged(model.warpEnabled);
        repaint();
    };
}

void SelectionInspectorComponent::setModel(const SelectionInspectorModel& newModel)
{
    model = newModel;
    setVisible(model.hasSelection);
    syncButtonStates();
    resized();
    repaint();
}

void SelectionInspectorComponent::paint(juce::Graphics& g)
{
    if (! model.hasSelection)
        return;

    const auto accent = model.accent;
    auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    const float radius = 14.0f;

    // Soft drop shadow.
    g.setColour(juce::Colours::black.withAlpha(0.40f));
    g.fillRoundedRectangle(bounds.translated(0.0f, 3.0f), radius);

    // Card body (flat dark) + accent border, exactly like the reference cards.
    g.setColour(juce::Colour(0xff141821));
    g.fillRoundedRectangle(bounds, radius);
    g.setColour(accent.withAlpha(0.95f));
    g.drawRoundedRectangle(bounds.reduced(1.0f), radius, 2.0f);

    auto inner = getLocalBounds().reduced(18, 14);

    // Title (top-left, bold) — leave room on the right for the M/S buttons.
    auto titleRow = inner.removeFromTop(30);
    titleRow.removeFromRight(86);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(21.0f, juce::Font::bold));
    g.drawFittedText(model.title, titleRow, juce::Justification::centredLeft, 1);

    // Gain bar (rounded, filled in the accent colour) + live level overlay.
    const auto track = getGainTrackBounds().toFloat();
    g.setColour(juce::Colour(0xff0a0c10));
    g.fillRoundedRectangle(track, track.getHeight() * 0.5f);

    const auto norm = juce::jmap(juce::jlimit(minGainDb, maxGainDb, model.gainDb), minGainDb, maxGainDb, 0.0, 1.0);
    if (norm > 0.001)
    {
        auto fill = track.withWidth(juce::jmax(track.getHeight(), static_cast<float>(track.getWidth() * norm)));
        g.setColour(accent);
        g.fillRoundedRectangle(fill, track.getHeight() * 0.5f);
    }

    const auto liveLevel = onRequestLiveLevel ? juce::jlimit(0.0f, 1.0f, onRequestLiveLevel()) : 0.0f;
    if (liveLevel > 0.001f)
    {
        auto liveFill = track.withWidth(track.getWidth() * liveLevel);
        const auto liveColour = liveLevel > 0.92f ? theme::status::error
                              : liveLevel > 0.75f ? theme::status::warning
                                                   : theme::status::success;
        g.setColour(liveColour.withAlpha(0.78f));
        g.fillRoundedRectangle(liveFill, track.getHeight() * 0.5f);
    }

    juce::Rectangle<int> dbArea(getWidth() - 18 - 68,
                                static_cast<int>(track.getCentreY()) - 11,
                                68, 22);
    const auto liveDb = onRequestLiveLevelDb ? onRequestLiveLevelDb() : -100.0f;
    g.setColour(juce::Colours::white.withAlpha(0.78f));
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawFittedText(liveDb <= -60.0f ? juce::String("-inf") : juce::String(liveDb, 1) + " dB",
                     dbArea,
                     juce::Justification::centredRight,
                     1);
}

void SelectionInspectorComponent::resized()
{
    auto inner = getLocalBounds().reduced(18, 14);

    // M/S buttons: two rounded squares at the top-right.
    auto topRight = inner.removeFromTop(30).removeFromRight(74);
    muteButton.setBounds(topRight.removeFromLeft(34));
    topRight.removeFromLeft(6);
    soloButton.setBounds(topRight.removeFromLeft(34));

    // Optional WARP chip at the bottom-left.
    warpButton.setVisible(model.hasSelection && model.showWarp);
    if (model.showWarp)
    {
        auto bottom = getLocalBounds().reduced(18, 12).removeFromBottom(20);
        warpButton.setBounds(bottom.removeFromLeft(70));
    }
}

void SelectionInspectorComponent::mouseDown(const juce::MouseEvent& event)
{
    if (! model.hasSelection)
        return;

    if (getGainTrackBounds().expanded(5, 6).contains(event.getPosition()))
    {
        draggingGain = true;
        if (onGainDragStart)
            onGainDragStart();
        applyGainFromPoint(event.getPosition());
    }
}

void SelectionInspectorComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (draggingGain)
        applyGainFromPoint(event.getPosition());
}

void SelectionInspectorComponent::mouseUp(const juce::MouseEvent&)
{
    draggingGain = false;
}

juce::Rectangle<int> SelectionInspectorComponent::getGainTrackBounds() const noexcept
{
    auto area = getLocalBounds().reduced(18, 14);
    area.removeFromTop(30);              // title row
    area.removeFromTop(juce::jmax(6, (area.getHeight() - 12) / 2)); // push the bar to the lower half
    area.removeFromRight(78);           // reserve space for the dB readout
    return area.removeFromTop(12);      // the gain bar itself
}

void SelectionInspectorComponent::applyGainFromPoint(juce::Point<int> point)
{
    const auto track = getGainTrackBounds();
    if (track.getWidth() <= 0)
        return;

    const auto normalized = juce::jlimit(0.0, 1.0, (point.x - track.getX()) / static_cast<double>(track.getWidth()));
    const auto nextGain = std::round(juce::jmap(normalized, minGainDb, maxGainDb) * 10.0) / 10.0;

    if (std::abs(model.gainDb - nextGain) < 0.001)
        return;

    model.gainDb = nextGain;
    if (onGainChanged)
        onGainChanged(model.gainDb);
    repaint();
}

void SelectionInspectorComponent::syncButtonStates()
{
    const juce::ScopedValueSetter<bool> guard(updatingButtons, true);
    muteButton.setToggleState(model.muted, juce::dontSendNotification);
    soloButton.setToggleState(model.solo, juce::dontSendNotification);
    warpButton.setToggleState(model.warpEnabled, juce::dontSendNotification);
}
}  // namespace orion
