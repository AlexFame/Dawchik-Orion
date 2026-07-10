// Checks orion::LoudnessMeter against the EBU TECH 3341 compliance cases. The tolerance the
// spec allows for an integrated-loudness meter is +/-0.1 LU.
//
//   ./OrionLoudnessTest

#include "../Audio/LoudnessMeter.h"

#include <cmath>
#include <cstdio>

namespace
{

int failures = 0;

void check (double measured, double expected, double toleranceLu, const char* what)
{
    const auto delta = std::abs (measured - expected);
    const auto ok = delta <= toleranceLu;

    std::printf ("  [%s] %-46s measured %7.2f LUFS, expected %7.2f (delta %.3f)\n",
                 ok ? "PASS" : "FAIL", what, measured, expected, delta);

    if (! ok)
        ++failures;
}

// A 1kHz sine at the given dBFS amplitude. BS.1770's constants are calibrated so that a 1kHz
// sine at -23 dBFS on one channel of a stereo pair... is not -23 LUFS: the spec's own test uses
// the SAME signal in both channels, which sums to +3 LU over a single channel.
juce::AudioBuffer<float> makeSine (double sampleRate, double seconds, double amplitudeDb, int channels)
{
    const auto numSamples = static_cast<int> (sampleRate * seconds);
    juce::AudioBuffer<float> buffer (channels, numSamples);
    const auto amplitude = std::pow (10.0, amplitudeDb / 20.0);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto v = static_cast<float> (amplitude * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * static_cast<double> (i) / sampleRate));
        for (int ch = 0; ch < channels; ++ch)
            buffer.setSample (ch, i, v);
    }

    return buffer;
}

double measure (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    orion::LoudnessMeter meter (sampleRate, buffer.getNumChannels());
    meter.process (buffer, 0, buffer.getNumSamples());
    return meter.getIntegratedLufs();
}

// TECH 3341 case 1/2: a stereo 1kHz sine, both channels identical, 20s.
void testStereoSine (double sampleRate, double amplitudeDb, double expected)
{
    const auto buffer = makeSine (sampleRate, 20.0, amplitudeDb, 2);
    char label[96];
    std::snprintf (label, sizeof (label), "stereo 1kHz sine @ %.0f dBFS, %.0f kHz", amplitudeDb, sampleRate / 1000.0);
    check (measure (buffer, sampleRate), expected, 0.1, label);
}

void testSilence()
{
    juce::AudioBuffer<float> buffer (2, 96000);
    buffer.clear();

    const auto lufs = measure (buffer, 48000.0);
    const auto ok = ! std::isfinite (lufs);
    std::printf ("  [%s] digital silence gates out (got %s)\n", ok ? "PASS" : "FAIL",
                 ok ? "-inf" : std::to_string (lufs).c_str());
    if (! ok)
        ++failures;
}

void testTooShort()
{
    // Under one 400ms gating block there is nothing to report.
    const auto buffer = makeSine (48000.0, 0.2, -23.0, 2);
    const auto lufs = measure (buffer, 48000.0);
    const auto ok = ! std::isfinite (lufs);
    std::printf ("  [%s] a clip shorter than one gating block reports -inf\n", ok ? "PASS" : "FAIL");
    if (! ok)
        ++failures;
}

void testRelativeGate()
{
    // 10s of -23 dBFS tone followed by 10s at -60 dBFS. Without the relative gate the two halves
    // average to about -26 LUFS. With it, the gate sits at -36 LUFS, the quiet blocks drop out,
    // and the answer is the loud half alone: -23. (A couple of blocks straddle the seam, hence
    // the slightly wider tolerance.)
    const auto loud = makeSine (48000.0, 10.0, -23.0, 2);
    const auto quiet = makeSine (48000.0, 10.0, -60.0, 2);

    orion::LoudnessMeter meter (48000.0, 2);
    meter.process (loud, 0, loud.getNumSamples());
    meter.process (quiet, 0, quiet.getNumSamples());

    check (meter.getIntegratedLufs(), -23.0, 0.15, "relative gate ignores a quiet second half");
}

void testGainOffsetIsLinearInDb()
{
    // The whole feature rests on this: dropping a clip by N dB must drop its LUFS by N.
    const auto a = measure (makeSine (48000.0, 5.0, -20.0, 2), 48000.0);
    const auto b = measure (makeSine (48000.0, 5.0, -26.0, 2), 48000.0);

    check (a - b, 6.0, 0.05, "a 6 dB amplitude change moves LUFS by 6 LU");
}

} // namespace

int main()
{
    std::printf ("LoudnessMeter — EBU TECH 3341 compliance\n\n");

    // TECH 3341 case 1: -23 dBFS stereo sine reads -23.0 LUFS.
    // The channel weights (1.0 + 1.0) exactly offset the -3 dB each channel would contribute.
    testStereoSine (48000.0, -23.0, -23.0);
    testStereoSine (48000.0, -33.0, -33.0);   // case 2
    testStereoSine (44100.0, -23.0, -23.0);   // filters must be designed for the actual rate
    testStereoSine (96000.0, -23.0, -23.0);

    std::printf ("\n");
    testGainOffsetIsLinearInDb();
    testRelativeGate();

    std::printf ("\n");
    testSilence();
    testTooShort();

    std::printf ("\n%s (%d failure%s)\n",
                 failures == 0 ? "ALL PASS" : "FAILURES",
                 failures, failures == 1 ? "" : "s");

    return failures == 0 ? 0 : 1;
}
