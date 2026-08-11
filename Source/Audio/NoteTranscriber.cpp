#include "NoteTranscriber.h"

#include <cmath>
#include <algorithm>

namespace orion::transcribe
{
namespace
{
    constexpr double kYinThreshold = 0.15;   // absolute threshold on the cumulative-mean-normalised diff

    struct FrameResult
    {
        float midi { -1.0f };   // -1 = unvoiced
        float rms  { 0.0f };
    };

    // One YIN analysis at sample offset `pos` over a window sized to the lowest expected pitch.
    FrameResult analyseFrame (const float* x, int numSamples, int pos, int tauMin, int tauMax, double sampleRate)
    {
        FrameResult out;
        const int integ = tauMax;                    // integration length
        if (pos + tauMax + integ >= numSamples)
            return out;

        // RMS of the analysis window (for the voiced gate + velocity).
        double e = 0.0;
        for (int j = 0; j < integ; ++j) { const double s = x[pos + j]; e += s * s; }
        out.rms = static_cast<float> (std::sqrt (e / juce::jmax (1, integ)));

        // Difference function d(tau).
        std::vector<double> d (static_cast<size_t> (tauMax + 1), 0.0);
        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            double sum = 0.0;
            for (int j = 0; j < integ; ++j)
            {
                const double diff = static_cast<double> (x[pos + j]) - x[pos + j + tau];
                sum += diff * diff;
            }
            d[static_cast<size_t> (tau)] = sum;
        }

        // Cumulative mean normalised difference d'(tau).
        std::vector<double> dp (static_cast<size_t> (tauMax + 1), 1.0);
        double running = 0.0;
        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            running += d[static_cast<size_t> (tau)];
            dp[static_cast<size_t> (tau)] = running > 0.0
                ? d[static_cast<size_t> (tau)] * static_cast<double> (tau) / running : 1.0;
        }

        // Absolute threshold: first local min below the threshold, else the global min.
        int bestTau = -1;
        for (int tau = tauMin + 1; tau < tauMax; ++tau)
        {
            if (dp[static_cast<size_t> (tau)] < kYinThreshold
                && dp[static_cast<size_t> (tau)] <= dp[static_cast<size_t> (tau - 1)]
                && dp[static_cast<size_t> (tau)] <= dp[static_cast<size_t> (tau + 1)])
            { bestTau = tau; break; }
        }
        if (bestTau < 0)
        {
            double mn = 1.0e9;
            for (int tau = tauMin; tau <= tauMax; ++tau)
                if (dp[static_cast<size_t> (tau)] < mn) { mn = dp[static_cast<size_t> (tau)]; bestTau = tau; }
        }
        if (bestTau <= 0)
            return out;

        const double confidence = 1.0 - dp[static_cast<size_t> (bestTau)];
        if (confidence < 0.4)   // too aperiodic to be a pitch
            return out;

        // Parabolic interpolation on the raw difference for sub-sample tau.
        double tauR = bestTau;
        if (bestTau > tauMin && bestTau < tauMax)
        {
            const double a = d[static_cast<size_t> (bestTau - 1)];
            const double b = d[static_cast<size_t> (bestTau)];
            const double c = d[static_cast<size_t> (bestTau + 1)];
            const double denom = a - 2.0 * b + c;
            if (std::abs (denom) > 1.0e-12)
                tauR = bestTau + 0.5 * (a - c) / denom;
        }

        const double f0 = sampleRate / juce::jmax (1.0e-6, tauR);
        out.midi = static_cast<float> (69.0 + 12.0 * std::log2 (f0 / 440.0));
        return out;
    }
}

std::vector<Note> monophonic (const juce::AudioBuffer<float>& audio, double sampleRate, Options opt)
{
    std::vector<Note> notes;
    const int n = audio.getNumSamples();
    if (n < 2048 || sampleRate <= 0.0)
        return notes;

    // Downmix to mono.
    std::vector<float> mono (static_cast<size_t> (n), 0.0f);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const auto* src = audio.getReadPointer (ch);
        for (int i = 0; i < n; ++i) mono[static_cast<size_t> (i)] += src[i];
    }
    const float inv = 1.0f / juce::jmax (1, audio.getNumChannels());
    for (auto& s : mono) s *= inv;
    const float* x = mono.data();

    const double fMin = 440.0 * std::pow (2.0, (opt.minMidi - 69) / 12.0);
    const double fMax = 440.0 * std::pow (2.0, (opt.maxMidi - 69) / 12.0);
    const int tauMax = juce::jlimit (2, n / 2 - 1, static_cast<int> (sampleRate / fMin));
    const int tauMin = juce::jlimit (2, tauMax - 1, static_cast<int> (sampleRate / fMax));
    const int hop    = juce::jmax (1, static_cast<int> (sampleRate * 0.01));   // 10 ms frames
    const double silenceRms = std::pow (10.0f, opt.silenceDb / 20.0f);

    // Per-frame pitch/energy.
    struct FR { double t; float midi; float rms; };
    std::vector<FR> frames;
    frames.reserve (static_cast<size_t> (n / hop + 1));
    for (int pos = 0; pos + tauMax * 2 < n; pos += hop)
    {
        const auto fr = analyseFrame (x, n, pos, tauMin, tauMax, sampleRate);
        const bool voiced = fr.midi > 0.0f && fr.rms > silenceRms;
        frames.push_back ({ pos / sampleRate, voiced ? fr.midi : -1.0f, fr.rms });
    }
    if (frames.size() < 3)
        return notes;

    // Median-filter the pitch track (3 frames) to kill single-frame octave jumps.
    for (size_t i = 1; i + 1 < frames.size(); ++i)
    {
        if (frames[i - 1].midi < 0 || frames[i].midi < 0 || frames[i + 1].midi < 0) continue;
        std::array<float, 3> t { frames[i - 1].midi, frames[i].midi, frames[i + 1].midi };
        std::sort (t.begin(), t.end());
        frames[i].midi = t[1];
    }

    // Segment consecutive voiced frames of the same rounded pitch into notes.
    const double hopSec = hop / sampleRate;
    int   curPitch = -1;
    double curStart = 0.0, curPeakRms = 0.0;
    auto flush = [&] (double endSec)
    {
        if (curPitch < 0) return;
        if (endSec - curStart >= opt.minNoteSec)
        {
            const float vel = juce::jlimit (0.15f, 1.0f, static_cast<float> (curPeakRms) * 3.0f + 0.15f);
            notes.push_back ({ juce::jlimit (opt.minMidi, opt.maxMidi, curPitch), curStart, endSec - curStart, vel });
        }
        curPitch = -1; curPeakRms = 0.0;
    };

    for (size_t i = 0; i < frames.size(); ++i)
    {
        const int p = frames[i].midi > 0.0f ? static_cast<int> (std::lround (frames[i].midi)) : -1;
        if (p != curPitch)
        {
            flush (frames[i].t);
            curPitch = p;
            curStart = frames[i].t;
            curPeakRms = frames[i].rms;
        }
        else
        {
            curPeakRms = juce::jmax (curPeakRms, static_cast<double> (frames[i].rms));
        }
    }
    flush (frames.back().t + hopSec);
    return notes;
}
}
