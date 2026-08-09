#pragma once

#include "ChordWheelModel.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace orion::chordwheel
{
// Fixed circular selector. The component owns only presentation and pointer tracking;
// the host decides whether a selected chord is auditioned, committed, or dragged to the grid.
class Component final : public juce::Component
{
public:
    Component();

    void setKey (int rootPc, const std::array<int, 7>& pattern);
    void setChord (const chords::ChordSpec& chord);
    const chords::ChordSpec& getChord() const noexcept { return model.chord(); }

    // Used by camera and other pointing sources. Coordinates are local component coordinates.
    // Passing an invalid point clears the hover without changing the selected chord.
    void setPointingPosition (juce::Point<float> position);
    void clearPointing() noexcept;

    std::function<void (const chords::ChordSpec&)> onRootPointed;
    std::function<void (const chords::ChordSpec&)> onRootSelected;
    std::function<void()> onPointingExited;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    int pitchClassAt (juce::Point<float>) const noexcept;
    void updatePointing (juce::Point<float>, bool notify);

    Model model;
    int hoveredPc { -1 };
    bool pointingInside { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Component)
};
}
