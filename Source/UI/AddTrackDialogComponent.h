#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace orion
{
// Modern, rounded "Add Track" dialog overlay that fades/zooms in (no background dim).
// Four icon tab-cards (Audio / MIDI / Sampler / Folder) + Name, Count (with stepper),
// Color + Auto-Color, Output.
class AddTrackDialogComponent final : public juce::Component,
                                      private juce::Timer,
                                      private juce::ChangeListener
{
public:
    enum class TrackType { audio, midi, sampler, folder };

    struct Result
    {
        TrackType type { TrackType::audio };
        juce::String name;
        int count { 1 };
        juce::Colour colour;
        bool autoColour { true };
        int outputBus { -1 };       // -1 = Master, else bus index
    };

    AddTrackDialogComponent();

    std::function<void(const Result&)> onCreate;
    std::function<void()> onClose;

    void show(int existingTrackCount, const juce::StringArray& busNames);
    void closeDialog();

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void applyTypeToFields();
    void setCount(int newCount);

    void drawTabIcon(juce::Graphics& g, TrackType type, juce::Rectangle<float> area, juce::Colour colour) const;

    juce::Rectangle<int> getPanelBounds() const;
    juce::Rectangle<int> getTabBounds(int index) const;
    juce::Rectangle<int> getColourSwatchBounds() const;
    juce::Rectangle<int> getCountUpBounds() const;
    juce::Rectangle<int> getCountDownBounds() const;
    juce::Rectangle<int> getCloseBounds() const;

    TrackType type { TrackType::audio };
    int trackCountAtOpen { 0 };

    juce::TextEditor nameBox;
    juce::TextEditor countBox;
    juce::ComboBox outputCombo;
    juce::ToggleButton autoColourToggle { "Auto-Color" };
    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton createButton { "Create" };
    juce::Colour chosenColour { juce::Colour(0xffe8401f) };

    float anim { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AddTrackDialogComponent)
};
}  // namespace orion
