#include "SampleEmbedding.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace orion
{
namespace
{
constexpr int    kIndexVersion = 3;    // bump when the feature layout changes → old disk index is dropped
constexpr double kMaxSeconds = 3.0;    // analyse the head only (representative + fast)
constexpr int    kFftOrder   = 10;     // 1024-point FFT
constexpr int    kFftSize    = 1 << kFftOrder;
constexpr int    kHop        = kFftSize / 2;
constexpr int    kMelBands   = 24;
constexpr int    kMaxIndexedFilesPerRun = 12000;

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

// Triangular mel filterbank edges (in FFT-bin indices) for a given sample rate, built once per SR.
struct MelBank
{
    double sr = 0.0;
    std::array<std::array<float, kFftSize / 2 + 1>, kMelBands> weights {};

    void build(double sampleRate)
    {
        sr = sampleRate;
        const double loHz = 40.0;
        const double hiHz = juce::jmin(sampleRate * 0.5, 12000.0);
        const double loMel = hzToMel(loHz), hiMel = hzToMel(hiHz);
        std::array<double, kMelBands + 2> centres {};
        for (int i = 0; i < kMelBands + 2; ++i)
            centres[(std::size_t) i] = melToHz(loMel + (hiMel - loMel) * i / (kMelBands + 1));

        const int nBins = kFftSize / 2 + 1;
        for (int b = 0; b < kMelBands; ++b)
        {
            const double f0 = centres[(std::size_t) b], f1 = centres[(std::size_t) (b + 1)], f2 = centres[(std::size_t) (b + 2)];
            for (int k = 0; k < nBins; ++k)
            {
                const double f = (double) k * sampleRate / kFftSize;
                double w = 0.0;
                if (f >= f0 && f <= f1) w = (f - f0) / juce::jmax(1.0e-9, f1 - f0);
                else if (f > f1 && f <= f2) w = (f2 - f) / juce::jmax(1.0e-9, f2 - f1);
                weights[(std::size_t) b][(std::size_t) k] = (float) juce::jmax(0.0, w);
            }
        }
    }
};

}  // namespace

SampleEmbedding::SampleEmbedding()  { formatManager.registerBasicFormats(); loadIndex(); }
SampleEmbedding::~SampleEmbedding()
{
    indexerStop = true;
    if (indexerThread.joinable()) indexerThread.join();
    saveIndex();
}

juce::File SampleEmbedding::indexFile() const
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
             .getChildFile("Orion").getChildFile("sample_index.dat");
}

void SampleEmbedding::loadIndex()
{
    const auto f = indexFile();
    if (! f.existsAsFile())
        return;
    juce::FileInputStream in(f);
    if (! in.openedOk() || in.readInt() != kIndexVersion)
        return;   // missing or stale format → re-index from scratch
    const int count = in.readInt();
    std::lock_guard<std::mutex> lk(cacheMutex);
    for (int i = 0; i < count && ! in.isExhausted(); ++i)
    {
        const int plen = in.readInt();
        if (plen <= 0 || plen > 8192) break;
        juce::MemoryBlock pb;
        in.readIntoMemoryBlock(pb, plen);
        std::string path(static_cast<const char*>(pb.getData()), (std::size_t) plen);
        const int vlen = in.readInt();
        if (vlen < 0 || vlen > 4096) break;
        std::vector<float> v((std::size_t) vlen);
        for (int d = 0; d < vlen; ++d) v[(std::size_t) d] = in.readFloat();
        cache.emplace(std::move(path), std::move(v));
    }
}

void SampleEmbedding::saveIndex()
{
    const auto f = indexFile();
    f.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp(f);
    {
        juce::FileOutputStream out(tmp.getFile());
        if (! out.openedOk())
            return;
        std::lock_guard<std::mutex> lk(cacheMutex);
        out.writeInt(kIndexVersion);
        out.writeInt((int) cache.size());
        for (const auto& [path, v] : cache)
        {
            out.writeInt((int) path.size());
            out.write(path.data(), path.size());
            out.writeInt((int) v.size());
            for (float x : v) out.writeFloat(x);
        }
    }
    tmp.overwriteTargetFileWithTemporary();
    unsaved = 0;
}

void SampleEmbedding::indexFolders(const std::vector<juce::File>& roots)
{
    if (indexerRunning.exchange(true))
        return;   // one background indexer at a time
    if (indexerThread.joinable())
        indexerThread.join();   // reap a previous finished run
    indexerStop = false;

    std::vector<juce::File> r = roots;
    indexerThread = std::thread([this, r]
    {
        int indexedThisRun = 0;
        for (const auto& root : r)
        {
            if (indexerStop.load() || ! root.isDirectory())
                continue;

            for (const auto& entry : juce::RangedDirectoryIterator(root, true, "*", juce::File::findFiles))
            {
                if (indexerStop.load())
                    break;

                const auto file = entry.getFile();
                if (! file.hasFileExtension("wav;wave;aif;aiff;mp3;flac;ogg"))
                    continue;

                const auto path = file.getFullPathName().toStdString();
                { std::lock_guard<std::mutex> lk(cacheMutex); if (cache.count(path)) continue; }
                auto v = analyseFile(file);
                { std::lock_guard<std::mutex> lk(cacheMutex); cache[path] = std::move(v); }
                if (++unsaved >= 300) saveIndex();
                if (++indexedThisRun >= kMaxIndexedFilesPerRun)
                    break;
            }

            if (indexerStop.load() || indexedThisRun >= kMaxIndexedFilesPerRun)
                break;
        }
        if (unsaved.load() > 0) saveIndex();
        indexerRunning = false;
    });
}

float SampleEmbedding::cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.empty() || a.size() != b.size())
        return 0.0f;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        dot += (double) a[i] * b[i];
        na  += (double) a[i] * a[i];
        nb  += (double) b[i] * b[i];
    }
    if (na < 1.0e-12 || nb < 1.0e-12)
        return 0.0f;
    return (float) juce::jlimit(-1.0, 1.0, dot / (std::sqrt(na) * std::sqrt(nb)));
}

std::vector<float> SampleEmbedding::analyseFile(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return {};

    const double sr = reader->sampleRate;
    const int wantSamples = static_cast<int>(juce::jmin<juce::int64>(
        reader->lengthInSamples, static_cast<juce::int64>(kMaxSeconds * sr)));
    if (wantSamples < kFftSize)
        return {};

    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), wantSamples);
    reader->read(&buffer, 0, wantSamples, 0, true, true);

    juce::AudioBuffer<float> mono(1, wantSamples);
    mono.clear();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        mono.addFrom(0, 0, buffer, ch, 0, wantSamples, 1.0f / static_cast<float>(buffer.getNumChannels()));
    const float* x = mono.getReadPointer(0);

    static MelBank bank;
    static juce::CriticalSection bankLock;
    {
        const juce::ScopedLock sl(bankLock);
        if (std::abs(bank.sr - sr) > 1.0) bank.build(sr);
    }
    MelBank localBank;
    { const juce::ScopedLock sl(bankLock); localBank = bank; }

    juce::dsp::FFT fft(kFftOrder);
    std::vector<float> window((std::size_t) kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[(std::size_t) i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * i / (kFftSize - 1));

    constexpr int kNumMfcc = 12;   // drop c0 (loudness) — keep c1..c12 (timbre shape)
    std::array<double, kNumMfcc> mfccSum {}, mfccSumSq {};
    std::array<double, 5> bandSum {};        // low / low-mid / mid / high / air energy fractions
    double centroidSum = 0.0, flatnessSum = 0.0, rolloffSum = 0.0, zcrSum = 0.0;
    int frames = 0;

    // Precompute DCT-II basis (mel band b → mfcc coeff j).
    static std::array<std::array<double, kMelBands>, kNumMfcc> dct;
    static bool dctReady = false;
    if (! dctReady)
    {
        for (int j = 0; j < kNumMfcc; ++j)
            for (int b = 0; b < kMelBands; ++b)
                dct[(std::size_t) j][(std::size_t) b] = std::cos(juce::MathConstants<double>::pi * (j + 1) * (b + 0.5) / kMelBands);
        dctReady = true;
    }

    std::vector<float> fftBuf((std::size_t) kFftSize * 2);
    const int nBins = kFftSize / 2 + 1;

    for (int start = 0; start + kFftSize <= wantSamples; start += kHop)
    {
        std::fill(fftBuf.begin(), fftBuf.end(), 0.0f);
        for (int i = 0; i < kFftSize; ++i)
            fftBuf[(std::size_t) i] = x[start + i] * window[(std::size_t) i];
        fft.performFrequencyOnlyForwardTransform(fftBuf.data());   // magnitudes in [0..kFftSize/2]

        // Power spectrum + scalar descriptors + band-energy fractions (bass↔bright is the key cue).
        double totalMag = 0.0, weightedFreq = 0.0, logSum = 0.0, powSum = 0.0;
        double band[5] = { 0, 0, 0, 0, 0 };
        for (int k = 0; k < nBins; ++k)
        {
            const double mag = fftBuf[(std::size_t) k];
            const double p = mag * mag;
            const double f = (double) k * sr / kFftSize;
            totalMag += mag;
            weightedFreq += f * mag;
            logSum += std::log(p + 1.0e-9);
            powSum += p;
            if      (f < 200.0)   band[0] += p;
            else if (f < 800.0)   band[1] += p;
            else if (f < 3000.0)  band[2] += p;
            else if (f < 8000.0)  band[3] += p;
            else                  band[4] += p;
        }
        if (totalMag < 1.0e-9 || powSum < 1.0e-12) continue;

        for (int j = 0; j < 5; ++j) bandSum[(std::size_t) j] += band[j] / powSum;         // fractions, sum ~1

        centroidSum += std::log(1.0 + weightedFreq / totalMag);                            // log-Hz brightness
        const double geoMean = std::exp(logSum / nBins);
        const double ariMean = powSum / nBins;
        flatnessSum += ariMean > 1.0e-12 ? geoMean / ariMean : 0.0;                         // 0..1 tonal..noisy

        double acc = 0.0; int rollK = nBins - 1;
        for (int k = 0; k < nBins; ++k) { acc += fftBuf[(std::size_t) k] * fftBuf[(std::size_t) k]; if (acc >= 0.85 * powSum) { rollK = k; break; } }
        rolloffSum += std::log(1.0 + (double) rollK * sr / kFftSize);

        int zc = 0;
        for (int i = 1; i < kFftSize; ++i)
            if ((x[start + i] >= 0.0f) != (x[start + i - 1] >= 0.0f)) ++zc;
        zcrSum += (double) zc / kFftSize;

        // Mel log energies → MFCC via DCT (decorrelated timbre; c0 dropped so loudness doesn't dominate).
        std::array<double, kMelBands> melLog;
        for (int b = 0; b < kMelBands; ++b)
        {
            double e = 0.0;
            const auto& w = localBank.weights[(std::size_t) b];
            for (int k = 0; k < nBins; ++k) e += w[(std::size_t) k] * (double) fftBuf[(std::size_t) k] * fftBuf[(std::size_t) k];
            melLog[(std::size_t) b] = std::log(e + 1.0e-9);
        }
        for (int j = 0; j < kNumMfcc; ++j)
        {
            double c = 0.0;
            for (int b = 0; b < kMelBands; ++b) c += melLog[(std::size_t) b] * dct[(std::size_t) j][(std::size_t) b];
            mfccSum[(std::size_t) j] += c;
            mfccSumSq[(std::size_t) j] += c * c;
        }
        ++frames;
    }

    if (frames == 0)
        return {};

    // --- Temporal descriptors over the whole head ---
    int peakIndex = 0; float peak = 0.0f;
    for (int i = 0; i < wantSamples; ++i) { const float a = std::abs(x[i]); if (a > peak) { peak = a; peakIndex = i; } }
    const double attack = std::log(1.0 + (double) peakIndex / sr);
    const int half = wantSamples / 2;
    double eEarly = 1.0e-9, eLate = 1.0e-9;
    for (int i = 0; i < half; ++i)          eEarly += (double) x[i] * x[i];
    for (int i = half; i < wantSamples; ++i) eLate  += (double) x[i] * x[i];
    const double sustain = std::log(1.0 + eLate / eEarly);

    // RAW feature layout (36 dims): [ mfccMean(12) | mfccStd(12) | band(5) | spectral(4) | temporal(3) ].
    // Discrimination comes from per-dimension standardisation over the corpus at search time
    // (rankSimilar), then group weighting (see featureWeights) that emphasises the cues carrying
    // instrument identity — band fractions (bass↔bright) + envelope (pluck↔pad).
    std::vector<float> out;
    out.reserve(36);
    for (int j = 0; j < kNumMfcc; ++j) out.push_back((float) (mfccSum[(std::size_t) j] / frames));
    for (int j = 0; j < kNumMfcc; ++j)
    {
        const double m = mfccSum[(std::size_t) j] / frames;
        const double v = juce::jmax(0.0, mfccSumSq[(std::size_t) j] / frames - m * m);
        out.push_back((float) std::sqrt(v));
    }
    for (int j = 0; j < 5; ++j) out.push_back((float) (bandSum[(std::size_t) j] / frames));
    out.push_back((float) (centroidSum / frames));
    out.push_back((float) (flatnessSum / frames));
    out.push_back((float) (rolloffSum / frames));
    out.push_back((float) (zcrSum / frames));
    // crest factor (peak/rms) — sharp transients (plucks/drums) vs smooth (pads).
    double rms = 0.0; for (int i = 0; i < wantSamples; ++i) rms += (double) x[i] * x[i];
    rms = std::sqrt(rms / juce::jmax(1, wantSamples));
    out.push_back((float) std::log(1.0 + (rms > 1.0e-9 ? peak / rms : 0.0)));
    out.push_back((float) attack);
    out.push_back((float) sustain);
    return out;
}

const std::vector<float>& SampleEmbedding::featureWeights()
{
    // Per-dimension weights applied AFTER standardisation, matching the 36-dim layout above.
    static const std::vector<float> w = []
    {
        std::vector<float> v;
        for (int i = 0; i < 12; ++i) v.push_back(1.0f);   // mfcc mean  (timbre shape)
        for (int i = 0; i < 12; ++i) v.push_back(0.6f);   // mfcc std
        for (int i = 0; i < 5;  ++i) v.push_back(1.8f);   // band fractions — strongest instrument cue
        v.push_back(1.4f); v.push_back(1.0f); v.push_back(1.2f); v.push_back(0.8f);  // centroid/flatness/rolloff/zcr
        v.push_back(1.3f); v.push_back(1.5f); v.push_back(1.5f);  // crest / attack / sustain (envelope)
        return v;
    }();
    return w;
}

std::vector<SampleEmbedding::Match> SampleEmbedding::rankSimilar(const std::vector<float>& query,
                                                                 const std::vector<std::vector<float>>& candidates)
{
    std::vector<Match> out;
    if (query.empty())
        return out;
    const int dim = (int) query.size();

    // Per-dimension mean/std over all valid candidates (+ query) → z-score whitening.
    std::vector<double> mean(dim, 0.0), var(dim, 0.0);
    int n = 0;
    const auto accumulate = [&](const std::vector<float>& v) { if ((int) v.size() == dim) { for (int d = 0; d < dim; ++d) mean[(std::size_t) d] += v[(std::size_t) d]; ++n; } };
    for (const auto& c : candidates) accumulate(c);
    accumulate(query);
    if (n == 0) return out;
    for (int d = 0; d < dim; ++d) mean[(std::size_t) d] /= n;
    const auto accVar = [&](const std::vector<float>& v) { if ((int) v.size() == dim) for (int d = 0; d < dim; ++d) { const double e = v[(std::size_t) d] - mean[(std::size_t) d]; var[(std::size_t) d] += e * e; } };
    for (const auto& c : candidates) accVar(c);
    accVar(query);
    std::vector<double> inv(dim);
    for (int d = 0; d < dim; ++d) inv[(std::size_t) d] = 1.0 / std::sqrt(juce::jmax(1.0e-9, var[(std::size_t) d] / n));

    // Group weights (emphasise instrument-identity cues) when the vector matches the known layout.
    const auto& wts = featureWeights();
    const bool useW = ((int) wts.size() == dim);
    double wsum = 0.0;
    for (int d = 0; d < dim; ++d) wsum += useW ? (double) wts[(std::size_t) d] * wts[(std::size_t) d] : 1.0;
    const double wnorm = std::sqrt(juce::jmax(1.0, wsum));

    std::vector<float> zq(dim);
    for (int d = 0; d < dim; ++d) zq[(std::size_t) d] = (float) ((query[(std::size_t) d] - mean[(std::size_t) d]) * inv[(std::size_t) d]);

    for (int i = 0; i < (int) candidates.size(); ++i)
    {
        const auto& c = candidates[(std::size_t) i];
        if ((int) c.size() != dim) { out.push_back({ i, 0.0f }); continue; }
        double dist = 0.0;
        for (int d = 0; d < dim; ++d)
        {
            const double zc = (c[(std::size_t) d] - mean[(std::size_t) d]) * inv[(std::size_t) d];
            const double w = useW ? wts[(std::size_t) d] : 1.0f;
            const double e = (zc - zq[(std::size_t) d]) * w;
            dist += e * e;
        }
        dist = std::sqrt(dist);
        out.push_back({ i, (float) (1.0 / (1.0 + dist / wnorm)) });   // 1 = identical, →0 = far
    }
    std::sort(out.begin(), out.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    return out;
}

std::vector<float> SampleEmbedding::computeNow(const juce::File& file)
{
    if (auto c = cachedEmbedding(file)) return *c;
    auto e = analyseFile(file);
    {
        std::lock_guard<std::mutex> lk(cacheMutex);
        cache[file.getFullPathName().toStdString()] = e;
    }
    return e;
}

std::optional<std::vector<float>> SampleEmbedding::cachedEmbedding(const juce::File& file) const
{
    std::lock_guard<std::mutex> lk(cacheMutex);
    const auto it = cache.find(file.getFullPathName().toStdString());
    if (it == cache.end())
        return std::nullopt;
    return it->second;
}

void SampleEmbedding::requestEmbedding(const juce::File& file, std::function<void(std::vector<float>)> onReady)
{
    const auto path = file.getFullPathName().toStdString();
    {
        std::lock_guard<std::mutex> lk(cacheMutex);
        if (const auto it = cache.find(path); it != cache.end())
        {
            auto v = it->second;
            juce::MessageManager::callAsync([onReady = std::move(onReady), v = std::move(v)]() mutable { onReady(std::move(v)); });
            return;
        }
        if (pending.count(path))
            return;
        pending.insert(path);
    }

    pool.addJob([this, file, path, onReady = std::move(onReady)]() mutable
    {
        auto e = analyseFile(file);
        {
            std::lock_guard<std::mutex> lk(cacheMutex);
            cache[path] = e;
            pending.erase(path);
        }
        juce::MessageManager::callAsync([onReady = std::move(onReady), e = std::move(e)]() mutable { onReady(std::move(e)); });
    });
}
}  // namespace orion
