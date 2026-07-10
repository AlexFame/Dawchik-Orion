// Offline sanity check for sample-similarity embeddings.
// Scans a folder, computes a timbral embedding per file, and for a few query files prints the
// top-N most similar sounds. Eyeball: kicks should surface kicks, hats→hats, pads→pads.
//
// usage: OrionSimTest <folder> [queriesToShow]

#include "../Analysis/SampleEmbedding.h"

#include <algorithm>
#include <iostream>
#include <vector>

int main(int argc, char* argv[])
{
    if (argc < 2) { std::cout << "usage: OrionSimTest <folder> [numQueries]\n"; return 1; }
    const juce::File folder(juce::String::fromUTF8(argv[1]));
    const int numQueries = argc >= 3 ? juce::String(argv[2]).getIntValue() : 6;

    auto files = folder.findChildFiles(juce::File::findFiles, true, "*.wav;*.aif;*.aiff;*.mp3;*.flac");
    files.sort();
    if (files.isEmpty()) { std::cout << "no audio files found\n"; return 1; }

    orion::SampleEmbedding emb;
    struct Entry { juce::File file; std::vector<float> v; };
    std::vector<Entry> db;
    for (const auto& f : files)
    {
        auto v = emb.computeNow(f);
        if (! v.empty()) db.push_back({ f, std::move(v) });
    }
    std::cout << "indexed " << db.size() << " / " << files.size() << " files\n\n";

    const int step = juce::jmax(1, (int) db.size() / juce::jmax(1, numQueries));
    for (int qi = 0; qi < (int) db.size(); qi += step)
    {
        const auto& q = db[(std::size_t) qi];
        std::vector<std::vector<float>> cands;
        for (const auto& e : db) cands.push_back(e.v);
        auto ranked = orion::SampleEmbedding::rankSimilar(q.v, cands);

        std::cout << "QUERY: " << q.file.getFileName() << "\n";
        int shown = 0;
        for (const auto& m : ranked)
        {
            if (m.index == qi) continue;   // skip self
            std::cout << "   " << juce::String(m.score, 3) << "  " << db[(std::size_t) m.index].file.getFileName() << "\n";
            if (++shown >= 5) break;
        }
        std::cout << "\n";
    }
    return 0;
}
