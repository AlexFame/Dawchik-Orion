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

    // Replaces the contents of projectState with the project stored in sourceFile.
    // Returns false (and sets errorMessage) if the file is missing or unparseable;
    // projectState is left untouched on failure.
    static bool loadFromFile(ProjectState& projectState,
                             const juce::File& sourceFile,
                             juce::String* errorMessage = nullptr);
};
}  // namespace orion
