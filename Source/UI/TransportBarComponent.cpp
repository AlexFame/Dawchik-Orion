#include "TransportBarComponent.h"

#include "OrionTheme.h"

namespace orion
{
namespace
{
void styleButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, theme::core::voidBlack);
    button.setColour(juce::TextButton::buttonOnColourId, theme::warm::pink);
    button.setColour(juce::TextButton::textColourOffId, theme::warm::pink.withAlpha(0.92f));
    button.setColour(juce::TextButton::textColourOnId, theme::text::inverse);
}

class IconButtonLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& buttonBackgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat();
        const auto active = button.getToggleState();
        auto fill = active ? button.findColour(juce::TextButton::buttonOnColourId) : buttonBackgroundColour;
        if (shouldDrawButtonAsDown)
            fill = fill.darker(0.16f);
        else if (shouldDrawButtonAsHighlighted)
            fill = fill.brighter(0.06f);

        g.setColour(juce::Colours::black.withAlpha(0.38f));
        g.fillRect(bounds.translated(0.0f, 2.0f));
        g.setColour(fill);
        g.fillRect(bounds);
        g.setColour((active ? theme::warm::red : theme::line::strong).withAlpha(active ? 0.72f : 0.54f));
        g.drawRect(bounds.reduced(0.5f), 1.0f);
    }

    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool,
                        bool) override
    {
        const auto role = button.getComponentID();
        const auto active = button.getToggleState();
        auto colour = active ? theme::text::inverse : theme::warm::pink;
        if (role == "record")
            colour = active ? theme::warm::red : theme::warm::salmon;
        else if (role == "play")
            colour = active ? theme::text::inverse : theme::text::primary;
        else if (role == "loop" && active)
            colour = theme::text::inverse;

        auto area = button.getLocalBounds().toFloat().reduced(role == "play" ? 14.0f : 9.0f);
        g.setColour(colour);

        if (role == "play")
        {
            juce::Path path;
            path.addTriangle(area.getX() + area.getWidth() * 0.28f, area.getY() + area.getHeight() * 0.12f,
                             area.getRight() - area.getWidth() * 0.14f, area.getCentreY(),
                             area.getX() + area.getWidth() * 0.28f, area.getBottom() - area.getHeight() * 0.12f);
            g.fillPath(path);
        }
        else if (role == "stop")
        {
            g.fillRect(area.withSizeKeepingCentre(area.getWidth() * 0.34f, area.getHeight() * 0.34f));
        }
        else if (role == "record")
        {
            g.setColour(theme::warm::red);
            g.fillEllipse(area.withSizeKeepingCentre(area.getWidth() * 0.46f, area.getHeight() * 0.46f));
        }
        else if (role == "loop")
        {
            juce::Path loop;
            const auto w = area.getWidth();
            const auto h = area.getHeight();
            loop.startNewSubPath(area.getX() + w * 0.28f, area.getY() + h * 0.32f);
            loop.lineTo(area.getX() + w * 0.70f, area.getY() + h * 0.32f);
            loop.quadraticTo(area.getRight() - w * 0.08f, area.getY() + h * 0.32f,
                             area.getRight() - w * 0.08f, area.getCentreY());
            loop.quadraticTo(area.getRight() - w * 0.08f, area.getBottom() - h * 0.18f,
                             area.getX() + w * 0.50f, area.getBottom() - h * 0.18f);
            loop.lineTo(area.getX() + w * 0.30f, area.getBottom() - h * 0.18f);
            g.strokePath(loop, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            juce::Path arrow;
            arrow.addTriangle(area.getX() + w * 0.32f, area.getY() + h * 0.20f,
                              area.getX() + w * 0.12f, area.getY() + h * 0.32f,
                              area.getX() + w * 0.32f, area.getY() + h * 0.44f);
            g.fillPath(arrow);
        }
        else if (role == "metronome")
        {
            juce::Path metro;
            metro.startNewSubPath(area.getCentreX(), area.getY() + area.getHeight() * 0.12f);
            metro.lineTo(area.getX() + area.getWidth() * 0.28f, area.getBottom() - area.getHeight() * 0.12f);
            metro.lineTo(area.getRight() - area.getWidth() * 0.28f, area.getBottom() - area.getHeight() * 0.12f);
            metro.closeSubPath();
            g.strokePath(metro, juce::PathStrokeType(2.8f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
            g.drawLine(area.getCentreX(), area.getY() + area.getHeight() * 0.26f,
                       area.getCentreX() + area.getWidth() * 0.13f, area.getCentreY(), 2.6f);
        }
    }
};

IconButtonLookAndFeel iconButtonLookAndFeel;
}  // namespace

TransportBarComponent::TransportBarComponent()
{
    playButton.setComponentID("play");
    stopButton.setComponentID("stop");
    recordButton.setComponentID("record");
    metronomeButton.setComponentID("metronome");
    loopButton.setComponentID("loop");

    for (auto* button : { &playButton, &stopButton, &recordButton, &metronomeButton, &loopButton })
    {
        styleButton(*button);
        button->setLookAndFeel(&iconButtonLookAndFeel);
        button->addListener(this);
        addAndMakeVisible(*button);
    }

    for (auto* button : { &recordButton, &metronomeButton, &loopButton })
        button->setClickingTogglesState(true);

    playButton.setClickingTogglesState(false);
    recordButton.addMouseListener(this, false);
}

TransportBarComponent::~TransportBarComponent()
{
    for (auto* button : { &playButton, &stopButton, &recordButton, &metronomeButton, &loopButton })
    {
        button->removeListener(this);
        button->setLookAndFeel(nullptr);
    }
}

void TransportBarComponent::setState(const TransportBarState& newState)
{
    state = newState;
    syncButtons();
    repaint();
}

juce::Rectangle<int> TransportBarComponent::getTempoEditorBounds() const noexcept
{
    return tempoCardBounds.reduced(8, 7).removeFromTop(24);
}

juce::Rectangle<int> TransportBarComponent::getKeyBounds() const noexcept
{
    return keyCardBounds;
}

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(theme::core::deepSpace);

    const auto area = getLocalBounds().toFloat();
    juce::ColourGradient leftGlow(theme::cool::cyan.withAlpha(0.14f), area.getX(), area.getY(),
                                  juce::Colours::transparentBlack, area.getX() + area.getWidth() * 0.42f, area.getBottom(), false);
    g.setGradientFill(leftGlow);
    g.fillRect(area);

    juce::ColourGradient rightGlow(theme::warm::red.withAlpha(0.24f), area.getRight(), area.getY(),
                                   juce::Colours::transparentBlack, area.getX() + area.getWidth() * 0.48f, area.getBottom(), false);
    g.setGradientFill(rightGlow);
    g.fillRect(area);

    g.setColour(theme::line::subtle.withAlpha(0.62f));
    g.drawLine(0.0f, static_cast<float>(getHeight() - 1), static_cast<float>(getWidth()), static_cast<float>(getHeight() - 1), 1.0f);

    auto bounds = getLocalBounds().reduced(18, 10);
    const auto bpmBackdrop = tempoCardBounds.expanded(12, 10).toFloat();
    g.setColour(theme::surface::primary.withAlpha(0.62f));
    g.fillRoundedRectangle(bpmBackdrop, 8.0f);

    auto infoArea = bounds.withTrimmedLeft(tempoCardBounds.getRight() + 44 - bounds.getX())
                         .withTrimmedRight(getWidth() - positionCardBounds.getRight() - 18);
    g.setColour(theme::line::subtle.withAlpha(0.58f));
    g.drawLine(static_cast<float>(keyCardBounds.getX() - 32), static_cast<float>(bounds.getY() + 10),
               static_cast<float>(keyCardBounds.getX() - 32), static_cast<float>(bounds.getBottom() - 10), 1.0f);
    g.drawLine(static_cast<float>(positionCardBounds.getX() - 32), static_cast<float>(bounds.getY() + 10),
               static_cast<float>(positionCardBounds.getX() - 32), static_cast<float>(bounds.getBottom() - 10), 1.0f);
    juce::ignoreUnused(infoArea);

    drawButtonFrame(g, tempoCardBounds.toFloat(), theme::core::voidBlack, true);
    for (const auto card : { keyCardBounds, positionCardBounds })
    {
        g.setColour(theme::core::voidBlack.withAlpha(0.0f));
        g.fillRoundedRectangle(card.toFloat(), 6.0f);
    }

    auto tempoText = tempoCardBounds.reduced(7, 7);
    g.setColour(theme::warm::coral);
    g.setFont(juce::FontOptions(18.5f, juce::Font::bold));
    g.drawText(juce::String(state.tempoBpm, 0), tempoText.removeFromTop(22), juce::Justification::centred);
    g.setFont(juce::FontOptions(12.5f, juce::Font::plain));
    g.drawText("BPM", tempoText, juce::Justification::centredTop);

    auto keyValue = keyCardBounds;
    auto positionValue = positionCardBounds;
    g.setColour(theme::text::primary.withAlpha(0.96f));
    g.setFont(juce::FontOptions(19.0f, juce::Font::bold));
    g.drawText(state.keyText, keyValue.removeFromTop(24), juce::Justification::centred);
    g.drawText(state.positionText, positionValue.removeFromTop(24), juce::Justification::centred);

    g.setColour(theme::text::tertiary.withAlpha(0.62f));
    g.setFont(juce::FontOptions(12.5f, juce::Font::plain));
    g.drawText("KEY", keyValue, juce::Justification::centredTop);
    g.drawText("TIME", positionValue, juce::Justification::centredTop);

    if (state.scanVisible)
    {
        auto scan = getLocalBounds().reduced(24, 0).removeFromBottom(4).toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.48f));
        g.fillRoundedRectangle(scan, 2.0f);
        g.setColour(theme::warm::red);
        g.fillRoundedRectangle(scan.withWidth(scan.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, state.scanProgress))), 2.0f);
    }
}

void TransportBarComponent::resized()
{
    auto area = getLocalBounds().reduced(24, 12);
    tempoCardBounds = area.removeFromLeft(70).withSizeKeepingCentre(56, 56);
    area.removeFromLeft(48);
    keyCardBounds = area.removeFromLeft(150).withSizeKeepingCentre(118, 44);
    area.removeFromLeft(24);
    positionCardBounds = area.removeFromLeft(214).withSizeKeepingCentre(176, 44);

    // Transport controls are centred in the bar (info on the left, transport in the middle).
    auto bar = getLocalBounds().reduced(26, 12);
    constexpr int controlsWidth = 50 + 12 + 50 + 12 + 66 + 12 + 50 + 12 + 50;   // buttons + gaps
    auto controls = bar.withSizeKeepingCentre(controlsWidth, bar.getHeight());
    metronomeButton.setBounds(controls.removeFromLeft(50).withSizeKeepingCentre(44, 44));
    controls.removeFromLeft(12);
    stopButton.setBounds(controls.removeFromLeft(50).withSizeKeepingCentre(44, 44));
    controls.removeFromLeft(12);
    playButton.setBounds(controls.removeFromLeft(66).withSizeKeepingCentre(60, 60));
    controls.removeFromLeft(12);
    recordButton.setBounds(controls.removeFromLeft(50).withSizeKeepingCentre(44, 44));
    controls.removeFromLeft(12);
    loopButton.setBounds(controls.removeFromLeft(50).withSizeKeepingCentre(44, 44));
}

void TransportBarComponent::mouseDown(const juce::MouseEvent& event)
{
    if (event.eventComponent == &recordButton && event.mods.isPopupMenu())
    {
        if (onRecordOptions)
            onRecordOptions();
        return;
    }

    if (tempoCardBounds.contains(event.getPosition()) && event.getNumberOfClicks() >= 2)
    {
        if (onTempoEdit)
            onTempoEdit();
        return;
    }

    if (keyCardBounds.contains(event.getPosition()))
    {
        if (onKeySelect)
            onKeySelect();
    }
}

void TransportBarComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (tempoCardBounds.contains(event.getPosition()) && onTempoEdit)
        onTempoEdit();
}

void TransportBarComponent::buttonClicked(juce::Button* button)
{
    if (syncingButtons)
        return;

    if (button == &playButton && onPlay)
        onPlay();
    else if (button == &stopButton && onStop)
        onStop();
    else if (button == &recordButton && onRecordChanged)
        onRecordChanged(recordButton.getToggleState());
    else if (button == &metronomeButton && onMetronomeChanged)
        onMetronomeChanged(metronomeButton.getToggleState());
    else if (button == &loopButton && onLoopChanged)
        onLoopChanged(loopButton.getToggleState());
}

void TransportBarComponent::syncButtons()
{
    const juce::ScopedValueSetter<bool> guard(syncingButtons, true);
    playButton.setToggleState(state.playing, juce::dontSendNotification);
    recordButton.setToggleState(state.recording, juce::dontSendNotification);
    metronomeButton.setToggleState(state.metronome, juce::dontSendNotification);
    loopButton.setToggleState(state.loop, juce::dontSendNotification);
}

void TransportBarComponent::drawButtonFrame(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour colour, bool active) const
{
    g.setColour(colour);
    g.fillRoundedRectangle(bounds, 6.0f);
    g.setColour((active ? theme::warm::red : theme::line::normal).withAlpha(active ? 0.68f : 0.34f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.0f);
}
}  // namespace orion
