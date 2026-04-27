#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace orion
{
class MainComponent final : public juce::Component
{
public:
    MainComponent();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::Label headerLabel;
    juce::Label transportLabel;
    juce::Label playlistLabel;
    juce::Label pianoRollLabel;
    juce::TextButton newProjectButton;
    juce::TextButton browserButton;
    juce::TextButton scanPluginsButton;
};
}  // namespace orion
