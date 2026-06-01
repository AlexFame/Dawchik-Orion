#include "OrionStretchEngine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace orion
{
namespace
{
constexpr double pi = 3.14159265358979323846;

std::atomic<bool> g_orionWarpEnabled { false };

// WSOLA tuning. Larger grain + correlation window so low frequencies (long
// periods) splice without a phase jump — kills the low-end warble/buzz on
// tonal material. (Costs a little transient sharpness; fine for melodic.)
constexpr int kFrame = 4096;        // grain length
constexpr int kHop   = kFrame / 2;  // synthesis hop (50% overlap)
constexpr int kTol   = 1024;        // ± splice search window
constexpr int kCorr  = 2048;        // samples compared when scoring a splice

constexpr int kPvFrame = 2048;      // phase-vocoder FFT size (melodic path)

std::vector<float> makeHann(int n)
{
    std::vector<float> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        w[static_cast<std::size_t>(i)] = 0.5f * (1.0f - std::cos(2.0 * pi * i / (n - 1)));
    return w;
}

float cubicHermiteAt(const float* x, long n, double pos)
{
    if (n == 0) return 0.0f;
    const long i = static_cast<long>(std::floor(pos));
    const float t = static_cast<float>(pos - i);
    auto at = [&](long idx) -> float
    {
        idx = std::clamp(idx, 0L, n - 1);
        return x[idx];
    };
    const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

[[maybe_unused]] juce::AudioBuffer<float> wsola(const juce::AudioBuffer<float>& in, double alpha)
{
    const int channels = in.getNumChannels();
    const long inLen = in.getNumSamples();
    const int N = kFrame;
    const int Hs = kHop;
    const int Ha = std::max(1, static_cast<int>(std::lround(Hs / alpha)));
    const int tol = kTol;
    const int corr = std::min(kCorr, N);

    const auto hann = makeHann(N);

    // Mono mix used only for splice scoring (keeps channels phase-aligned).
    std::vector<float> mono(static_cast<std::size_t>(inLen), 0.0f);
    {
        const float scale = 1.0f / std::max(1, channels);
        for (int ch = 0; ch < channels; ++ch)
        {
            const float* p = in.getReadPointer(ch);
            for (long i = 0; i < inLen; ++i)
                mono[static_cast<std::size_t>(i)] += p[i] * scale;
        }
    }

    const long outLen = static_cast<long>(std::ceil(inLen * alpha)) + 2 * N;
    juce::AudioBuffer<float> out(channels, static_cast<int>(outLen));
    out.clear();
    std::vector<float> norm(static_cast<std::size_t>(outLen), 0.0f);

    if (inLen < N)
        return in;

    auto score = [&](long cand, const std::vector<float>& target) -> float
    {
        double dot = 0.0, energy = 0.0;
        for (int i = 0; i < corr; ++i)
        {
            const long si = cand + i;
            const float a = (si >= 0 && si < inLen) ? mono[static_cast<std::size_t>(si)] : 0.0f;
            dot += static_cast<double>(a) * target[static_cast<std::size_t>(i)];
            energy += static_cast<double>(a) * a;
        }
        return static_cast<float>(dot / std::sqrt(energy + 1e-9));
    };

    std::vector<float> target(static_cast<std::size_t>(corr), 0.0f);
    long analysisPos = 0, outPos = 0;
    bool first = true;

    while (outPos + N < outLen && analysisPos + N < inLen)
    {
        long src = analysisPos;
        if (! first && tol > 0)
        {
            float best = -1e30f;
            long bestSrc = analysisPos;
            for (int d = -tol; d <= tol; ++d)
            {
                const long cand = analysisPos + d;
                if (cand < 0 || cand + N >= inLen) continue;
                const float s = score(cand, target);
                if (s > best) { best = s; bestSrc = cand; }
            }
            src = bestSrc;
        }
        first = false;

        for (int ch = 0; ch < channels; ++ch)
        {
            const float* inCh = in.getReadPointer(ch);
            float* outCh = out.getWritePointer(ch);
            for (int i = 0; i < N; ++i)
            {
                const long si = src + i;
                if (si < 0 || si >= inLen) continue;
                outCh[outPos + i] += inCh[si] * hann[static_cast<std::size_t>(i)];
            }
        }
        for (int i = 0; i < N; ++i)
            norm[static_cast<std::size_t>(outPos + i)] += hann[static_cast<std::size_t>(i)];

        for (int i = 0; i < corr; ++i)
        {
            const long si = src + Hs + i;
            target[static_cast<std::size_t>(i)] = (si >= 0 && si < inLen) ? mono[static_cast<std::size_t>(si)] : 0.0f;
        }

        analysisPos += Ha;
        outPos += Hs;
    }

    const long finalLen = std::min(outLen, static_cast<long>(std::llround(inLen * alpha)));
    juce::AudioBuffer<float> trimmed(channels, static_cast<int>(std::max(1L, finalLen)));
    trimmed.clear();
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* outCh = out.getReadPointer(ch);
        float* dst = trimmed.getWritePointer(ch);
        for (long i = 0; i < finalLen; ++i)
        {
            const float n = norm[static_cast<std::size_t>(i)];
            dst[i] = (n > 1e-6f) ? outCh[i] / n : outCh[i];
        }
    }
    return trimmed;
}

juce::AudioBuffer<float> resampleBuffer(const juce::AudioBuffer<float>& in, double ratio)
{
    const int channels = in.getNumChannels();
    const long inLen = in.getNumSamples();
    const long outLen = std::max(1L, static_cast<long>(std::llround(inLen / ratio)));
    juce::AudioBuffer<float> out(channels, static_cast<int>(outLen));
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* inCh = in.getReadPointer(ch);
        float* outCh = out.getWritePointer(ch);
        for (long i = 0; i < outLen; ++i)
            outCh[i] = cubicHermiteAt(inCh, inLen, i * ratio);
    }
    return out;
}

// In-place iterative radix-2 FFT (double precision for clean phases).
void fftD(std::vector<double>& re, std::vector<double>& im, bool inverse)
{
    const int n = static_cast<int>(re.size());
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * pi / len * (inverse ? 1.0 : -1.0);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len)
        {
            double cwr = 1.0, cwi = 0.0;
            for (int k = 0; k < len / 2; ++k)
            {
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2] * cwr - im[i + k + len / 2] * cwi;
                const double vi = re[i + k + len / 2] * cwi + im[i + k + len / 2] * cwr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                const double nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < n; ++i) { re[i] /= n; im[i] /= n; }
}

// Phase-vocoder time-stretch by `alpha`. Interpolates each frequency bin's phase
// instead of repeating waveform chunks, so tonal material stretches cleanly with
// no "doubling" / lo-fi artefacts (the WSOLA weakness at large ratios).
juce::AudioBuffer<float> phaseVocoder(const juce::AudioBuffer<float>& in, double alpha)
{
    const int channels = in.getNumChannels();
    const long inLen = in.getNumSamples();
    const int N = kPvFrame;
    const int Ha = N / 4;                 // 75% overlap analysis hop
    const int Hs = std::max(1, static_cast<int>(std::lround(Ha * alpha)));
    const int half = N / 2;
    const double expBase = 2.0 * pi * Ha / static_cast<double>(N);

    if (inLen < N) return in;

    const auto win = makeHann(N);

    // Pad the input with N samples of silence front and back. The overlap-add
    // ramps up/down over that silence, so the real audio never sits in a partial-
    // overlap region — no edge click, and (crucially) no need for an aggressive
    // normalisation clamp that was causing the volume pumping on big stretches.
    const long pad = N;
    const long paddedLen = inLen + 2 * pad;
    juce::AudioBuffer<float> padded(channels, static_cast<int>(paddedLen));
    padded.clear();
    for (int ch = 0; ch < channels; ++ch)
        padded.copyFrom(ch, static_cast<int>(pad), in, ch, 0, static_cast<int>(inLen));

    const long outLen = static_cast<long>(std::ceil(paddedLen * alpha)) + N;
    juce::AudioBuffer<float> out(channels, static_cast<int>(outLen));
    out.clear();
    std::vector<float> norm(static_cast<std::size_t>(outLen), 0.0f);

    std::vector<double> re(N), im(N);
    std::vector<double> prevPhase(half + 1), sumPhase(half + 1);

    for (int ch = 0; ch < channels; ++ch)
    {
        std::fill(prevPhase.begin(), prevPhase.end(), 0.0);
        std::fill(sumPhase.begin(), sumPhase.end(), 0.0);
        const float* inCh = padded.getReadPointer(ch);
        float* outCh = out.getWritePointer(ch);
        long outPos = 0;
        bool firstFrame = true;

        for (long a = 0; a + N <= paddedLen; a += Ha)
        {
            for (int i = 0; i < N; ++i) { re[static_cast<std::size_t>(i)] = inCh[a + i] * win[static_cast<std::size_t>(i)]; im[static_cast<std::size_t>(i)] = 0.0; }
            fftD(re, im, false);

            for (int k = 0; k <= half; ++k)
            {
                const double m = std::hypot(re[static_cast<std::size_t>(k)], im[static_cast<std::size_t>(k)]);
                const double phase = std::atan2(im[static_cast<std::size_t>(k)], re[static_cast<std::size_t>(k)]);
                if (firstFrame)
                {
                    // Reproduce the very first frame exactly — start from its own phase.
                    sumPhase[static_cast<std::size_t>(k)] = phase;
                }
                else
                {
                    double dev = phase - prevPhase[static_cast<std::size_t>(k)] - expBase * k;
                    dev -= 2.0 * pi * std::round(dev / (2.0 * pi)); // wrap to [-pi, pi]
                    const double trueFreq = expBase * k + dev;
                    sumPhase[static_cast<std::size_t>(k)] += trueFreq * (static_cast<double>(Hs) / Ha);
                }
                prevPhase[static_cast<std::size_t>(k)] = phase;
                re[static_cast<std::size_t>(k)] = m * std::cos(sumPhase[static_cast<std::size_t>(k)]);
                im[static_cast<std::size_t>(k)] = m * std::sin(sumPhase[static_cast<std::size_t>(k)]);
            }
            firstFrame = false;
            for (int k = half + 1; k < N; ++k) // restore conjugate symmetry
            {
                re[static_cast<std::size_t>(k)] = re[static_cast<std::size_t>(N - k)];
                im[static_cast<std::size_t>(k)] = -im[static_cast<std::size_t>(N - k)];
            }

            fftD(re, im, true);

            for (int i = 0; i < N; ++i)
            {
                const long oi = outPos + i;
                if (oi >= outLen) break;
                outCh[oi] += static_cast<float>(re[static_cast<std::size_t>(i)]) * win[static_cast<std::size_t>(i)];
                if (ch == 0) norm[static_cast<std::size_t>(oi)] += win[static_cast<std::size_t>(i)] * win[static_cast<std::size_t>(i)];
            }
            outPos += Hs;
        }
    }

    // The real audio sits after the stretched leading pad. Honest per-sample
    // normalisation (tiny floor only, just to avoid divide-by-zero) — no clamp,
    // so no amplitude pumping.
    const long leadOffset = static_cast<long>(std::llround(pad * alpha));
    const long finalLen = static_cast<long>(std::llround(inLen * alpha));

    juce::AudioBuffer<float> trimmed(channels, static_cast<int>(std::max(1L, finalLen)));
    trimmed.clear();
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* o = out.getReadPointer(ch);
        float* d = trimmed.getWritePointer(ch);
        for (long i = 0; i < finalLen; ++i)
        {
            const long oi = leadOffset + i;
            if (oi < 0 || oi >= outLen) continue;
            const float n = norm[static_cast<std::size_t>(oi)];
            d[i] = (n > 1e-4f) ? o[oi] / n : 0.0f;
        }
    }
    return trimmed;
}

// Force the buffer to exactly `target` samples (pad with silence or trim).
juce::AudioBuffer<float> fitLength(const juce::AudioBuffer<float>& in, int target)
{
    if (in.getNumSamples() == target)
        return in;
    juce::AudioBuffer<float> out(in.getNumChannels(), std::max(1, target));
    out.clear();
    const int copy = std::min(in.getNumSamples(), target);
    for (int ch = 0; ch < in.getNumChannels(); ++ch)
        out.copyFrom(ch, 0, in, ch, 0, copy);
    return out;
}
}  // namespace

void setOrionWarpEnabled(bool enabled) noexcept { g_orionWarpEnabled.store(enabled, std::memory_order_relaxed); }
bool isOrionWarpEnabled() noexcept { return g_orionWarpEnabled.load(std::memory_order_relaxed); }

juce::AudioBuffer<float> orionStretchWarp(const juce::AudioBuffer<float>& source,
                                          int outputSamples,
                                          double sampleRate,
                                          double pitchScale)
{
    juce::ignoreUnused(sampleRate);
    const int channels = source.getNumChannels();
    const long inLen = source.getNumSamples();
    if (channels == 0 || inLen <= 0 || outputSamples <= 0)
        return source;

    const double pitchRatio = pitchScale > 0.0 ? pitchScale : 1.0;
    const double timeStretch = static_cast<double>(outputSamples) / static_cast<double>(inLen);
    const double alpha = std::max(0.01, timeStretch * pitchRatio);

    // Melodic-first: the phase vocoder gives clean tonal stretching (no doubling).
    // WSOLA is kept for the upcoming transient-aware drum path.
    juce::AudioBuffer<float> stretched = phaseVocoder(source, alpha);

    juce::AudioBuffer<float> result = (std::abs(pitchRatio - 1.0) > 1e-6)
                                          ? resampleBuffer(stretched, pitchRatio)
                                          : stretched;

    return fitLength(result, outputSamples);
}
}  // namespace orion
