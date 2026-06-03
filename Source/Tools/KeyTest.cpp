// Offline key-detection accuracy harness.
//
// Scans a folder of audio files, runs the chroma key detector on the SIGNAL
// (ignoring the filename), and compares against the ground-truth key parsed
// from the filename (e.g. "... 140 BPM F Min.wav"). Prints accuracy so the
// algorithm can be tuned against real material instead of by ear.
//
// The detector code here mirrors WarpEngine.cpp's helpers; once tuned, the
// winning version is ported back verbatim.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
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

// ---- TUNABLE: chroma extraction ----
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

    // Precompute Goertzel coefficients per note.
    std::array<double, hiMidi - loMidi + 1> coeffs {};
    for (int midi = loMidi; midi <= hiMidi; ++midi)
    {
        const auto freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
        coeffs[static_cast<std::size_t>(midi - loMidi)] = (freq > sr * 0.45) ? -100.0 : 2.0 * std::cos(twoPi * freq / sr);
    }

    // Per-frame chroma, normalized then accumulated, so loud sustained notes don't swamp
    // the tonal profile — each moment contributes its harmonic "shape" equally.
    int frames = 0;
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
        {
            for (int i = 0; i < 12; ++i) chroma[static_cast<std::size_t>(i)] += frameChroma[static_cast<std::size_t>(i)] / fsum;
            ++frames;
        }
    }

    double sum = 0.0;
    for (auto v : chroma) sum += v;
    if (sum > 0.0)
        for (auto& v : chroma) v /= sum;
    juce::ignoreUnused(frames);
    return chroma;
}

struct KeyEstimate { int root { -1 }; bool minor { false }; double confidence { 0.0 }; };

// ---- TUNABLE: profiles + correlation ----
KeyEstimate estimateKeyFromChroma(const std::array<double, 12>& chroma)
{
    // Albrecht & Shanahan (2013) corpus-derived key profiles.
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
        if (cMaj > best.confidence) best = { root, false, cMaj };
        const auto cMin = correlate(minor, root);
        if (cMin > best.confidence) best = { root, true, cMin };
    }
    return best;
}

// Ground-truth key from a Cymatics-style name: "... 140 BPM F Min.wav".
KeyEstimate parseTruth(const juce::File& file)
{
    auto t = juce::String(' ') + file.getFileNameWithoutExtension().toLowerCase() + juce::String(' ');
    const auto semi = [](juce::juce_wchar c) -> int
    {
        switch (c) { case 'c': return 0; case 'd': return 2; case 'e': return 4; case 'f': return 5;
                     case 'g': return 7; case 'a': return 9; case 'b': return 11; }
        return -1;
    };
    for (int i = 1; i + 1 < t.length(); ++i)
    {
        if (t[i - 1] != ' ') continue;
        const auto s = semi(t[i]);
        if (s < 0) continue;
        int root = s, pos = i + 1;
        if (pos < t.length() && t[pos] == '#') { root = (root + 1) % 12; ++pos; }
        else if (pos < t.length() && t[pos] == 'b' && pos + 1 < t.length() && t[pos + 1] == ' ') { root = (root + 11) % 12; ++pos; }
        while (pos < t.length() && t[pos] == ' ') ++pos;
        const auto rem = t.substring(pos, juce::jmin(pos + 3, t.length()));
        if (rem.startsWith("min")) return { root, true, 1.0 };
        if (rem.startsWith("maj")) return { root, false, 1.0 };
    }
    return {};
}

const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
juce::String keyName(const KeyEstimate& k) { return k.root < 0 ? juce::String("?") : juce::String(names[k.root]) + (k.minor ? "m" : ""); }
}  // namespace

int main(int argc, char* argv[])
{
    if (argc < 2) { std::cout << "usage: OrionKeyTest <folder> [confThreshold]\n"; return 1; }
    const juce::File folder(juce::String::fromUTF8(argv[1]));
    const double confThreshold = argc >= 3 ? juce::String(argv[2]).getDoubleValue() : 0.0;

    auto files = folder.findChildFiles(juce::File::findFiles, true, "*.wav;*.aif;*.aiff");
    files.sort();

    int total = 0, exact = 0, relative = 0, fifth = 0, parallel = 0, reported = 0, correctReported = 0, unknownTruth = 0;
    int majTotal = 0, majExact = 0, minTotal = 0, minExact = 0;
    std::vector<juce::String> mismatches;

    for (const auto& f : files)
    {
        // Skip stem/MIDI subfolders — stems (drums/bass/perc) carry the loop's key in
        // their name but aren't tonal, which isn't a fair test of melodic key detection.
        if (f.getFullPathName().containsIgnoreCase("Stem") || f.getFullPathName().containsIgnoreCase("MIDI"))
            continue;
        const auto truth = parseTruth(f);
        if (truth.root < 0) { ++unknownTruth; continue; }
        ++total;

        double sr = 0.0;
        auto mono = loadMonoForAnalysis(f, sr);
        if (mono.getNumSamples() == 0 || sr <= 0.0) continue;
        const auto est = estimateKeyFromChroma(computeChroma(mono, sr));

        const bool isExact = est.root == truth.root && est.minor == truth.minor;
        // relative major/minor: minor i <-> relative major at +3
        const bool isRelative = (truth.minor && ! est.minor && est.root == (truth.root + 3) % 12)
                              || (! truth.minor && est.minor && est.root == (truth.root + 9) % 12);
        const bool isFifth = est.minor == truth.minor && (est.root == (truth.root + 7) % 12 || est.root == (truth.root + 5) % 12);
        const bool isParallel = est.root == truth.root && est.minor != truth.minor;

        if (truth.minor) { ++minTotal; if (isExact) ++minExact; }
        else             { ++majTotal; if (isExact) ++majExact; }

        if (isExact) ++exact;
        else if (isRelative) ++relative;
        else if (isFifth) ++fifth;
        else if (isParallel) ++parallel;

        if (est.confidence >= confThreshold) { ++reported; if (isExact) ++correctReported; }

        if (! isExact && mismatches.size() < 30)
            mismatches.push_back(f.getFileName() + "  truth=" + keyName(truth) + "  got=" + keyName(est)
                           + " (conf " + juce::String(est.confidence, 2) + ")"
                           + (isRelative ? " [rel]" : isFifth ? " [5th]" : isParallel ? " [par]" : ""));
    }

    std::cout << "\n=== Key detection accuracy ===\n";
    std::cout << "files with ground truth: " << total << "  (skipped unknown-name: " << unknownTruth << ")\n";
    if (total > 0)
    {
        const auto pct = [&](int n) { return juce::String(100.0 * n / total, 1); };
        std::cout << "EXACT (root+mode):  " << exact << "  (" << pct(exact) << "%)\n";
        std::cout << "  major files: " << majExact << "/" << majTotal
                  << " (" << (majTotal ? juce::String(100.0 * majExact / majTotal, 1) : "0") << "%)"
                  << "   minor files: " << minExact << "/" << minTotal
                  << " (" << (minTotal ? juce::String(100.0 * minExact / minTotal, 1) : "0") << "%)\n";
        std::cout << "  +relative maj/min: " << relative << "  (" << pct(exact + relative) << "% cumulative)\n";
        std::cout << "  +perfect fifth:    " << fifth << "\n";
        std::cout << "  +parallel maj/min: " << parallel << "\n";
        if (confThreshold > 0.0)
            std::cout << "at conf>=" << confThreshold << ": reported " << reported << ", of those exact "
                      << correctReported << " (" << (reported ? juce::String(100.0 * correctReported / reported, 1) : "0") << "%)\n";
    }
    std::cout << "\n--- mismatches (first " << mismatches.size() << ") ---\n";
    for (const auto& m : mismatches) std::cout << "  " << m << "\n";
    std::cout << std::endl;
    return 0;
}
