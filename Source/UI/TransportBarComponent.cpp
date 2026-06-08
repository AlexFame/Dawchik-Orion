#include "TransportBarComponent.h"

#include "OrionTheme.h"

namespace orion
{
namespace
{
// Shared so paint() (background) and resized() (layout) stay in lock-step.
constexpr int kPanelWidth = 500;
constexpr int kPanelHeight = 80;
constexpr int kReadoutRowHeight = 36;   // remainder of the panel is the button row

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

        g.setColour(juce::Colours::black.withAlpha(0.30f));
        g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f), 6.0f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 6.0f);

        // Uniform subtle frame for every button — on/off state is shown by the fill,
        // not by coloured outlines (no cyan on play, no red on active).
        g.setColour(theme::line::subtle.withAlpha(active ? 0.6f : 0.45f));
        g.drawRoundedRectangle(bounds.reduced(0.6f), 6.0f, 1.0f);
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

        // Icons are drawn inside a CENTERED SQUARE, independent of the button's
        // (wide) rectangle, so shapes stay proportional and never stretch.
        const auto full = button.getLocalBounds().toFloat();
        const float side = juce::jmin(full.getWidth(), full.getHeight()) - (role == "play" ? 16.0f : 15.0f);
        const auto area = juce::Rectangle<float>(side, side).withCentre(full.getCentre());
        const auto w = area.getWidth();
        const auto h = area.getHeight();
        g.setColour(colour);

        if (role == "play")
        {
            juce::Path path;
            path.addTriangle(area.getX() + w * 0.16f, area.getY() + h * 0.06f,
                             area.getRight() - w * 0.02f, area.getCentreY(),
                             area.getX() + w * 0.16f, area.getBottom() - h * 0.06f);
            g.fillPath(path);
        }
        else if (role == "stop")
        {
            g.fillRect(area.withSizeKeepingCentre(w * 0.62f, h * 0.62f));
        }
        else if (role == "record")
        {
            g.setColour(theme::warm::red);
            g.fillEllipse(area.withSizeKeepingCentre(w * 0.58f, h * 0.58f));
        }
        else if (role == "loop")
        {
            // Return / loop hook: an arc across the top curving down the right, with
            // an arrowhead pointing left at the bottom-left.
            const auto cx = area.getCentreX();
            const auto cy = area.getCentreY();
            juce::Path hook;
            hook.startNewSubPath(area.getX() + w * 0.18f, cy + h * 0.20f);
            hook.lineTo(area.getX() + w * 0.62f, cy + h * 0.20f);
            hook.quadraticTo(area.getRight(), cy + h * 0.20f, area.getRight(), cy - h * 0.04f);
            hook.quadraticTo(area.getRight(), area.getY() + h * 0.04f, cx, area.getY() + h * 0.04f);
            hook.lineTo(area.getX() + w * 0.18f, area.getY() + h * 0.04f);
            g.strokePath(hook, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            juce::Path arrow;
            arrow.addTriangle(area.getX() + w * 0.18f, cy + h * 0.04f,
                              area.getX() + w * 0.18f, cy + h * 0.36f,
                              area.getX() - w * 0.02f, cy + h * 0.20f);
            g.fillPath(arrow);
        }
        else if (role == "metronome")
        {
            // Classic metronome: trapezoid body (narrow top, wide base), a base bar,
            // and a diagonal pendulum rod with a small weight.
            const auto cx = area.getCentreX();
            const auto top = area.getY() + h * 0.13f;
            const auto bottom = area.getBottom() - h * 0.15f;

            juce::Path body;
            body.startNewSubPath(cx - w * 0.11f, top);
            body.lineTo(cx + w * 0.11f, top);
            body.lineTo(cx + w * 0.31f, bottom);
            body.lineTo(cx - w * 0.31f, bottom);
            body.closeSubPath();
            g.strokePath(body, juce::PathStrokeType(2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));

            // Base bar.
            g.fillRect(juce::Rectangle<float>(cx - w * 0.31f, bottom - 1.0f, w * 0.62f, 2.0f));

            // Pendulum rod, leaning right, with a weight midway.
            const juce::Point<float> rodTop(cx - w * 0.04f, top + h * 0.07f);
            const juce::Point<float> rodBottom(cx + w * 0.08f, bottom - h * 0.10f);
            g.drawLine(rodTop.x, rodTop.y, rodBottom.x, rodBottom.y, 1.8f);
            const juce::Point<float> weightCentre(rodTop.x + (rodBottom.x - rodTop.x) * 0.55f,
                                                  rodTop.y + (rodBottom.y - rodTop.y) * 0.55f);
            g.fillRect(juce::Rectangle<float>(w * 0.13f, h * 0.07f).withCentre(weightCentre));
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
    // A compact field over the value row (top of the column), sized to the number —
    // not the full column — so clicking to edit doesn't blow the field up wide.
    auto column = tempoCardBounds;
    return column.removeFromTop(22).withSizeKeepingCentre(86, 22);
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

    juce::ColourGradient rightGlow(theme::warm::red.withAlpha(0.10f), area.getRight(), area.getY(),
                                   juce::Colours::transparentBlack, area.getX() + area.getWidth() * 0.55f, area.getBottom(), false);
    g.setGradientFill(rightGlow);
    g.fillRect(area);

    g.setColour(theme::line::subtle.withAlpha(0.62f));
    g.drawLine(0.0f, static_cast<float>(getHeight() - 1), static_cast<float>(getWidth()), static_cast<float>(getHeight() - 1), 1.0f);

    const auto panel = getLocalBounds().withSizeKeepingCentre(kPanelWidth, kPanelHeight).toFloat();
    g.setColour(theme::core::voidBlack.withAlpha(0.58f));
    g.fillRoundedRectangle(panel, 9.0f);
    g.setColour(theme::line::normal.withAlpha(0.46f));
    g.drawRoundedRectangle(panel.reduced(0.5f), 9.0f, 1.0f);

    // A single hairline splits the readouts from the transport controls. No
    // vertical dividers between the readouts — they just added clutter.
    auto topDivider = panel.withTrimmedTop(static_cast<float>(kReadoutRowHeight))
                           .withHeight(1.0f).reduced(18.0f, 0.0f);
    g.setColour(theme::line::subtle.withAlpha(0.45f));
    g.fillRect(topDivider);

    // One readout column: big value, an optional small muted unit inline (e.g. "BPM"),
    // an optional dropdown chevron, and a caption underneath — the whole group centred.
    auto drawReadout = [&g](juce::Rectangle<int> bounds,
                            const juce::String& value,
                            const juce::String& suffix,
                            const juce::String& caption,
                            juce::Colour valueColour,
                            bool showChevron)
    {
        const auto valueRow = bounds.removeFromTop(22).toFloat();

        const juce::Font valueFont(juce::FontOptions(18.0f, juce::Font::bold));
        const juce::Font suffixFont(juce::FontOptions(11.5f, juce::Font::plain));

        const float valueW  = juce::GlyphArrangement::getStringWidth(valueFont, value);
        const float suffGap = suffix.isNotEmpty() ? 5.0f : 0.0f;
        const float suffW   = suffix.isNotEmpty() ? juce::GlyphArrangement::getStringWidth(suffixFont, suffix) : 0.0f;
        const float chevGap = showChevron ? 7.0f : 0.0f;
        const float chevW   = showChevron ? 9.0f : 0.0f;
        const float groupW  = valueW + suffGap + suffW + chevGap + chevW;

        float x = valueRow.getCentreX() - groupW * 0.5f;
        const float cy = valueRow.getCentreY();

        g.setColour(valueColour);
        g.setFont(valueFont);
        g.drawText(value, juce::Rectangle<float>(x, valueRow.getY(), valueW, valueRow.getHeight()),
                   juce::Justification::centredLeft);
        x += valueW;

        if (suffix.isNotEmpty())
        {
            x += suffGap;
            g.setColour(theme::text::tertiary.withAlpha(0.85f));
            g.setFont(suffixFont);
            g.drawText(suffix, juce::Rectangle<float>(x, valueRow.getY(), suffW, valueRow.getHeight()),
                       juce::Justification::centredLeft);
            x += suffW;
        }

        if (showChevron)
        {
            x += chevGap;
            const float chx = x + chevW * 0.5f;
            juce::Path chev;
            chev.startNewSubPath(chx - chevW * 0.5f, cy - 2.0f);
            chev.lineTo(chx, cy + 3.0f);
            chev.lineTo(chx + chevW * 0.5f, cy - 2.0f);
            g.setColour(theme::text::tertiary.withAlpha(0.8f));
            g.strokePath(chev, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour(theme::text::tertiary.withAlpha(0.78f));
        g.setFont(juce::FontOptions(9.5f, juce::Font::plain));
        g.drawText(caption, bounds, juce::Justification::centredTop);
    };

    // Order: BPM | TIME (centre) | KEY. BPM has no chevron (typed/dragged); KEY does
    // (it opens a picker).
    drawReadout(tempoCardBounds, juce::String(state.tempoBpm, 0), "BPM", "BPM", theme::warm::coral, false);
    drawReadout(positionCardBounds, state.positionText, {}, "TIME", theme::text::primary.withAlpha(0.96f), false);
    drawReadout(keyCardBounds, state.keyText, {}, "KEY", theme::text::primary.withAlpha(0.96f), true);

    auto cpuArea = getLocalBounds().reduced(16, 0).removeFromRight(72).withSizeKeepingCentre(64, 40);
    g.setColour(theme::surface::primary.withAlpha(0.45f));
    g.fillRoundedRectangle(cpuArea.toFloat(), 6.0f);
    g.setColour(theme::line::subtle.withAlpha(0.35f));
    g.drawRoundedRectangle(cpuArea.toFloat().reduced(0.5f), 6.0f, 1.0f);
    auto cpu = cpuArea.reduced(8, 6);
    g.setColour(theme::text::tertiary.withAlpha(0.60f));
    g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    g.drawText("CPU", cpu.removeFromTop(12), juce::Justification::centred);
    auto loadRow = cpu.removeFromTop(18);
    auto bars = loadRow.removeFromLeft(28).withSizeKeepingCentre(24, 12);
    const auto activeBars = juce::roundToInt(juce::jlimit(0.0f, 1.0f, state.engineLoad) * 4.0f);
    for (int i = 0; i < 4; ++i)
    {
        auto bar = bars.removeFromLeft(4);
        bars.removeFromLeft(2);
        g.setColour(i < activeBars ? theme::warm::red : theme::line::subtle.withAlpha(0.42f));
        g.fillRect(bar.withTop(bar.getBottom() - 3 - i * 3));
    }
    g.setColour(theme::text::primary.withAlpha(0.88f));
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(juce::String(juce::roundToInt(state.engineLoad * 100.0f)) + "%", loadRow, juce::Justification::centredRight);

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
    auto panel = getLocalBounds().withSizeKeepingCentre(kPanelWidth, kPanelHeight);

    // Readouts: three equal columns spread across the full width (BPM | TIME | KEY).
    auto readouts = panel.removeFromTop(kReadoutRowHeight).reduced(16, 0);
    const int colWidth = readouts.getWidth() / 3;
    tempoCardBounds    = readouts.removeFromLeft(colWidth);
    keyCardBounds      = readouts.removeFromRight(colWidth);
    positionCardBounds = readouts;   // centre column = whatever remains

    // Transport controls: five wide rounded buttons filling the width evenly.
    auto band = panel.reduced(16, 3);   // bottom row, with side + vertical padding
    constexpr int buttonGap = 10;
    const int cellWidth = (band.getWidth() - buttonGap * 4) / 5;

    const auto place = [&band, buttonGap, cellWidth](juce::Button& button, bool last)
    {
        button.setBounds(last ? band : band.removeFromLeft(cellWidth));
        if (! last)
            band.removeFromLeft(buttonGap);
    };

    place(loopButton, false);
    place(metronomeButton, false);
    place(stopButton, false);
    place(playButton, false);
    place(recordButton, true);   // takes the remainder so rounding never leaves a gap
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
