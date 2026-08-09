#include "ChordWheelModel.h"

#include <cmath>

namespace orion::chordwheel
{
void Model::setKey (int rootPc, const std::array<int, 7>& pattern) noexcept
{
    keyRootPc = ((rootPc % 12) + 12) % 12;
    keyScale = pattern;
}

void Model::setChord (const chords::ChordSpec& chord) noexcept
{
    selected = chord;
    selected.rootPc = ((selected.rootPc % 12) + 12) % 12;
}

bool Model::isInWheel (float x, float y, float centreX, float centreY,
                       float outerRadius, float innerRadius) const noexcept
{
    const auto dx = x - centreX;
    const auto dy = y - centreY;
    const auto distance = std::sqrt (dx * dx + dy * dy);
    return distance >= innerRadius && distance <= outerRadius;
}

int Model::pitchClassAt (float x, float y, float centreX, float centreY,
                         float outerRadius, float innerRadius) const noexcept
{
    if (! isInWheel (x, y, centreX, centreY, outerRadius, innerRadius))
        return -1;

    auto angle = std::atan2 (x - centreX, centreY - y);
    if (angle < 0.0f)
        angle += juce::MathConstants<float>::twoPi;
    return static_cast<int> (std::round (angle / juce::MathConstants<float>::twoPi * 12.0f)) % 12;
}

chords::ChordSpec Model::chordForRoot (int pitchClass) const noexcept
{
    auto result = selected;
    result.rootPc = ((pitchClass % 12) + 12) % 12;
    return result;
}
}
