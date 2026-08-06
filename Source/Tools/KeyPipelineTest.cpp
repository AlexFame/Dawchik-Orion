#include "Audio/WarpEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#include <array>
#include <cmath>
#include <iostream>

namespace
{
struct LoadedAudio
{
    juce::AudioBuffer<float> buffer;
    double sampleRate { 0.0 };
};

LoadedAudio loadAudio(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return {};

    LoadedAudio result;
    result.sampleRate = reader->sampleRate;
    result.buffer.setSize(static_cast<int>(reader->numChannels),
                          static_cast<int>(reader->lengthInSamples));
    reader->read(&result.buffer, 0, result.buffer.getNumSamples(), 0, true, true);
    return result;
}

std::array<double, 12> chromaFor(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    std::array<double, 12> chroma {};
    if (audio.getNumSamples() < 4096 || audio.getNumChannels() == 0 || sampleRate <= 0.0)
        return chroma;

    constexpr int frameSize = 4096;
    constexpr int hop = 4096;
    constexpr int lowestMidi = 36;
    constexpr int highestMidi = 95;
    const int frames = juce::jmax(1, (audio.getNumSamples() - frameSize) / hop + 1);
    const int frameStride = juce::jmax(1, frames / 32);
    const auto twoPi = juce::MathConstants<double>::twoPi;

    for (int start = 0, frameIndex = 0; start + frameSize <= audio.getNumSamples(); start += hop, ++frameIndex)
    {
        if (frameIndex % frameStride != 0)
            continue;

        for (int midi = lowestMidi; midi <= highestMidi; ++midi)
        {
            const double frequency = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
            const double omega = twoPi * frequency / sampleRate;
            double real = 0.0;
            double imaginary = 0.0;
            for (int i = 0; i < frameSize; ++i)
            {
                double mono = 0.0;
                for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                    mono += audio.getSample(channel, start + i);
                mono /= audio.getNumChannels();
                const double window = 0.5 - 0.5 * std::cos(twoPi * i / (frameSize - 1));
                real += mono * window * std::cos(omega * i);
                imaginary -= mono * window * std::sin(omega * i);
            }
            chroma[static_cast<std::size_t>(midi % 12)] += std::hypot(real, imaginary);
        }
    }

    double sum = 0.0;
    for (const auto value : chroma)
        sum += value;
    if (sum > 0.0)
        for (auto& value : chroma)
            value /= sum;
    return chroma;
}

int measuredShift(const std::array<double, 12>& source, const std::array<double, 12>& rendered)
{
    int bestShift = 0;
    double bestScore = -1.0;
    for (int shift = -6; shift <= 6; ++shift)
    {
        double score = 0.0;
        for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
        {
            const int shifted = (pitchClass + shift + 24) % 12;
            score += source[static_cast<std::size_t>(pitchClass)]
                   * rendered[static_cast<std::size_t>(shifted)];
        }
        if (score > bestScore)
        {
            bestScore = score;
            bestShift = shift;
        }
    }
    return bestShift;
}

juce::AudioBuffer<float> renderStreaming(const juce::AudioBuffer<float>& source,
                                         int outputSamples,
                                         double sampleRate,
                                         int semitones)
{
    const int channels = juce::jlimit(1, 2, source.getNumChannels());
    juce::AudioBuffer<float> output(channels, outputSamples);
    output.clear();

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(channels, static_cast<float>(sampleRate));
    stretch.setTransposeSemitones(static_cast<float>(semitones));

    const double inputRatio = static_cast<double>(source.getNumSamples())
                            / juce::jmax(1, outputSamples);
    int inputPosition = 0;
    const int seekLength = juce::jmin(source.getNumSamples(),
                                     stretch.outputSeekLength(static_cast<float>(inputRatio)));
    if (seekLength > 0)
    {
        const float* inputPointers[2] = {
            source.getReadPointer(0),
            channels > 1 ? source.getReadPointer(juce::jmin(1, source.getNumChannels() - 1)) : nullptr
        };
        stretch.outputSeek(inputPointers, seekLength);
        inputPosition = seekLength;
    }

    juce::AudioBuffer<float> scratch(channels, 4096);
    int produced = 0;
    while (produced < outputSamples)
    {
        const int outputChunk = juce::jmin(2048, outputSamples - produced);
        const int inputChunk = juce::jlimit(1, scratch.getNumSamples(),
            static_cast<int>(std::llround(outputChunk * inputRatio)));
        for (int channel = 0; channel < channels; ++channel)
        {
            auto* destination = scratch.getWritePointer(channel);
            const int sourceChannel = juce::jmin(channel, source.getNumChannels() - 1);
            for (int i = 0; i < inputChunk; ++i)
            {
                const int sourceIndex = inputPosition + i;
                destination[i] = sourceIndex < source.getNumSamples()
                    ? source.getSample(sourceChannel, sourceIndex) : 0.0f;
            }
        }
        inputPosition += inputChunk;

        const float* inputPointers[2] = {
            scratch.getReadPointer(0), channels > 1 ? scratch.getReadPointer(1) : nullptr
        };
        float* outputPointers[2] = {
            output.getWritePointer(0) + produced,
            channels > 1 ? output.getWritePointer(1) + produced : nullptr
        };
        stretch.process(inputPointers, inputChunk, outputPointers, outputChunk);
        produced += outputChunk;
    }
    return output;
}
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: OrionKeyPipelineTest <folder>\n";
        return 1;
    }

    const juce::File folder(juce::String::fromUTF8(argv[1]));
    auto files = folder.findChildFiles(juce::File::findFiles, true, "*.wav;*.aif;*.aiff");
    files.sort();

    int tested = 0;
    int passed = 0;
    int unknown = 0;
    for (const auto& file : files)
    {
        const auto metadata = orion::analyzeAudioWarpMetadata(file, 126.0, 4, false);
        if (metadata.sourceKeyRoot < 0)
        {
            ++unknown;
            std::cout << "UNKNOWN," << file.getFileName() << '\n';
            continue;
        }

        const int expected = orion::computeKeyMatchSemitones(metadata.sourceKeyRoot,
                                                              metadata.sourceKeyIsMinor,
                                                              0, true);
        const auto source = loadAudio(file);
        if (source.buffer.getNumSamples() == 0)
            continue;

        const double sourceBpm = metadata.sourceBpm > 0.0 ? metadata.sourceBpm : 126.0;
        const int targetSamples = juce::jmax(1, static_cast<int>(std::llround(
            source.buffer.getNumSamples() * sourceBpm / 126.0)));
        const double pitchScale = std::pow(2.0, expected / 12.0);
        const auto offline = orion::stretchBufferToLengthWithExperimentalBackend(
            source.buffer, targetSamples, source.sampleRate,
            file.getFullPathName(), pitchScale);
        const auto streaming = renderStreaming(source.buffer, targetSamples, source.sampleRate, expected);
        const auto sourceChroma = chromaFor(source.buffer, source.sampleRate);
        const int offlineShift = measuredShift(sourceChroma, chromaFor(offline, source.sampleRate));
        const int streamingShift = measuredShift(sourceChroma, chromaFor(streaming, source.sampleRate));
        const bool ok = offlineShift == expected && streamingShift == expected;
        ++tested;
        if (ok)
            ++passed;
        else
            std::cout << "FAIL," << file.getFileName() << ",key="
                      << orion::formatKeyName(metadata.sourceKeyRoot, metadata.sourceKeyIsMinor)
                      << ",expected=" << expected << ",offline=" << offlineShift
                      << ",streaming=" << streamingShift
                      << ",sourceBpm=" << sourceBpm << ",samples=" << targetSamples << '\n';
    }

    std::cout << "SUMMARY,tested=" << tested << ",passed=" << passed
              << ",failed=" << (tested - passed) << ",unknown=" << unknown << '\n';
    return tested == passed && unknown == 0 ? 0 : 2;
}
