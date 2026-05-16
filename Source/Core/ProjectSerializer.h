#pragma once

#include <juce_core/juce_core.h>

#include "ProjectState.h"

namespace orion
{
class ProjectSerializer final
{
public:
    static bool saveToFile(const ProjectState& projectState,
                           const juce::File& destinationFile,
                           juce::String* errorMessage = nullptr);
};
}  // namespace orion
