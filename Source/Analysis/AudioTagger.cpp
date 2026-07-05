#include "AudioTagger.h"
#include "SoundClassifier.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include <cmath>

namespace orion
{
namespace
{
constexpr double kMaxAnalysisSeconds = 4.0;    // only the head of the file is analysed
constexpr int    kFftOrder = 11;               // 2048-point FFT
constexpr int    kFftSize = 1 << kFftOrder;

struct Features
{
    double durationSeconds = 0.0;
    double centroidHz = 0.0;       // spectral centroid (brightness)
    double flatness = 0.0;         // 0..1, high = noisy/broadband, low = tonal
    double bandLow = 0.0;          // < 200 Hz  (normalised energy fractions, sum ~1)
    double bandLowMid = 0.0;       // 200 .. 800
    double bandMid = 0.0;          // 800 .. 3k
    double bandHigh = 0.0;         // 3k .. 8k
    double bandAir = 0.0;          // > 8k
    double attackSeconds = 0.0;    // time from start to the signal's peak
    double sustainRatio = 0.0;     // late-half RMS / early-half RMS (percussive ~0, sustained ~1+)
    bool   valid = false;
};

// Decode the head of the file, mono-summed, and pull out spectral + temporal features.
Features extractFeatures(juce::AudioFormatManager& fm, const juce::File& file)
{
    Features f;
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return f;

    const double sr = reader->sampleRate;
    const auto totalLen = reader->lengthInSamples;
    f.durationSeconds = static_cast<double>(totalLen) / sr;

    const int wantSamples = static_cast<int>(juce::jmin<juce::int64>(
        totalLen, static_cast<juce::int64>(kMaxAnalysisSeconds * sr)));
    if (wantSamples < kFftSize / 2)
        return f;   // too short to analyse meaningfully

    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), wantSamples);
    reader->read(&buffer, 0, wantSamples, 0, true, true);

    // Mono sum.
    juce::AudioBuffer<float> mono(1, wantSamples);
    mono.clear();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        mono.addFrom(0, 0, buffer, ch, 0, wantSamples, 1.0f / static_cast<float>(buffer.getNumChannels()));
    const float* x = mono.getReadPointer(0);

    // --- Temporal: peak position (attack) + early/late energy (sustain vs decay) ---
    int peakIndex = 0;
    float peak = 0.0f;
    for (int i = 0; i < wantSamples; ++i)
    {
        const float a = std::abs(x[i]);
        if (a > peak) { peak = a; peakIndex = i; }
    }
    f.attackSeconds = static_cast<double>(peakIndex) / sr;

    const int half = wantSamples / 2;
    double eEarly = 0.0, eLate = 0.0;
    for (int i = 0; i < half; ++i)          eEarly += static_cast<double>(x[i]) * x[i];
    for (int i = half; i < wantSamples; ++i) eLate  += static_cast<double>(x[i]) * x[i];
    eEarly = std::sqrt(eEarly / juce::jmax(1, half));
    eLate  = std::sqrt(eLate  / juce::jmax(1, wantSamples - half));
    f.sustainRatio = eLate / juce::jmax(1.0e-9, eEarly);

    // --- Spectral: averaged magnitude over Hann-windowed frames ---
    juce::dsp::FFT fft(kFftOrder);
    std::vector<float> window(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[i] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / (kFftSize - 1));

    std::vector<float> fftBuf(2 * kFftSize, 0.0f);
    std::vector<double> mag(kFftSize / 2, 0.0);
    int frames = 0;
    const int hop = kFftSize / 2;
    for (int start = 0; start + kFftSize <= wantSamples; start += hop)
    {
        std::fill(fftBuf.begin(), fftBuf.end(), 0.0f);
        for (int i = 0; i < kFftSize; ++i)
            fftBuf[static_cast<size_t>(i)] = x[start + i] * window[static_cast<size_t>(i)];
        fft.performRealOnlyForwardTransform(fftBuf.data(), true);
        for (int k = 0; k < kFftSize / 2; ++k)
        {
            const float re = fftBuf[static_cast<size_t>(2 * k)];
            const float im = fftBuf[static_cast<size_t>(2 * k + 1)];
            mag[static_cast<size_t>(k)] += std::sqrt(static_cast<double>(re) * re + static_cast<double>(im) * im);
        }
        ++frames;
    }
    if (frames == 0)
        return f;
    for (auto& m : mag) m /= frames;

    const double binHz = sr / kFftSize;
    double sumMag = 0.0, sumFreqMag = 0.0, logSum = 0.0, arithSum = 0.0;
    double bLow = 0.0, bLowMid = 0.0, bMid = 0.0, bHigh = 0.0, bAir = 0.0;
    int nz = 0;
    for (int k = 1; k < kFftSize / 2; ++k)
    {
        const double m = mag[static_cast<size_t>(k)];
        const double hz = k * binHz;
        sumMag += m;
        sumFreqMag += m * hz;
        arithSum += m;
        logSum += std::log(m + 1.0e-12);
        ++nz;
        if      (hz < 200.0)  bLow    += m;
        else if (hz < 800.0)  bLowMid += m;
        else if (hz < 3000.0) bMid    += m;
        else if (hz < 8000.0) bHigh   += m;
        else                  bAir    += m;
    }
    const double total = juce::jmax(1.0e-12, sumMag);
    f.centroidHz = sumFreqMag / total;
    const double geoMean = std::exp(logSum / juce::jmax(1, nz));
    const double ariMean = arithSum / juce::jmax(1, nz);
    f.flatness = juce::jlimit(0.0, 1.0, geoMean / juce::jmax(1.0e-12, ariMean));
    f.bandLow    = bLow / total;
    f.bandLowMid = bLowMid / total;
    f.bandMid    = bMid / total;
    f.bandHigh   = bHigh / total;
    f.bandAir    = bAir / total;
    f.valid = peak > 1.0e-4f;
    return f;
}

// Turn features into human tags. Conservative: only emit a tag when reasonably confident.
juce::StringArray classify(const Features& f)
{
    juce::StringArray tags;
    if (! f.valid)
        return tags;
    const auto add = [&tags](const juce::String& t) { if (! tags.contains(t)) tags.add(t); };

    const bool shortHit   = f.durationSeconds < 1.6;
    const bool fastAttack = f.attackSeconds < 0.06;
    const bool decays     = f.sustainRatio < 0.45;            // energy drops off after the hit
    const bool noisy      = f.flatness > 0.22;
    const bool tonal      = f.flatness < 0.12;
    const bool percussive = shortHit && fastAttack && decays;

    if (percussive)
    {
        add("Drums");
        if (f.bandLow > 0.45 && f.centroidHz < 300.0)                 add("Kick");
        else if (f.bandHigh + f.bandAir > 0.5 && f.centroidHz > 4000) add("Hat");
        else if (noisy && f.bandMid + f.bandLowMid > 0.4)            add("Snare");
        else                                                         add("Perc");
    }
    else
    {
        // Sustained / tonal material.
        if (f.centroidHz < 250.0 && (tonal || f.bandLow + f.bandLowMid > 0.6))
            add("Bass");
        else if (tonal)
            add("Melodic");

        if (noisy && f.sustainRatio > 0.6)
            add("FX");
    }

    // Brightness (any material).
    if (f.centroidHz > 3500.0)      add("Bright");
    else if (f.centroidHz < 600.0)  add("Dark");

    // Loop vs one-shot by sound: long + steady tail = loop, short = one-shot.
    if (f.durationSeconds >= 2.5 && f.sustainRatio > 0.5) add("Loop");
    else if (f.durationSeconds < 1.6)                     add("One-shot");

    return tags;
}
}  // namespace

AudioTagger::AudioTagger()
{
    formatManager.registerBasicFormats();
}

AudioTagger::~AudioTagger()
{
    // Finish/stop background jobs while cache + mutex are still alive.
    pool.removeAllJobs(true, 4000);
}

std::optional<juce::StringArray> AudioTagger::cachedTags(const juce::File& file) const
{
    const std::lock_guard<std::mutex> lock(cacheMutex);
    const auto it = cache.find(file.getFullPathName().toStdString());
    if (it == cache.end())
        return std::nullopt;
    return it->second;
}

void AudioTagger::requestTags(const juce::File& file, std::function<void(juce::StringArray)> onReady)
{
    const auto key = file.getFullPathName().toStdString();
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        if (const auto it = cache.find(key); it != cache.end())
        {
            auto tags = it->second;
            juce::MessageManager::callAsync([onReady = std::move(onReady), tags = std::move(tags)]() mutable
                                            { if (onReady) onReady(tags); });
            return;
        }
        if (! pending.insert(key).second)
            return;   // already being analysed; the first caller's callback will fire
    }

    pool.addJob([this, file, key, onReady = std::move(onReady)]() mutable
    {
        auto tags = analyseFile(file);
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            cache[key] = tags;
            pending.erase(key);
        }
        juce::MessageManager::callAsync([onReady = std::move(onReady), tags = std::move(tags)]() mutable
                                        { if (onReady) onReady(tags); });
    });
}

juce::StringArray AudioTagger::analyseFile(const juce::File& file)
{
    // Instruments recognised BY SOUND via Apple's SoundAnalysis (violin, piano, guitar…).
    juce::StringArray tags = classifyWithSoundAnalysis(file);

    // Add the DSP broad tags (Loop/One-shot, Bright/Dark, Bass, Drums…) that the ML doesn't cover.
    for (const auto& t : classify(extractFeatures(formatManager, file)))
        if (! tags.contains(t))
            tags.add(t);

    return tags;
}
}  // namespace orion
