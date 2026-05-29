#include "MixerPanelComponent.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace
{
const auto panelBackground = juce::Colour(0xff151c23);
const auto panelStroke     = juce::Colour(0xff31404d);
const auto stripBackground = juce::Colour(0xff1c252e);
const auto accentColour    = juce::Colour(0xffeb6f3a);
const auto mutedText       = juce::Colours::white.withAlpha(0.62f);

constexpr double minGainDb = -60.0;
constexpr double maxGainDb = 6.0;

constexpr int stripWidth     = 76;
constexpr int stripGap       = 8;
constexpr int masterStripWidth = 96;
constexpr int panelPadding   = 18;
constexpr int titleHeight    = 30;

juce::String gainTextFromValue(double value)
{
    if (value <= minGainDb + 0.01)
        return "-inf";
    return juce::String(value, 1) + " dB";
}
}  // namespace

namespace orion
{
MixerPanelComponent::MixerPanelComponent(ProjectState& projectState)
    : project(projectState)
{
    setVisible(false);

    masterVolume.setSliderStyle(juce::Slider::LinearVertical);
    masterVolume.setRange(minGainDb, maxGainDb, 0.1);
    masterVolume.setValue(0.0, juce::dontSendNotification);
    masterVolume.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 18);
    masterVolume.setColour(juce::Slider::thumbColourId, accentColour);
    masterVolume.setColour(juce::Slider::trackColourId, accentColour.withAlpha(0.55f));
    masterVolume.textFromValueFunction = [](double v) { return gainTextFromValue(v); };
    masterVolume.onValueChange = [this]
    {
        if (onSetMasterGainDb)
            onSetMasterGainDb(masterVolume.getValue());
    };
    addChildComponent(masterVolume);
}

void MixerPanelComponent::open()
{
    rebuildStrips();

    if (onRequestMasterGainDb)
        masterVolume.setValue(onRequestMasterGainDb(), juce::dontSendNotification);

    setVisible(true);
    toFront(true);
    masterVolume.setVisible(true);
    startTimerHz(30);
    resized();
    repaint();
}

void MixerPanelComponent::closePanel()
{
    stopTimer();
    setVisible(false);
    if (onClose)
        onClose();
}

void MixerPanelComponent::rebuildStrips()
{
    for (auto& strip : strips)
    {
        if (strip == nullptr)
            continue;
        if (strip->volume != nullptr) removeChildComponent(strip->volume.get());
        if (strip->mute != nullptr)   removeChildComponent(strip->mute.get());
        if (strip->solo != nullptr)   removeChildComponent(strip->solo.get());
    }
    strips.clear();

    const auto& tracks = project.getTracks();
    builtTrackCount = static_cast<int>(tracks.size());

    for (int i = 0; i < builtTrackCount; ++i)
    {
        auto strip = std::make_unique<ChannelStrip>();
        strip->trackIndex = i;

        const auto& track = tracks[static_cast<std::size_t>(i)];

        strip->volume = std::make_unique<juce::Slider>();
        strip->volume->setSliderStyle(juce::Slider::LinearVertical);
        strip->volume->setRange(minGainDb, maxGainDb, 0.1);
        strip->volume->setValue(track.volumeDb, juce::dontSendNotification);
        strip->volume->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 16);
        strip->volume->setColour(juce::Slider::thumbColourId, juce::Colours::white);
        strip->volume->setColour(juce::Slider::trackColourId, accentColour.withAlpha(0.5f));
        strip->volume->textFromValueFunction = [](double v) { return gainTextFromValue(v); };
        const int trackIndex = i;
        strip->volume->onValueChange = [this, trackIndex]
        {
            auto& t = project.getTracks();
            if (trackIndex < 0 || trackIndex >= static_cast<int>(t.size()))
                return;
            if (auto* s = strips[static_cast<std::size_t>(trackIndex)].get(); s != nullptr && s->volume != nullptr)
                t[static_cast<std::size_t>(trackIndex)].volumeDb = s->volume->getValue();
            if (onTrackChanged)
                onTrackChanged();
        };
        addChildComponent(strip->volume.get());
        strip->volume->setVisible(true);

        strip->mute = std::make_unique<juce::TextButton>("M");
        strip->mute->setClickingTogglesState(true);
        strip->mute->setToggleState(track.muted, juce::dontSendNotification);
        strip->mute->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xfff0c419));
        strip->mute->setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        strip->mute->onClick = [this, trackIndex]
        {
            auto& t = project.getTracks();
            if (trackIndex < 0 || trackIndex >= static_cast<int>(t.size()))
                return;
            if (auto* s = strips[static_cast<std::size_t>(trackIndex)].get(); s != nullptr && s->mute != nullptr)
                t[static_cast<std::size_t>(trackIndex)].muted = s->mute->getToggleState();
            if (onTrackChanged)
                onTrackChanged();
        };
        addChildComponent(strip->mute.get());
        strip->mute->setVisible(true);

        strip->solo = std::make_unique<juce::TextButton>("S");
        strip->solo->setClickingTogglesState(true);
        strip->solo->setToggleState(track.solo, juce::dontSendNotification);
        strip->solo->setColour(juce::TextButton::buttonOnColourId, accentColour);
        strip->solo->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        strip->solo->onClick = [this, trackIndex]
        {
            auto& t = project.getTracks();
            if (trackIndex < 0 || trackIndex >= static_cast<int>(t.size()))
                return;
            if (auto* s = strips[static_cast<std::size_t>(trackIndex)].get(); s != nullptr && s->solo != nullptr)
                t[static_cast<std::size_t>(trackIndex)].solo = s->solo->getToggleState();
            if (onTrackChanged)
                onTrackChanged();
        };
        addChildComponent(strip->solo.get());
        strip->solo->setVisible(true);

        strips.push_back(std::move(strip));
    }
}

void MixerPanelComponent::syncControlsFromProject()
{
    const auto& tracks = project.getTracks();
    for (auto& strip : strips)
    {
        if (strip == nullptr || strip->trackIndex < 0 || strip->trackIndex >= static_cast<int>(tracks.size()))
            continue;
        const auto& track = tracks[static_cast<std::size_t>(strip->trackIndex)];
        if (strip->volume != nullptr && ! strip->volume->isMouseButtonDown())
            strip->volume->setValue(track.volumeDb, juce::dontSendNotification);
        if (strip->mute != nullptr)
            strip->mute->setToggleState(track.muted, juce::dontSendNotification);
        if (strip->solo != nullptr)
            strip->solo->setToggleState(track.solo, juce::dontSendNotification);
    }
}

void MixerPanelComponent::timerCallback()
{
    // Rebuild if the track count changed (tracks added/removed/project loaded).
    if (static_cast<int>(project.getTracks().size()) != builtTrackCount)
    {
        rebuildStrips();
        resized();
    }
    else
    {
        syncControlsFromProject();
    }

    float peak = onRequestMasterPeak ? onRequestMasterPeak() : 0.0f;
    // Convert to a 0..1 display value over the fader's dB range, with a smooth fall.
    const auto peakDb = peak > 0.0f ? juce::Decibels::gainToDecibels(peak, static_cast<float>(minGainDb)) : static_cast<float>(minGainDb);
    const auto target = juce::jlimit(0.0f, 1.0f, juce::jmap(peakDb, static_cast<float>(minGainDb), 0.0f, 0.0f, 1.0f));
    masterMeterDisplay = juce::jmax(target, masterMeterDisplay * 0.82f);

    // Logic-style peak hold for the master number (this timer runs at 30 Hz).
    constexpr int   masterHoldFrames        = 30;    // ~1.0 s hold
    constexpr float masterReleaseDbPerTick  = 0.7f;  // ~21 dB/s release afterwards
    const auto masterPeakDb = peak > 0.0001f ? juce::Decibels::gainToDecibels(peak) : -100.0f;
    if (masterPeakDb >= masterLevelDb)
    {
        masterLevelDb = masterPeakDb;
        masterPeakHoldFrames = masterHoldFrames;
    }
    else if (masterPeakHoldFrames > 0)
    {
        --masterPeakHoldFrames;
    }
    else
    {
        masterLevelDb = juce::jmax(-100.0f, masterLevelDb - masterReleaseDbPerTick);
    }

    if (! masterMeterBounds.isEmpty())
        repaint(masterMeterBounds.expanded(2));
    if (! masterLevelTextBounds.isEmpty())
        repaint(masterLevelTextBounds);

    // Per-track strip meters read the decayed levels owned by MainComponent.
    for (auto& strip : strips)
    {
        if (strip == nullptr)
            continue;
        strip->meterDisplay = onRequestTrackLevel ? onRequestTrackLevel(strip->trackIndex) : 0.0f;
        strip->levelDbDisplay = onRequestTrackLevelDb ? onRequestTrackLevelDb(strip->trackIndex) : -100.0f;
        if (! strip->meterBounds.isEmpty())
            repaint(strip->meterBounds.expanded(2));
        if (! strip->levelTextBounds.isEmpty())
            repaint(strip->levelTextBounds);
    }
}

void MixerPanelComponent::paint(juce::Graphics& g)
{
    // Dim the timeline behind the panel.
    g.fillAll(juce::Colours::black.withAlpha(0.45f));

    const auto panel = getPanelBounds();
    g.setColour(panelBackground);
    g.fillRoundedRectangle(panel.toFloat(), 10.0f);
    g.setColour(panelStroke);
    g.drawRoundedRectangle(panel.toFloat(), 10.0f, 1.0f);

    auto inner = panel.reduced(panelPadding);

    auto titleRow = inner.removeFromTop(titleHeight);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("MIXER", titleRow, juce::Justification::centredLeft);

    // Close (x) button.
    const auto closeBounds = getCloseButtonBounds();
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(20.0f, juce::Font::plain));
    g.drawText("x", closeBounds, juce::Justification::centred);

    inner.removeFromTop(6);

    const auto& tracks = project.getTracks();

    // Track strip backgrounds + names.
    auto stripArea = inner;
    stripArea.removeFromRight(masterStripWidth + stripGap);

    if (tracks.empty())
    {
        g.setColour(mutedText);
        g.setFont(juce::FontOptions(14.0f, juce::Font::plain));
        g.drawText("No tracks yet — add a track to mix.", stripArea, juce::Justification::centred);
    }

    int x = stripArea.getX();
    for (std::size_t i = 0; i < strips.size(); ++i)
    {
        juce::Rectangle<int> col(x, stripArea.getY(), stripWidth, stripArea.getHeight());
        g.setColour(stripBackground);
        g.fillRoundedRectangle(col.toFloat(), 6.0f);

        if (i < tracks.size())
        {
            auto nameRow = col.reduced(4).removeFromTop(18);
            g.setColour(tracks[i].colour);
            g.fillRoundedRectangle(nameRow.removeFromLeft(8).withSizeKeepingCentre(8, 8).toFloat(), 2.0f);
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(tracks[i].name, col.reduced(4).removeFromTop(18).withTrimmedLeft(10),
                       juce::Justification::centredLeft, true);
        }

        // Per-strip output level meter.
        const auto& strip = strips[i];

        // Live signal-level numeric readout (real-time dB).
        if (strip != nullptr && ! strip->levelTextBounds.isEmpty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.82f));
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(strip->levelDbDisplay <= -60.0f ? juce::String("-inf")
                                                       : juce::String(strip->levelDbDisplay, 1) + " dB",
                       strip->levelTextBounds, juce::Justification::centred, false);
        }

        if (strip != nullptr && ! strip->meterBounds.isEmpty())
        {
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.fillRoundedRectangle(strip->meterBounds.toFloat(), 3.0f);

            const auto level = juce::jlimit(0.0f, 1.0f, strip->meterDisplay);
            if (level > 0.001f)
            {
                auto fill = strip->meterBounds.toFloat();
                fill = fill.removeFromBottom(fill.getHeight() * level);
                const auto colour = level > 0.92f ? juce::Colours::red
                                  : level > 0.75f ? juce::Colours::orange
                                                  : juce::Colour(0xff4fd27a);
                g.setColour(colour);
                g.fillRoundedRectangle(fill, 3.0f);
            }
            g.setColour(panelStroke);
            g.drawRoundedRectangle(strip->meterBounds.toFloat(), 3.0f, 1.0f);
        }

        x += stripWidth + stripGap;
    }

    // Master strip background.
    auto masterCol = inner.removeFromRight(masterStripWidth);
    g.setColour(stripBackground.brighter(0.04f));
    g.fillRoundedRectangle(masterCol.toFloat(), 6.0f);
    g.setColour(accentColour.withAlpha(0.9f));
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText("MASTER", masterCol.reduced(4).removeFromTop(18), juce::Justification::centred);

    // Master live signal-level readout (real-time dB).
    if (! masterLevelTextBounds.isEmpty())
    {
        g.setColour(juce::Colours::white.withAlpha(0.82f));
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(masterLevelDb <= -60.0f ? juce::String("-inf")
                                           : juce::String(masterLevelDb, 1) + " dB",
                   masterLevelTextBounds, juce::Justification::centred, false);
    }

    // Master meter bar.
    if (! masterMeterBounds.isEmpty())
    {
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.fillRoundedRectangle(masterMeterBounds.toFloat(), 3.0f);

        auto fill = masterMeterBounds.toFloat();
        const auto h = fill.getHeight() * juce::jlimit(0.0f, 1.0f, masterMeterDisplay);
        auto level = fill.removeFromBottom(h);
        const auto colour = masterMeterDisplay > 0.92f ? juce::Colours::red
                          : masterMeterDisplay > 0.75f ? juce::Colours::orange
                                                       : juce::Colour(0xff4fd27a);
        g.setColour(colour);
        g.fillRoundedRectangle(level, 3.0f);

        g.setColour(panelStroke);
        g.drawRoundedRectangle(masterMeterBounds.toFloat(), 3.0f, 1.0f);
    }
}

void MixerPanelComponent::resized()
{
    const auto panel = getPanelBounds();
    auto inner = panel.reduced(panelPadding);
    inner.removeFromTop(titleHeight);
    inner.removeFromTop(6);

    auto layoutStripControls = [](juce::Rectangle<int> col,
                                  juce::Slider* volume,
                                  juce::TextButton* mute,
                                  juce::TextButton* solo,
                                  juce::Rectangle<int>* meterOut,
                                  juce::Rectangle<int>* levelTextOut)
    {
        col = col.reduced(4);
        col.removeFromTop(20); // name row (painted)

        // Live signal-level readout row (painted), just below the name.
        if (levelTextOut != nullptr)
            *levelTextOut = col.removeFromTop(14);
        col.removeFromTop(2);

        auto buttonRow = col.removeFromTop(22);
        const auto buttonW = (buttonRow.getWidth() - 4) / 2;
        if (mute != nullptr) mute->setBounds(buttonRow.removeFromLeft(buttonW));
        buttonRow.removeFromLeft(4);
        if (solo != nullptr) solo->setBounds(buttonRow.removeFromLeft(buttonW));

        col.removeFromTop(6);
        if (meterOut != nullptr)
        {
            // Reserve a thin meter column on the right for the master strip.
            *meterOut = col.removeFromRight(14);
            col.removeFromRight(6);
        }
        if (volume != nullptr) volume->setBounds(col);
    };

    auto stripArea = inner;
    auto masterCol = stripArea.removeFromRight(masterStripWidth);
    stripArea.removeFromRight(stripGap);

    int x = stripArea.getX();
    for (auto& strip : strips)
    {
        juce::Rectangle<int> col(x, stripArea.getY(), stripWidth, stripArea.getHeight());
        if (strip != nullptr)
            layoutStripControls(col, strip->volume.get(), strip->mute.get(), strip->solo.get(),
                                &strip->meterBounds, &strip->levelTextBounds);
        x += stripWidth + stripGap;
    }

    layoutStripControls(masterCol, &masterVolume, nullptr, nullptr, &masterMeterBounds, &masterLevelTextBounds);
}

bool MixerPanelComponent::hitTest(int, int)
{
    // Capture the whole area so clicks outside the panel can dismiss it, while
    // child controls still receive their own events.
    return true;
}

void MixerPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    if (getCloseButtonBounds().contains(event.getPosition()))
    {
        closePanel();
        return;
    }

    // Clicking the dimmed area outside the panel closes the mixer.
    if (! getPanelBounds().contains(event.getPosition()))
        closePanel();
}

juce::Rectangle<int> MixerPanelComponent::getPanelBounds() const
{
    auto area = getLocalBounds().reduced(40);
    // Cap the panel to a sensible size so it floats rather than filling the window.
    const auto maxWidth = juce::jmin(area.getWidth(), 920);
    const auto maxHeight = juce::jmin(area.getHeight(), 520);
    return area.withSizeKeepingCentre(maxWidth, maxHeight);
}

juce::Rectangle<int> MixerPanelComponent::getCloseButtonBounds() const
{
    const auto panel = getPanelBounds();
    return juce::Rectangle<int>(panel.getRight() - panelPadding - 24, panel.getY() + panelPadding, 24, titleHeight);
}
}  // namespace orion
