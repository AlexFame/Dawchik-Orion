#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace orion
{
// A modern, searchable plugin picker overlay (instruments or effects). Dim background +
// centred rounded panel, a search field, and a styled scrollable list. Click a row to pick.
class PluginPickerComponent final : public juce::Component,
                                    private juce::ListBoxModel,
                                    private juce::TextEditor::Listener,
                                    private juce::Timer
{
public:
    PluginPickerComponent();

    std::function<void(const juce::PluginDescription&)> onPick;
    std::function<void()> onRescan;
    std::function<void()> onClose;

    // Populate + show. `title` e.g. "Load Instrument" / "Add Effect".
    void show(const juce::String& title, juce::Array<juce::PluginDescription> plugins, bool scanning);
    void closePicker();

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& event) override;
    void returnKeyPressed(int lastRowSelected) override;

    void textEditorTextChanged(juce::TextEditor& editor) override;
    void timerCallback() override;

    void pickRow(int row);
    void rebuildFiltered();
    juce::Rectangle<int> getPanelBounds() const;
    juce::Rectangle<int> getCloseBounds() const;
    juce::Rectangle<int> getRescanBounds() const;

    juce::String titleText { "Load Instrument" };
    juce::Array<juce::PluginDescription> allPlugins;
    std::vector<int> filtered;          // indices into allPlugins
    bool isScanning { false };

    juce::TextEditor searchBox;
    juce::ListBox list;
    float anim { 0.0f };   // fade/zoom-in progress

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginPickerComponent)
};
}  // namespace orion
