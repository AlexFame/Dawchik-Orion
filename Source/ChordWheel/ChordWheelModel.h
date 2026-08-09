#pragma once

#include "../Core/ChordTheory.h"
#include <array>

namespace orion::chordwheel
{
// Pure state and geometry for the pointing wheel. It deliberately has no JUCE Component
// dependency so camera, mouse and future MIDI/OSC inputs can share the same selection rules.
class Model
{
public:
    void setKey (int rootPc, const std::array<int, 7>& pattern) noexcept;
    void setChord (const chords::ChordSpec& chord) noexcept;

    int keyRoot() const noexcept { return keyRootPc; }
    const std::array<int, 7>& keyPattern() const noexcept { return keyScale; }
    const chords::ChordSpec& chord() const noexcept { return selected; }

    int pitchClassAt (float x, float y, float centreX, float centreY,
                      float outerRadius, float innerRadius) const noexcept;
    bool isInWheel (float x, float y, float centreX, float centreY,
                    float outerRadius, float innerRadius) const noexcept;
    chords::ChordSpec chordForRoot (int pitchClass) const noexcept;

private:
    int keyRootPc { 0 };
    std::array<int, 7> keyScale { { 0, 2, 3, 5, 7, 8, 10 } };
    chords::ChordSpec selected;
};
}
