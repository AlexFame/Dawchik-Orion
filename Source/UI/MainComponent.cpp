#include "MainComponent.h"

namespace
{
const auto backgroundColour = juce::Colour(0xff0b0f12);
const auto panelColour = juce::Colour(0xff131a20);
const auto accentColour = juce::Colour(0xffeb6f3a);
const auto gridColour = juce::Colour(0xff202a33);
}  // namespace

namespace orion
{
MainComponent::MainComponent()
{
    headerLabel.setText("Orion", juce::dontSendNotification);
    headerLabel.setFont(juce::FontOptions(28.0f, juce::Font::bold));
    headerLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(headerLabel);

    transportLabel.setText("Transport: play, stop, tempo, export", juce::dontSendNotification);
    transportLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(transportLabel);

    playlistLabel.setText("Playlist: arrangement-first timeline for clips and loops", juce::dontSendNotification);
    playlistLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(playlistLabel);

    pianoRollLabel.setText("Piano Roll: scales, snap, left-click add, right-click delete", juce::dontSendNotification);
    pianoRollLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(pianoRollLabel);

    newProjectButton.setButtonText("New Project");
    browserButton.setButtonText("Loop Browser");
    scanPluginsButton.setButtonText("Scan VST3");

    for (auto* button : { &newProjectButton, &browserButton, &scanPluginsButton })
    {
        button->setColour(juce::TextButton::buttonColourId, accentColour);
        button->setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible(*button);
    }
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto bounds = getLocalBounds().reduced(24);
    auto topStrip = bounds.removeFromTop(92);

    g.setColour(panelColour);
    g.fillRoundedRectangle(topStrip.toFloat(), 18.0f);

    auto lowerArea = bounds.withTrimmedTop(20);
    auto leftPanel = lowerArea.removeFromLeft(lowerArea.proportionOfWidth(0.68f));
    auto rightPanel = lowerArea.reduced(0, 0);

    g.setColour(panelColour);
    g.fillRoundedRectangle(leftPanel.toFloat(), 20.0f);
    g.fillRoundedRectangle(rightPanel.toFloat(), 20.0f);

    auto playlistArea = leftPanel.reduced(24);
    auto playlistHeader = playlistArea.removeFromTop(40);

    g.setColour(juce::Colours::white.withAlpha(0.9f));
    g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    g.drawText("Arrangement", playlistHeader, juce::Justification::centredLeft);

    auto gridArea = playlistArea.reduced(0, 12);
    g.setColour(gridColour);

    for (int row = 0; row < 8; ++row)
    {
        const auto y = gridArea.getY() + row * gridArea.getHeight() / 8;
        g.drawLine(static_cast<float>(gridArea.getX()), static_cast<float>(y), static_cast<float>(gridArea.getRight()), static_cast<float>(y), 1.0f);
    }

    for (int column = 0; column < 16; ++column)
    {
        const auto x = gridArea.getX() + column * gridArea.getWidth() / 16;
        g.drawLine(static_cast<float>(x), static_cast<float>(gridArea.getY()), static_cast<float>(x), static_cast<float>(gridArea.getBottom()), column % 4 == 0 ? 2.0f : 1.0f);
    }

    g.setColour(accentColour.withAlpha(0.88f));
    g.fillRoundedRectangle(gridArea.getX() + 40.0f, gridArea.getY() + 36.0f, 220.0f, 42.0f, 10.0f);
    g.fillRoundedRectangle(gridArea.getX() + 200.0f, gridArea.getY() + 124.0f, 300.0f, 42.0f, 10.0f);
    g.fillRoundedRectangle(gridArea.getX() + 110.0f, gridArea.getY() + 212.0f, 180.0f, 42.0f, 10.0f);

    auto inspector = rightPanel.reduced(24);

    g.setColour(juce::Colours::white.withAlpha(0.9f));
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Creation Focus", inspector.removeFromTop(36), juce::Justification::centredLeft);

    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.fillRoundedRectangle(inspector.removeFromTop(124).toFloat(), 14.0f);
    inspector.removeFromTop(18);
    g.fillRoundedRectangle(inspector.removeFromTop(180).toFloat(), 14.0f);
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(40);
    auto topStrip = bounds.removeFromTop(92).reduced(24, 18);

    headerLabel.setBounds(topStrip.removeFromLeft(280));

    auto actions = topStrip.removeFromRight(420);
    newProjectButton.setBounds(actions.removeFromLeft(128).reduced(4, 6));
    browserButton.setBounds(actions.removeFromLeft(128).reduced(4, 6));
    scanPluginsButton.setBounds(actions.removeFromLeft(128).reduced(4, 6));

    auto lowerArea = bounds.withTrimmedTop(20);
    lowerArea.removeFromLeft(lowerArea.proportionOfWidth(0.68f));
    auto rightPanel = lowerArea.reduced(24);

    transportLabel.setBounds(rightPanel.removeFromTop(32));
    playlistLabel.setBounds(rightPanel.removeFromTop(32));
    pianoRollLabel.setBounds(rightPanel.removeFromTop(32));
}
}  // namespace orion
