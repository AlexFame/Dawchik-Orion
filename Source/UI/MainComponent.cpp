#include "MainComponent.h"

#include <signalsmith-stretch/signalsmith-stretch.h>
#if ORION_HAVE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace
{
const auto backgroundColour = juce::Colour(0xff0b0f12);
const auto panelColour = juce::Colour(0xff131a20);
const auto accentColour = juce::Colour(0xffeb6f3a);
const auto panelStroke = juce::Colour(0xff25313c);
const auto mutedText = juce::Colours::white.withAlpha(0.64f);
const auto transportShelfColour = juce::Colour(0xff171d23);
const auto transportShelfStroke = juce::Colour(0xff2b3640);
const auto transportButtonColour = juce::Colour(0xfff0e8dc);
const auto transportButtonText = juce::Colour(0xff222222);
const auto transportDarkPanel = juce::Colour(0xff20252c);
const auto transportSectionFill = juce::Colour(0xff11171d);
const auto transportSectionStroke = juce::Colour(0xff303c47);
const auto recordAccent = juce::Colour(0xffd95050);
constexpr double previewMaxLengthSeconds = 12.0;
constexpr int minBrowserPanelWidth = 220;
constexpr int maxBrowserPanelWidth = 520;
constexpr int browserResizeHandleWidth = 10;
constexpr int transportBrandWidth = 92;
constexpr int transportClusterWidth = 314;
constexpr int transportTempoWidth = 112;
constexpr int transportModeWidth = 266;
constexpr int transportUtilityWidth = 236;
constexpr int transportSectionGap = 12;
constexpr int transportControlHeight = 62;
constexpr int transportSectionHeight = 76;
constexpr int transportContentVerticalNudge = -3;
constexpr const char* warpBackendCacheVersion = ORION_HAVE_RUBBERBAND ? "rubberband_exp1" : "signalsmith_fallback";

struct AudioWarpAnalysis
{
    double durationSeconds { 0.0 };
    double sourceBpm { 0.0 };
    int detectedBars { 0 };
    juce::String bpmSource { "none" };
    double bpmConfidence { 0.0 };
    bool bpmGuessed { false };
};

double parseBpmFromFileName(const juce::File& file)
{
    auto text = file.getFileNameWithoutExtension().toLowerCase();
    const auto bpmIndex = text.indexOf("bpm");
    if (bpmIndex < 0)
    {
        if (! (text.contains("loop") || text.contains("break") || text.contains("drum") || text.contains("beat")))
            return 0.0;

        double bestBpm = 0.0;
        int bestDigitCount = 0;
        juce::String currentNumber;

        const auto textWithDelimiter = text + " ";
        for (int i = 0; i < textWithDelimiter.length(); ++i)
        {
            const auto character = textWithDelimiter[i];
            if (juce::CharacterFunctions::isDigit(character))
            {
                currentNumber += juce::String::charToString(character);
                continue;
            }

            if (currentNumber.isNotEmpty())
            {
                const auto candidateBpm = currentNumber.getDoubleValue();
                if (candidateBpm >= 40.0 && candidateBpm <= 260.0 && currentNumber.length() >= bestDigitCount)
                {
                    bestBpm = candidateBpm;
                    bestDigitCount = currentNumber.length();
                }

                currentNumber.clear();
            }
        }

        return bestBpm;
    }

    auto start = bpmIndex - 1;
    while (start >= 0 && (juce::CharacterFunctions::isDigit(text[start]) || text[start] == ' ' || text[start] == '_' || text[start] == '-'))
        --start;

    auto numberText = text.substring(start + 1, bpmIndex).retainCharacters("0123456789").trim();
    if (numberText.isEmpty())
        return 0.0;

    const auto bpm = numberText.getDoubleValue();
    return (bpm >= 40.0 && bpm <= 260.0) ? bpm : 0.0;
}

AudioWarpAnalysis analyzeAudioWarpMetadata(const juce::File& file, double projectTempoBpm, int numerator)
{
    AudioWarpAnalysis result;

    if (! file.existsAsFile() || projectTempoBpm <= 0.0)
        return result;

    static juce::AudioFormatManager audioFormatManager;
    static bool formatsRegistered = false;
    if (! formatsRegistered)
    {
        audioFormatManager.registerBasicFormats();
        formatsRegistered = true;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return result;

    result.durationSeconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    const auto nameBpm = parseBpmFromFileName(file);
    if (nameBpm > 0.0)
    {
        result.sourceBpm = nameBpm;
        result.bpmSource = "filename";
        result.bpmConfidence = 1.0;
        result.bpmGuessed = false;
        const auto sourceBeats = result.durationSeconds * (nameBpm / 60.0);
        const auto roundedBeats = std::round(sourceBeats);
        const auto beatsPerBar = static_cast<double>(juce::jmax(1, numerator));
        const auto roundedBars = static_cast<int>(std::round(roundedBeats / beatsPerBar));
        if (roundedBars > 0 && std::abs(roundedBeats - static_cast<double>(roundedBars) * beatsPerBar) <= 0.25)
            result.detectedBars = roundedBars;
        DBG("[Warp] " + file.getFileName() + " | bpmSource=filename | confidence=1.0 | sourceBpm=" + juce::String(nameBpm, 1));
        return result;
    }

    constexpr double neutralCenter = 128.0;
    constexpr int commonLoopBars[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
    double bestBpm = 0.0;
    int bestBars = 0;
    double bestScore = std::numeric_limits<double>::max();

    for (const auto bars : commonLoopBars)
    {
        const auto loopBeats = static_cast<double>(bars * juce::jmax(1, numerator));
        const auto candidateBpm = (loopBeats / result.durationSeconds) * 60.0;
        if (candidateBpm < 70.0 || candidateBpm > 180.0)
            continue;

        const auto score = std::abs(candidateBpm - neutralCenter);
        if (score < bestScore)
        {
            bestScore = score;
            bestBpm = candidateBpm;
            bestBars = bars;
        }
    }

    if (bestBars > 0)
    {
        result.sourceBpm = bestBpm;
        result.detectedBars = bestBars;
        result.bpmSource = "duration-bars";
        result.bpmGuessed = true;

        // Confidence: how close the detected beats align to perfect bar boundaries
        const auto beatsPerBar = static_cast<double>(juce::jmax(1, numerator));
        const auto sourceBeats = result.durationSeconds * (bestBpm / 60.0);
        const auto barFraction = std::abs(sourceBeats - std::round(sourceBeats / beatsPerBar) * beatsPerBar);
        result.bpmConfidence = juce::jlimit(0.0, 1.0, 1.0 - barFraction / beatsPerBar);

        DBG("[Warp] " + file.getFileName()
            + " | bpmSource=duration-bars | confidence=" + juce::String(result.bpmConfidence, 2)
            + " | sourceBpm=" + juce::String(bestBpm, 1)
            + " | bars=" + juce::String(bestBars));
    }
    else
    {
        // No usable BPM candidate — warp cannot be applied
        DBG("[Warp] " + file.getFileName() + " | bpmSource=none | confidence=0 | sourceBpm=0");
    }

    return result;
}

int findStrongestTransientNearStart(const juce::AudioBuffer<float>& buffer, double sampleRate, double searchSeconds)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0 || sampleRate <= 0.0)
        return 0;

    const auto searchSamples = juce::jmin(buffer.getNumSamples(), static_cast<int>(std::round(sampleRate * searchSeconds)));
    int bestSample = 0;
    float bestEnergy = 0.0f;

    for (int sample = 0; sample < searchSamples; ++sample)
    {
        float energy = 0.0f;
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto value = buffer.getSample(channel, sample);
            energy += value * value;
        }

        if (energy > bestEnergy)
        {
            bestEnergy = energy;
            bestSample = sample;
        }
    }

    return bestSample;
}

void rotateBufferLeft(juce::AudioBuffer<float>& buffer, int samplesToRotate)
{
    const auto totalSamples = buffer.getNumSamples();
    if (totalSamples <= 1)
        return;

    samplesToRotate %= totalSamples;
    if (samplesToRotate <= 0)
        return;

    juce::AudioBuffer<float> rotated(buffer.getNumChannels(), totalSamples);
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        rotated.copyFrom(channel, 0, buffer, channel, samplesToRotate, totalSamples - samplesToRotate);
        rotated.copyFrom(channel, totalSamples - samplesToRotate, buffer, channel, 0, samplesToRotate);
    }

    buffer = std::move(rotated);
}

inline float cubicHermite(float t, float y0, float y1, float y2, float y3) noexcept
{
    const auto c1 = 0.5f * (y2 - y0);
    const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

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

double detectTransientDensity(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    const auto onsets = detectOnsets(buffer, sampleRate);
    const auto duration = static_cast<double>(buffer.getNumSamples()) / sampleRate;
    return (duration > 0.05) ? static_cast<double>(onsets.size()) / duration : 0.0;
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

        const auto copyLen = juce::jmin(sliceLen, space, outputSamples - dstStart);
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

    DBG("[Warp] Slice mode: " + juce::String(static_cast<int>(onsets.size())) + " slices, ratio=" + juce::String(ratio, 3));
    return output;
}

juce::AudioBuffer<float> stretchBufferToLength(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    if (source.getNumSamples() <= 1 || outputSamples <= 1 || sampleRate <= 0.0)
        return source;

    if (std::abs(outputSamples - source.getNumSamples()) <= 1)
        return source;

    // Auto-detect: use slice-based warp for transient-heavy content
    const auto onsets = detectOnsets(source, sampleRate);
    const auto duration = static_cast<double>(source.getNumSamples()) / sampleRate;
    const auto density = (duration > 0.05) ? static_cast<double>(onsets.size()) / duration : 0.0;
    const auto ratio = static_cast<double>(outputSamples) / static_cast<double>(source.getNumSamples());
    const bool useSlice = onsets.size() >= 3 && density > 4.0;

    DBG("[Warp-Stretch] sourceSamples=" + juce::String(source.getNumSamples())
        + " targetSamples=" + juce::String(outputSamples)
        + " ratio=" + juce::String(ratio, 4)
        + " onsets=" + juce::String(static_cast<int>(onsets.size()))
        + " density=" + juce::String(density, 2) + "/sec"
        + " path=" + juce::String(useSlice ? "SLICE" : "SIGNALSMITH"));

    if (useSlice)
        return sliceStretchBuffer(source, outputSamples, sampleRate);

    const auto channels = source.getNumChannels();
    juce::AudioBuffer<float> stretched(channels, outputSamples);
    stretched.clear();

    signalsmith::stretch::SignalsmithStretch<float> stretcher;
    // Shorter analysis window (80ms vs default 120ms) for tighter transient
    // reproduction on kicks/snares. Interval kept proportional for overlap.
    const auto blockSamples = static_cast<int>(sampleRate * 0.08);
    const auto intervalSamples = static_cast<int>(sampleRate * 0.02);
    stretcher.configure(channels, blockSamples, intervalSamples);
    stretcher.setTransposeFactor(1.0f);

    std::vector<const float*> inputChannels(static_cast<std::size_t>(channels));
    std::vector<float*> outputChannels(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel)
    {
        inputChannels[static_cast<std::size_t>(channel)] = source.getReadPointer(channel);
        outputChannels[static_cast<std::size_t>(channel)] = stretched.getWritePointer(channel);
    }

    if (! stretcher.exact(inputChannels.data(), source.getNumSamples(), outputChannels.data(), outputSamples))
        return source;

    return stretched;
}

#if ORION_HAVE_RUBBERBAND
juce::AudioBuffer<float> rubberBandStretchBufferToLength(const juce::AudioBuffer<float>& source, int outputSamples, double sampleRate)
{
    if (source.getNumSamples() <= 1 || outputSamples <= 1 || sampleRate <= 0.0)
        return source;

    if (std::abs(outputSamples - source.getNumSamples()) <= 1)
        return source;

    try
    {
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

        DBG("[Warp-RubberBand] sourceSamples=" + juce::String(sourceSamples)
            + " targetSamples=" + juce::String(outputSamples)
            + " outputSamples=" + juce::String(stretched.getNumSamples())
            + " writtenSamples=" + juce::String(writePosition)
            + " timeRatio=" + juce::String(timeRatio, 6)
            + " pitchScale=1.000000");

        return stretched;
    }
    catch (...)
    {
        DBG("[Warp-RubberBand] failed; falling back to Signalsmith");
        return stretchBufferToLength(source, outputSamples, sampleRate);
    }
}
#endif

juce::AudioBuffer<float> stretchBufferToLengthWithExperimentalBackend(const juce::AudioBuffer<float>& source,
                                                                      int outputSamples,
                                                                      double sampleRate)
{
#if ORION_HAVE_RUBBERBAND
    auto stretched = rubberBandStretchBufferToLength(source, outputSamples, sampleRate);
    if (stretched.getNumSamples() == outputSamples && stretched.getNumChannels() == source.getNumChannels())
        return stretched;

    DBG("[Warp-RubberBand] invalid output; falling back to Signalsmith");
#endif
    return stretchBufferToLength(source, outputSamples, sampleRate);
}

juce::AudioBuffer<float> makeTempoFittedPreviewBuffer(const juce::AudioBuffer<float>& source,
                                                       double sourceBpm,
                                                       double projectTempoBpm,
                                                       double sampleRate)
{
    if (source.getNumSamples() <= 1 || sourceBpm <= 0.0 || projectTempoBpm <= 0.0)
        return source;

    const auto tempoRatio = sourceBpm / projectTempoBpm;
    if (std::abs(tempoRatio - 1.0) < 0.001)
        return source;

    const auto outputSamples = juce::jmax(1, static_cast<int>(std::round(static_cast<double>(source.getNumSamples()) * tempoRatio)));
    return stretchBufferToLengthWithExperimentalBackend(source, outputSamples, sampleRate);
}

juce::String compactInspectorFileName(const juce::File& file, const juce::String& fallbackName)
{
    auto name = file.existsAsFile() ? file.getFileNameWithoutExtension() : fallbackName;
    if (name.length() <= 22)
        return name;

    return name.substring(0, 19) + "...";
}

class TransportButtonLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& buttonBackgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat();
        auto fill = buttonBackgroundColour;

        if (button.getToggleState())
            fill = fill.interpolatedWith(accentColour, 0.78f);
        else if (shouldDrawButtonAsDown)
            fill = fill.darker(0.18f);
        else if (shouldDrawButtonAsHighlighted)
            fill = fill.brighter(0.05f);

        g.setColour(juce::Colours::black.withAlpha(0.14f));
        g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f), 8.0f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 8.0f);
        g.setColour(button.getToggleState() ? accentColour.brighter(0.2f) : juce::Colours::black.withAlpha(0.16f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);
    }

    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool,
                        bool) override
    {
        const auto textColour = button.findColour(button.getToggleState()
            ? juce::TextButton::textColourOnId
            : juce::TextButton::textColourOffId);

        auto bounds = button.getLocalBounds().reduced(4, 4);
        g.setColour(textColour);
        auto iconBounds = bounds.removeFromTop(static_cast<int>(bounds.getHeight() * 0.56f)).reduced(8, 4);
        auto labelBounds = bounds.withTrimmedTop(2);
        drawTransportIcon(g, button.getComponentID(), iconBounds, textColour);

        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        g.drawText(button.getButtonText(), labelBounds, juce::Justification::centredTop);
    }

private:
    static void drawTransportIcon(juce::Graphics& g,
                                  const juce::String& role,
                                  juce::Rectangle<int> bounds,
                                  juce::Colour colour)
    {
        auto area = bounds.toFloat();
        g.setColour(colour);

        if (role == "play")
        {
            juce::Path triangle;
            triangle.addTriangle(area.getX() + area.getWidth() * 0.28f, area.getY() + area.getHeight() * 0.18f,
                                 area.getRight() - area.getWidth() * 0.22f, area.getCentreY(),
                                 area.getX() + area.getWidth() * 0.28f, area.getBottom() - area.getHeight() * 0.18f);
            g.fillPath(triangle);
        }
        else if (role == "stop")
        {
            g.fillRoundedRectangle(area.reduced(area.getWidth() * 0.28f, area.getHeight() * 0.2f), 2.0f);
        }
        else if (role == "record")
        {
            g.setColour(recordAccent);
            g.fillEllipse(area.reduced(area.getWidth() * 0.28f, area.getHeight() * 0.22f));
        }
        else if (role == "undo" || role == "redo")
        {
            juce::Path path;
            const auto left = area.getX() + area.getWidth() * 0.2f;
            const auto right = area.getRight() - area.getWidth() * 0.2f;
            const auto midY = area.getCentreY();
            if (role == "undo")
            {
                path.startNewSubPath(right, midY - area.getHeight() * 0.18f);
                path.quadraticTo(left + area.getWidth() * 0.24f, midY - area.getHeight() * 0.18f, left + area.getWidth() * 0.24f, midY);
                path.quadraticTo(left + area.getWidth() * 0.24f, midY + area.getHeight() * 0.18f, right - area.getWidth() * 0.08f, midY + area.getHeight() * 0.18f);
                g.strokePath(path, juce::PathStrokeType(2.2f));

                juce::Path arrow;
                arrow.addTriangle(left, midY, left + area.getWidth() * 0.22f, midY - area.getHeight() * 0.2f, left + area.getWidth() * 0.22f, midY + area.getHeight() * 0.2f);
                g.fillPath(arrow);
            }
            else
            {
                path.startNewSubPath(left, midY - area.getHeight() * 0.18f);
                path.quadraticTo(right - area.getWidth() * 0.24f, midY - area.getHeight() * 0.18f, right - area.getWidth() * 0.24f, midY);
                path.quadraticTo(right - area.getWidth() * 0.24f, midY + area.getHeight() * 0.18f, left + area.getWidth() * 0.08f, midY + area.getHeight() * 0.18f);
                g.strokePath(path, juce::PathStrokeType(2.2f));

                juce::Path arrow;
                arrow.addTriangle(right, midY, right - area.getWidth() * 0.22f, midY - area.getHeight() * 0.2f, right - area.getWidth() * 0.22f, midY + area.getHeight() * 0.2f);
                g.fillPath(arrow);
            }
        }
        else if (role == "metronome")
        {
            juce::Path metro;
            metro.startNewSubPath(area.getCentreX(), area.getY() + area.getHeight() * 0.12f);
            metro.lineTo(area.getX() + area.getWidth() * 0.3f, area.getBottom() - area.getHeight() * 0.12f);
            metro.lineTo(area.getRight() - area.getWidth() * 0.3f, area.getBottom() - area.getHeight() * 0.12f);
            metro.closeSubPath();
            g.strokePath(metro, juce::PathStrokeType(2.0f));
            g.drawLine(area.getCentreX(), area.getY() + area.getHeight() * 0.24f,
                       area.getCentreX() + area.getWidth() * 0.12f, area.getCentreY(), 2.0f);
        }
        else if (role == "loop")
        {
            juce::Path loop;
            loop.addCentredArc(area.getCentreX(), area.getCentreY(), area.getWidth() * 0.22f, area.getHeight() * 0.22f, 0.0f,
                               juce::MathConstants<float>::pi * 0.25f, juce::MathConstants<float>::pi * 1.7f, true);
            g.strokePath(loop, juce::PathStrokeType(2.2f));
            juce::Path arrow;
            arrow.addTriangle(area.getRight() - area.getWidth() * 0.26f, area.getCentreY() - area.getHeight() * 0.12f,
                              area.getRight() - area.getWidth() * 0.16f, area.getCentreY(),
                              area.getRight() - area.getWidth() * 0.28f, area.getCentreY() + area.getHeight() * 0.1f);
            g.fillPath(arrow);
        }
        else if (role == "countin")
        {
            const auto barWidth = area.getWidth() * 0.08f;
            const auto gap = area.getWidth() * 0.11f;
            for (int i = 0; i < 3; ++i)
            {
                const auto x = area.getCentreX() - gap + i * gap;
                g.fillRoundedRectangle(x, area.getY() + area.getHeight() * 0.2f, barWidth, area.getHeight() * (0.44f + 0.12f * i), 1.0f);
            }
        }
        else if (role == "browser")
        {
            auto icon = area.reduced(area.getWidth() * 0.24f, area.getHeight() * 0.2f);
            g.drawRoundedRectangle(icon.reduced(0.5f), 2.0f, 1.8f);
            g.drawLine(icon.getX(), icon.getY() + icon.getHeight() * 0.33f, icon.getRight(), icon.getY() + icon.getHeight() * 0.33f, 1.4f);
            g.drawLine(icon.getX() + icon.getWidth() * 0.5f, icon.getY(), icon.getX() + icon.getWidth() * 0.5f, icon.getBottom(), 1.4f);
        }
        else if (role == "save")
        {
            auto icon = area.reduced(area.getWidth() * 0.24f, area.getHeight() * 0.16f);
            g.drawRoundedRectangle(icon.reduced(0.5f), 2.0f, 1.8f);
            g.fillRect(icon.withHeight(static_cast<int>(icon.getHeight() * 0.26f)).reduced(2, 0));
            g.drawRect(icon.reduced(static_cast<int>(icon.getWidth() * 0.24f), static_cast<int>(icon.getHeight() * 0.38f)), 1);
        }
        else if (role == "export")
        {
            g.drawLine(area.getCentreX(), area.getBottom() - area.getHeight() * 0.18f, area.getCentreX(), area.getY() + area.getHeight() * 0.24f, 2.0f);
            juce::Path arrow;
            arrow.addTriangle(area.getCentreX(), area.getY() + area.getHeight() * 0.14f,
                              area.getCentreX() - area.getWidth() * 0.14f, area.getY() + area.getHeight() * 0.34f,
                              area.getCentreX() + area.getWidth() * 0.14f, area.getY() + area.getHeight() * 0.34f);
            g.fillPath(arrow);
            g.drawLine(area.getX() + area.getWidth() * 0.26f, area.getBottom() - area.getHeight() * 0.22f,
                       area.getRight() - area.getWidth() * 0.26f, area.getBottom() - area.getHeight() * 0.22f, 2.0f);
        }
        else if (role == "settings")
        {
            g.drawEllipse(area.reduced(area.getWidth() * 0.3f, area.getHeight() * 0.22f), 2.0f);
            g.fillEllipse(area.reduced(area.getWidth() * 0.42f, area.getHeight() * 0.34f));
        }
    }
};

TransportButtonLookAndFeel transportButtonLookAndFeel;
}  // namespace

namespace orion
{
class MainComponent::BufferPreviewSource final : public juce::PositionableAudioSource
{
public:
    BufferPreviewSource(juce::AudioBuffer<float> previewBuffer, double sourceSampleRate)
        : buffer(std::move(previewBuffer)),
          sampleRate(sourceSampleRate)
    {
    }

    void prepareToPlay(int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        if (buffer.getNumSamples() == 0 || info.numSamples <= 0)
            return;

        const auto remaining = juce::jmax(0, buffer.getNumSamples() - static_cast<int>(positionSamples));
        const auto samplesToCopy = juce::jmin(info.numSamples, remaining);
        if (samplesToCopy <= 0)
            return;

        for (int channel = 0; channel < info.buffer->getNumChannels(); ++channel)
        {
            const auto sourceChannel = juce::jmin(channel, buffer.getNumChannels() - 1);
            info.buffer->copyFrom(channel, info.startSample, buffer, sourceChannel, static_cast<int>(positionSamples), samplesToCopy);
        }

        positionSamples += samplesToCopy;
    }

    void setNextReadPosition(juce::int64 newPosition) override
    {
        positionSamples = juce::jlimit<juce::int64>(0, buffer.getNumSamples(), newPosition);
    }

    juce::int64 getNextReadPosition() const override
    {
        return positionSamples;
    }

    juce::int64 getTotalLength() const override
    {
        return buffer.getNumSamples();
    }

    bool isLooping() const override
    {
        return false;
    }

    double getSampleRate() const noexcept
    {
        return sampleRate;
    }

private:
    juce::AudioBuffer<float> buffer;
    double sampleRate { 44100.0 };
    juce::int64 positionSamples { 0 };
};

class MainComponent::ArrangementPlaybackSource final : public juce::AudioSource
{
public:
    ArrangementPlaybackSource(ProjectState& state, TransportEngine& engine, juce::AudioFormatManager& formatManager)
        : project(state),
          transport(engine),
          audioFormatManager(formatManager)
    {
    }

    void prepareToPlay(int, double newSampleRate) override
    {
        outputSampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    }

    void releaseResources() override {}

    void syncToTransportPosition() noexcept
    {
        currentTimelineBeat = transport.getPlayheadBeat();
        wasPlaying = false;
    }

    void prepareWarpCacheForCurrentTempo()
    {
        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0)
            return;

        for (const auto& track : project.getTracks())
        {
            for (const auto& clip : track.clips)
            {
                if (clip.type != ClipType::audio || ! clip.warpEnabled || clip.sourcePath.isEmpty() || clip.lengthInBeats <= 0.0)
                    continue;

                if (const auto* originalAudioData = getAudioFileData(clip.sourcePath))
                    juce::ignoreUnused(getWarpedAudioFileData(clip, *originalAudioData, beatsPerSecond, true));
            }
        }
    }

    void renderOfflineBlock(juce::AudioBuffer<float>& outputBuffer,
                            int startSample,
                            int numSamples,
                            double blockStartBeat,
                            double renderSampleRate)
    {
        if (startSample < 0 || numSamples <= 0 || renderSampleRate <= 0.0)
            return;

        const auto availableSamples = outputBuffer.getNumSamples() - startSample;
        if (availableSamples <= 0)
            return;

        const auto samplesToRender = juce::jmin(numSamples, availableSamples);
        renderAudioIntoBuffer(outputBuffer, startSample, samplesToRender, blockStartBeat, renderSampleRate, false, false);
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();

        if (outputSampleRate <= 0.0 || info.numSamples <= 0)
            return;

        if (! transport.isPlaying())
        {
            currentTimelineBeat = transport.getPlayheadBeat();
            wasPlaying = false;
            return;
        }

        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0)
            return;

        const auto loopActive = transport.isLoopEnabled() && project.hasLoopRange();
        const auto loopStartBeat = project.getLoopStartBeat();
        const auto loopEndBeat = project.getLoopEndBeat();
        const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);
        if (! loopActive && project.getContentEndInBeats() <= 0.0)
            return;

        if (! wasPlaying)
        {
            currentTimelineBeat = transport.getPlayheadBeat();
            wasPlaying = true;
        }

        const auto beatAdvancePerSample = beatsPerSecond / outputSampleRate;
        renderAudioIntoBuffer(*info.buffer, info.startSample, info.numSamples, currentTimelineBeat, outputSampleRate, loopActive, ! loopActive);

        currentTimelineBeat += static_cast<double>(info.numSamples) * beatAdvancePerSample;
        while (loopActive && currentTimelineBeat >= loopEndBeat)
            currentTimelineBeat = loopStartBeat + std::fmod(currentTimelineBeat - loopStartBeat, loopSpanBeats);
        if (! loopActive)
        {
            const auto repeatEndBeat = project.getContentEndInBeats();
            while (repeatEndBeat > 0.0 && currentTimelineBeat >= repeatEndBeat)
                currentTimelineBeat = std::fmod(currentTimelineBeat, repeatEndBeat);
        }
    }

private:
    struct AudioFileData
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate { 44100.0 };
    };

    void renderAudioIntoBuffer(juce::AudioBuffer<float>& targetBuffer,
                               int startSample,
                               int numSamples,
                               double blockStartBeat,
                               double renderSampleRate,
                               bool wrapToLoop,
                               bool wrapToProjectEnd)
    {
        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0 || renderSampleRate <= 0.0 || numSamples <= 0)
            return;

        const auto loopStartBeat = project.getLoopStartBeat();
        const auto loopEndBeat = project.getLoopEndBeat();
        const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);
        const auto repeatEndBeat = project.getContentEndInBeats();
        const auto wrapToProject = wrapToProjectEnd && ! wrapToLoop && repeatEndBeat > 0.0;
        const auto beatAdvancePerSample = beatsPerSecond / renderSampleRate;
        const auto& tracks = project.getTracks();

        bool anySoloActive = false;
        for (const auto& soloTrack : tracks)
        {
            if (soloTrack.solo)
            {
                anySoloActive = true;
                break;
            }

            for (const auto& soloClip : soloTrack.clips)
            {
                if (soloClip.type == ClipType::audio && soloClip.solo)
                {
                    anySoloActive = true;
                    break;
                }
            }

            if (anySoloActive)
                break;
        }

        for (const auto& track : tracks)
        {
            if (track.muted)
                continue;

            const auto trackGain = juce::Decibels::decibelsToGain(static_cast<float>(track.volumeDb));
            for (const auto& clip : track.clips)
            {
                if (clip.type != ClipType::audio || clip.sourcePath.isEmpty() || clip.muted)
                    continue;
                if (anySoloActive && ! track.solo && ! clip.solo)
                    continue;

                const auto* originalAudioData = getAudioFileData(clip.sourcePath);
                if (originalAudioData == nullptr || originalAudioData->buffer.getNumSamples() <= 0 || originalAudioData->sampleRate <= 0.0)
                    continue;

                const auto clipStartBeat = clip.startBeat;
                const auto clipEndBeat = clip.startBeat + clip.lengthInBeats;
                const auto linearGain = juce::Decibels::decibelsToGain(static_cast<float>(clip.gainDb)) * trackGain;
                const auto* audioData = originalAudioData;
                if (clip.warpEnabled && clip.lengthInBeats > 0.0)
                {
                    if (const auto* warpedAudioData = getWarpedAudioFileData(clip, *originalAudioData, beatsPerSecond, false))
                        audioData = warpedAudioData;
                    else
                        continue;
                }

                for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
                {
                    auto timelineBeat = blockStartBeat + static_cast<double>(sampleIndex) * beatAdvancePerSample;

                    if (wrapToLoop)
                    {
                        while (timelineBeat >= loopEndBeat)
                            timelineBeat = loopStartBeat + std::fmod(timelineBeat - loopStartBeat, loopSpanBeats);
                    }
                    else if (wrapToProject)
                    {
                        while (timelineBeat >= repeatEndBeat)
                            timelineBeat = std::fmod(timelineBeat, repeatEndBeat);
                    }

                    if (! wrapToLoop && ! wrapToProject && timelineBeat >= repeatEndBeat)
                        continue;

                    if (timelineBeat < clipStartBeat || timelineBeat >= clipEndBeat)
                        continue;

                    const auto clipBeatOffset = timelineBeat - clipStartBeat;
                    const auto clipSeconds = clipBeatOffset / beatsPerSecond;
                    const auto sourceSamplePosition = clipSeconds * audioData->sampleRate;

                    const auto sourceIndex = static_cast<int>(sourceSamplePosition);
                    if (sourceIndex < 0 || sourceIndex >= audioData->buffer.getNumSamples())
                        continue;

                    const auto sourceFraction = static_cast<float>(sourceSamplePosition - static_cast<double>(sourceIndex));
                    const auto lastSample = audioData->buffer.getNumSamples() - 1;
                    const auto i0 = juce::jmax(0, sourceIndex - 1);
                    const auto i1 = sourceIndex;
                    const auto i2 = juce::jmin(sourceIndex + 1, lastSample);
                    const auto i3 = juce::jmin(sourceIndex + 2, lastSample);
                    for (int channel = 0; channel < targetBuffer.getNumChannels(); ++channel)
                    {
                        const auto sourceChannel = juce::jmin(channel, audioData->buffer.getNumChannels() - 1);
                        const auto y0 = audioData->buffer.getSample(sourceChannel, i0);
                        const auto y1 = audioData->buffer.getSample(sourceChannel, i1);
                        const auto y2 = audioData->buffer.getSample(sourceChannel, i2);
                        const auto y3 = audioData->buffer.getSample(sourceChannel, i3);
                        const auto sampleValue = cubicHermite(sourceFraction, y0, y1, y2, y3) * linearGain;
                        targetBuffer.addSample(channel, startSample + sampleIndex, sampleValue * 0.75f);
                    }
                }
            }
        }
    }

    const AudioFileData* getAudioFileData(const juce::String& path)
    {
        const auto key = path.toStdString();
        if (const auto it = audioCache.find(key); it != audioCache.end())
            return it->second.get();

        juce::File file(path);
        if (! file.existsAsFile())
            return nullptr;

        std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0 || reader->numChannels <= 0)
            return nullptr;

        auto data = std::make_unique<AudioFileData>();
        data->sampleRate = reader->sampleRate;
        data->buffer.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
        reader->read(&data->buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

        const auto* dataPtr = data.get();
        audioCache.emplace(key, std::move(data));
        return dataPtr;
    }

    const AudioFileData* getWarpedAudioFileData(const TimelineClip& clip,
                                                const AudioFileData& originalData,
                                                double beatsPerSecond,
                                                bool allowBuild)
    {
        if (beatsPerSecond <= 0.0 || originalData.sampleRate <= 0.0 || clip.lengthInBeats <= 0.0)
            return nullptr;

        const auto targetSamples = juce::jmax(1, static_cast<int>(std::round((clip.lengthInBeats / beatsPerSecond) * originalData.sampleRate)));
        const auto key = clip.sourcePath.toStdString() + "|" + std::to_string(targetSamples) + "|" + warpBackendCacheVersion;
        if (const auto it = warpedAudioCache.find(key); it != warpedAudioCache.end())
        {
            DBG("[Warp-Cache] HIT key=" + juce::String(key));
            return it->second.get();
        }

        if (! allowBuild)
            return nullptr;

        DBG("[Warp-Cache] MISS key=" + juce::String(key) + " building...");

        auto data = std::make_unique<AudioFileData>();
        data->sampleRate = originalData.sampleRate;
        data->buffer = stretchBufferToLengthWithExperimentalBackend(originalData.buffer, targetSamples, originalData.sampleRate);

        const auto* dataPtr = data.get();
        warpedAudioCache.emplace(key, std::move(data));
        return dataPtr;
    }

    ProjectState& project;
    TransportEngine& transport;
    juce::AudioFormatManager& audioFormatManager;
    double outputSampleRate { 44100.0 };
    double currentTimelineBeat { 0.0 };
    bool wasPlaying { false };
    std::map<std::string, std::unique_ptr<AudioFileData>> audioCache;
    std::map<std::string, std::unique_ptr<AudioFileData>> warpedAudioCache;
};

class MainComponent::ClickTrackSource final : public juce::AudioSource
{
public:
    ClickTrackSource(ProjectState& state,
                     TransportEngine& engine,
                     juce::TextButton& metronomeToggle)
        : project(state),
          transport(engine),
          metronomeButton(metronomeToggle)
    {
    }

    void prepareToPlay(int, double newSampleRate) override
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    }

    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (info.buffer == nullptr || info.numSamples <= 0)
            return;

        const auto shouldTick = metronomeButton.getToggleState() || transport.isCountInActive();
        if (! shouldTick || (! transport.isPlaying() && ! transport.isCountInActive()) || sampleRate <= 0.0)
        {
            lastBeatIndex = transport.isCountInActive() ? static_cast<int>(std::floor(transport.getClickBeat()))
                                                        : static_cast<int>(std::floor(transport.getPlayheadBeat()));
            currentAmplitude = 0.0f;
            return;
        }

        const auto beatsPerSecond = project.getTempoBpm() / 60.0;
        if (beatsPerSecond <= 0.0)
            return;

        const auto beatAdvancePerSample = beatsPerSecond / sampleRate;
        auto beatPosition = transport.getClickBeat();
        const auto loopActive = transport.isLoopEnabled() && project.hasLoopRange() && ! transport.isCountInActive();
        const auto loopStartBeat = project.getLoopStartBeat();
        const auto loopEndBeat = project.getLoopEndBeat();
        const auto loopSpanBeats = juce::jmax(1.0, loopEndBeat - loopStartBeat);

        for (int sampleIndex = 0; sampleIndex < info.numSamples; ++sampleIndex)
        {
            if (loopActive)
            {
                while (beatPosition >= loopEndBeat)
                    beatPosition = loopStartBeat + std::fmod(beatPosition - loopStartBeat, loopSpanBeats);
            }

            const auto beatIndex = static_cast<int>(std::floor(beatPosition + 0.0001));
            if (beatIndex != lastBeatIndex)
            {
                lastBeatIndex = beatIndex;
                clickPhase = 0.0;
                const auto beatInBar = beatIndex % juce::jmax(1, project.getNumerator());
                clickFrequency = (beatInBar == 0) ? 1760.0 : 1320.0;
                currentAmplitude = (beatInBar == 0) ? 0.42f : 0.28f;
            }

            float clickSample = 0.0f;
            if (currentAmplitude > 0.0005f)
            {
                clickSample = static_cast<float>(std::sin(clickPhase) * currentAmplitude);
                clickPhase += juce::MathConstants<double>::twoPi * clickFrequency / sampleRate;
                currentAmplitude *= 0.9962f;
            }

            for (int channel = 0; channel < info.buffer->getNumChannels(); ++channel)
                info.buffer->addSample(channel, info.startSample + sampleIndex, clickSample);

            beatPosition += beatAdvancePerSample;
        }
    }

private:
    ProjectState& project;
    TransportEngine& transport;
    juce::TextButton& metronomeButton;
    double sampleRate { 44100.0 };
    double clickPhase { 0.0 };
    double clickFrequency { 1320.0 };
    float currentAmplitude { 0.0f };
    int lastBeatIndex { -1 };
};

class SettingsContent final : public juce::Component
{
public:
    SettingsContent(juce::AudioDeviceManager& manager,
                    int initialBrowserWidth,
                    int initialExportSampleRate,
                    std::function<void(int)> onBrowserWidthChanged,
                    std::function<void(int)> onExportSampleRateChanged,
                    std::function<void()> onSave)
        : audioSelector(manager, 0, 2, 0, 2, true, false, false, false),
          browserWidthChanged(std::move(onBrowserWidthChanged)),
          exportSampleRateChanged(std::move(onExportSampleRateChanged)),
          saveCallback(std::move(onSave))
    {
        titleLabel.setText("Settings", juce::dontSendNotification);
        titleLabel.setFont(juce::FontOptions(22.0f, juce::Font::bold));
        titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(titleLabel);

        browserWidthLabel.setText("Browser Width", juce::dontSendNotification);
        browserWidthLabel.setColour(juce::Label::textColourId, mutedText);
        addAndMakeVisible(browserWidthLabel);

        browserWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        browserWidthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 24);
        browserWidthSlider.setRange(minBrowserPanelWidth, maxBrowserPanelWidth, 1.0);
        browserWidthSlider.setValue(initialBrowserWidth, juce::dontSendNotification);
        browserWidthSlider.setColour(juce::Slider::trackColourId, accentColour);
        browserWidthSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
        browserWidthSlider.onValueChange = [this]
        {
            browserWidthChanged(static_cast<int>(std::round(browserWidthSlider.getValue())));
        };
        addAndMakeVisible(browserWidthSlider);

        exportSampleRateLabel.setText("Export Sample Rate", juce::dontSendNotification);
        exportSampleRateLabel.setColour(juce::Label::textColourId, mutedText);
        addAndMakeVisible(exportSampleRateLabel);

        exportSampleRateBox.addItem("44.1 kHz", 44100);
        exportSampleRateBox.addItem("48 kHz", 48000);
        exportSampleRateBox.addItem("96 kHz", 96000);
        exportSampleRateBox.setSelectedId(initialExportSampleRate, juce::dontSendNotification);
        exportSampleRateBox.onChange = [this]
        {
            const auto selected = exportSampleRateBox.getSelectedId();
            if (selected > 0)
                exportSampleRateChanged(selected);
        };
        addAndMakeVisible(exportSampleRateBox);

        audioLabel.setText("Audio Device", juce::dontSendNotification);
        audioLabel.setColour(juce::Label::textColourId, mutedText);
        addAndMakeVisible(audioLabel);

        addAndMakeVisible(audioSelector);

        saveButton.setButtonText("Save");
        saveButton.setColour(juce::TextButton::buttonColourId, accentColour);
        saveButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        saveButton.onClick = [this]
        {
            if (saveCallback)
                saveCallback();

            if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                dialog->setVisible(false);
        };
        addAndMakeVisible(saveButton);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(panelColour);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(18);
        titleLabel.setBounds(area.removeFromTop(28));
        area.removeFromTop(12);

        browserWidthLabel.setBounds(area.removeFromTop(20));
        browserWidthSlider.setBounds(area.removeFromTop(32));
        area.removeFromTop(10);

        exportSampleRateLabel.setBounds(area.removeFromTop(20));
        exportSampleRateBox.setBounds(area.removeFromTop(28).removeFromLeft(140));
        area.removeFromTop(14);

        audioLabel.setBounds(area.removeFromTop(20));
        area.removeFromTop(6);
        auto footerArea = area.removeFromBottom(48);
        audioSelector.setBounds(area);
        saveButton.setBounds(footerArea.removeFromRight(120).reduced(0, 6));
    }

private:
    juce::Label titleLabel;
    juce::Label browserWidthLabel;
    juce::Slider browserWidthSlider;
    juce::Label exportSampleRateLabel;
    juce::ComboBox exportSampleRateBox;
    juce::Label audioLabel;
    juce::AudioDeviceSelectorComponent audioSelector;
    juce::TextButton saveButton;
    std::function<void(int)> browserWidthChanged;
    std::function<void(int)> exportSampleRateChanged;
    std::function<void()> saveCallback;
};

MainComponent::MainComponent()
    : transportEngine(projectState),
      transportController(projectState, transportEngine),
      arrangementTimeline(projectState, transportEngine)
{
    setWantsKeyboardFocus(true);

    headerLabel.setText("ORION", juce::dontSendNotification);
    headerLabel.setFont(juce::FontOptions(27.0f, juce::Font::bold));
    headerLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.96f));
    addAndMakeVisible(headerLabel);

    bpmCaptionLabel.setText("TEMPO", juce::dontSendNotification);
    bpmCaptionLabel.setColour(juce::Label::textColourId, mutedText);
    bpmCaptionLabel.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    addAndMakeVisible(bpmCaptionLabel);

    bpmValueLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    bpmValueLabel.setFont(juce::FontOptions(26.0f, juce::Font::bold));
    bpmValueLabel.setJustificationType(juce::Justification::centred);
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmValueLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(bpmValueLabel);
    bpmValueLabel.setVisible(false);

    bpmEditor.setColour(juce::TextEditor::backgroundColourId, transportDarkPanel);
    bpmEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    bpmEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    bpmEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    bpmEditor.setColour(juce::TextEditor::highlightColourId, juce::Colours::white.withAlpha(0.18f));
    bpmEditor.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
    bpmEditor.setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
    bpmEditor.setJustification(juce::Justification::centred);
    bpmEditor.setFont(bpmValueLabel.getFont());
    bpmEditor.applyFontToAllText(bpmValueLabel.getFont());
    bpmEditor.setBorder(juce::BorderSize<int>(0));
    bpmEditor.setIndents(0, 0);
    bpmEditor.setInputRestrictions(6, "0123456789.");
    bpmEditor.setVisible(false);
    bpmEditor.setAlwaysOnTop(true);
    bpmEditor.onReturnKey = [this]() { endTempoEditing(true); };
    bpmEditor.onEscapeKey = [this]() { endTempoEditing(false); };
    bpmEditor.onFocusLost = [this]() { endTempoEditing(true); };
    addAndMakeVisible(bpmEditor);

    meterCaptionLabel.setText("BPM", juce::dontSendNotification);
    meterCaptionLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.68f));
    meterCaptionLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    addAndMakeVisible(meterCaptionLabel);

    meterValueLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.76f));
    meterValueLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    meterValueLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(meterValueLabel);

    statusLabel.setText("DAW", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.74f));
    statusLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(statusLabel);

    tempoLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(tempoLabel);

    meterLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(meterLabel);

    playheadLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(playheadLabel);

    playlistLabel.setText("Playlist: arrangement-first timeline for clips and loops", juce::dontSendNotification);
    playlistLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(playlistLabel);

    pianoRollLabel.setText("Piano Roll: double-click a MIDI clip to open full focus editor", juce::dontSendNotification);
    pianoRollLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(pianoRollLabel);

    clipInspectorEmptyLabel.setText("Select audio clip", juce::dontSendNotification);
    clipInspectorEmptyLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.58f));
    clipInspectorEmptyLabel.setJustificationType(juce::Justification::centred);
    clipInspectorEmptyLabel.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorEmptyLabel);

    clipInspectorTitleLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.96f));
    clipInspectorTitleLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    addAndMakeVisible(clipInspectorTitleLabel);

    clipInspectorTrackLabel.setColour(juce::Label::textColourId, mutedText);
    clipInspectorTrackLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorTrackLabel);

    clipInspectorFileLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.82f));
    clipInspectorFileLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorFileLabel);

    clipWarpLabel.setText("Warp", juce::dontSendNotification);
    clipWarpLabel.setColour(juce::Label::textColourId, mutedText);
    addAndMakeVisible(clipWarpLabel);

    clipWarpInfoLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
    clipWarpInfoLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipWarpInfoLabel);

    clipSourceBpmLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    clipSourceBpmLabel.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    addAndMakeVisible(clipSourceBpmLabel);

    clipBarsLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    clipBarsLabel.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    addAndMakeVisible(clipBarsLabel);

    clipWarpToggle.setButtonText("");
    clipWarpToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipWarpToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            if ((clip->sourceDurationSeconds <= 0.0 || (clip->sourceBpm <= 0.0 && clip->detectedBars == 0)) && clip->sourcePath.isNotEmpty())
            {
                const auto analysis = analyzeAudioWarpMetadata(juce::File(clip->sourcePath), projectState.getTempoBpm(), projectState.getNumerator());
                if (clip->sourceDurationSeconds <= 0.0)
                    clip->sourceDurationSeconds = analysis.durationSeconds;
                if (clip->sourceBpm <= 0.0 && analysis.sourceBpm > 0.0)
                {
                    clip->sourceBpm = analysis.sourceBpm;
                    clip->bpmGuessed = analysis.bpmGuessed;
                }
                if (clip->detectedBars == 0 && analysis.detectedBars > 0)
                    clip->detectedBars = analysis.detectedBars;
            }

            clip->warpEnabled = clipWarpToggle.getToggleState();
            refreshAudioClipWarpLengths();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipWarpToggle);

    clipGainLabel.setText("Gain", juce::dontSendNotification);
    clipGainLabel.setColour(juce::Label::textColourId, mutedText);
    addAndMakeVisible(clipGainLabel);

    clipGainValueLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipGainValueLabel.setJustificationType(juce::Justification::centredRight);
    clipGainValueLabel.setEditable(false, true, false);
    clipGainValueLabel.onTextChange = [this]() { applyGainFromInspectorText(); };
    addAndMakeVisible(clipGainValueLabel);

    clipGainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    clipGainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    clipGainSlider.setRange(-24.0, 12.0, 0.1);
    clipGainSlider.setColour(juce::Slider::trackColourId, accentColour);
    clipGainSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    clipGainSlider.onValueChange = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->gainDb = clipGainSlider.getValue();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipGainSlider);

    clipMuteToggle.setButtonText("M");
    clipMuteToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipMuteToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->muted = clipMuteToggle.getToggleState();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipMuteToggle);

    clipSoloToggle.setButtonText("S");
    clipSoloToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipSoloToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->solo = clipSoloToggle.getToggleState();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipSoloToggle);

    playButton.setButtonText("PLAY");
    stopButton.setButtonText("STOP");
    recordButton.setButtonText("REC");
    rewindButton.setButtonText("UNDO");
    undoButton.setButtonText("UNDO");
    redoButton.setButtonText("REDO");
    metronomeButton.setButtonText("METRONOME");
    loopButton.setButtonText("LOOP");
    countInButton.setButtonText("COUNT IN");
    browserButton.setButtonText("BROWSER");
    scanPluginsButton.setButtonText("Scan VST3");
    saveButton.setButtonText("SAVE");
    exportButton.setButtonText("EXPORT");
    settingsButton.setButtonText("SETTINGS");

    playButton.setComponentID("play");
    stopButton.setComponentID("stop");
    recordButton.setComponentID("record");
    rewindButton.setComponentID("undo");
    undoButton.setComponentID("undo");
    redoButton.setComponentID("redo");
    metronomeButton.setComponentID("metronome");
    loopButton.setComponentID("loop");
    countInButton.setComponentID("countin");
    browserButton.setComponentID("browser");
    saveButton.setComponentID("save");
    exportButton.setComponentID("export");
    settingsButton.setComponentID("settings");

    for (auto* button : { &playButton, &stopButton, &recordButton, &rewindButton, &undoButton, &redoButton,
                          &metronomeButton, &loopButton, &countInButton, &browserButton, &scanPluginsButton,
                          &saveButton, &exportButton, &settingsButton })
    {
        button->setLookAndFeel(&transportButtonLookAndFeel);
        button->setColour(juce::TextButton::buttonColourId, transportButtonColour);
        button->setColour(juce::TextButton::textColourOffId, transportButtonText);
        button->setColour(juce::TextButton::buttonOnColourId, accentColour);
        button->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        button->addListener(this);
        addAndMakeVisible(*button);
    }

    for (auto* toggleButton : { &metronomeButton, &loopButton, &countInButton })
        toggleButton->setClickingTogglesState(true);
    recordButton.setClickingTogglesState(true);
    metronomeButton.setToggleState(false, juce::dontSendNotification);
    loopButton.setToggleState(false, juce::dontSendNotification);
    countInButton.setToggleState(false, juce::dontSendNotification);

    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xfff7e5e5));
    recordButton.setColour(juce::TextButton::textColourOffId, recordAccent.darker(0.2f));
    rewindButton.setVisible(false);
    scanPluginsButton.setVisible(false);

    addAndMakeVisible(arrangementTimeline);
    addAndMakeVisible(browserPanel);
    addAndMakeVisible(midiEditorOverlay);
    audioFormatManager.registerBasicFormats();
    arrangementPlaybackSource = std::make_unique<ArrangementPlaybackSource>(projectState, transportEngine, audioFormatManager);
    clickTrackSource = std::make_unique<ClickTrackSource>(projectState, transportEngine, metronomeButton);
    audioDeviceManager.initialise(0, 2, nullptr, true);
    audioDeviceManager.addAudioCallback(&previewSourcePlayer);
    masterMixerSource.addInputSource(&previewTransportSource, false);
    masterMixerSource.addInputSource(arrangementPlaybackSource.get(), false);
    masterMixerSource.addInputSource(clickTrackSource.get(), false);
    previewSourcePlayer.setSource(&masterMixerSource);
    browserPanel.onPreviewItem = [this](const BrowserItem& item)
    {
        if (item.isDirectory)
        {
            statusLabel.setText("Folder: " + item.file.getFullPathName(), juce::dontSendNotification);
            return;
        }

        playBrowserPreview(item);
    };
    midiEditorOverlay.onClose = [this]() { arrangementTimeline.grabKeyboardFocus(); };
    midiEditorOverlay.onTogglePlayback = [this]() { toggleTransportFromUi(); };
    midiEditorOverlay.onRequestPlayheadBeat = [this]() { return transportEngine.getPlayheadBeat(); };
    midiEditorOverlay.onRequestPlayingState = [this]() { return transportEngine.isPlaying(); };
    arrangementTimeline.onMidiClipDoubleClick = [this](int trackIndex, int clipIndex)
    {
        auto& track = projectState.getTracks()[static_cast<std::size_t>(trackIndex)];
        auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
        midiEditorOverlay.openClip(track, clip);
    };
    arrangementTimeline.onClipSelectionChanged = [this](int trackIndex, int clipIndex)
    {
        if (trackIndex >= 0 && clipIndex >= 0)
            selectedArrangementClip = std::pair { trackIndex, clipIndex };
        else
            selectedArrangementClip.reset();

        refreshClipInspector();
    };

    resetToPlaylistView();
    setClipInspectorVisible(false);
    updateTransportLabels();
    grabKeyboardFocus();
    startTimerHz(60);
}

MainComponent::~MainComponent()
{
    for (auto* button : { &playButton, &stopButton, &recordButton, &rewindButton, &undoButton, &redoButton,
                          &metronomeButton, &loopButton, &countInButton, &browserButton, &scanPluginsButton,
                          &saveButton, &exportButton, &settingsButton })
    {
        button->setLookAndFeel(nullptr);
    }

    stopBrowserPreview(true);
    masterMixerSource.removeAllInputs();
    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();
    clickTrackSource.reset();
    arrangementPlaybackSource.reset();
    currentPreviewFile = juce::File();
    currentPreviewTempoBpm = 0.0;
    previewSourcePlayer.setSource(nullptr);
    audioDeviceManager.removeAudioCallback(&previewSourcePlayer);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto bounds = getLocalBounds();
    auto topStrip = bounds.removeFromTop(118);

    g.setColour(transportShelfColour);
    g.fillRect(topStrip);
    g.setColour(transportShelfStroke);
    g.drawLine(static_cast<float>(topStrip.getX()), static_cast<float>(topStrip.getBottom() - 1),
               static_cast<float>(topStrip.getRight()), static_cast<float>(topStrip.getBottom() - 1), 1.0f);

    auto transportVisual = topStrip.reduced(18, 14);
    const auto contentWidth = transportBrandWidth + transportClusterWidth + transportTempoWidth + transportModeWidth
        + transportUtilityWidth + transportSectionGap * 4;
    auto contentRow = transportVisual.withSizeKeepingCentre(juce::jmin(contentWidth, transportVisual.getWidth()), transportVisual.getHeight());
    contentRow.removeFromLeft(transportBrandWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto transportCluster = contentRow.removeFromLeft(transportClusterWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto bpmCard = contentRow.removeFromLeft(transportTempoWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto modeCluster = contentRow.removeFromLeft(transportModeWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto rightUtility = contentRow.removeFromLeft(transportUtilityWidth);

    const auto centeredTransportCluster = transportCluster.withSizeKeepingCentre(transportCluster.getWidth(), transportSectionHeight);
    const auto centeredBpmCard = bpmCard.withSizeKeepingCentre(bpmCard.getWidth(), transportSectionHeight);
    const auto centeredModeCluster = modeCluster.withSizeKeepingCentre(modeCluster.getWidth(), transportSectionHeight);
    const auto centeredRightUtility = rightUtility.withSizeKeepingCentre(rightUtility.getWidth(), transportSectionHeight);

    for (const auto& section : { centeredTransportCluster, centeredBpmCard, centeredModeCluster, centeredRightUtility })
    {
        const auto isTempoCard = section == centeredBpmCard;
        g.setColour(isTempoCard ? transportDarkPanel : transportSectionFill);
        g.fillRoundedRectangle(section.toFloat(), 12.0f);
        g.setColour(isTempoCard ? transportShelfStroke.brighter(0.25f) : transportSectionStroke);
        g.drawRoundedRectangle(section.toFloat(), 12.0f, 1.0f);
    }

    for (const auto x : { centeredTransportCluster.getRight() + (transportSectionGap / 2), centeredBpmCard.getRight() + (transportSectionGap / 2), centeredModeCluster.getRight() + (transportSectionGap / 2) })
    {
        g.setColour(juce::Colours::white.withAlpha(0.09f));
        g.drawLine(static_cast<float>(x), static_cast<float>(centeredTransportCluster.getY() + 6),
                   static_cast<float>(x), static_cast<float>(centeredTransportCluster.getBottom() - 6), 1.0f);
    }

    if (! bpmEditor.isVisible())
    {
        auto bpmTextBounds = centeredBpmCard.withSizeKeepingCentre(centeredBpmCard.getWidth(), transportControlHeight)
                                .translated(0, transportContentVerticalNudge);
        bpmTextBounds = bpmTextBounds.removeFromTop(38);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(26.0f, juce::Font::bold));
        g.drawText(juce::String(projectState.getTempoBpm(), 2), bpmTextBounds, juce::Justification::centred);
    }

    auto workArea = bounds.withTrimmedTop(20);
    auto browserPanelBounds = workArea.removeFromLeft(browserPanelWidth);
    auto arrangementPanel = workArea;

    g.setColour(panelColour);
    for (const auto panel : { browserPanelBounds, arrangementPanel })
    {
        g.fillRoundedRectangle(panel.toFloat(), 20.0f);
        g.setColour(panelStroke);
        g.drawRoundedRectangle(panel.toFloat(), 20.0f, 1.0f);
        g.setColour(panelColour);
    }

    const auto resizeHandleBounds = getBrowserResizeHandleBounds();
    g.setColour(juce::Colours::white.withAlpha(isResizingBrowserPanel ? 0.18f : 0.08f));
    g.fillRoundedRectangle(resizeHandleBounds.toFloat(), 4.0f);
    g.setColour(juce::Colours::white.withAlpha(isResizingBrowserPanel ? 0.42f : 0.18f));
    g.drawRoundedRectangle(resizeHandleBounds.toFloat(), 4.0f, 1.0f);

    auto arrangementHeader = arrangementPanel.reduced(18);
    g.setColour(juce::Colours::white.withAlpha(0.92f));
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Playlist", arrangementHeader.removeFromTop(28), juce::Justification::centredLeft);
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(12.5f, juce::Font::plain));
    g.drawText("Double-click a MIDI clip to open Piano Roll full screen", arrangementHeader.removeFromTop(20), juce::Justification::centredLeft);
}

void MainComponent::resized()
{
    // Reduced padding for a sleeker edge-to-edge floating layout
    auto bounds = getLocalBounds().reduced(8);
    auto topStrip = bounds.removeFromTop(118).reduced(18, 14);
    const auto contentWidth = transportBrandWidth + transportClusterWidth + transportTempoWidth + transportModeWidth
        + transportUtilityWidth + transportSectionGap * 4;
    auto contentRow = topStrip.withSizeKeepingCentre(juce::jmin(contentWidth, topStrip.getWidth()), topStrip.getHeight());
    auto leftBrand = contentRow.removeFromLeft(transportBrandWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto transportCluster = contentRow.removeFromLeft(transportClusterWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto bpmCard = contentRow.removeFromLeft(transportTempoWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto modeCluster = contentRow.removeFromLeft(transportModeWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto rightUtility = contentRow.removeFromLeft(transportUtilityWidth);

    auto leftBrandContent = leftBrand.withSizeKeepingCentre(leftBrand.getWidth(), 56);
    headerLabel.setBounds(leftBrandContent.removeFromTop(34));
    statusLabel.setBounds(leftBrandContent.removeFromTop(20));

    auto centeredTransportCluster = transportCluster.withSizeKeepingCentre(transportCluster.getWidth(), transportSectionHeight);
    auto centeredBpmCard = bpmCard.withSizeKeepingCentre(bpmCard.getWidth(), transportSectionHeight);
    auto centeredModeCluster = modeCluster.withSizeKeepingCentre(modeCluster.getWidth(), transportSectionHeight);
    auto centeredRightUtility = rightUtility.withSizeKeepingCentre(rightUtility.getWidth(), transportSectionHeight);

    auto transportButtons = centeredTransportCluster.withSizeKeepingCentre(centeredTransportCluster.getWidth(), transportControlHeight)
                                .translated(0, transportContentVerticalNudge)
                                .reduced(8, 0);
    playButton.setBounds(transportButtons.removeFromLeft(54));
    transportButtons.removeFromLeft(8);
    stopButton.setBounds(transportButtons.removeFromLeft(54));
    transportButtons.removeFromLeft(8);
    recordButton.setBounds(transportButtons.removeFromLeft(54));
    transportButtons.removeFromLeft(8);
    undoButton.setBounds(transportButtons.removeFromLeft(54));
    transportButtons.removeFromLeft(8);
    redoButton.setBounds(transportButtons.removeFromLeft(54));

    auto bpmBounds = centeredBpmCard.withSizeKeepingCentre(centeredBpmCard.getWidth(), transportControlHeight)
                        .translated(0, transportContentVerticalNudge);
    auto bpmTop = bpmBounds.removeFromTop(38);
    bpmValueLabel.setBounds(bpmTop);
    bpmEditor.setBounds(bpmTop);
    if (bpmEditor.isVisible())
        bpmEditor.toFront(false);
    else
    {
        bpmValueLabel.setVisible(false);
    }
    auto bpmBottom = bpmBounds;
    auto bottomLeft = bpmBottom.removeFromLeft(bpmBottom.getWidth() / 2);
    bpmCaptionLabel.setBounds(bottomLeft);
    meterValueLabel.setBounds(bpmBottom);
    meterCaptionLabel.setBounds({});

    auto modeButtons = centeredModeCluster.withSizeKeepingCentre(centeredModeCluster.getWidth(), transportControlHeight)
                           .translated(0, transportContentVerticalNudge)
                           .reduced(8, 0);
    metronomeButton.setBounds(modeButtons.removeFromLeft(56));
    modeButtons.removeFromLeft(8);
    loopButton.setBounds(modeButtons.removeFromLeft(56));
    modeButtons.removeFromLeft(8);
    countInButton.setBounds(modeButtons.removeFromLeft(66));
    modeButtons.removeFromLeft(8);
    browserButton.setBounds(modeButtons.removeFromLeft(56));

    auto utilityButtons = centeredRightUtility.withSizeKeepingCentre(centeredRightUtility.getWidth(), transportControlHeight)
                              .translated(0, transportContentVerticalNudge)
                              .reduced(8, 0);
    saveButton.setBounds(utilityButtons.removeFromLeft(56));
    utilityButtons.removeFromLeft(8);
    exportButton.setBounds(utilityButtons.removeFromLeft(66));
    utilityButtons.removeFromLeft(8);
    settingsButton.setBounds(utilityButtons.removeFromLeft(74));
    scanPluginsButton.setBounds({});

    auto workArea = bounds.withTrimmedTop(20);
    auto browserPanelBounds = workArea.removeFromLeft(browserPanelWidth);
    auto arrangementPanel = workArea;

    auto arrangementInner = arrangementPanel.reduced(18);
    arrangementInner.removeFromTop(42);
    arrangementTimeline.setBounds(arrangementInner);

    auto browserInner = browserPanelBounds.reduced(16);
    browserPanel.setBounds(browserInner);

    tempoLabel.setBounds({});
    meterLabel.setBounds({});
    playheadLabel.setBounds({});
    playlistLabel.setBounds({});
    pianoRollLabel.setBounds({});
    exportButton.setVisible(true);
    saveButton.setVisible(true);
    settingsButton.setVisible(true);
    browserButton.setVisible(true);
    scanPluginsButton.setVisible(false);

    if (const auto trackInspectorBounds = arrangementTimeline.getSelectedTrackInspectorBounds(); trackInspectorBounds.has_value())
    {
        const auto timelineBounds = arrangementTimeline.getBounds();
        auto clipSection = trackInspectorBounds->translated(arrangementTimeline.getX(), arrangementTimeline.getY())
                               .getIntersection(timelineBounds)
                               .reduced(10, 8)
                               .withHeight(juce::jmin(78, juce::jmax(0, trackInspectorBounds->getHeight() - 16)));
        clipInspectorEmptyLabel.setBounds(clipSection);

        clipSection.removeFromTop(30);
        clipInspectorTitleLabel.setBounds({});
        clipInspectorTrackLabel.setBounds({});

        clipInspectorFileLabel.setBounds(clipSection.removeFromTop(14));
        clipSection.removeFromTop(3);

        auto warpRow = clipSection.removeFromTop(14);
        clipWarpLabel.setBounds(warpRow.removeFromLeft(30));
        clipWarpToggle.setBounds(warpRow.removeFromLeft(26));
        clipGainValueLabel.setBounds(warpRow.removeFromRight(48));
        clipWarpInfoLabel.setBounds(warpRow);

        clipSourceBpmLabel.setBounds({});
        clipBarsLabel.setBounds({});
        clipSection.removeFromTop(4);

        clipGainLabel.setBounds({});
        auto controlRow = clipSection.removeFromTop(16);
        clipGainSlider.setBounds(controlRow.removeFromLeft(86));
        controlRow.removeFromLeft(6);

        clipMuteToggle.setBounds(controlRow.removeFromLeft(46));
        controlRow.removeFromLeft(4);
        clipSoloToggle.setBounds(controlRow.removeFromLeft(46));
    }

    midiEditorOverlay.setBounds(getLocalBounds());
}

void MainComponent::mouseMove(const juce::MouseEvent& event)
{
    if (getBrowserResizeHandleBounds().expanded(2, 0).contains(event.getPosition()))
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    else if (! isResizingBrowserPanel)
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void MainComponent::mouseExit(const juce::MouseEvent&)
{
    if (! isResizingBrowserPanel)
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void MainComponent::mouseDown(const juce::MouseEvent& event)
{
    if (event.getNumberOfClicks() >= 2 && bpmValueLabel.getBounds().contains(event.getPosition()))
    {
        beginTempoEditing();
        return;
    }

    if (! getBrowserResizeHandleBounds().expanded(2, 0).contains(event.getPosition()))
        return;

    isResizingBrowserPanel = true;
    browserResizeStartX = event.getPosition().x;
    browserResizeStartWidth = browserPanelWidth;
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
}

void MainComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (bpmValueLabel.getBounds().contains(event.getPosition()))
    {
        beginTempoEditing();
        return;
    }
}

void MainComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (! isResizingBrowserPanel)
        return;

    const auto deltaX = event.getPosition().x - browserResizeStartX;
    browserPanelWidth = juce::jlimit(minBrowserPanelWidth, maxBrowserPanelWidth, browserResizeStartWidth + deltaX);
    resized();
    repaint();
}

void MainComponent::mouseUp(const juce::MouseEvent&)
{
    if (! isResizingBrowserPanel)
        return;

    isResizingBrowserPanel = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress('l', 0, 0) || key == juce::KeyPress('l', juce::ModifierKeys::commandModifier, 0))
    {
        const auto shouldEnable = getSelectedTimelineClip() != nullptr ? true : ! transportEngine.isLoopEnabled();
        loopButton.setToggleState(shouldEnable, juce::sendNotification);
        return true;
    }

    if (key != juce::KeyPress::spaceKey)
        return false;

    if (transportEngine.isPlaying() || transportEngine.isCountInActive())
        stopTransportFromUi();
    else
        toggleTransportFromUi();

    return true;
}

void MainComponent::resetToPlaylistView()
{
    midiEditorOverlay.setVisible(false);
    arrangementTimeline.grabKeyboardFocus();
    repaint();
}

void MainComponent::timerCallback()
{
    updateTransportLabels();
}

void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &playButton)
        toggleTransportFromUi();
    else if (button == &stopButton)
        stopTransportFromUi();
    else if (button == &recordButton)
    {
        transportController.setRecordArmed(recordButton.getToggleState());
        recordButton.setToggleState(transportEngine.isRecordArmed(), juce::dontSendNotification);
        updateTransportLabels();
    }
    else if (button == &rewindButton)
        rewindTransportFromUi();
    else if (button == &undoButton)
    {
        arrangementTimeline.undo();
        updateTransportLabels();
    }
    else if (button == &redoButton)
    {
        arrangementTimeline.redo();
        updateTransportLabels();
    }
    else if (button == &loopButton)
        toggleLoopFromUi();
    else if (button == &metronomeButton || button == &countInButton)
    {
        updateTransportLabels();
    }
    else if (button == &browserButton)
        browserPanel.chooseRootFolder();
    else if (button == &saveButton)
    {
        saveProjectInteractively();
    }
    else if (button == &exportButton)
    {
        exportProjectInteractively();
    }
    else if (button == &settingsButton)
    {
        openSettingsDialog();
    }
}

void MainComponent::updateTransportLabels()
{
    if (! bpmEditor.isVisible())
    {
        bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
        bpmValueLabel.setVisible(false);
        repaint();
    }

    if (transportEngine.isCountInActive())
        meterValueLabel.setText("COUNT", juce::dontSendNotification);
    else
        meterValueLabel.setText(juce::String(projectState.getNumerator()) + "/" + juce::String(projectState.getDenominator()), juce::dontSendNotification);

    if (transportEngine.isCountInActive())
        meterCaptionLabel.setText("COUNT-IN", juce::dontSendNotification);
    else if (transportEngine.isPlaying())
        meterCaptionLabel.setText("RUNNING", juce::dontSendNotification);
    else if (transportEngine.isPaused())
        meterCaptionLabel.setText("PAUSED", juce::dontSendNotification);
    else
        meterCaptionLabel.setText("STOPPED", juce::dontSendNotification);

    playButton.setToggleState(transportEngine.isPlaying() || transportEngine.isCountInActive(), juce::dontSendNotification);
    recordButton.setToggleState(transportEngine.isRecordArmed(), juce::dontSendNotification);
    loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
    undoButton.setEnabled(arrangementTimeline.canUndo());
    redoButton.setEnabled(arrangementTimeline.canRedo());
    tempoLabel.setText("Tempo: " + juce::String(projectState.getTempoBpm(), 0) + " BPM", juce::dontSendNotification);
    meterLabel.setText("Meter: " + juce::String(projectState.getNumerator()) + "/" + juce::String(projectState.getDenominator()), juce::dontSendNotification);
    playheadLabel.setText("Playhead: beat " + juce::String(transportEngine.getPlayheadBeat(), 2), juce::dontSendNotification);
}

void MainComponent::playBrowserPreview(const BrowserItem& item)
{
    statusLabel.setText("Previewing: " + item.file.getFileName(), juce::dontSendNotification);

    if (! item.file.existsAsFile())
    {
        statusLabel.setText("Preview failed: file missing", juce::dontSendNotification);
        return;
    }

    if (previewBufferSource != nullptr
        && item.file == currentPreviewFile
        && std::abs(currentPreviewTempoBpm - projectState.getTempoBpm()) < 0.001)
    {
        previewTransportSource.setPosition(0.0);
        previewTransportSource.start();
        return;
    }

    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();
    currentPreviewFile = juce::File();
    currentPreviewTempoBpm = 0.0;

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(item.file));
    if (reader == nullptr)
    {
        statusLabel.setText("Preview failed: unsupported file", juce::dontSendNotification);
        return;
    }

    const auto sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    const auto maxPreviewSamples = static_cast<juce::int64>(previewMaxLengthSeconds * sampleRate);
    const auto samplesToRead = static_cast<int>(juce::jmin(reader->lengthInSamples, maxPreviewSamples));
    if (samplesToRead <= 0)
    {
        statusLabel.setText("Preview failed: empty file", juce::dontSendNotification);
        return;
    }

    juce::AudioBuffer<float> previewBuffer(static_cast<int>(reader->numChannels), samplesToRead);
    reader->read(&previewBuffer, 0, samplesToRead, 0, true, true);

    if (transportEngine.isPlaying())
    {
        const auto analysis = analyzeAudioWarpMetadata(item.file, projectState.getTempoBpm(), projectState.getNumerator());
        previewBuffer = makeTempoFittedPreviewBuffer(previewBuffer, analysis.sourceBpm, projectState.getTempoBpm(), sampleRate);
    }

    previewBufferSource = std::make_unique<BufferPreviewSource>(std::move(previewBuffer), sampleRate);
    previewTransportSource.setSource(previewBufferSource.get(), 0, nullptr, sampleRate);
    currentPreviewFile = item.file;
    currentPreviewTempoBpm = projectState.getTempoBpm();
    previewTransportSource.setPosition(0.0);
    previewTransportSource.start();
}

void MainComponent::stopBrowserPreview(bool resetPosition)
{
    previewTransportSource.stop();
    if (resetPosition)
        previewTransportSource.setPosition(0.0);
}

void MainComponent::toggleTransportFromUi()
{
    transportController.togglePlayback(
        countInButton.getToggleState(),
        [this]() { stopBrowserPreview(true); },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
        },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    updateTransportLabels();
}

void MainComponent::stopTransportFromUi()
{
    transportController.stop(
        [this]() { stopBrowserPreview(true); },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::rewindTransportFromUi()
{
    transportController.rewind(
        [this]() { stopBrowserPreview(true); },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::toggleLoopFromUi()
{
    transportController.setLoopEnabled(
        loopButton.getToggleState(),
        getSelectedTimelineClip(),
        [this](const juce::String& statusText)
        {
            statusLabel.setText(statusText, juce::dontSendNotification);
        });
    loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::saveProjectInteractively()
{
    auto saveToTarget = [this](const juce::File& targetFile)
    {
        juce::String errorMessage;
        if (ProjectSerializer::saveToFile(projectState, targetFile, &errorMessage))
        {
            currentProjectFile = targetFile;
            statusLabel.setText("Saved: " + targetFile.getFileName(), juce::dontSendNotification);
        }
        else
        {
            statusLabel.setText("Save failed: " + errorMessage, juce::dontSendNotification);
        }
    };

    if (currentProjectFile.existsAsFile())
    {
        saveToTarget(currentProjectFile);
        return;
    }

    auto defaultDirectory = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto defaultTarget = defaultDirectory.getChildFile("Untitled.orion.json");
    saveFileChooser = std::make_unique<juce::FileChooser>("Save Orion Project",
                                                          defaultTarget,
                                                          "*.orion.json");

    auto chooserFlags = juce::FileBrowserComponent::saveMode
                      | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    saveFileChooser->launchAsync(chooserFlags,
                                 [this, saveToTarget](const juce::FileChooser& chooser)
                                 {
                                     auto selectedFile = chooser.getResult();
                                     saveFileChooser.reset();

                                     if (selectedFile == juce::File())
                                     {
                                         statusLabel.setText("Save cancelled", juce::dontSendNotification);
                                         return;
                                     }

                                     if (! selectedFile.hasFileExtension("orion.json"))
                                         selectedFile = selectedFile.withFileExtension(".orion.json");

                                     saveToTarget(selectedFile);
                                 });
}

void MainComponent::exportProjectInteractively()
{
    if (arrangementPlaybackSource == nullptr)
    {
        statusLabel.setText("Export failed: playback source unavailable", juce::dontSendNotification);
        return;
    }

    auto defaultDirectory = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto defaultTarget = defaultDirectory.getChildFile("Orion Export.wav");
    exportFileChooser = std::make_unique<juce::FileChooser>("Export Orion Mix",
                                                            defaultTarget,
                                                            "*.wav");

    auto chooserFlags = juce::FileBrowserComponent::saveMode
                      | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    exportFileChooser->launchAsync(chooserFlags,
                                   [this](const juce::FileChooser& chooser)
                                   {
                                       auto selectedFile = chooser.getResult();
                                       exportFileChooser.reset();

                                       if (selectedFile == juce::File())
                                       {
                                           statusLabel.setText("Export cancelled", juce::dontSendNotification);
                                           return;
                                       }

                                       if (! selectedFile.hasFileExtension("wav"))
                                           selectedFile = selectedFile.withFileExtension(".wav");

                                       juce::String errorMessage;
                                       const auto exported = ExportService::exportToWav(
                                           projectState,
                                           transportEngine.isLoopEnabled(),
                                           exportSampleRate,
                                           selectedFile,
                                           [this]()
                                           {
                                               if (arrangementPlaybackSource != nullptr)
                                                   arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
                                           },
                                           [this](juce::AudioBuffer<float>& buffer,
                                                  int startSample,
                                                  int numSamples,
                                                  double blockStartBeat,
                                                  double renderSampleRate)
                                           {
                                               if (arrangementPlaybackSource != nullptr)
                                                   arrangementPlaybackSource->renderOfflineBlock(buffer,
                                                                                                 startSample,
                                                                                                 numSamples,
                                                                                                 blockStartBeat,
                                                                                                 renderSampleRate);
                                           },
                                           &errorMessage);

                                       statusLabel.setText(exported
                                                               ? "Exported: " + selectedFile.getFileName()
                                                               : "Export failed: " + errorMessage,
                                                           juce::dontSendNotification);
                                   });
}

void MainComponent::openSettingsDialog()
{
    auto settingsComponent = std::make_unique<SettingsContent>(
        audioDeviceManager,
        browserPanelWidth,
        exportSampleRate,
        [this](int newWidth)
        {
            browserPanelWidth = juce::jlimit(minBrowserPanelWidth, maxBrowserPanelWidth, newWidth);
            resized();
            repaint();
        },
        [this](int newRate)
        {
            exportSampleRate = newRate;
            statusLabel.setText("Export sample rate: " + juce::String(exportSampleRate / 1000.0, 1) + " kHz",
                                juce::dontSendNotification);
        },
        [this]()
        {
            statusLabel.setText("Settings saved", juce::dontSendNotification);
        });

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(settingsComponent.release());
    options.dialogTitle = "Orion Settings";
    options.dialogBackgroundColour = panelColour;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = true;
    options.componentToCentreAround = this;
    options.content->setSize(560, 520);
    options.launchAsync();
}

void MainComponent::refreshAudioClipWarpLengths()
{
    for (auto& track : projectState.getTracks())
    {
        for (auto& clip : track.clips)
        {
            if (clip.type != ClipType::audio || clip.sourcePath.isEmpty())
                continue;

            const auto sourceFile = juce::File(clip.sourcePath);
            const auto namedBpm = parseBpmFromFileName(sourceFile);
            const auto needsAnalysis = clip.sourceDurationSeconds <= 0.0
                || (clip.sourceBpm <= 0.0 && clip.detectedBars == 0)
                || (namedBpm > 0.0 && std::abs(clip.sourceBpm - namedBpm) > 0.01);

            if (needsAnalysis)
            {
                const auto analysis = analyzeAudioWarpMetadata(sourceFile, projectState.getTempoBpm(), projectState.getNumerator());
                if (clip.sourceDurationSeconds <= 0.0 && analysis.durationSeconds > 0.0)
                    clip.sourceDurationSeconds = analysis.durationSeconds;
                if ((clip.sourceBpm <= 0.0 || namedBpm > 0.0) && analysis.sourceBpm > 0.0)
                {
                    clip.sourceBpm = analysis.sourceBpm;
                    clip.bpmGuessed = analysis.bpmGuessed;
                }
                if ((clip.detectedBars == 0 || namedBpm > 0.0) && analysis.detectedBars > 0)
                    clip.detectedBars = analysis.detectedBars;
            }

            if (clip.warpEnabled)
            {
                if (clip.detectedBars > 0)
                    clip.lengthInBeats = juce::jmax(1.0, static_cast<double>(clip.detectedBars * juce::jmax(1, projectState.getNumerator())));
                else if (clip.sourceDurationSeconds > 0.0 && clip.sourceBpm > 0.0)
                    clip.lengthInBeats = juce::jmax(1.0, clip.sourceDurationSeconds * (clip.sourceBpm / 60.0));
            }
            else if (clip.sourceDurationSeconds > 0.0)
            {
                clip.lengthInBeats = juce::jmax(1.0, clip.sourceDurationSeconds * (projectState.getTempoBpm() / 60.0));
            }
        }
    }
}

void MainComponent::refreshClipInspector()
{
    const auto* clip = getSelectedTimelineClip();
    if (clip == nullptr || clip->type != ClipType::audio)
    {
        setClipInspectorVisible(false);
        clipInspectorEmptyLabel.setVisible(false);
        clipInspectorEmptyLabel.setText("Select audio clip", juce::dontSendNotification);
        resized();
        repaint();
        return;
    }

    setClipInspectorVisible(true);
    clipInspectorEmptyLabel.setVisible(false);
    clipInspectorTitleLabel.setText(clip->name, juce::dontSendNotification);

    const auto [trackIndex, clipIndex] = *selectedArrangementClip;
    juce::ignoreUnused(clipIndex);
    clipInspectorTrackLabel.setText("Track: " + projectState.getTracks()[static_cast<std::size_t>(trackIndex)].name, juce::dontSendNotification);

    auto* mutableClip = getSelectedTimelineClip();
    if (mutableClip != nullptr && mutableClip->sourcePath.isNotEmpty())
    {
        const auto sourceFile = juce::File(mutableClip->sourcePath);
        const auto namedBpm = parseBpmFromFileName(sourceFile);
        const auto shouldEnableDefaultWarp = ! mutableClip->warpEnabled
            && mutableClip->sourceDurationSeconds <= 0.0
            && mutableClip->sourceBpm <= 0.0
            && mutableClip->detectedBars == 0;

        if (mutableClip->sourceDurationSeconds <= 0.0
            || (mutableClip->sourceBpm <= 0.0 && mutableClip->detectedBars == 0)
            || (namedBpm > 0.0 && std::abs(mutableClip->sourceBpm - namedBpm) > 0.01))
        {
            const auto analysis = analyzeAudioWarpMetadata(sourceFile, projectState.getTempoBpm(), projectState.getNumerator());
            if (mutableClip->sourceDurationSeconds <= 0.0 && analysis.durationSeconds > 0.0)
                mutableClip->sourceDurationSeconds = analysis.durationSeconds;
            if ((mutableClip->sourceBpm <= 0.0 || namedBpm > 0.0) && analysis.sourceBpm > 0.0)
            {
                mutableClip->sourceBpm = analysis.sourceBpm;
                mutableClip->bpmGuessed = analysis.bpmGuessed;
            }
            if ((mutableClip->detectedBars == 0 || namedBpm > 0.0) && analysis.detectedBars > 0)
                mutableClip->detectedBars = analysis.detectedBars;
        }

        if (shouldEnableDefaultWarp && mutableClip->sourceBpm > 0.0)
            mutableClip->warpEnabled = true;
        if (mutableClip->warpEnabled && mutableClip->detectedBars > 0)
            mutableClip->lengthInBeats = static_cast<double>(mutableClip->detectedBars * juce::jmax(1, projectState.getNumerator()));
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
    }

    auto sourceFile = juce::File(clip->sourcePath);
    clipInspectorFileLabel.setText(compactInspectorFileName(sourceFile, clip->name), juce::dontSendNotification);
    clipWarpToggle.setToggleState(clip->warpEnabled, juce::dontSendNotification);
    clipWarpInfoLabel.setText(clip->warpEnabled ? "Warp" : "Raw", juce::dontSendNotification);
    clipSourceBpmLabel.setText(
        clip->sourceBpm > 0.0 ? "Source BPM: " + juce::String(clip->sourceBpm, 1) : "Source BPM: not detected",
        juce::dontSendNotification);
    clipBarsLabel.setText(
        clip->detectedBars > 0
            ? "Detected loop: " + juce::String(clip->detectedBars) + " bar" + (clip->detectedBars == 1 ? "" : "s")
            : "Detected loop: free length",
        juce::dontSendNotification);

    clipGainSlider.setValue(clip->gainDb, juce::dontSendNotification);
    clipGainValueLabel.setText(juce::String(clip->gainDb, 1) + " dB", juce::dontSendNotification);

    clipMuteToggle.setToggleState(clip->muted, juce::dontSendNotification);
    clipSoloToggle.setToggleState(clip->solo, juce::dontSendNotification);
    resized();
    repaint();
}

void MainComponent::setClipInspectorVisible(bool shouldShow)
{
    // The floating clip inspector is disabled until it is rebuilt inside the
    // track header; drawing it as a MainComponent overlay made it drift away
    // from the selected track when the playlist scrolled or resized.
    shouldShow = false;

    clipInspectorEmptyLabel.setVisible(false);
    clipInspectorTitleLabel.setVisible(false);
    clipInspectorTrackLabel.setVisible(false);
    clipGainLabel.setVisible(false);
    clipSourceBpmLabel.setVisible(false);
    clipBarsLabel.setVisible(false);
    for (auto* component : { static_cast<juce::Component*>(&clipInspectorFileLabel),
                             static_cast<juce::Component*>(&clipWarpLabel),
                             static_cast<juce::Component*>(&clipWarpInfoLabel),
                             static_cast<juce::Component*>(&clipWarpToggle),
                             static_cast<juce::Component*>(&clipGainValueLabel),
                             static_cast<juce::Component*>(&clipGainSlider),
                             static_cast<juce::Component*>(&clipMuteToggle),
                             static_cast<juce::Component*>(&clipSoloToggle) })
    {
        component->setVisible(shouldShow);
        if (shouldShow)
            component->toFront(false);
    }

    if (shouldShow)
    {
        clipInspectorEmptyLabel.toFront(false);
        bpmEditor.toFront(false);
    }
}

void MainComponent::applyGainFromInspectorText()
{
    auto* clip = getSelectedTimelineClip();
    if (clip == nullptr)
        return;

    auto text = clipGainValueLabel.getText().upToFirstOccurrenceOf("dB", false, false).trim();
    if (text.isEmpty())
        return;

    const auto parsedValue = text.getDoubleValue();
    const auto clampedValue = juce::jlimit(-24.0, 12.0, parsedValue);
    clip->gainDb = clampedValue;
    clipGainSlider.setValue(clampedValue, juce::dontSendNotification);
    clipGainValueLabel.setText(juce::String(clampedValue, 1) + " dB", juce::dontSendNotification);
    arrangementTimeline.repaint();
}

void MainComponent::applyTempoFromTransportText()
{
    auto text = bpmEditor.getText().trim();
    if (text.isEmpty())
        return;

    const auto parsedValue = text.getDoubleValue();
    if (parsedValue <= 0.0)
    {
        updateTransportLabels();
        return;
    }

    transportController.setTempoBpm(parsedValue);
    refreshAudioClipWarpLengths();
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    updateTransportLabels();
    refreshClipInspector();
    arrangementTimeline.repaint();
}

void MainComponent::beginTempoEditing()
{
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmValueLabel.setVisible(false);
    bpmEditor.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmEditor.setVisible(true);
    bpmEditor.toFront(false);
    bpmEditor.repaint();
    bpmEditor.selectAll();
    bpmEditor.grabKeyboardFocus();
}

void MainComponent::endTempoEditing(bool applyChanges)
{
    if (! bpmEditor.isVisible())
        return;

    if (applyChanges)
        applyTempoFromTransportText();

    bpmEditor.setVisible(false);
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmValueLabel.setVisible(false);
    repaint();
}

TimelineClip* MainComponent::getSelectedTimelineClip() noexcept
{
    if (! selectedArrangementClip.has_value())
        return nullptr;

    const auto [trackIndex, clipIndex] = *selectedArrangementClip;
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || clipIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return nullptr;

    auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
    if (clipIndex >= static_cast<int>(clips.size()))
        return nullptr;

    return &clips[static_cast<std::size_t>(clipIndex)];
}

const TimelineClip* MainComponent::getSelectedTimelineClip() const noexcept
{
    if (! selectedArrangementClip.has_value())
        return nullptr;

    const auto [trackIndex, clipIndex] = *selectedArrangementClip;
    const auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || clipIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return nullptr;

    const auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
    if (clipIndex >= static_cast<int>(clips.size()))
        return nullptr;

    return &clips[static_cast<std::size_t>(clipIndex)];
}

juce::Rectangle<int> MainComponent::getBrowserResizeHandleBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(8);
    bounds.removeFromTop(112);
    auto workArea = bounds;
    auto browserPanelBounds = workArea.removeFromLeft(browserPanelWidth);
    return browserPanelBounds.withTrimmedLeft(browserPanelBounds.getWidth() - (browserResizeHandleWidth / 2))
        .withWidth(browserResizeHandleWidth)
        .reduced(0, 16);
}

}  // namespace orion
