#include "ExportService.h"

namespace orion
{
bool ExportService::exportToWav(const ProjectState& projectState,
                                bool loopEnabled,
                                int exportSampleRate,
                                const juce::File& destinationFile,
                                PrepareCallback prepareForExport,
                                RenderCallback renderOfflineBlock,
                                juce::String* errorMessage)
{
    if (renderOfflineBlock == nullptr)
    {
        if (errorMessage != nullptr)
            *errorMessage = "playback renderer unavailable";
        return false;
    }

    constexpr int exportChannels = 2;
    constexpr int exportBlockSize = 8192;
    const auto exportRate = static_cast<double>(exportSampleRate);
    if (exportRate <= 0.0 || projectState.getTempoBpm() <= 0.0)
    {
        if (errorMessage != nullptr)
            *errorMessage = "invalid export sample rate or tempo";
        return false;
    }

    if (prepareForExport != nullptr)
        prepareForExport();

    const auto exportStartBeat = (loopEnabled && projectState.hasLoopRange())
        ? projectState.getLoopStartBeat()
        : 0.0;
    const auto exportEndBeat = (loopEnabled && projectState.hasLoopRange())
        ? projectState.getLoopEndBeat()
        : projectState.getContentEndInBeats();
    const auto exportLengthBeats = juce::jmax(1.0, exportEndBeat - exportStartBeat);
    const auto totalSeconds = exportLengthBeats * 60.0 / projectState.getTempoBpm();
    const auto totalSamples = juce::jmax(1, static_cast<int>(std::ceil(totalSeconds * exportRate)));

    auto outputStream = destinationFile.createOutputStream();
    if (outputStream == nullptr)
    {
        if (errorMessage != nullptr)
            *errorMessage = "could not create file";
        return false;
    }

    juce::WavAudioFormat wavFormat;
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(
        wavFormat.createWriterFor(outputStream.get(),
                                  exportRate,
                                  static_cast<unsigned int>(exportChannels),
                                  24,
                                  {},
                                  0));

    if (writer == nullptr)
    {
        if (errorMessage != nullptr)
            *errorMessage = "could not create WAV writer";
        return false;
    }

    outputStream.release();
    juce::AudioBuffer<float> renderBuffer(exportChannels, exportBlockSize);
    auto renderedSamples = 0;

    while (renderedSamples < totalSamples)
    {
        const auto blockSamples = juce::jmin(exportBlockSize, totalSamples - renderedSamples);
        renderBuffer.clear();

        const auto blockStartBeat = exportStartBeat
            + static_cast<double>(renderedSamples) * (projectState.getTempoBpm() / 60.0) / exportRate;
        renderOfflineBlock(renderBuffer, 0, blockSamples, blockStartBeat, exportRate);

        if (! writer->writeFromAudioSampleBuffer(renderBuffer, 0, blockSamples))
        {
            if (errorMessage != nullptr)
                *errorMessage = "failed while writing audio";
            return false;
        }

        renderedSamples += blockSamples;
    }

    writer.reset();
    return true;
}
}  // namespace orion
