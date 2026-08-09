#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>

namespace orion::camera
{
// Returns the index fingertip in normalised top-left coordinates, or no value when
// Vision cannot confidently see a hand. The camera UI remains usable without it.
std::optional<juce::Point<float>> detectIndexTip (const juce::Image& frame);
}
