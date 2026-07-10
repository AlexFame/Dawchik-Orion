// Offline accuracy harness for the chord-progression detector.
//
// Scans a folder of loops, parses the ground-truth KEY from the filename
// ("... 140 BPM F Min.wav"), runs orion::chorddetect::detectBarChords on the
// SIGNAL, and measures how well the detected chords fit that key:
//   - % of detected chord roots that are diatonic to the key
//   - mode agreement (major/minor chords vs the key's mode)
//   - whether the tonic chord is the most common detected root
//
// There's no per-chord ground truth in filenames, so "% in-key" is the proxy:
// a good detector on tonal loops should land almost entirely on diatonic chords.
//
// usage: OrionChordTest <folder>

#include <juce_audio_formats/juce_audio_formats.h>
#include "../Audio/ChordDetector.h"

#include <array>
#include <iostream>
#include <map>

namespace
{
    struct TruthKey { int root { -1 }; bool minor { false }; };

    TruthKey parseTruth(const juce::File& file)
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
            if (rem.startsWith("min")) return { root, true };
            if (rem.startsWith("maj")) return { root, false };
        }
        return {};
    }

    int parseBpm(const juce::File& file)
    {
        auto t = file.getFileNameWithoutExtension().toLowerCase();
        const auto idx = t.indexOf("bpm");
        if (idx <= 0) return 0;
        int end = idx; while (end > 0 && t[end - 1] == ' ') --end;
        int start = end; while (start > 0 && juce::CharacterFunctions::isDigit(t[start - 1])) --start;
        return t.substring(start, end).getIntValue();
    }

    bool isDiatonic(int pc, int keyRoot, bool minor)
    {
        static const std::array<int, 7> maj { { 0, 2, 4, 5, 7, 9, 11 } };
        static const std::array<int, 7> min { { 0, 2, 3, 5, 7, 8, 10 } };
        const int rel = (((pc - keyRoot) % 12) + 12) % 12;
        for (int s : (minor ? min : maj)) if (s == rel) return true;
        return false;
    }

    const char* NM[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

int main(int argc, char* argv[])
{
    if (argc < 2) { std::cout << "usage: OrionChordTest <folder>\n"; return 1; }
    const juce::File folder(juce::String::fromUTF8(argv[1]));
    auto files = folder.findChildFiles(juce::File::findFiles, true, "*.wav;*.aif;*.aiff");
    files.sort();

    int totalFiles = 0, totalChords = 0, diatonic = 0, modeMatch = 0, tonicIsTop = 0;
    juce::AudioFormatManager fm; fm.registerBasicFormats();

    for (const auto& f : files)
    {
        if (f.getFullPathName().containsIgnoreCase("Stem") || f.getFullPathName().containsIgnoreCase("MIDI"))
            continue;
        const auto truth = parseTruth(f);
        if (truth.root < 0) continue;

        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(f));
        if (reader == nullptr || reader->lengthInSamples <= 0) continue;
        const double durSec = reader->lengthInSamples / reader->sampleRate;
        const int bpm = parseBpm(f);
        const int numBars = bpm > 0 ? juce::jmax(1, (int) std::round(durSec * bpm / 60.0 / 4.0)) : 4;

        orion::chorddetect::Options opts;
        opts.numBars = numBars;
        opts.keyRoot = truth.root;   // as in the app: key known at drop → diatonic-restricted detection
        opts.keyMinor = truth.minor;
        auto chords = orion::chorddetect::detectBarChords(f, opts);
        if (chords.empty()) continue;

        ++totalFiles;
        std::map<int, int> rootHist;
        int fileDia = 0, fileMode = 0;
        for (const auto& c : chords)
        {
            ++totalChords;
            rootHist[c.rootPc]++;
            if (isDiatonic(c.rootPc, truth.root, truth.minor)) { ++diatonic; ++fileDia; }
            const bool cMinor = (c.quality == orion::chords::Quality::minor || c.quality == orion::chords::Quality::dim);
            if (cMinor == truth.minor) { ++modeMatch; ++fileMode; }
        }
        int topRoot = -1, topN = -1;
        for (auto& [r, n] : rootHist) if (n > topN) { topN = n; topRoot = r; }
        const bool tonicTop = topRoot == truth.root;
        if (tonicTop) ++tonicIsTop;

        std::cout << f.getFileName()
                  << "  key=" << NM[truth.root] << (truth.minor ? "m" : "")
                  << "  bars=" << numBars
                  << "  inKey=" << fileDia << "/" << (int) chords.size()
                  << "  top=" << NM[topRoot] << (tonicTop ? " (tonic)" : "")
                  << "\n";
    }

    std::cout << "\n===== SUMMARY =====\n"
              << "files: " << totalFiles << ", chords: " << totalChords << "\n"
              << "in-key roots:   " << diatonic << "/" << totalChords
              << "  (" << (totalChords ? 100 * diatonic / totalChords : 0) << "%)\n"
              << "mode match:     " << modeMatch << "/" << totalChords
              << "  (" << (totalChords ? 100 * modeMatch / totalChords : 0) << "%)\n"
              << "tonic = top root: " << tonicIsTop << "/" << totalFiles
              << "  (" << (totalFiles ? 100 * tonicIsTop / totalFiles : 0) << "%)\n";
    return 0;
}
