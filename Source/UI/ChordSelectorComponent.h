#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Core/ChordTheory.h"
#include <array>
#include <functional>
#include <vector>

// A self-contained chord picker: root wheel + quality + extensions + a live keyboard preview,
// seeded with the seven diatonic chords of the project key. Clicking a chord auditions it;
// dragging it out drops the chord onto the grid (wired by the host).
class ChordSelectorComponent final : public juce::Component
{
public:
    ChordSelectorComponent();

    void setProjectKey (int rootPc, const std::array<int, 7>& pattern, const juce::String& keyName);
    void setChord (const orion::chords::ChordSpec& newSpec, bool audition);
    orion::chords::ChordSpec getChord() const noexcept { return spec; }
    std::vector<int> currentPitches() const;

    std::function<void (const std::vector<int>&)> onAudition;                 // play the chord
    std::function<void (const orion::chords::ChordSpec&)> onChordChanged;     // spec changed
    std::function<void (const std::vector<int>&)> onDragChordOut;             // begin drag-to-grid
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> panelBounds() const;
    juce::Rectangle<int> headerBounds() const;
    juce::Rectangle<int> wheelBounds() const;
    juce::Rectangle<int> qualityRowBounds() const;
    juce::Rectangle<int> extensionGridBounds() const;
    juce::Rectangle<int> diatonicRowBounds() const;
    juce::Rectangle<int> keyboardBounds() const;
    juce::Rectangle<int> closeButtonBounds() const;

    std::array<juce::Rectangle<int>, 8>  qualityRects() const;
    std::array<juce::Rectangle<int>, 12> extensionRects() const;
    std::array<juce::Rectangle<int>, 7>  diatonicRects() const;
    int wheelPcAtPoint (juce::Point<int> p) const;   // -1 if outside the ring

    void commitChange (bool audition);

    orion::chords::ChordSpec spec;
    int keyRootPc { 0 };
    std::array<int, 7> keyPattern { { 0, 2, 3, 5, 7, 8, 10 } };
    juce::String keyName { "C Minor" };

    int hoverPc { -1 };
    bool dragStarted { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordSelectorComponent)
};
