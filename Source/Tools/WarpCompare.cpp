#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#if ORION_HAVE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
std::vector<int> detectOnsets(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    std::vector<int> onsets;
    const auto numSamples = buffer.getNumSamples();
    const auto numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return onsets;

    const auto windowSize = juce::jmax(1, static_cast<int>(sampleRate * 0.002));
    const auto envelopeSize = numSamples / windowSize;
    if (envelopeSize < 3)
        return onsets;

    std::vector<float> envelope(static_cast<size_t>(envelopeSize), 0.0f);
    for (int i = 0; i < envelopeSize; ++i)
    {
        float energy = 0.0f;
        const auto start = i * windowSize;
        const auto end = juce::jmin(start + windowSize, numSamples);
        for (int s = start; s < end; ++s)
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto v = buffer.getSample(ch, s);
                energy += v * v;
            }
        envelope[static_cast<size_t>(i)] = energy / static_cast<float>((end - start) * numChannels);
    }

    float maxOnset = 0.0f;
    std::vector<float> onsetFn(static_cast<size_t>(envelopeSize), 0.0f);
    for (int i = 1; i < envelopeSize; ++i)
    {
        onsetFn[static_cast<size_t>(i)] = juce::jmax(0.0f, envelope[static_cast<size_t>(i)] - envelope[static_cast<size_t>(i - 1)]);
        maxOnset = juce::jmax(maxOnset, onsetFn[static_cast<size_t>(i)]);
    }

    if (maxOnset <= 0.0f)
        return onsets;

    const auto threshold = maxOnset * 0.15f;
    const auto minGapWindows = juce::jmax(1, static_cast<int>(sampleRate * 0.03 / windowSize));
    int lastPeak = -minGapWindows;

    for (int i = 1; i < envelopeSize - 1; ++i)
    {
        if (onsetFn[static_cast<size_t>(i)] > threshold
            && onsetFn[static_cast<size_t>(i)] >= onsetFn[static_cast<size_t>(i - 1)]
            && onsetFn[static_cast<size_t>(i)] >= onsetFn[static_cast<size_t>(i + 1)]
            && (i - lastPeak) >= minGapWindows)
        {
            onsets.push_back(i * windowSize);
            lastPeak = i;
        }
    }

    return onsets;
}

std::vector<int> detectStrongTransients(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    std::vector<int> transients;
    const auto numSamples = buffer.getNumSamples();
    const auto numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return transients;

    const auto windowSize = juce::jmax(1, static_cast<int>(sampleRate * 0.0015));
    const auto envelopeSize = numSamples / windowSize;
    if (envelopeSize < 3)
        return transients;

    std::vector<float> envelope(static_cast<size_t>(envelopeSize), 0.0f);
    for (int i = 0; i < envelopeSize; ++i)
    {
        float peak = 0.0f;
        const auto start = i * windowSize;
        const auto end = juce::jmin(start + windowSize, numSamples);
        for (int s = start; s < end; ++s)
            for (int ch = 0; ch < numChannels; ++ch)
                peak = juce::jmax(peak, std::abs(buffer.getSample(ch, s)));

        envelope[static_cast<size_t>(i)] = peak;
    }

    float maxRise = 0.0f;
    std::vector<float> rise(static_cast<size_t>(envelopeSize), 0.0f);
    for (int i = 1; i < envelopeSize; ++i)
    {
        rise[static_cast<size_t>(i)] = juce::jmax(0.0f, envelope[static_cast<size_t>(i)] - envelope[static_cast<size_t>(i - 1)]);
        maxRise = juce::jmax(maxRise, rise[static_cast<size_t>(i)]);
    }

    if (maxRise <= 0.0f)
        return transients;

    const auto threshold = maxRise * 0.45f;
    const auto minPeakLevel = 0.12f;
    const auto minGapWindows = juce::jmax(1, static_cast<int>(sampleRate * 0.08 / windowSize));
    int lastPeak = -minGapWindows;

    for (int i = 1; i < envelopeSize - 1; ++i)
    {
        if (rise[static_cast<size_t>(i)] > threshold
            && envelope[static_cast<size_t>(i)] >= minPeakLevel
            && rise[static_cast<size_t>(i)] >= rise[static_cast<size_t>(i - 1)]
            && rise[static_cast<size_t>(i)] >= rise[static_cast<size_t>(i + 1)]
            && (i - lastPeak) >= minGapWindows)
        {
            transients.push_back(i * windowSize);
            lastPeak = i;
        }
    }

    return transients;
}

void logTransientPreview(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    const auto transients = detectStrongTransients(source, sampleRate);
    const auto ratio = source.getNumSamples() > 0
        ? static_cast<double>(outputSamples) / static_cast<double>(source.getNumSamples())
        : 1.0;

    std::cout << "StrongTransients count=" << transients.size() << '\n';
    std::cout << "StrongTransients originalFirst10=";
    for (size_t i = 0; i < juce::jmin<size_t>(10, transients.size()); ++i)
        std::cout << (i == 0 ? "" : ",") << transients[i];
    std::cout << '\n';

    std::cout << "StrongTransients mappedFirst10=";
    for (size_t i = 0; i < juce::jmin<size_t>(10, transients.size()); ++i)
        std::cout << (i == 0 ? "" : ",")
                  << juce::jlimit(0, outputSamples - 1, static_cast<int>(std::round(static_cast<double>(transients[i]) * ratio)));
    std::cout << '\n';
}

juce::AudioBuffer<float> exactLengthCopy(juce::AudioBuffer<float> buffer, int targetSamples)
{
    if (buffer.getNumSamples() == targetSamples)
        return buffer;

    juce::AudioBuffer<float> adjusted(buffer.getNumChannels(), targetSamples);
    adjusted.clear();
    const auto samplesToCopy = juce::jmin(buffer.getNumSamples(), targetSamples);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        adjusted.copyFrom(ch, 0, buffer, ch, 0, samplesToCopy);
    return adjusted;
}

juce::AudioBuffer<float> sliceStretchBuffer(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    const auto numSamples = source.getNumSamples();
    const auto numChannels = source.getNumChannels();

    auto onsets = detectOnsets(source, sampleRate);
    if (onsets.empty() || onsets.front() != 0)
        onsets.insert(onsets.begin(), 0);
    if (onsets.back() != numSamples)
        onsets.push_back(numSamples);

    const auto ratio = static_cast<double>(outputSamples) / static_cast<double>(numSamples);
    constexpr int fadeLen = 64;

    juce::AudioBuffer<float> output(numChannels, outputSamples);
    output.clear();

    for (size_t i = 0; i + 1 < onsets.size(); ++i)
    {
        const auto srcStart = onsets[i];
        const auto sliceLen = onsets[i + 1] - srcStart;
        if (sliceLen <= 0)
            continue;

        const auto dstStart = juce::jlimit(0, outputSamples, static_cast<int>(std::round(static_cast<double>(srcStart) * ratio)));
        const auto nextDst = (i + 2 < onsets.size())
            ? juce::jlimit(0, outputSamples, static_cast<int>(std::round(static_cast<double>(onsets[i + 1]) * ratio)))
            : outputSamples;
        const auto space = nextDst - dstStart;
        if (space <= 0)
            continue;

        const auto copyLen = juce::jmin(sliceLen, juce::jmin(space, outputSamples - dstStart));
        if (copyLen <= 0)
            continue;

        for (int ch = 0; ch < numChannels; ++ch)
            output.copyFrom(ch, dstStart, source, ch, srcStart, copyLen);

        const auto actualFade = juce::jmin(fadeLen, copyLen);
        if (actualFade > 1)
        {
            const auto fadeStart = dstStart + copyLen - actualFade;
            for (int f = 0; f < actualFade; ++f)
            {
                const auto gain = 1.0f - static_cast<float>(f) / static_cast<float>(actualFade);
                for (int ch = 0; ch < numChannels; ++ch)
                    output.setSample(ch, fadeStart + f, output.getSample(ch, fadeStart + f) * gain);
            }
        }
    }

    return output;
}

juce::AudioBuffer<float> slicePrerollExperimentBuffer(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    const auto numSamples = source.getNumSamples();
    auto onsets = detectOnsets(source, sampleRate);

    if (onsets.empty() || onsets.front() != 0)
        onsets.insert(onsets.begin(), 0);
    if (onsets.back() != numSamples)
        onsets.push_back(numSamples);

    const auto preRollSamples = juce::jmax(1, static_cast<int>(std::round(sampleRate * 0.005)));
    for (size_t i = 1; i + 1 < onsets.size(); ++i)
        onsets[i] = juce::jlimit(0, numSamples, onsets[i] - preRollSamples);

    std::sort(onsets.begin(), onsets.end());
    onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());

    juce::AudioBuffer<float> output(source.getNumChannels(), outputSamples);
    output.clear();

    const auto ratio = static_cast<double>(outputSamples) / static_cast<double>(numSamples);
    const auto preservedAttackSamples = juce::jmax(1, static_cast<int>(std::round(sampleRate * 0.015)));
    constexpr int fadeLen = 64;

    for (size_t i = 0; i + 1 < onsets.size(); ++i)
    {
        const auto srcStart = onsets[i];
        const auto sliceLen = onsets[i + 1] - srcStart;
        const auto dstStart = juce::jlimit(0, outputSamples, static_cast<int>(std::round(static_cast<double>(srcStart) * ratio)));
        const auto nextDst = (i + 2 < onsets.size())
            ? juce::jlimit(0, outputSamples, static_cast<int>(std::round(static_cast<double>(onsets[i + 1]) * ratio)))
            : outputSamples;
        const auto copyLen = juce::jmin(sliceLen, juce::jmin(nextDst - dstStart, outputSamples - dstStart));
        if (copyLen <= 0)
            continue;

        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            output.copyFrom(ch, dstStart, source, ch, srcStart, copyLen);

        const auto fadeAvailable = juce::jmax(0, copyLen - preservedAttackSamples);
        const auto actualFade = juce::jmin(fadeLen, fadeAvailable);
        if (actualFade > 1)
        {
            const auto fadeStart = dstStart + copyLen - actualFade;
            for (int f = 0; f < actualFade; ++f)
            {
                const auto gain = 1.0f - static_cast<float>(f) / static_cast<float>(actualFade);
                for (int ch = 0; ch < source.getNumChannels(); ++ch)
                    output.setSample(ch, fadeStart + f, output.getSample(ch, fadeStart + f) * gain);
            }
        }
    }

    return output;
}

void addWithCrossfade(juce::AudioBuffer<float>& output,
                      const juce::AudioBuffer<float>& source,
                      int sourceStart,
                      int destinationStart,
                      int samplesToCopy,
                      int crossfadeSamples)
{
    if (samplesToCopy <= 0)
        return;

    const auto channels = juce::jmin(output.getNumChannels(), source.getNumChannels());
    const auto sourceSamples = source.getNumSamples();
    const auto outputSamples = output.getNumSamples();
    samplesToCopy = juce::jmin(samplesToCopy, juce::jmin(sourceSamples - sourceStart, outputSamples - destinationStart));
    if (samplesToCopy <= 0)
        return;

    const auto fadeLen = juce::jmin(crossfadeSamples, samplesToCopy);
    for (int channel = 0; channel < channels; ++channel)
    {
        auto* destination = output.getWritePointer(channel, destinationStart);
        const auto* sourceData = source.getReadPointer(channel, sourceStart);
        for (int sample = 0; sample < samplesToCopy; ++sample)
        {
            auto gain = 1.0f;
            if (destinationStart > 0 && sample < fadeLen)
                gain = static_cast<float>(sample) / static_cast<float>(fadeLen);

            destination[sample] = destination[sample] * (1.0f - gain) + sourceData[sample] * gain;
        }
    }
}

juce::AudioBuffer<float> drumBeatsTailfillV1Buffer(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    const auto sourceSamples = source.getNumSamples();
    const auto channels = source.getNumChannels();
    if (sourceSamples <= 0 || outputSamples <= 0 || channels <= 0 || sampleRate <= 0.0)
        return {};

    auto transients = detectStrongTransients(source, sampleRate);
    if (transients.empty() || transients.front() != 0)
        transients.insert(transients.begin(), 0);
    if (transients.back() != sourceSamples)
        transients.push_back(sourceSamples);

    std::sort(transients.begin(), transients.end());
    transients.erase(std::unique(transients.begin(), transients.end()), transients.end());

    const auto ratio = static_cast<double>(outputSamples) / static_cast<double>(sourceSamples);
    const auto attackSamples = juce::jmax(1, static_cast<int>(std::round(sampleRate * 0.020)));
    const auto crossfadeSamples = juce::jmax(1, static_cast<int>(std::round(sampleRate * 0.003)));

    juce::AudioBuffer<float> output(channels, outputSamples);
    output.clear();

    for (size_t i = 0; i + 1 < transients.size(); ++i)
    {
        const auto sourceStart = transients[i];
        const auto sourceEnd = transients[i + 1];
        const auto sourceSliceLength = sourceEnd - sourceStart;
        if (sourceSliceLength <= 0)
            continue;

        const auto destinationStart = juce::jlimit(0, outputSamples, static_cast<int>(std::round(static_cast<double>(sourceStart) * ratio)));
        const auto destinationEnd = juce::jlimit(0, outputSamples, static_cast<int>(std::round(static_cast<double>(sourceEnd) * ratio)));
        const auto destinationSliceLength = destinationEnd - destinationStart;
        if (destinationSliceLength <= 0)
            continue;

        const auto preservedAttack = juce::jmin(attackSamples, juce::jmin(sourceSliceLength, destinationSliceLength));
        addWithCrossfade(output, source, sourceStart, destinationStart, preservedAttack, crossfadeSamples);

        const auto tailSourceStart = sourceStart + preservedAttack;
        const auto tailSourceLength = sourceEnd - tailSourceStart;
        const auto tailDestinationStart = destinationStart + preservedAttack;
        const auto tailDestinationLength = destinationEnd - tailDestinationStart;
        if (tailSourceLength <= 0 || tailDestinationLength <= 0)
            continue;

        auto writePosition = tailDestinationStart;
        while (writePosition < destinationEnd)
        {
            const auto relativeTailPosition = writePosition - tailDestinationStart;
            const auto sourceOffset = tailSourceStart + (relativeTailPosition % tailSourceLength);
            const auto availableFromSource = sourceEnd - sourceOffset;
            const auto framesToCopy = juce::jmin(availableFromSource, destinationEnd - writePosition);
            addWithCrossfade(output, source, sourceOffset, writePosition, framesToCopy, crossfadeSamples);
            writePosition += framesToCopy;
        }
    }

    return exactLengthCopy(std::move(output), outputSamples);
}

juce::AudioBuffer<float> signalsmithStretchBufferToLength(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    const auto channels = source.getNumChannels();
    juce::AudioBuffer<float> stretched(channels, outputSamples);
    stretched.clear();

    signalsmith::stretch::SignalsmithStretch<float> stretcher;
    stretcher.configure(channels, static_cast<int>(sampleRate * 0.08), static_cast<int>(sampleRate * 0.02));
    stretcher.setTransposeFactor(1.0f);

    std::vector<const float*> inputChannels(static_cast<std::size_t>(channels));
    std::vector<float*> outputChannels(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel)
    {
        inputChannels[static_cast<std::size_t>(channel)] = source.getReadPointer(channel);
        outputChannels[static_cast<std::size_t>(channel)] = stretched.getWritePointer(channel);
    }

    if (! stretcher.exact(inputChannels.data(), source.getNumSamples(), outputChannels.data(), outputSamples))
        return exactLengthCopy(source, outputSamples);

    return stretched;
}

juce::AudioBuffer<float> signalsmithTransientRestoreBuffer(const juce::AudioBuffer<float>& source,
                                                           int outputSamples,
                                                           double sampleRate,
                                                           double attackMs,
                                                           double crossfadeMs,
                                                           float blend,
                                                           bool replaceMode)
{
    auto restored = signalsmithStretchBufferToLength(source, outputSamples, sampleRate);
    restored = exactLengthCopy(std::move(restored), outputSamples);

    const auto sourceSamples = source.getNumSamples();
    const auto channels = juce::jmin(source.getNumChannels(), restored.getNumChannels());
    if (sourceSamples <= 0 || outputSamples <= 0 || channels <= 0 || sampleRate <= 0.0)
        return restored;

    const auto ratio = static_cast<double>(outputSamples) / static_cast<double>(sourceSamples);
    const auto attackSamples = juce::jmax(1, static_cast<int>(std::round(sampleRate * attackMs / 1000.0)));
    const auto crossfadeSamples = juce::jmax(1, static_cast<int>(std::round(sampleRate * crossfadeMs / 1000.0)));
    const auto transients = detectStrongTransients(source, sampleRate);

    for (const auto sourceStart : transients)
    {
        const auto destinationStart = juce::jlimit(0, outputSamples - 1, static_cast<int>(std::round(static_cast<double>(sourceStart) * ratio)));
        const auto copyLen = juce::jmin(attackSamples, juce::jmin(sourceSamples - sourceStart, outputSamples - destinationStart));
        if (copyLen <= 0)
            continue;

        for (int sample = 0; sample < copyLen; ++sample)
        {
            auto alpha = 1.0f;
            if (destinationStart > 0 && sample < crossfadeSamples)
                alpha = static_cast<float>(sample) / static_cast<float>(crossfadeSamples);
            if (sample >= copyLen - crossfadeSamples)
            {
                const auto fadeOut = static_cast<float>(copyLen - sample - 1) / static_cast<float>(crossfadeSamples);
                alpha = juce::jmin(alpha, juce::jlimit(0.0f, 1.0f, fadeOut));
            }

            const auto transientBlend = replaceMode ? juce::jmin(1.0f, alpha * blend) : alpha * blend;
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto existing = restored.getSample(ch, destinationStart + sample);
                const auto transient = source.getSample(ch, sourceStart + sample);
                const auto mixed = replaceMode
                    ? existing * (1.0f - transientBlend) + transient * transientBlend
                    : existing + (transient - existing) * transientBlend;
                restored.setSample(ch, destinationStart + sample, juce::jlimit(-1.0f, 1.0f, mixed));
            }
        }
    }

    return restored;
}

juce::AudioBuffer<float> signalsmithTransientRestoreSoftBuffer(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    return signalsmithTransientRestoreBuffer(source, outputSamples, sampleRate, 12.0, 5.0, 0.6f, false);
}

juce::AudioBuffer<float> signalsmithTransientRestoreMediumBuffer(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    return signalsmithTransientRestoreBuffer(source, outputSamples, sampleRate, 18.0, 3.0, 1.0f, false);
}

juce::AudioBuffer<float> signalsmithTransientRestoreHardBuffer(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    return signalsmithTransientRestoreBuffer(source, outputSamples, sampleRate, 25.0, 2.0, 1.3f, false);
}

juce::AudioBuffer<float> signalsmithTransientRestoreReplaceBuffer(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    return signalsmithTransientRestoreBuffer(source, outputSamples, sampleRate, 18.0, 2.0, 1.0f, true);
}

juce::AudioBuffer<float> rubberBandExp1StretchBufferToLength(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
#if ORION_HAVE_RUBBERBAND
    const auto channels = source.getNumChannels();
    const auto sourceSamples = source.getNumSamples();
    const auto timeRatio = static_cast<double>(outputSamples) / static_cast<double>(sourceSamples);

    RubberBand::RubberBandStretcher stretcher(
        static_cast<std::size_t>(std::round(sampleRate)),
        static_cast<std::size_t>(channels),
        RubberBand::RubberBandStretcher::OptionProcessOffline
            | RubberBand::RubberBandStretcher::OptionEngineFiner
            | RubberBand::RubberBandStretcher::OptionTransientsCrisp
            | RubberBand::RubberBandStretcher::OptionDetectorCompound
            | RubberBand::RubberBandStretcher::OptionChannelsTogether,
        timeRatio,
        1.0);

    stretcher.setExpectedInputDuration(static_cast<std::size_t>(sourceSamples));
    stretcher.setTimeRatio(timeRatio);
    stretcher.setPitchScale(1.0);

    std::vector<const float*> inputChannels(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel)
        inputChannels[static_cast<std::size_t>(channel)] = source.getReadPointer(channel);

    stretcher.study(inputChannels.data(), static_cast<std::size_t>(sourceSamples), true);
    stretcher.process(inputChannels.data(), static_cast<std::size_t>(sourceSamples), true);

    juce::AudioBuffer<float> stretched(channels, outputSamples);
    stretched.clear();

    std::vector<float*> outputChannels(static_cast<std::size_t>(channels));
    int writePosition = 0;
    int guard = 0;
    while (writePosition < outputSamples && ++guard < 4096)
    {
        const auto available = stretcher.available();
        if (available < 0)
            break;
        if (available == 0)
            continue;

        const auto framesToRead = juce::jmin(available, outputSamples - writePosition);
        for (int channel = 0; channel < channels; ++channel)
            outputChannels[static_cast<std::size_t>(channel)] = stretched.getWritePointer(channel, writePosition);

        const auto retrieved = stretcher.retrieve(outputChannels.data(), static_cast<std::size_t>(framesToRead));
        if (retrieved == 0)
            break;

        writePosition += static_cast<int>(retrieved);
    }

    return stretched;
#else
    juce::ignoreUnused(sampleRate);
    return exactLengthCopy(source, outputSamples);
#endif
}

bool writeWavFile(const juce::File& outputFile, const juce::AudioBuffer<float>& buffer, double sampleRate);

double ratioDistance(double a, double b)
{
    if (a <= 0.0 || b <= 0.0)
        return std::numeric_limits<double>::max();

    return std::abs(std::log(a / b));
}

juce::AudioBuffer<float> tileBufferToLength(const juce::AudioBuffer<float>& source, int outputSamples)
{
    if (source.getNumSamples() <= 0 || outputSamples <= 0)
        return {};

    juce::AudioBuffer<float> output(source.getNumChannels(), outputSamples);
    output.clear();

    auto writePosition = 0;
    while (writePosition < outputSamples)
    {
        const auto framesToCopy = juce::jmin(source.getNumSamples(), outputSamples - writePosition);
        for (int channel = 0; channel < source.getNumChannels(); ++channel)
            output.copyFrom(channel, writePosition, source, channel, 0, framesToCopy);

        writePosition += framesToCopy;
    }

    return output;
}

void renderDrumHalftimeTile(const juce::AudioBuffer<float>& input,
                            double sourceBpm,
                            double targetBpm,
                            int targetSamples,
                            double sampleRate,
                            const juce::File& outputDirectory,
                            const juce::String& baseName)
{
    const auto halfBpm = sourceBpm / 2.0;
    const auto useHalfTime = ratioDistance(halfBpm, targetBpm) < ratioDistance(sourceBpm, targetBpm);
    const auto effectiveBpm = useHalfTime ? halfBpm : sourceBpm;
    const auto lightStretchSamples = juce::jmax(1, static_cast<int>(std::round(static_cast<double>(input.getNumSamples()) * effectiveBpm / targetBpm)));

    auto lightlyStretched = rubberBandExp1StretchBufferToLength(input, lightStretchSamples, sampleRate);
    lightlyStretched = exactLengthCopy(std::move(lightlyStretched), lightStretchSamples);
    auto output = tileBufferToLength(lightlyStretched, targetSamples);

    const auto outputFile = outputDirectory.getChildFile(baseName + "_drum_halftime_tile.wav");
    const auto ok = writeWavFile(outputFile, output, sampleRate);

    std::cout << (ok ? "Wrote " : "Failed ")
              << outputFile.getFullPathName().toStdString()
              << " samples=" << output.getNumSamples()
              << " sourceBpm=" << sourceBpm
              << " effectiveBpm=" << effectiveBpm
              << " targetBpm=" << targetBpm
              << " lightStretchSamples=" << lightStretchSamples
              << " finalTargetSamples=" << targetSamples
              << " halftime=" << (useHalfTime ? "true" : "false")
              << '\n';
}

struct RubberBandDrainResult
{
    juce::AudioBuffer<float> buffer;
    int writtenSamples { 0 };
    int targetSamples { 0 };
    int processCalls { 0 };
    int retrieveCalls { 0 };
    int finalAvailable { 0 };
};

RubberBandDrainResult rubberBandExp1DrainTest(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    RubberBandDrainResult result;
    result.targetSamples = outputSamples;

#if ORION_HAVE_RUBBERBAND
    const auto channels = source.getNumChannels();
    const auto sourceSamples = source.getNumSamples();
    const auto timeRatio = static_cast<double>(outputSamples) / static_cast<double>(sourceSamples);
    constexpr int blockSize = 16384;

    RubberBand::RubberBandStretcher stretcher(
        static_cast<std::size_t>(std::round(sampleRate)),
        static_cast<std::size_t>(channels),
        RubberBand::RubberBandStretcher::OptionProcessOffline
            | RubberBand::RubberBandStretcher::OptionEngineFiner
            | RubberBand::RubberBandStretcher::OptionTransientsCrisp
            | RubberBand::RubberBandStretcher::OptionDetectorCompound
            | RubberBand::RubberBandStretcher::OptionChannelsTogether,
        timeRatio,
        1.0);

    stretcher.setExpectedInputDuration(static_cast<std::size_t>(sourceSamples));
    stretcher.setMaxProcessSize(static_cast<std::size_t>(blockSize));
    stretcher.setTimeRatio(timeRatio);
    stretcher.setPitchScale(1.0);

    std::vector<const float*> inputChannels(static_cast<std::size_t>(channels));
    std::vector<std::vector<float>> collected(static_cast<std::size_t>(channels));
    for (auto& channel : collected)
        channel.reserve(static_cast<std::size_t>(juce::jmax(outputSamples, sourceSamples)));

    auto assignInputChannels = [&source, &inputChannels, channels](int startSample)
    {
        for (int channel = 0; channel < channels; ++channel)
            inputChannels[static_cast<std::size_t>(channel)] = source.getReadPointer(channel, startSample);
    };

    auto drainAvailableOutput = [&]()
    {
        while (true)
        {
            const auto available = stretcher.available();
            result.finalAvailable = static_cast<int>(available);

            if (available <= 0)
                break;

            const auto framesToRead = static_cast<int>(juce::jmin<std::size_t>(available, static_cast<std::size_t>(blockSize)));
            juce::AudioBuffer<float> chunk(channels, framesToRead);
            chunk.clear();

            std::vector<float*> outputChannels(static_cast<std::size_t>(channels));
            for (int channel = 0; channel < channels; ++channel)
                outputChannels[static_cast<std::size_t>(channel)] = chunk.getWritePointer(channel);

            const auto retrieved = stretcher.retrieve(outputChannels.data(), static_cast<std::size_t>(framesToRead));
            if (retrieved == 0)
                break;

            const auto retrievedFrames = static_cast<int>(retrieved);
            ++result.retrieveCalls;

            for (int channel = 0; channel < channels; ++channel)
            {
                auto& destination = collected[static_cast<std::size_t>(channel)];
                const auto* sourceData = chunk.getReadPointer(channel);
                destination.insert(destination.end(), sourceData, sourceData + retrievedFrames);
            }
        }
    };

    for (int position = 0; position < sourceSamples; position += blockSize)
    {
        const auto frames = juce::jmin(blockSize, sourceSamples - position);
        assignInputChannels(position);
        stretcher.study(inputChannels.data(), static_cast<std::size_t>(frames), position + frames >= sourceSamples);
    }

    for (int position = 0; position < sourceSamples; position += blockSize)
    {
        const auto frames = juce::jmin(blockSize, sourceSamples - position);
        assignInputChannels(position);
        stretcher.process(inputChannels.data(), static_cast<std::size_t>(frames), position + frames >= sourceSamples);
        ++result.processCalls;
        drainAvailableOutput();
    }

    drainAvailableOutput();

    result.writtenSamples = collected.empty() ? 0 : static_cast<int>(collected.front().size());
    result.buffer.setSize(channels, result.writtenSamples);
    for (int channel = 0; channel < channels; ++channel)
    {
        if (result.writtenSamples > 0)
            result.buffer.copyFrom(channel, 0, collected[static_cast<std::size_t>(channel)].data(), result.writtenSamples);
    }
#else
    juce::ignoreUnused(source, sampleRate);
#endif

    return result;
}

void renderRubberBandDrainTest(const juce::AudioBuffer<float>& input,
                               int targetSamples,
                               double sampleRate,
                               const juce::File& outputDirectory,
                               const juce::String& baseName)
{
    auto result = rubberBandExp1DrainTest(input, targetSamples, sampleRate);
    const auto delta = result.writtenSamples - result.targetSamples;

    std::cout << "RubberBandDrainTest"
              << " targetSamples=" << result.targetSamples
              << " writtenSamples=" << result.writtenSamples
              << " delta=" << delta
              << " processCalls=" << result.processCalls
              << " retrieveCalls=" << result.retrieveCalls
              << " finalAvailable=" << result.finalAvailable
              << '\n';

    if (result.writtenSamples <= 0)
    {
        std::cout << "RubberBandDrainTest produced no output.\n";
        return;
    }

    const auto writtenFile = outputDirectory.getChildFile(baseName + "_rubberband_exp1_drain_written.wav");
    const auto wroteWritten = writeWavFile(writtenFile, result.buffer, sampleRate);
    std::cout << (wroteWritten ? "Wrote " : "Failed ")
              << writtenFile.getFullPathName().toStdString()
              << " samples=" << result.buffer.getNumSamples()
              << '\n';

    if (result.writtenSamples >= result.targetSamples)
    {
        juce::AudioBuffer<float> exact(result.buffer.getNumChannels(), result.targetSamples);
        for (int channel = 0; channel < exact.getNumChannels(); ++channel)
            exact.copyFrom(channel, 0, result.buffer, channel, 0, result.targetSamples);

        const auto exactFile = outputDirectory.getChildFile(baseName + "_rubberband_exp1_drain_exact.wav");
        const auto wroteExact = writeWavFile(exactFile, exact, sampleRate);
        std::cout << (wroteExact ? "Wrote " : "Failed ")
                  << exactFile.getFullPathName().toStdString()
                  << " samples=" << exact.getNumSamples()
                  << '\n';
    }
    else
    {
        std::cout << "RubberBandDrainTest underfilled by " << -delta
                  << " samples. No padded exact-target WAV was written.\n";
    }
}

bool readAudioFile(const juce::File& inputFile, juce::AudioBuffer<float>& buffer, double& sampleRate)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(inputFile));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0 || reader->numChannels <= 0)
        return false;

    sampleRate = reader->sampleRate;
    buffer.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    return reader->read(&buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
}

bool writeWavFile(const juce::File& outputFile, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    outputFile.deleteFile();
    auto stream = outputFile.createOutputStream();
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wavFormat.createWriterFor(stream.get(),
                                  sampleRate,
                                  static_cast<unsigned int>(buffer.getNumChannels()),
                                  24,
                                  {},
                                  0));
    if (writer == nullptr)
        return false;

    stream.release();
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

void renderBackend(const juce::String& backendName,
                   const juce::AudioBuffer<float>& input,
                   int targetSamples,
                   double sampleRate,
                   const juce::File& outputDirectory,
                   const juce::String& baseName,
                   juce::AudioBuffer<float> (*renderer)(const juce::AudioBuffer<float>&, int, double))
{
    auto output = renderer(input, targetSamples, sampleRate);
    output = exactLengthCopy(std::move(output), targetSamples);

    const auto outputFile = outputDirectory.getChildFile(baseName + "_" + backendName + ".wav");
    const auto ok = writeWavFile(outputFile, output, sampleRate);
    std::cout << (ok ? "Wrote " : "Failed ")
              << outputFile.getFullPathName().toStdString()
              << " samples=" << output.getNumSamples()
              << '\n';
}
}  // namespace

int main(int argc, char* argv[])
{
    if (argc < 4 || argc > 5)
    {
        std::cout << "Usage: OrionWarpCompare <input-audio> <source-bpm> <target-bpm> [output-dir]\n";
        return 1;
    }

    const juce::File inputFile { juce::String(argv[1]) };
    const auto sourceBpm = juce::String(argv[2]).getDoubleValue();
    const auto targetBpm = juce::String(argv[3]).getDoubleValue();
    auto outputDirectory = argc == 5 ? juce::File(juce::String(argv[4])) : inputFile.getParentDirectory();

    if (! inputFile.existsAsFile() || sourceBpm <= 0.0 || targetBpm <= 0.0)
    {
        std::cout << "Invalid input file or BPM values.\n";
        return 1;
    }

    outputDirectory.createDirectory();
    if (! outputDirectory.isDirectory())
    {
        std::cout << "Could not create output directory: " << outputDirectory.getFullPathName().toStdString() << '\n';
        return 1;
    }

    juce::AudioBuffer<float> input;
    double sampleRate = 0.0;
    if (! readAudioFile(inputFile, input, sampleRate))
    {
        std::cout << "Could not read input audio file.\n";
        return 1;
    }

    const auto targetSamples = juce::jmax(1, static_cast<int>(std::round(static_cast<double>(input.getNumSamples()) * sourceBpm / targetBpm)));
    const auto baseName = inputFile.getFileNameWithoutExtension()
        + "_"
        + juce::String(sourceBpm, 2).replaceCharacter('.', 'p')
        + "to"
        + juce::String(targetBpm, 2).replaceCharacter('.', 'p');

    std::cout << "Input: " << inputFile.getFullPathName().toStdString() << '\n'
              << "sourceBpm=" << sourceBpm
              << " targetBpm=" << targetBpm
              << " sourceSamples=" << input.getNumSamples()
              << " targetSamples=" << targetSamples
              << " sampleRate=" << sampleRate
              << '\n';
    logTransientPreview(input, targetSamples, sampleRate);

    renderBackend("rubberband_exp1", input, targetSamples, sampleRate, outputDirectory, baseName, rubberBandExp1StretchBufferToLength);
    renderRubberBandDrainTest(input, targetSamples, sampleRate, outputDirectory, baseName);
    renderDrumHalftimeTile(input, sourceBpm, targetBpm, targetSamples, sampleRate, outputDirectory, baseName);
    renderBackend("signalsmith", input, targetSamples, sampleRate, outputDirectory, baseName, signalsmithStretchBufferToLength);
    renderBackend("signalsmith_restore_soft", input, targetSamples, sampleRate, outputDirectory, baseName, signalsmithTransientRestoreSoftBuffer);
    renderBackend("signalsmith_restore_medium", input, targetSamples, sampleRate, outputDirectory, baseName, signalsmithTransientRestoreMediumBuffer);
    renderBackend("signalsmith_restore_hard", input, targetSamples, sampleRate, outputDirectory, baseName, signalsmithTransientRestoreHardBuffer);
    renderBackend("signalsmith_restore_replace", input, targetSamples, sampleRate, outputDirectory, baseName, signalsmithTransientRestoreReplaceBuffer);
    renderBackend("slice_existing", input, targetSamples, sampleRate, outputDirectory, baseName, sliceStretchBuffer);
    renderBackend("slice_preroll_exp", input, targetSamples, sampleRate, outputDirectory, baseName, slicePrerollExperimentBuffer);
    renderBackend("drum_beats_tailfill_v1", input, targetSamples, sampleRate, outputDirectory, baseName, drumBeatsTailfillV1Buffer);

    return 0;
}
