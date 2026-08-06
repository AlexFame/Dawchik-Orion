#include "ChordDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace orion::chorddetect
{
    using orion::chords::ChordSpec;
    using orion::chords::Quality;

    namespace
    {
        // 12-bin chroma over a slice, via Goertzel filters across ~5 octaves (C2..B6). Mirrors the
        // key detector's extraction so both stay consistent.
        std::array<double, 12> chromaOfSlice (const float* x, int n, double sr)
        {
            std::array<double, 12> chroma {};
            chroma.fill (0.0);
            if (n < 2048 || sr <= 0.0)
                return chroma;

            constexpr int frame = 4096;
            constexpr int hop = 2048;
            constexpr int loMidi = 24, hiMidi = 95;   // C1..B6 (down to C1 to see bass + harmonics)
            constexpr int N = hiMidi - loMidi + 1;
            const auto twoPi = juce::MathConstants<double>::twoPi;

            std::array<double, N> coeffs {};
            for (int midi = loMidi; midi <= hiMidi; ++midi)
            {
                const auto freq = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
                coeffs[static_cast<std::size_t> (midi - loMidi)] = (freq > sr * 0.45) ? -100.0 : 2.0 * std::cos (twoPi * freq / sr);
            }

            // Per-NOTE log-frequency spectrum (not folded yet), averaged over frames with per-frame
            // normalisation so a loud sustained note doesn't dominate the whole window.
            std::array<double, N> spec {};
            spec.fill (0.0);
            const int usableFrame = juce::jmin (frame, n);
            for (int start = 0; start + usableFrame <= n; start += hop)
            {
                std::array<double, N> fm {};
                double fsum = 0.0;
                for (int midi = loMidi; midi <= hiMidi; ++midi)
                {
                    const auto coeff = coeffs[static_cast<std::size_t> (midi - loMidi)];
                    if (coeff < -10.0) { fm[static_cast<std::size_t> (midi - loMidi)] = 0.0; continue; }
                    double s1 = 0.0, s2 = 0.0;
                    for (int i = 0; i < usableFrame; ++i)
                    {
                        const auto s0 = static_cast<double> (x[start + i]) + coeff * s1 - s2;
                        s2 = s1; s1 = s0;
                    }
                    const auto mag = std::sqrt (juce::jmax (0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2));
                    fm[static_cast<std::size_t> (midi - loMidi)] = mag;
                    fsum += mag;
                }
                if (fsum > 1.0e-9)
                    for (int i = 0; i < N; ++i) spec[static_cast<std::size_t> (i)] += fm[static_cast<std::size_t> (i)] / fsum;
            }

            // NNLS note-activation (Chordino-style): model the spectrum as spec ≈ D·a, where each note
            // f contributes a harmonic comb (fundamental + overtones) and a>=0 are the note activations.
            // Solving for `a` removes overtone smearing MATHEMATICALLY (not by crude subtraction), so a
            // chord's true notes survive and the tonic stops "leaking" onto every pitch class. Solved
            // with non-negative multiplicative (NMF-style) updates — stable and always non-negative.
            static const int    hSemi[6] = { 0, 12, 19, 24, 28, 31 };   // 1st..6th harmonic offsets (semitones)
            static const double hW[6]    = { 1.0, 0.5, 0.33, 0.25, 0.2, 0.16 };

            // (D·a)[b] = Σ_h hW[h] · a[b − hSemi[h]]   (a note's harmonics add into higher bins)
            const auto applyD = [&] (const std::array<double, N>& a, std::array<double, N>& outv)
            {
                outv.fill (0.0);
                for (int f = 0; f < N; ++f)
                {
                    const double af = a[static_cast<std::size_t> (f)];
                    if (af <= 0.0) continue;
                    for (int h = 0; h < 6; ++h)
                    {
                        const int b = f + hSemi[h];
                        if (b < N) outv[static_cast<std::size_t> (b)] += hW[h] * af;
                    }
                }
            };
            // (Dᵀ·v)[f] = Σ_h hW[h] · v[f + hSemi[h]]
            const auto applyDt = [&] (const std::array<double, N>& v, std::array<double, N>& outv)
            {
                for (int f = 0; f < N; ++f)
                {
                    double s = 0.0;
                    for (int h = 0; h < 6; ++h)
                    {
                        const int b = f + hSemi[h];
                        if (b < N) s += hW[h] * v[static_cast<std::size_t> (b)];
                    }
                    outv[static_cast<std::size_t> (f)] = s;
                }
            };

            std::array<double, N> a, DtS, Da, DtDa;
            applyDt (spec, DtS);
            a.fill (0.0);
            for (int f = 0; f < N; ++f) a[static_cast<std::size_t> (f)] = juce::jmax (1.0e-6, DtS[static_cast<std::size_t> (f)]);   // warm start
            for (int it = 0; it < 30; ++it)
            {
                applyD (a, Da);
                applyDt (Da, DtDa);
                for (int f = 0; f < N; ++f)
                    a[static_cast<std::size_t> (f)] *= DtS[static_cast<std::size_t> (f)] / (DtDa[static_cast<std::size_t> (f)] + 1.0e-9);
            }

            for (int midi = loMidi; midi <= hiMidi; ++midi)
                chroma[static_cast<std::size_t> (midi % 12)] += a[static_cast<std::size_t> (midi - loMidi)];

            double sum = 0.0;
            for (auto v : chroma) sum += v;
            if (sum > 0.0)
                for (auto& v : chroma) v /= sum;
            return chroma;
        }

        struct Template
        {
            const char* name;
            std::vector<int> tones;   // pitch-class offsets from the chord root
            Quality quality;
            juce::uint32 extensions;
        };

        const std::vector<Template>& templates()
        {
            static const std::vector<Template> t {
                { "maj",  { 0, 4, 7 },      Quality::major, orion::chords::ext_none },
                { "min",  { 0, 3, 7 },      Quality::minor, orion::chords::ext_none },
                { "dom7", { 0, 4, 7, 10 },  Quality::major, orion::chords::ext_7 },
                { "min7", { 0, 3, 7, 10 },  Quality::minor, orion::chords::ext_7 },
                { "maj7", { 0, 4, 7, 11 },  Quality::major, orion::chords::ext_maj7 },
                { "dim",  { 0, 3, 6 },      Quality::dim,   orion::chords::ext_none },
                // sus2/sus4 intentionally omitted: thirdless templates over-fire on noisy chroma and
                // spammed spurious "sus" chords. Power ("5") is handled by the post-check instead.
            };
            return t;
        }

        // Low-register chroma (C1..D#3) to estimate the BASS note — helps pin the chord root and
        // avoid inversion confusion (the bass usually plays the root).
        std::array<double, 12> bassChromaOfSlice (const float* x, int n, double sr)
        {
            std::array<double, 12> chroma {};
            chroma.fill (0.0);
            if (n < 2048 || sr <= 0.0)
                return chroma;

            constexpr int frame = 4096, hop = 2048, loMidi = 24, hiMidi = 51;   // C1..D#3
            const auto twoPi = juce::MathConstants<double>::twoPi;
            std::array<double, hiMidi - loMidi + 1> coeffs {};
            for (int midi = loMidi; midi <= hiMidi; ++midi)
            {
                const auto freq = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
                coeffs[static_cast<std::size_t> (midi - loMidi)] = (freq > sr * 0.45) ? -100.0 : 2.0 * std::cos (twoPi * freq / sr);
            }
            const int usableFrame = juce::jmin (frame, n);
            for (int start = 0; start + usableFrame <= n; start += hop)
                for (int midi = loMidi; midi <= hiMidi; ++midi)
                {
                    const auto coeff = coeffs[static_cast<std::size_t> (midi - loMidi)];
                    if (coeff < -10.0) continue;
                    double s1 = 0.0, s2 = 0.0;
                    for (int i = 0; i < usableFrame; ++i) { const auto s0 = static_cast<double> (x[start + i]) + coeff * s1 - s2; s2 = s1; s1 = s0; }
                    chroma[static_cast<std::size_t> (midi % 12)] += std::sqrt (juce::jmax (0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2));
                }
            double sum = 0.0; for (auto v : chroma) sum += v;
            if (sum > 0.0) for (auto& v : chroma) v /= sum;
            return chroma;
        }

        // Score a rooted template against the chroma: reward energy on chord tones (the THIRD weighted
        // heavier so maj/min is decided reliably), penalise energy off them.
        double score (const std::array<double, 12>& chroma, const Template& tpl, int root)
        {
            std::array<bool, 12> isTone {};
            for (int t : tpl.tones)
                isTone[static_cast<std::size_t> ((t + root) % 12)] = true;

            double on = 0.0, wSum = 0.0;
            for (std::size_t k = 0; k < tpl.tones.size(); ++k)
            {
                // tones[0]=root, [1]=third, [2]=fifth, [3+]=7th/tensions.
                const double w = (k == 1) ? 1.9 : (k >= 3 ? 1.2 : 1.0);
                on   += w * chroma[static_cast<std::size_t> ((tpl.tones[k] + root) % 12)];
                wSum += w;
            }

            double off = 0.0; int offN = 0;
            for (int i = 0; i < 12; ++i)
                if (! isTone[static_cast<std::size_t> (i)]) { off += chroma[static_cast<std::size_t> (i)]; ++offN; }

            return on / juce::jmax (1.0, wSum) - 0.55 * (off / juce::jmax (1, offN));
        }

        bool pcInKey (int pc, int keyRoot, bool minor)
        {
            static const std::array<int, 7> maj { { 0, 2, 4, 5, 7, 9, 11 } };
            static const std::array<int, 7> min { { 0, 2, 3, 5, 7, 8, 10 } };
            const auto& scale = minor ? min : maj;
            const int rel = (((pc - keyRoot) % 12) + 12) % 12;
            for (int s : scale) if (s == rel) return true;
            return false;
        }

        // Score an explicit set of chord-tone pitch classes (third weighted heavier), with a bass
        // bonus for the root. Off-tone energy is penalised.
        double scorePcs (const std::array<double, 12>& chroma, const std::array<double, 12>& bass,
                         int rootPc, int thirdPc, int fifthPc, int seventhPc)
        {
            std::array<bool, 12> isTone {};
            isTone[static_cast<std::size_t> (rootPc)] = true;
            isTone[static_cast<std::size_t> (thirdPc)] = true;
            isTone[static_cast<std::size_t> (fifthPc)] = true;
            if (seventhPc >= 0) isTone[static_cast<std::size_t> (seventhPc)] = true;

            double on = chroma[static_cast<std::size_t> (rootPc)] * 1.0
                      + chroma[static_cast<std::size_t> (thirdPc)] * 1.9
                      + chroma[static_cast<std::size_t> (fifthPc)] * 1.0;
            double wSum = 3.9;
            if (seventhPc >= 0) { on += chroma[static_cast<std::size_t> (seventhPc)] * 1.2; wSum += 1.2; }

            double off = 0.0; int offN = 0;
            for (int i = 0; i < 12; ++i)
                if (! isTone[static_cast<std::size_t> (i)]) { off += chroma[static_cast<std::size_t> (i)]; ++offN; }

            return on / wSum - 0.55 * (off / juce::jmax (1, offN)) + 0.45 * bass[static_cast<std::size_t> (rootPc)];
        }

        // Emission: how well the (root, template) chord fits a beat's chroma. Third weighted heavier,
        // bass bonus for the root, slight triad preference, and a soft in-key bias when the key is known.
        double emissionScore (const std::array<double, 12>& chroma, const std::array<double, 12>& bass,
                              const Template& tpl, int root, int keyRoot, bool keyMinor)
        {
            double s = score (chroma, tpl, root) + 0.40 * bass[static_cast<std::size_t> (root)];
            if (tpl.tones.size() >= 4) s -= 0.05;   // prefer plain triads over 7ths/sus
            if (keyRoot >= 0)
            {
                int inK = 0;
                for (int t : tpl.tones)
                    if (pcInKey ((t + root) % 12, keyRoot, keyMinor)) ++inK;
                s += 0.08 * (static_cast<double> (inK) / static_cast<double> (tpl.tones.size()));
            }
            return s;
        }

        // ---------------------------------------------------------------------------------------------
        // Chordino front-end (primary): Orion bundles a self-contained GPL helper binary
        // (Resources/chorddetect/bin/orion-chorddetect, built from QMUL's NNLS-Chroma/Chordino) and
        // runs it as a SEPARATE PROCESS. Process isolation keeps the GPL code cleanly separated from
        // Orion's proprietary code ("mere aggregation"). The helper prints "<seconds>: <label>" lines;
        // we sample its timeline onto our per-beat grid. If the helper is missing or fails, we fall
        // back to the native template+Viterbi detector below.
        // ---------------------------------------------------------------------------------------------

        // Parse a Chordino label ("Bbm", "F#maj7", "Ab/Gb", "N") → ChordSpec. Returns false for "N"/blank.
        bool parseChordLabel (const juce::String& raw, ChordSpec& out)
        {
            auto label = raw.trim();
            if (label.isEmpty() || label == "N" || label == "X")
                return false;

            // Root note.
            const juce::juce_wchar c = label[0];
            int pc;
            switch (c)
            {
                case 'C': pc = 0; break;  case 'D': pc = 2; break;  case 'E': pc = 4; break;
                case 'F': pc = 5; break;  case 'G': pc = 7; break;  case 'A': pc = 9; break;
                case 'B': pc = 11; break; default: return false;
            }
            int i = 1;
            if (i < label.length() && (label[i] == '#')) { pc = (pc + 1) % 12; ++i; }
            else if (i < label.length() && (label[i] == 'b')) { pc = (pc + 11) % 12; ++i; }

            // Quality = remainder up to any slash; the slash bass (e.g. E/B) is captured separately.
            const auto rest = label.substring (i);
            auto q = rest.upToFirstOccurrenceOf ("/", false, false).trim();
            int bassPc = -1;
            if (rest.contains ("/"))
            {
                const auto bassStr = rest.fromFirstOccurrenceOf ("/", false, false).trim();
                if (bassStr.isNotEmpty())
                {
                    int bpc;
                    switch (bassStr[0])
                    {
                        case 'C': bpc = 0; break;  case 'D': bpc = 2; break;  case 'E': bpc = 4; break;
                        case 'F': bpc = 5; break;  case 'G': bpc = 7; break;  case 'A': bpc = 9; break;
                        case 'B': bpc = 11; break; default: bpc = -1; break;
                    }
                    if (bpc >= 0)
                    {
                        if (bassStr.length() > 1 && bassStr[1] == '#') bpc = (bpc + 1) % 12;
                        else if (bassStr.length() > 1 && bassStr[1] == 'b') bpc = (bpc + 11) % 12;
                        bassPc = bpc;
                    }
                }
            }

            Quality quality = Quality::major;
            juce::uint32 ext = orion::chords::ext_none;
            if      (q.isEmpty())      { quality = Quality::major; }
            else if (q == "m")         { quality = Quality::minor; }
            else if (q == "maj7")      { quality = Quality::major; ext = orion::chords::ext_maj7; }
            else if (q == "7")         { quality = Quality::major; ext = orion::chords::ext_7; }
            else if (q == "6")         { quality = Quality::major; ext = orion::chords::ext_6; }
            else if (q == "m7")        { quality = Quality::minor; ext = orion::chords::ext_7; }
            else if (q == "m6")        { quality = Quality::minor; ext = orion::chords::ext_6; }
            else if (q == "dim")       { quality = Quality::dim; }
            else if (q == "dim7")      { quality = Quality::dim;   ext = orion::chords::ext_6; }
            else if (q == "aug")       { quality = Quality::aug; }
            else
            {
                // Best-effort fallback for any other suffix.
                if (q.startsWith ("maj7"))      { quality = Quality::major; ext = orion::chords::ext_maj7; }
                else if (q.startsWith ("m"))    { quality = Quality::minor; if (q.contains ("7")) ext = orion::chords::ext_7; }
                else if (q.contains ("7"))      { quality = Quality::major; ext = orion::chords::ext_7; }
            }

            out = ChordSpec { pc, quality, ext, bassPc };
            return true;
        }

        // Find the bundled Chordino helper: env override → next to the app bundle → dev tree.
        juce::File locateChordino()
        {
            if (auto* env = std::getenv ("ORION_CHORDDETECT_BIN"))
            {
                juce::File f (juce::String::fromUTF8 (env));
                if (f.existsAsFile()) return f;
            }
            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
            // Orion.app/Contents/MacOS/Orion → Contents/Resources/chorddetect/bin/orion-chorddetect
            const juce::StringArray rels {
                "../Resources/chorddetect/bin/orion-chorddetect",   // inside app bundle
                "chorddetect/bin/orion-chorddetect",                // next to a console tool
                "Resources/chorddetect/bin/orion-chorddetect",
            };
            for (auto& r : rels)
            {
                auto f = exe.getParentDirectory().getChildFile (r);
                if (f.existsAsFile()) return f;
            }
            return {};
        }

        // One Chordino output row: time (seconds) + parsed chord (has=false for "N"/no-chord).
        struct Ev { double t; bool has; ChordSpec spec; };

        // Run the helper on `file` and parse its "<seconds>: <label>" lines into a time-sorted list.
        // Returns false on any failure (→ callers fall back to the native detector).
        bool runChordino (const juce::File& file, std::vector<Ev>& evs)
        {
            const auto bin = locateChordino();
            if (! bin.existsAsFile())
                return false;

            juce::ChildProcess cp;
            juce::StringArray args;
            args.add (bin.getFullPathName());
            args.add (file.getFullPathName());
            if (! cp.start (args, juce::ChildProcess::wantStdOut))
                return false;

            const juce::String text = cp.readAllProcessOutput();   // blocks until the helper exits
            cp.waitForProcessToFinish (20000);

            for (auto line : juce::StringArray::fromLines (text))
            {
                line = line.trim();
                const int colon = line.indexOfChar (':');
                if (colon <= 0) continue;
                const auto tStr = line.substring (0, colon).trim();
                if (! tStr.containsOnly ("0123456789.-+eE")) continue;
                const double t = tStr.getDoubleValue();
                const auto lbl = line.substring (colon + 1).trim();
                ChordSpec sp;
                const bool has = parseChordLabel (lbl, sp);
                evs.push_back ({ t, has, sp });
            }
            if (evs.empty())
                return false;

            std::sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });
            return true;
        }

        // Sample a parsed Chordino timeline onto `segs` equal beats (used by the per-beat interface
        // + offline harness). Returns empty on failure (→ caller falls back to native).
        std::vector<ChordSpec> detectViaChordino (const juce::File& file, int segs, double durationSec)
        {
            std::vector<ChordSpec> out;
            std::vector<Ev> evs;
            if (segs <= 0 || durationSec <= 0.0 || ! runChordino (file, evs))
                return out;

            const double beatDur = durationSec / segs;
            for (int b = 0; b < segs; ++b)
            {
                const double sampleT = (b + 0.5) * beatDur;   // beat centre
                // Last event at or before this beat centre.
                int idx = -1;
                for (int e = 0; e < (int) evs.size(); ++e)
                    if (evs[(std::size_t) e].t <= sampleT + 1.0e-6) idx = e; else break;
                if (idx < 0) idx = 0;
                // Skip "N" (no-chord) segments: reuse the nearest sounding chord.
                int use = idx;
                while (use >= 0 && ! evs[(std::size_t) use].has) --use;
                if (use < 0) { use = idx; while (use < (int) evs.size() && ! evs[(std::size_t) use].has) ++use; }
                if (use < 0 || use >= (int) evs.size() || ! evs[(std::size_t) use].has)
                    out.push_back (ChordSpec {});   // truly no chord anywhere → placeholder
                else
                    out.push_back (evs[(std::size_t) use].spec);
            }
            return out;
        }
    }

    KeyRootEvidence detectKeyRootEvidence (const juce::File& file)
    {
        KeyRootEvidence result;
        const auto bin = locateChordino();
        if (! bin.existsAsFile() || ! file.existsAsFile())
            return result;

        juce::ChildProcess process;
        juce::StringArray args { bin.getFullPathName(), "--key-features", file.getFullPathName() };
        if (! process.start (args, juce::ChildProcess::wantStdOut))
            return result;

        const auto output = process.readAllProcessOutput();
        if (! process.waitForProcessToFinish (60000))
            return result;

        using Chroma = std::array<double, 12>;
        std::vector<Chroma> chromaFrames, bassFrames;
        Chroma chroma {}, bass {}, low {}, mid {}, high {};

        for (auto line : juce::StringArray::fromLines (output))
        {
            const auto colon = line.indexOfChar (':');
            if (colon < 0) continue;
            juce::StringArray values;
            values.addTokens (line.substring (colon + 1), " \t", "");
            values.removeEmptyStrings();
            if (values.size() != 108) continue;

            Chroma frame {}, bassFrame {};
            double frameSum = 0.0, bassSum = 0.0, spectrumSum = 0.0;
            for (int i = 0; i < 24; ++i)
            {
                const auto pitchClass = static_cast<std::size_t> ((i + 9) % 12);
                const auto value = juce::jmax (0.0, values[i].getDoubleValue());
                auto& target = i < 12 ? bassFrame : frame;
                target[pitchClass] = value;
                if (i < 12) bassSum += value; else frameSum += value;
            }
            for (int i = 0; i < 84; ++i)
                spectrumSum += juce::jmax (0.0, values[24 + i].getDoubleValue());
            if (frameSum <= 1.0e-9 || bassSum <= 1.0e-9 || spectrumSum <= 1.0e-9)
                continue;

            for (int pitch = 0; pitch < 12; ++pitch)
            {
                frame[static_cast<std::size_t> (pitch)] /= frameSum;
                bassFrame[static_cast<std::size_t> (pitch)] /= bassSum;
                chroma[static_cast<std::size_t> (pitch)] += frame[static_cast<std::size_t> (pitch)];
                bass[static_cast<std::size_t> (pitch)] += bassFrame[static_cast<std::size_t> (pitch)];
            }
            chromaFrames.push_back (frame);
            bassFrames.push_back (bassFrame);

            for (int i = 0; i < 84; ++i)
            {
                const auto value = juce::jmax (0.0, values[24 + i].getDoubleValue()) / spectrumSum;
                const auto midi = 21 + i; // NNLS semitone bin zero is A0.
                auto& band = midi <= 47 ? low : midi <= 71 ? mid : high;
                band[static_cast<std::size_t> (midi % 12)] += value;
            }
        }

        if (chromaFrames.empty())
            return result;

        const auto normalize = [] (Chroma& values)
        {
            double sum = 0.0;
            for (const auto value : values) sum += value;
            if (sum > 1.0e-9)
                for (auto& value : values) value /= sum;
        };
        normalize (chroma);
        normalize (bass);
        normalize (low);
        normalize (mid);
        normalize (high);

        using Scores = std::array<double, 24>;
        const auto score = [] (const Chroma& values)
        {
            static const double major[12] = { 0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400 };
            static const double minor[12] = { 0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330 };
            double valueMean = 0.0;
            for (const auto value : values) valueMean += value;
            valueMean /= 12.0;

            const auto correlate = [&] (const double* profile, int rotation)
            {
                double profileMean = 0.0;
                for (int i = 0; i < 12; ++i) profileMean += profile[i];
                profileMean /= 12.0;
                double numerator = 0.0, valueEnergy = 0.0, profileEnergy = 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    const auto value = values[static_cast<std::size_t> ((i + rotation) % 12)] - valueMean;
                    const auto profileValue = profile[i] - profileMean;
                    numerator += value * profileValue;
                    valueEnergy += value * value;
                    profileEnergy += profileValue * profileValue;
                }
                return valueEnergy > 0.0 && profileEnergy > 0.0
                     ? numerator / std::sqrt (valueEnergy * profileEnergy) : 0.0;
            };

            Scores scores {};
            for (int root = 0; root < 12; ++root)
            {
                scores[static_cast<std::size_t> (root)] = correlate (major, root);
                scores[static_cast<std::size_t> (12 + root)] = correlate (minor, root);
            }
            return scores;
        };
        const auto bestRoot = [] (const Scores& scores)
        {
            const auto best = static_cast<int> (std::distance (scores.begin(),
                                                               std::max_element (scores.begin(), scores.end())));
            return best % 12;
        };
        const auto temporalRoot = [&] (const Chroma& global, const std::vector<Chroma>& frames)
        {
            constexpr int windowFrames = 64;
            if (frames.size() < static_cast<std::size_t> (windowFrames))
                return bestRoot (score (global));

            Scores votes {};
            for (int start = 0; start + windowFrames <= static_cast<int> (frames.size()); start += windowFrames / 2)
            {
                Chroma window {};
                for (int frameIndex = 0; frameIndex < windowFrames; ++frameIndex)
                    for (int pitch = 0; pitch < 12; ++pitch)
                        window[static_cast<std::size_t> (pitch)] += frames[static_cast<std::size_t> (start + frameIndex)][static_cast<std::size_t> (pitch)];
                const auto windowScores = score (window);
                const auto best = static_cast<int> (std::distance (windowScores.begin(),
                                                                   std::max_element (windowScores.begin(), windowScores.end())));
                votes[static_cast<std::size_t> (best)] += juce::jmax (0.05, windowScores[static_cast<std::size_t> (best)]);
            }
            const auto globalScores = score (global);
            const auto globalBest = static_cast<int> (std::distance (globalScores.begin(),
                                                                     std::max_element (globalScores.begin(), globalScores.end())));
            votes[static_cast<std::size_t> (globalBest)] += juce::jmax (0.05, globalScores[static_cast<std::size_t> (globalBest)]);
            return bestRoot (votes);
        };
        const auto normalizeScores = [] (Scores scores)
        {
            double mean = 0.0;
            for (const auto value : scores) mean += value;
            mean /= static_cast<double> (scores.size());
            double variance = 0.0;
            for (const auto value : scores) variance += (value - mean) * (value - mean);
            const auto stddev = std::sqrt (variance / static_cast<double> (scores.size()));
            if (stddev > 1.0e-9)
                for (auto& value : scores) value = (value - mean) / stddev;
            return scores;
        };

        const auto lowScores = normalizeScores (score (low));
        const auto midScores = normalizeScores (score (mid));
        const auto highScores = normalizeScores (score (high));
        Scores octaveScores {};
        for (std::size_t i = 0; i < octaveScores.size(); ++i)
            octaveScores[i] = midScores[i] + 0.35 * lowScores[i] + 0.10 * highScores[i];

        result.nnlsRoot = temporalRoot (chroma, chromaFrames);
        result.bassRoot = temporalRoot (bass, bassFrames);
        result.octaveRoot = bestRoot (octaveScores);
        return result;
    }

    std::vector<ChordSpec> detectBarChords (const juce::AudioBuffer<float>& mono, double sampleRate, const Options& opts)
    {
        std::vector<ChordSpec> out;
        const int n = mono.getNumSamples();
        if (n < 2048 || sampleRate <= 0.0 || mono.getNumChannels() < 1)
            return out;

        // Per-BEAT chroma + bass (short beat windows padded to ~0.35 s so chroma stays stable).
        const int bars = juce::jmax (1, opts.numBars);
        const int bpb  = juce::jlimit (1, 16, opts.beatsPerBar);
        const int segs = bars * bpb;
        const auto* x = mono.getReadPointer (0);
        const int minWin = static_cast<int> (sampleRate * 0.35);
        std::vector<std::array<double, 12>> chromas, basses;
        for (int s = 0; s < segs; ++s)
        {
            const int start = static_cast<int> (static_cast<int64_t> (n) * s / segs);
            const int endBeat = static_cast<int> (static_cast<int64_t> (n) * (s + 1) / segs);
            const int len = juce::jmin (n - start, juce::jmax (endBeat - start, minWin));
            if (len <= 0) break;
            chromas.push_back (chromaOfSlice (x + start, len, sampleRate));
            basses.push_back  (bassChromaOfSlice (x + start, len, sampleRate));
        }
        const int T = static_cast<int> (chromas.size());
        if (T == 0) return out;

        // States = every (root, template). HMM: emission = chord fit; a fixed cost to CHANGE chord
        // enforces temporal continuity (Viterbi), so the path stays on a chord instead of flickering
        // through spurious matches — this is what a proper DAW chord detector does.
        const auto& tpls = templates();
        const int nT = static_cast<int> (tpls.size());
        const int S  = 12 * nT;
        const auto tmplOf = [&] (int st) -> const Template& { return tpls[static_cast<std::size_t> (st % nT)]; };
        const auto rootOf = [&] (int st) { return st / nT; };

        std::vector<std::vector<double>> dp (static_cast<std::size_t> (T), std::vector<double> (static_cast<std::size_t> (S)));
        std::vector<std::vector<int>>    back (static_cast<std::size_t> (T), std::vector<int> (static_cast<std::size_t> (S), 0));
        for (int st = 0; st < S; ++st)
            dp[0][static_cast<std::size_t> (st)] = emissionScore (chromas[0], basses[0], tmplOf (st), rootOf (st), opts.keyRoot, opts.keyMinor);

        const double changePenalty = 0.10;   // higher = fewer chord changes (steadier), lower = more
        for (int t = 1; t < T; ++t)
        {
            // Transition is uniform (0 to stay, -penalty to switch), so each state's best predecessor is
            // either itself or the globally best previous state — O(S) instead of O(S^2).
            double bestPrev = -1.0e18; int bestPrevIdx = 0;
            for (int p = 0; p < S; ++p)
                if (dp[static_cast<std::size_t> (t - 1)][static_cast<std::size_t> (p)] > bestPrev)
                { bestPrev = dp[static_cast<std::size_t> (t - 1)][static_cast<std::size_t> (p)]; bestPrevIdx = p; }

            for (int st = 0; st < S; ++st)
            {
                const double stay = dp[static_cast<std::size_t> (t - 1)][static_cast<std::size_t> (st)];
                const double chg  = bestPrev - changePenalty;
                const double em   = emissionScore (chromas[static_cast<std::size_t> (t)], basses[static_cast<std::size_t> (t)],
                                                    tmplOf (st), rootOf (st), opts.keyRoot, opts.keyMinor);
                if (stay >= chg) { dp[static_cast<std::size_t> (t)][static_cast<std::size_t> (st)] = stay + em; back[static_cast<std::size_t> (t)][static_cast<std::size_t> (st)] = st; }
                else             { dp[static_cast<std::size_t> (t)][static_cast<std::size_t> (st)] = chg  + em; back[static_cast<std::size_t> (t)][static_cast<std::size_t> (st)] = bestPrevIdx; }
            }
        }

        int last = 0; double bv = -1.0e18;
        for (int st = 0; st < S; ++st)
            if (dp[static_cast<std::size_t> (T - 1)][static_cast<std::size_t> (st)] > bv)
            { bv = dp[static_cast<std::size_t> (T - 1)][static_cast<std::size_t> (st)]; last = st; }

        std::vector<int> path (static_cast<std::size_t> (T));
        path[static_cast<std::size_t> (T - 1)] = last;
        for (int t = T - 1; t > 0; --t)
            path[static_cast<std::size_t> (t - 1)] = back[static_cast<std::size_t> (t)][static_cast<std::size_t> (path[static_cast<std::size_t> (t)])];

        for (int t = 0; t < T; ++t)
        {
            const int st = path[static_cast<std::size_t> (t)];
            const auto& tp = tmplOf (st);
            ChordSpec spec { rootOf (st), tp.quality, tp.extensions };

            // Power chord: third nearly silent → "5" (root+fifth) instead of an assumed maj/min.
            const int q3 = (spec.quality == Quality::major || spec.quality == Quality::aug) ? 4
                         : (spec.quality == Quality::minor || spec.quality == Quality::dim) ? 3 : -1;
            if (q3 >= 0)
            {
                const int wr = spec.rootPc;
                const double rootE  = chromas[static_cast<std::size_t> (t)][static_cast<std::size_t> (wr)];
                const double fifthE = chromas[static_cast<std::size_t> (t)][static_cast<std::size_t> ((wr + 7) % 12)];
                const double thirdE = chromas[static_cast<std::size_t> (t)][static_cast<std::size_t> ((wr + q3) % 12)];
                if (thirdE < 0.40 * juce::jmax (rootE, fifthE))
                    spec = ChordSpec { wr, Quality::power, orion::chords::ext_none };
            }
            out.push_back (spec);
        }
        return out;
    }

    std::vector<ChordSpec> detectBarChords (const juce::File& file, const Options& opts)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return {};

        // Primary: bundled Chordino helper (matches Logic-level quality). Falls through to the native
        // template detector below if the helper is unavailable or produced nothing.
        {
            const double durationSec = reader->lengthInSamples / reader->sampleRate;
            const int bars = juce::jmax (1, opts.numBars);
            const int bpb  = juce::jlimit (1, 16, opts.beatsPerBar);
            auto viaChordino = detectViaChordino (file, bars * bpb, durationSec);
            if (! viaChordino.empty())
                return viaChordino;
        }

        const int total = static_cast<int> (juce::jmin<juce::int64> (reader->lengthInSamples, 48000 * 60));
        juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), total);
        reader->read (&buffer, 0, total, 0, true, true);

        // Downmix to mono.
        juce::AudioBuffer<float> mono (1, total);
        mono.clear();
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            mono.addFrom (0, 0, buffer, ch, 0, total, 1.0f / static_cast<float> (buffer.getNumChannels()));

        return detectBarChords (mono, reader->sampleRate, opts);
    }

    std::vector<TimedChord> detectTimedChords (const juce::File& file, const Options& opts)
    {
        std::vector<TimedChord> out;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
            return out;

        const double durationSec = reader->lengthInSamples / reader->sampleRate;
        const int    bars = juce::jmax (1, opts.numBars);
        const int    bpb  = juce::jlimit (1, 16, opts.beatsPerBar);
        // Warp-accurate seconds→beats: use the clip's real length in beats when known, so the mapping
        // matches how the (stretched) audio actually sits on the grid instead of a rounded bar count.
        const double totalBeats  = opts.clipLengthBeats > 0.0 ? opts.clipLengthBeats
                                                              : static_cast<double> (bars * bpb);
        const double beatsPerSec = totalBeats / durationSec;
        const auto   same = [] (const ChordSpec& a, const ChordSpec& b)
        { return a.rootPc == b.rootPc && a.quality == b.quality && a.extensions == b.extensions && a.bassPc == b.bassPc; };

        // Primary: Chordino's change points, mapped to beats in the SAME coordinates the (warped) audio
        // sits on the grid — so boundaries land exactly where the sample changes ("доля в долю"). We
        // keep the real sub-beat position and only quantise to a fine 1/16 grid to shave off Chordino's
        // ~0.12-beat frame jitter (snapping to whole beats would drag off-beat changes early/late).
        const auto snap16 = [] (double b) { return std::round (b * 4.0) * 0.25; };   // nearest 1/16 note
        std::vector<Ev> evs;
        if (runChordino (file, evs))
        {
            for (std::size_t i = 0; i < evs.size(); ++i)
            {
                if (! evs[i].has) continue;
                double startB = snap16 (juce::jmax (0.0, evs[i].t * beatsPerSec));
                // Segment runs until the next event; skip past following no-chord ("N") events.
                std::size_t j = i + 1;
                double endSec = (j < evs.size()) ? evs[j].t : durationSec;
                while (j < evs.size() && ! evs[j].has) { ++j; endSec = (j < evs.size()) ? evs[j].t : durationSec; }
                double endB = snap16 (juce::jmin (totalBeats, endSec * beatsPerSec));

                if (endB <= startB) continue;                 // collapsed by snapping (glitch-short) → drop

                if (! out.empty() && same (out.back().spec, evs[i].spec)
                    && std::abs ((out.back().startBeat + out.back().lengthInBeats) - startB) < 1.0e-6)
                    out.back().lengthInBeats = endB - out.back().startBeat;   // merge identical neighbour
                else if (! out.empty() && startB < out.back().startBeat + out.back().lengthInBeats + 1.0e-6)
                {
                    // Overlap after snapping: clip the previous block so blocks stay contiguous, no pile-up.
                    out.back().lengthInBeats = juce::jmax (0.0, startB - out.back().startBeat);
                    if (out.back().lengthInBeats < 1.0e-6) out.pop_back();
                    out.push_back ({ startB, endB - startB, evs[i].spec });
                }
                else
                    out.push_back ({ startB, endB - startB, evs[i].spec });
            }
            // Drop any zero-length leftovers.
            out.erase (std::remove_if (out.begin(), out.end(),
                       [] (const TimedChord& t) { return t.lengthInBeats < 1.0e-6; }), out.end());
            if (! out.empty())
                return out;
        }

        // Fallback: native per-beat detector → merge equal consecutive beats into integer-beat segments.
        const auto perBeat = detectBarChords (file, opts);
        for (std::size_t i = 0; i < perBeat.size();)
        {
            std::size_t j = i + 1;
            while (j < perBeat.size() && same (perBeat[j], perBeat[i])) ++j;
            out.push_back ({ static_cast<double> (i), static_cast<double> (j - i), perBeat[i] });
            i = j;
        }
        return out;
    }

    std::vector<ChordChange> detectChordChanges (const juce::File& file, const Options&)
    {
        std::vector<ChordChange> out;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
            return out;
        const double dur = reader->lengthInSamples / reader->sampleRate;

        std::vector<Ev> evs;
        if (! runChordino (file, evs))
            return out;

        const auto same = [] (const ChordSpec& a, const ChordSpec& b)
        { return a.rootPc == b.rootPc && a.quality == b.quality && a.extensions == b.extensions && a.bassPc == b.bassPc; };

        // Emit one change per NEW chord (skip "N" and consecutive duplicates), positioned as a fraction
        // of the full source file — the host maps this onto the timeline exactly like the waveform.
        for (const auto& e : evs)
        {
            if (! e.has) continue;
            if (! out.empty() && same (out.back().spec, e.spec)) continue;
            out.push_back ({ juce::jlimit (0.0, 1.0, e.t / dur), e.spec });
        }
        return out;
    }
}
