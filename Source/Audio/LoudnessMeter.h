#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace orion
{

/**
    Integrated loudness (LUFS) to ITU-R BS.1770-4, the standard behind EBU R128 and the
    normalisation Spotify/YouTube apply to uploads.

    Why this and not peak: peak says how close a signal is to clipping, loudness says how loud
    it sounds. A kick and a vocal can share a peak of -0.1 dBFS and still be nowhere near each
    other by ear, because the kick is a short transient and the vocal is dense. Matching clips
    by peak leaves them sounding uneven; matching them by LUFS does not.

    Feed audio with process() in any block size, then read getIntegratedLufs(). Returns -inf for
    silence or for material too short to fill one 400ms gating block.
*/
class LoudnessMeter
{
public:
    LoudnessMeter (double sampleRate, int numChannels);

    void process (const juce::AudioBuffer<float>& buffer, int startSample, int numSamples);

    /** Integrated loudness in LUFS, or -inf if there is nothing to measure. */
    double getIntegratedLufs() const;

    static constexpr double silence() { return -std::numeric_limits<double>::infinity(); }

private:
    // One biquad per channel, direct form I.
    struct Biquad
    {
        double b0 { 1.0 }, b1 { 0.0 }, b2 { 0.0 }, a1 { 0.0 }, a2 { 0.0 };
        double x1 { 0.0 }, x2 { 0.0 }, y1 { 0.0 }, y2 { 0.0 };

        double process (double x) noexcept
        {
            const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
    };

    void flushSubBlock();

    double sampleRate;
    int numChannels;

    std::vector<Biquad> shelf;      // stage 1: high-shelf, models the head
    std::vector<Biquad> highPass;   // stage 2: RLB high-pass

    // The gating blocks are 400ms long and overlap by 75%, so we accumulate mean-square in
    // 100ms sub-blocks and combine four of them per gating block.
    int subBlockLength { 0 };
    int subBlockFill { 0 };
    std::vector<double> subBlockSum;                  // per channel, sum of squares
    std::vector<std::vector<double>> subBlockHistory; // per sub-block, per channel mean-square
    std::vector<double> blockLoudness;                // one entry per 400ms gating block
    std::vector<std::vector<double>> blockMeanSquare; // ...and its per-channel mean-square
};

} // namespace orion
