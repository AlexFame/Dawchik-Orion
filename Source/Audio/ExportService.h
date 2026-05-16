#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>

#include "../Core/ProjectState.h"

namespace orion
{
class ExportService final
{
public:
    using PrepareCallback = std::function<void()>;
    using RenderCallback = std::function<void(juce::AudioBuffer<float>&, int, int, double, double)>;

    static bool exportToWav(const ProjectState& projectState,
                            bool loopEnabled,
                            int exportSampleRate,
                            const juce::File& destinationFile,
                            PrepareCallback prepareForExport,
                            RenderCallback renderOfflineBlock,
                            juce::String* errorMessage = nullptr);
};
}  // namespace orion
