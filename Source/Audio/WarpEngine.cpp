#include "WarpEngine.h"
#include "OrionStretchEngine.h"

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

#include <signalsmith-stretch/signalsmith-stretch.h>
#if ORION_HAVE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

namespace orion
{
const char* const warpBackendCacheVersion = ORION_HAVE_RUBBERBAND ? "rubberband_exp1_drain1" : "signalsmith_fallback";

juce::String currentWarpBackendTag()
{
    return juce::String(warpBackendCacheVersion) + (isOrionWarpEnabled() ? "|orion_v0" : "");
}

juce::String formatKeyName(int rootSemi, bool minor, bool fullName)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (rootSemi < 0 || rootSemi > 11) return "?";
    juce::String s(names[rootSemi]);
    if (fullName) s += minor ? " minor" : " major";
    else          s += minor ? "m" : "";
    return s;
}

namespace
{
// Parses musical key from a filename. Strict — only accepts standalone token candidates
// (rejects words like "Bass", "Drum" that start with a note letter). Handles "Cm",
// "C minor", "C_minor", "Cmin", "Cmaj", "C# minor", "Db", "Fsharp", "Bbm" etc.
struct ParsedKey { int root { -1 }; bool minor { false }; };
ParsedKey parseKeyFromFileName(const juce::File& file)
{
    auto raw  = file.getFileNameWithoutExtension();
    auto text = juce::String(' ') + raw.toLowerCase().replaceCharacters("_-.()[]", "       ") + juce::String(' ');

    auto letterToSemi = [](juce::juce_wchar c) -> int
    {
        switch (c)
        {
            case 'c': return 0;
            case 'd': return 2;
            case 'e': return 4;
            case 'f': return 5;
            case 'g': return 7;
            case 'a': return 9;
            case 'b': return 11;
        }
        return -1;
    };
    auto isLetter = [](juce::juce_wchar c) { return c >= 'a' && c <= 'z'; };

    ParsedKey best { -1, false };
    int bestScore = 0;

    for (int i = 1; i + 1 < text.length(); ++i)
    {
        const auto prev = text[i - 1];
        if (prev != ' ' && prev != '\t') continue;
        const auto c = text[i];
        const auto semi = letterToSemi(c);
        if (semi < 0) continue;

        int rootSemi = semi;
        int pos = i + 1;
        bool hadAccidental = false;

        if (pos < text.length())
        {
            const auto a = text[pos];
            if (a == '#')
            {
                rootSemi = (rootSemi + 1) % 12; ++pos; hadAccidental = true;
            }
            else if (a == 'b' && pos + 1 < text.length())
            {
                const auto next = text[pos + 1];
                if (next == 'm' || next == ' ' || next == '\t' || (next >= '0' && next <= '9'))
                {
                    rootSemi = (rootSemi + 11) % 12; ++pos; hadAccidental = true;
                }
            }
        }

        int modePos = pos;
        while (modePos < text.length() && (text[modePos] == ' ' || text[modePos] == '\t')) ++modePos;
        const bool skippedSpace = modePos > pos;

        bool isMinor = false, modeKnown = false, isValid = false;

        if (modePos >= text.length())
        {
            isValid = true;
        }
        else
        {
            const auto rem = text.substring(modePos, juce::jmin(modePos + 6, text.length()));
            if      (rem.startsWith("minor")) { isMinor = true;  modeKnown = true; isValid = true; }
            else if (rem.startsWith("major")) { isMinor = false; modeKnown = true; isValid = true; }
            else if (rem.startsWith("min"))   { isMinor = true;  modeKnown = true; isValid = true; }
            else if (rem.startsWith("maj"))   { isMinor = false; modeKnown = true; isValid = true; }
            else if (! skippedSpace && text[pos] == 'm')
            {
                if (pos + 1 >= text.length() || ! isLetter(text[pos + 1]))
                {
                    isMinor = true; modeKnown = true; isValid = true;
                }
            }
            else if (skippedSpace)
            {
                isValid = true;
            }
        }

        if (! isValid) continue;

        const int score = (modeKnown ? 1000 : 100) + (hadAccidental ? 50 : 0) + juce::jmax(0, 100 - i);
        if (score > bestScore)
        {
            bestScore = score;
            best.root = rootSemi;
            best.minor = isMinor;
        }
    }
    return best;
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

// Peak-based detector tuned for kick/snare: 1.5ms window, 45% rise threshold, 80ms min gap.
std::vector<int> detectStrongTransients(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    std::vector<int> transients;
    const auto numSamples  = buffer.getNumSamples();
    const auto numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return transients;

    const auto windowSize    = juce::jmax(1, static_cast<int>(sampleRate * 0.0015));
    const auto envelopeSize  = numSamples / windowSize;
    if (envelopeSize < 3)
        return transients;

    std::vector<float> envelope(static_cast<size_t>(envelopeSize), 0.0f);
    for (int i = 0; i < envelopeSize; ++i)
    {
        float peak = 0.0f;
        const auto start = i * windowSize;
        const auto end   = juce::jmin(start + windowSize, numSamples);
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

    const auto threshold    = maxRise * 0.45f;
    const auto minPeakLevel = 0.12f;
    const auto minGapWin    = juce::jmax(1, static_cast<int>(sampleRate * 0.08 / windowSize));
    int lastPeak = -minGapWin;

    for (int i = 1; i < envelopeSize - 1; ++i)
    {
        if (rise[static_cast<size_t>(i)] > threshold
            && envelope[static_cast<size_t>(i)] >= minPeakLevel
            && rise[static_cast<size_t>(i)] >= rise[static_cast<size_t>(i - 1)]
            && rise[static_cast<size_t>(i)] >= rise[static_cast<size_t>(i + 1)]
            && (i - lastPeak) >= minGapWin)
        {
            transients.push_back(i * windowSize);
            lastPeak = i;
        }
    }
    return transients;
}

// Crest factor = peak / RMS. High (>10) means percussive gaps between hits (drums).
// Low (<8) means sustained energy (melodic). Reliable discriminator.
float computeCrestFactor(const juce::AudioBuffer<float>& buffer)
{
    const auto numSamples  = buffer.getNumSamples();
    const auto numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return 1.0f;
    float peakVal = 0.0f, sumSq = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch)
        for (int s = 0; s < numSamples; ++s)
        {
            const auto v = std::abs(buffer.getSample(ch, s));
            peakVal = juce::jmax(peakVal, v);
            sumSq  += v * v;
        }
    const auto rms = std::sqrt(sumSq / static_cast<float>(numSamples * numChannels));
    return rms > 0.0001f ? peakVal / rms : 1.0f;
}

// Forward declaration of the underlying RubberBand path.
juce::AudioBuffer<float> rubberBandStretchBufferToLength(const juce::AudioBuffer<float>& source,
                                                          int outputSamples,
                                                          double sampleRate,
                                                          const std::map<std::size_t, std::size_t>& keyframes,
                                                          double pitchScale,
                                                          bool percussive);

// Drum warp: pre-emphasis + keyframed RubberBand + de-emphasis.
// (1) Apply a HF-boost filter to source. HF transient detail is amplified.
// (2) RubberBand stretches the pre-emphasised source. Its STFT-smearing affects the
//     loud HF content less severely (because it's loud → survives smearing relatively).
// (3) Apply symmetric HF-cut to the output. HF returns to natural level, but the
//     PROPORTION of transient HF energy is higher than without pre-emphasis.
// Classic technique from tape recording / audio coding to preserve detail through
// lossy processing. Headroom is consumed during stage 1 so we attenuate to avoid clipping.
juce::AudioBuffer<float> drumBeatsTailfillBuffer(const juce::AudioBuffer<float>& source,
                                                  int outputSamples,
                                                  double sampleRate)
{
    const auto sourceSamples = source.getNumSamples();
    const auto channels      = source.getNumChannels();
    if (sourceSamples <= 1 || outputSamples <= 1 || channels <= 0 || sampleRate <= 0.0)
        return source;

    // Pre-emphasis filter: y[n] = x[n] - alpha * x[n-1], boosts HF before stretching.
    // alpha = 0.85 at 44.1kHz boosts roughly +6dB above 2.5kHz so HF transient detail
    // survives RubberBand's STFT smearing. Headroom attenuation prevents clipping
    // from the boost; de-emphasis afterwards restores natural balance.
    const auto preAlpha   = 0.85f;
    const auto headroom   = 0.5f;
    juce::AudioBuffer<float> emphSource(channels, sourceSamples);
    for (int ch = 0; ch < channels; ++ch)
    {
        const auto srcCh = juce::jmin(ch, source.getNumChannels() - 1);
        float prev = 0.0f;
        for (int s = 0; s < sourceSamples; ++s)
        {
            const auto x = source.getSample(srcCh, s) * headroom;
            const auto y = x - preAlpha * prev;
            emphSource.setSample(ch, s, y);
            prev = x;
        }
    }

    // Stretch with keyframes.
    const auto transients = detectStrongTransients(source, sampleRate);
    const auto timeRatio  = static_cast<double>(outputSamples) / static_cast<double>(sourceSamples);

    std::map<std::size_t, std::size_t> keyframes;
    for (auto t : transients)
        if (t > 0 && t < sourceSamples)
            keyframes[static_cast<std::size_t>(t)] =
                static_cast<std::size_t>(std::round(static_cast<double>(t) * timeRatio));

    // NOTE: tried RubberBand's documented R2 percussive preset (short window +
    // independent phase) here — it actually smeared the snare and made the kick
    // watery. The R3 "Finer" engine (percussive=false) keeps drum transients
    // tighter, so we stay on it.
    auto output = rubberBandStretchBufferToLength(emphSource, outputSamples, sampleRate, keyframes, 1.0, /*percussive*/ false);
    if (output.getNumSamples() < outputSamples || output.getNumChannels() != channels)
        return output;

    // De-emphasis filter: inverse of pre-emphasis, restores natural HF balance.
    // y[n] = x[n] + alpha * y[n-1]. Also undoes the headroom attenuation.
    const auto restore = 1.0f / headroom;
    for (int ch = 0; ch < channels; ++ch)
    {
        float prev = 0.0f;
        for (int s = 0; s < output.getNumSamples(); ++s)
        {
            const auto x = output.getSample(ch, s);
            const auto y = x + preAlpha * prev;
            prev = y;
            output.setSample(ch, s, y * restore);
        }
    }

    return output;
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
juce::AudioBuffer<float> rubberBandStretchBufferToLength(const juce::AudioBuffer<float>& source,
                                                          int outputSamples,
                                                          double sampleRate,
                                                          const std::map<std::size_t, std::size_t>& keyframes = {},
                                                          double pitchScale = 1.0,
                                                          bool percussive = false)
{
    if (source.getNumSamples() <= 1 || outputSamples <= 1 || sampleRate <= 0.0)
        return source;

    // Bypass the stretcher only when neither time nor pitch is being changed.
    if (std::abs(outputSamples - source.getNumSamples()) <= 1 && std::abs(pitchScale - 1.0) < 0.0001)
        return source;

    try
    {
        const auto channels = source.getNumChannels();
        const auto sourceSamples = source.getNumSamples();
        const auto timeRatio = static_cast<double>(outputSamples) / static_cast<double>(sourceSamples);

        using RB = RubberBand::RubberBandStretcher;
        // Percussive (drums): RubberBand's documented percussive preset on the R2
        // engine — short window + independent phase + crisp transients + percussive
        // onset detector. Keeps drum attacks tight. (The R3 "Finer" engine ignores
        // these R2 options, which is why drums use the R2 engine here.)
        // Melodic: the R3 "Finer" engine with crisp transients + compound detector.
        const RB::Options options = percussive
            ? (RB::OptionProcessOffline
               | RB::OptionWindowShort
               | RB::OptionPhaseIndependent
               | RB::OptionTransientsCrisp
               | RB::OptionDetectorPercussive
               | RB::OptionChannelsTogether)
            : (RB::OptionProcessOffline
               | RB::OptionEngineFiner
               | RB::OptionTransientsCrisp
               | RB::OptionDetectorCompound
               | RB::OptionChannelsTogether);

        RubberBand::RubberBandStretcher stretcher(
            static_cast<std::size_t>(std::round(sampleRate)),
            static_cast<std::size_t>(channels),
            options,
            timeRatio,
            pitchScale);

        constexpr int blockSize = 16384;
        stretcher.setExpectedInputDuration(static_cast<std::size_t>(sourceSamples));
        stretcher.setMaxProcessSize(static_cast<std::size_t>(blockSize));
        stretcher.setTimeRatio(timeRatio);
        stretcher.setPitchScale(pitchScale);

        // Lock transient positions to the warp grid — this is how Ableton's Beats/Complex
        // modes preserve attacks. RubberBand stretches the gaps between keyframes only;
        // the transient samples themselves are aligned exactly to their target positions.
        if (! keyframes.empty())
            stretcher.setKeyFrameMap(keyframes);

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
            drainAvailableOutput();
        }

        drainAvailableOutput();

        const auto writtenSamples = collected.empty() ? 0 : static_cast<int>(collected.front().size());
        const auto returnedSamples = writtenSamples >= outputSamples ? outputSamples : writtenSamples;
        juce::AudioBuffer<float> stretched(channels, returnedSamples);
        for (int channel = 0; channel < channels; ++channel)
        {
            if (returnedSamples > 0)
                stretched.copyFrom(channel, 0, collected[static_cast<std::size_t>(channel)].data(), returnedSamples);
        }

        DBG("[Warp-RubberBand] sourceSamples=" + juce::String(sourceSamples)
            + " targetSamples=" + juce::String(outputSamples)
            + " outputSamples=" + juce::String(stretched.getNumSamples())
            + " writtenSamples=" + juce::String(writtenSamples)
            + " timeRatio=" + juce::String(timeRatio, 6)
            + " pitchScale=1.000000");

        if (writtenSamples != outputSamples)
            DBG("[Warp-RubberBand] drain length mismatch; not padding silent tail. targetSamples="
                + juce::String(outputSamples)
                + " writtenSamples=" + juce::String(writtenSamples));

        return stretched;
    }
    catch (...)
    {
        DBG("[Warp-RubberBand] failed; falling back to Signalsmith");
        return stretchBufferToLength(source, outputSamples, sampleRate);
    }
}
#endif

enum class LoopType { drum, melodic };

// Multi-signal loop classifier. Path keywords are the strongest signal;
// audio analysis (crest + transients) is the fallback for unnamed/ambiguous files.
LoopType inferLoopType(const juce::String& sourcePath,
                       const juce::AudioBuffer<float>& source,
                       double sampleRate)
{
    const auto pathLower = sourcePath.toLowerCase();

    static const std::vector<juce::String> drumKeywords {
        "drum", "kick", "snare", "break", "808", "hihat", "hi-hat", "hi_hat",
        "perc", "clap", "tom", "ride", "cymbal", "rim", "shaker", "tamb",
        "conga", "bongo"
    };
    static const std::vector<juce::String> melodicKeywords {
        "melody", "melodic", "lead", "pad", "synth", "piano", "guitar",
        "violin", "string", "flute", "horn", "sax", "chord", "vocal", "vox",
        "keys", "arp", "bell", "rhodes"
    };

    bool drumHit = false, melodicHit = false;
    for (const auto& kw : drumKeywords)
        if (pathLower.contains(kw)) { drumHit = true; break; }
    for (const auto& kw : melodicKeywords)
        if (pathLower.contains(kw)) { melodicHit = true; break; }

    // Path keyword is the strongest signal — if the file is named clearly, trust the name.
    if (drumHit && ! melodicHit)  { DBG("[Warp] inferLoopType: drum (path)");    return LoopType::drum; }
    if (melodicHit && ! drumHit)  { DBG("[Warp] inferLoopType: melodic (path)"); return LoopType::melodic; }

    // Ambiguous (both or neither matched): fall back to audio analysis.
    const auto strongTransients = detectStrongTransients(source, sampleRate);
    if (strongTransients.size() < 2)
    {
        DBG("[Warp] inferLoopType: melodic (no transients)");
        return LoopType::melodic;
    }

    const auto duration = sampleRate > 0.0 ? static_cast<double>(source.getNumSamples()) / sampleRate : 0.0;
    const auto density  = duration > 0.1   ? static_cast<double>(strongTransients.size()) / duration : 0.0;
    if (density < 1.0 || density > 8.0)
    {
        DBG("[Warp] inferLoopType: melodic (density=" + juce::String(density, 2) + ")");
        return LoopType::melodic;
    }

    const auto crest = computeCrestFactor(source);
    DBG("[Warp] inferLoopType: density=" + juce::String(density, 2)
        + " crest=" + juce::String(crest, 1)
        + " -> " + juce::String(crest >= 7.0f ? "drum" : "melodic"));
    return (crest >= 7.0f) ? LoopType::drum : LoopType::melodic;
}
}  // namespace

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

// ---- Real signal analysis (used when the filename gives no answer) ----------
namespace
{
// Load up to maxSeconds of a file, summed to mono, for offline analysis.
juce::AudioBuffer<float> loadMonoForAnalysis(const juce::File& file, double& sampleRateOut, double maxSeconds = 30.0)
{
    sampleRateOut = 0.0;
    static juce::AudioFormatManager fm;
    static bool registered = false;
    if (! registered) { fm.registerBasicFormats(); registered = true; }

    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return {};

    const auto cap = static_cast<juce::int64>(std::llround(maxSeconds * reader->sampleRate));
    const auto total = static_cast<int>(juce::jmin(reader->lengthInSamples, juce::jmax<juce::int64>(1, cap)));
    if (total <= 0)
        return {};

    juce::AudioBuffer<float> tmp(static_cast<int>(reader->numChannels), total);
    reader->read(&tmp, 0, total, 0, true, true);

    juce::AudioBuffer<float> mono(1, total);
    mono.clear();
    const auto invCh = 1.0f / static_cast<float>(juce::jmax(1, tmp.getNumChannels()));
    for (int ch = 0; ch < tmp.getNumChannels(); ++ch)
        mono.addFrom(0, 0, tmp, ch, 0, total, invCh);

    sampleRateOut = reader->sampleRate;
    return mono;
}

// 12-bin chroma (pitch-class energy) via Goertzel filters across ~5 octaves.
// Tuned against a real melody pack (see Source/Tools/KeyTest.cpp): per-frame
// normalization so loud sustained notes don't swamp the tonal profile.
std::array<double, 12> computeChroma(const juce::AudioBuffer<float>& mono, double sr)
{
    std::array<double, 12> chroma {};
    chroma.fill(0.0);
    const auto n = mono.getNumSamples();
    if (n < 4096 || sr <= 0.0)
        return chroma;

    const auto* x = mono.getReadPointer(0);
    constexpr int frame = 4096;
    constexpr int hop = 2048;
    constexpr int loMidi = 36, hiMidi = 95;   // C2..B6
    const auto twoPi = juce::MathConstants<double>::twoPi;

    std::array<double, hiMidi - loMidi + 1> coeffs {};
    for (int midi = loMidi; midi <= hiMidi; ++midi)
    {
        const auto freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
        coeffs[static_cast<std::size_t>(midi - loMidi)] = (freq > sr * 0.45) ? -100.0 : 2.0 * std::cos(twoPi * freq / sr);
    }

    for (int start = 0; start + frame <= n; start += hop)
    {
        std::array<double, 12> frameChroma {};
        frameChroma.fill(0.0);
        for (int midi = loMidi; midi <= hiMidi; ++midi)
        {
            const auto coeff = coeffs[static_cast<std::size_t>(midi - loMidi)];
            if (coeff < -10.0) continue;
            double s1 = 0.0, s2 = 0.0;
            for (int i = 0; i < frame; ++i)
            {
                const auto s0 = static_cast<double>(x[start + i]) + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            const auto power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            frameChroma[static_cast<std::size_t>(midi % 12)] += std::sqrt(juce::jmax(0.0, power));
        }
        double fsum = 0.0;
        for (auto v : frameChroma) fsum += v;
        if (fsum > 1.0e-9)
            for (int i = 0; i < 12; ++i) chroma[static_cast<std::size_t>(i)] += frameChroma[static_cast<std::size_t>(i)] / fsum;
    }

    double sum = 0.0;
    for (auto v : chroma) sum += v;
    if (sum > 0.0)
        for (auto& v : chroma) v /= sum;
    return chroma;
}

struct KeyEstimate { int root { -1 }; bool minor { false }; double confidence { 0.0 }; };

// Correlate the chroma against rotated major/minor profiles. Uses the corpus-derived
// Albrecht & Shanahan (2013) profiles, which on real melodic material gave markedly
// better mode (major/minor) accuracy than the classic Krumhansl-Schmuckler weights.
KeyEstimate estimateKeyFromChroma(const std::array<double, 12>& chroma)
{
    static const double major[12] = { 0.238, 0.006, 0.111, 0.006, 0.137, 0.094, 0.016, 0.214, 0.009, 0.080, 0.008, 0.081 };
    static const double minor[12] = { 0.220, 0.006, 0.104, 0.123, 0.019, 0.103, 0.012, 0.214, 0.062, 0.022, 0.061, 0.052 };

    double chromaMean = 0.0;
    for (auto v : chroma) chromaMean += v;
    chromaMean /= 12.0;

    const auto correlate = [&](const double* profile, int rotation)
    {
        double pMean = 0.0;
        for (int i = 0; i < 12; ++i) pMean += profile[i];
        pMean /= 12.0;

        double num = 0.0, dc = 0.0, dp = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const auto c = chroma[static_cast<std::size_t>((i + rotation) % 12)] - chromaMean;
            const auto p = profile[i] - pMean;
            num += c * p;
            dc += c * c;
            dp += p * p;
        }
        return (dc > 0.0 && dp > 0.0) ? num / std::sqrt(dc * dp) : 0.0;
    };

    KeyEstimate best;
    for (int root = 0; root < 12; ++root)
    {
        const auto cMaj = correlate(major, root);
        if (cMaj > best.confidence) { best = { root, false, cMaj }; }
        const auto cMin = correlate(minor, root);
        if (cMin > best.confidence) { best = { root, true, cMin }; }
    }
    return best;
}

struct TempoEstimate { double bpm { 0.0 }; double confidence { 0.0 }; };

// Onset-flux autocorrelation tempo estimate, octave-folded toward the project tempo.
TempoEstimate estimateTempoAutocorr(const juce::AudioBuffer<float>& mono, double sr, double projectTempoBpm)
{
    TempoEstimate result;
    const auto n = mono.getNumSamples();
    if (sr <= 0.0)
        return result;

    const auto hop = juce::jmax(1, static_cast<int>(std::round(sr * 0.01)));   // 10 ms frames
    const auto envSize = n / hop;
    if (envSize < 64)   // need a few seconds of material for a reliable autocorrelation
        return result;

    const auto* x = mono.getReadPointer(0);
    std::vector<double> flux(static_cast<std::size_t>(envSize), 0.0);
    double prevRms = 0.0;
    for (int i = 0; i < envSize; ++i)
    {
        const auto s = i * hop;
        const auto e = juce::jmin(s + hop, n);
        double energy = 0.0;
        for (int k = s; k < e; ++k) energy += static_cast<double>(x[k]) * x[k];
        const auto rms = std::sqrt(energy / juce::jmax(1, e - s));
        flux[static_cast<std::size_t>(i)] = juce::jmax(0.0, rms - prevRms);
        prevRms = rms;
    }

    double mean = 0.0;
    for (auto v : flux) mean += v;
    mean /= envSize;
    for (auto& v : flux) v -= mean;

    double zero = 0.0;
    for (auto v : flux) zero += v * v;
    if (zero <= 0.0)
        return result;

    const auto hopSec = static_cast<double>(hop) / sr;
    const auto minLag = juce::jmax(1, static_cast<int>(std::floor(60.0 / (200.0 * hopSec))));
    const auto maxLag = juce::jmin(envSize - 1, static_cast<int>(std::ceil(60.0 / (50.0 * hopSec))));

    double best = 0.0;
    int bestLag = 0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double ac = 0.0;
        for (int i = lag; i < envSize; ++i) ac += flux[static_cast<std::size_t>(i)] * flux[static_cast<std::size_t>(i - lag)];
        if (ac > best) { best = ac; bestLag = lag; }
    }
    if (bestLag <= 0)
        return result;

    auto bpm = 60.0 / (bestLag * hopSec);
    while (bpm < 70.0)  bpm *= 2.0;
    while (bpm > 180.0) bpm *= 0.5;
    // If doubling/halving lands closer to the session tempo, prefer that octave.
    if (projectTempoBpm > 0.0)
    {
        const auto closer = [&](double a, double b) { return std::abs(a - projectTempoBpm) < std::abs(b - projectTempoBpm); };
        if (bpm * 2.0 <= 200.0 && closer(bpm * 2.0, bpm)) bpm *= 2.0;
        else if (bpm * 0.5 >= 60.0 && closer(bpm * 0.5, bpm)) bpm *= 0.5;
    }

    result.bpm = bpm;
    result.confidence = juce::jlimit(0.0, 1.0, best / zero);
    return result;
}
}  // namespace

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

    // Mono audio is loaded lazily (only when filename metadata isn't enough) and shared
    // by the key + tempo analyzers so we read/decode the file at most once.
    juce::AudioBuffer<float> mono;
    double monoSr = 0.0;
    bool monoLoaded = false;
    const auto ensureMono = [&]()
    {
        if (! monoLoaded)
        {
            mono = loadMonoForAnalysis(file, monoSr);
            monoLoaded = true;
        }
        return mono.getNumSamples() > 0 && monoSr > 0.0;
    };

    // --- Key ---------------------------------------------------------------
    // Filename first (e.g. "Cm_120bpm", "F#maj_loop"); otherwise analyse the signal.
    const auto parsedKey = parseKeyFromFileName(file);
    result.sourceKeyRoot    = parsedKey.root;
    result.sourceKeyIsMinor = parsedKey.minor;
    if (parsedKey.root < 0 && ensureMono())
    {
        const auto est = estimateKeyFromChroma(computeChroma(mono, monoSr));
        // Threshold tuned on a real melody pack: ~95% of tonal melodies clear it while
        // drums/atonal material (flat chroma) score well below and stay "unknown".
        if (est.root >= 0 && est.confidence >= 0.6)
        {
            result.sourceKeyRoot    = est.root;
            result.sourceKeyIsMinor = est.minor;
            DBG("[Warp] " + file.getFileName() + " | key(audio)=" + formatKeyName(est.root, est.minor)
                + " | conf=" + juce::String(est.confidence, 2));
        }
    }

    // --- Tempo -------------------------------------------------------------
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

    const auto beatsPerBar = static_cast<double>(juce::jmax(1, numerator));

    // Pass 1: assume the file is a whole number of bars (exact for clean loops).
    constexpr double neutralCenter = 128.0;
    constexpr int commonLoopBars[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
    double bestBpm = 0.0;
    int bestBars = 0;
    double bestScore = std::numeric_limits<double>::max();
    for (const auto bars : commonLoopBars)
    {
        const auto candidateBpm = (bars * beatsPerBar / result.durationSeconds) * 60.0;
        if (candidateBpm < 70.0 || candidateBpm > 180.0)
            continue;
        const auto score = std::abs(candidateBpm - neutralCenter);
        if (score < bestScore) { bestScore = score; bestBpm = candidateBpm; bestBars = bars; }
    }

    if (bestBars > 0)
    {
        result.sourceBpm = bestBpm;
        result.detectedBars = bestBars;
        result.bpmSource = "duration-bars";
        result.bpmGuessed = true;
        const auto sourceBeats = result.durationSeconds * (bestBpm / 60.0);
        const auto barFraction = std::abs(sourceBeats - std::round(sourceBeats / beatsPerBar) * beatsPerBar);
        result.bpmConfidence = juce::jlimit(0.0, 1.0, 1.0 - barFraction / beatsPerBar);
        DBG("[Warp] " + file.getFileName() + " | bpmSource=duration-bars | confidence="
            + juce::String(result.bpmConfidence, 2) + " | sourceBpm=" + juce::String(bestBpm, 1)
            + " | bars=" + juce::String(bestBars));
        return result;
    }

    // Pass 2: real onset-autocorrelation tempo (works on non-bar-aligned material).
    if (ensureMono())
    {
        const auto tempo = estimateTempoAutocorr(mono, monoSr, projectTempoBpm);
        if (tempo.bpm >= 60.0 && tempo.bpm <= 200.0 && tempo.confidence >= 0.18)
        {
            result.sourceBpm = tempo.bpm;
            result.bpmSource = "audio-autocorr";
            result.bpmGuessed = true;
            result.bpmConfidence = tempo.confidence;
            const auto sourceBeats = result.durationSeconds * (tempo.bpm / 60.0);
            const auto bars = static_cast<int>(std::round(sourceBeats / beatsPerBar));
            if (bars > 0 && std::abs(sourceBeats - bars * beatsPerBar) <= 0.25)
                result.detectedBars = bars;
            DBG("[Warp] " + file.getFileName() + " | bpmSource=audio-autocorr | confidence="
                + juce::String(tempo.confidence, 2) + " | sourceBpm=" + juce::String(tempo.bpm, 1));
            return result;
        }
    }

    // Pass 3: short-sample fit. Allow sub-bar beat counts so short loops/chops still
    // get a tempo (and therefore warp), matching how Ableton fits short clips.
    constexpr int candidateBeats[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
    double shortBpm = 0.0;
    int shortBeats = 0;
    double shortScore = std::numeric_limits<double>::max();
    for (const auto beats : candidateBeats)
    {
        const auto candidateBpm = (beats / result.durationSeconds) * 60.0;
        if (candidateBpm < 60.0 || candidateBpm > 200.0)
            continue;
        const auto centre = projectTempoBpm > 0.0 ? projectTempoBpm : neutralCenter;
        const auto score = std::abs(candidateBpm - centre);
        if (score < shortScore) { shortScore = score; shortBpm = candidateBpm; shortBeats = beats; }
    }

    if (shortBeats > 0)
    {
        result.sourceBpm = shortBpm;
        result.bpmSource = "short-fit";
        result.bpmGuessed = true;
        result.bpmConfidence = 0.4;
        const auto bars = shortBeats / static_cast<int>(beatsPerBar);
        if (bars > 0 && shortBeats % static_cast<int>(beatsPerBar) == 0)
            result.detectedBars = bars;
        DBG("[Warp] " + file.getFileName() + " | bpmSource=short-fit | sourceBpm="
            + juce::String(shortBpm, 1) + " | beats=" + juce::String(shortBeats));
        return result;
    }

    DBG("[Warp] " + file.getFileName() + " | bpmSource=none | confidence=0 | sourceBpm=0");
    return result;
}

juce::AudioBuffer<float> stretchBufferToLengthWithExperimentalBackend(const juce::AudioBuffer<float>& source,
                                                                      int outputSamples,
                                                                      double sampleRate,
                                                                      const juce::String& sourcePath,
                                                                      double pitchScale)
{
    // Experimental: our own from-scratch engine. Off by default — when on, it
    // handles BOTH drum and melodic material so you can A/B it against the
    // existing backend without touching that code path.
    if (isOrionWarpEnabled())
    {
        auto orion = orionStretchWarp(source, outputSamples, sampleRate, pitchScale);
        if (orion.getNumSamples() > 0 && orion.getNumChannels() == source.getNumChannels())
            return orion;
        // Fall through to the proven path if something went wrong.
    }

    if (inferLoopType(sourcePath, source, sampleRate) == LoopType::drum)
    {
        // Drum path doesn't support pitch shift yet (tonal preservation for drums is iffy
        // anyway). Just stretch via the drum algorithm and ignore pitchScale for now.
        juce::ignoreUnused(pitchScale);
        return drumBeatsTailfillBuffer(source, outputSamples, sampleRate);
    }

    // Melodic: RubberBand with optional pitch shift for key matching.
#if ORION_HAVE_RUBBERBAND
    auto stretched = rubberBandStretchBufferToLength(source, outputSamples, sampleRate, {}, pitchScale);
    if (stretched.getNumSamples() > 0 && stretched.getNumChannels() == source.getNumChannels())
        return stretched;

    DBG("[Warp-RubberBand] invalid output; falling back to Signalsmith");
#endif
    return stretchBufferToLength(source, outputSamples, sampleRate);
}

juce::AudioBuffer<float> makeTempoFittedPreviewBuffer(const juce::AudioBuffer<float>& source,
                                                      double sourceBpm,
                                                      double projectTempoBpm,
                                                      double sampleRate,
                                                      const juce::String& sourcePath)
{
    if (source.getNumSamples() <= 1 || sourceBpm <= 0.0 || projectTempoBpm <= 0.0)
        return source;

    const auto tempoRatio = sourceBpm / projectTempoBpm;
    if (std::abs(tempoRatio - 1.0) < 0.001)
        return source;

    const auto outputSamples = juce::jmax(1, static_cast<int>(std::round(static_cast<double>(source.getNumSamples()) * tempoRatio)));
    return stretchBufferToLengthWithExperimentalBackend(source, outputSamples, sampleRate, sourcePath);
}
}  // namespace orion
