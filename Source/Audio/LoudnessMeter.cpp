#include "LoudnessMeter.h"

#include <cmath>
#include <limits>
#include <numeric>

namespace orion
{

namespace
{
constexpr double kOffset = -0.691;          // BS.1770 loudness constant
constexpr double absoluteGateLufs = -70.0;  // silence gate
constexpr double relativeGateLu = -10.0;    // gate relative to the ungated mean
}

// BS.1770 lists the K-weighting coefficients at 48kHz only. Rather than resample, design both
// filters analytically for whatever rate the file happens to be at.

// Stage 1: a +4dB high shelf around 1681Hz, standing in for the acoustic effect of the head.
static void designShelf (double sampleRate, double& b0, double& b1, double& b2, double& a1, double& a2)
{
    constexpr double f0 = 1681.974450955533;
    constexpr double G  = 3.999843853973347;
    constexpr double Q  = 0.7071752369554196;

    const auto K  = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
    const auto Vh = std::pow (10.0, G / 20.0);
    const auto Vb = std::pow (Vh, 0.4996667741545416);
    const auto a0 = 1.0 + K / Q + K * K;

    b0 = (Vh + Vb * K / Q + K * K) / a0;
    b1 = 2.0 * (K * K - Vh) / a0;
    b2 = (Vh - Vb * K / Q + K * K) / a0;
    a1 = 2.0 * (K * K - 1.0) / a0;
    a2 = (1.0 - K / Q + K * K) / a0;
}

// Stage 2: the RLB high-pass, which discards the low end we barely hear as loudness.
static void designHighPass (double sampleRate, double& b0, double& b1, double& b2, double& a1, double& a2)
{
    constexpr double f0 = 38.13547087602444;
    constexpr double Q  = 0.5003270373238773;

    const auto K = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
    const auto denom = 1.0 + K / Q + K * K;

    b0 = 1.0;
    b1 = -2.0;
    b2 = 1.0;
    a1 = 2.0 * (K * K - 1.0) / denom;
    a2 = (1.0 - K / Q + K * K) / denom;
}

LoudnessMeter::LoudnessMeter (double sr, int channels)
    : sampleRate (sr > 0.0 ? sr : 48000.0),
      numChannels (juce::jmax (1, channels))
{
    shelf.resize (static_cast<std::size_t> (numChannels));
    highPass.resize (static_cast<std::size_t> (numChannels));

    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    designShelf (sampleRate, b0, b1, b2, a1, a2);
    for (auto& f : shelf) { f.b0 = b0; f.b1 = b1; f.b2 = b2; f.a1 = a1; f.a2 = a2; }

    designHighPass (sampleRate, b0, b1, b2, a1, a2);
    for (auto& f : highPass) { f.b0 = b0; f.b1 = b1; f.b2 = b2; f.a1 = a1; f.a2 = a2; }

    subBlockLength = juce::jmax (1, static_cast<int> (std::llround (sampleRate * 0.1)));   // 100ms
    subBlockSum.assign (static_cast<std::size_t> (numChannels), 0.0);
}

void LoudnessMeter::flushSubBlock()
{
    std::vector<double> meanSquare (static_cast<std::size_t> (numChannels), 0.0);
    for (int ch = 0; ch < numChannels; ++ch)
        meanSquare[static_cast<std::size_t> (ch)] = subBlockSum[static_cast<std::size_t> (ch)] / static_cast<double> (subBlockLength);

    subBlockHistory.push_back (meanSquare);
    std::fill (subBlockSum.begin(), subBlockSum.end(), 0.0);
    subBlockFill = 0;

    // Four consecutive 100ms sub-blocks make one 400ms gating block, stepping by 100ms.
    if (subBlockHistory.size() < 4)
        return;

    const auto first = subBlockHistory.size() - 4;
    std::vector<double> blockMs (static_cast<std::size_t> (numChannels), 0.0);

    for (std::size_t i = first; i < subBlockHistory.size(); ++i)
        for (int ch = 0; ch < numChannels; ++ch)
            blockMs[static_cast<std::size_t> (ch)] += subBlockHistory[i][static_cast<std::size_t> (ch)] * 0.25;

    // Channel weights: L and R (and mono) are 1.0. We do not handle surround.
    const auto weighted = std::accumulate (blockMs.begin(), blockMs.end(), 0.0);
    if (weighted <= 0.0)
        return;

    blockLoudness.push_back (kOffset + 10.0 * std::log10 (weighted));
    blockMeanSquare.push_back (blockMs);
}

void LoudnessMeter::process (const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    const auto channelsToRead = juce::jmin (numChannels, buffer.getNumChannels());

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < channelsToRead; ++ch)
        {
            const auto x = static_cast<double> (buffer.getSample (ch, startSample + i));
            const auto y = highPass[static_cast<std::size_t> (ch)].process (shelf[static_cast<std::size_t> (ch)].process (x));
            subBlockSum[static_cast<std::size_t> (ch)] += y * y;
        }

        if (++subBlockFill >= subBlockLength)
            flushSubBlock();
    }
}

double LoudnessMeter::getIntegratedLufs() const
{
    if (blockLoudness.empty())
        return silence();

    // Absolute gate at -70 LUFS drops digital silence.
    std::vector<std::size_t> aboveAbsolute;
    for (std::size_t i = 0; i < blockLoudness.size(); ++i)
        if (blockLoudness[i] > absoluteGateLufs)
            aboveAbsolute.push_back (i);

    if (aboveAbsolute.empty())
        return silence();

    const auto meanOf = [this] (const std::vector<std::size_t>& indices)
    {
        std::vector<double> sum (static_cast<std::size_t> (numChannels), 0.0);
        for (auto i : indices)
            for (int ch = 0; ch < numChannels; ++ch)
                sum[static_cast<std::size_t> (ch)] += blockMeanSquare[i][static_cast<std::size_t> (ch)];

        for (auto& s : sum)
            s /= static_cast<double> (indices.size());

        return std::accumulate (sum.begin(), sum.end(), 0.0);
    };

    const auto ungated = meanOf (aboveAbsolute);
    if (ungated <= 0.0)
        return silence();

    // Relative gate: 10 LU below the ungated mean. This is what stops a quiet passage from
    // dragging the measurement down — loudness is judged on the parts that are actually playing.
    const auto relativeGate = kOffset + 10.0 * std::log10 (ungated) + relativeGateLu;

    std::vector<std::size_t> gated;
    for (auto i : aboveAbsolute)
        if (blockLoudness[i] > relativeGate)
            gated.push_back (i);

    if (gated.empty())
        return silence();

    const auto finalMean = meanOf (gated);
    if (finalMean <= 0.0)
        return silence();

    return kOffset + 10.0 * std::log10 (finalMean);
}

} // namespace orion
