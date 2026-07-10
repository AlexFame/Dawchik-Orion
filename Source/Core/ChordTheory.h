#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <vector>

namespace orion::chords
{
    // Base triad/dyad quality. "power" = 5th (root+fifth), "bass" = root only.
    enum class Quality
    {
        major, minor, aug, dim, sus2, sus4, power, bass
    };

    // Optional colour tones stacked above the triad. Stored as a bitmask so a chord can carry
    // several at once (e.g. a dominant 9 = dom7 + add9). Values 0..11 semitones map to alterations;
    // the higher tensions are expressed relative to the root across the octave.
    enum Extension : juce::uint32
    {
        ext_none   = 0,
        ext_flat5  = 1u << 0,   // ♭5  — lowers the fifth to 6 semitones
        ext_sharp5 = 1u << 1,   // ♯5  — raises the fifth to 8 semitones
        ext_6      = 1u << 2,   // add 6
        ext_7      = 1u << 3,   // dominant 7
        ext_maj7   = 1u << 4,   // major 7
        ext_9      = 1u << 5,   // add 9  (the "9 | 2")
        ext_flat9  = 1u << 6,   // ♭9
        ext_sharp9 = 1u << 7,   // ♯9
        ext_11     = 1u << 8,   // add 11
        ext_sharp11= 1u << 9,   // ♯11
        ext_13     = 1u << 10,  // add 13
        ext_flat13 = 1u << 11,  // ♭13
    };

    struct ChordSpec
    {
        int rootPc { 0 };                 // 0..11 pitch class (C=0)
        Quality quality { Quality::major };
        juce::uint32 extensions { ext_none };
        // Slash-chord bass pitch class (0..11), or -1 for root position. Kept LAST so existing
        // aggregate initialisers { root, quality, ext } stay valid.
        int bassPc { -1 };
    };

    inline bool hasExt (juce::uint32 mask, Extension e) noexcept { return (mask & e) != 0; }

    // Semitone offsets from the root, ascending, de-duplicated. Root is always 0.
    inline std::vector<int> intervals (const ChordSpec& c)
    {
        std::vector<int> out;
        out.push_back (0);   // root

        // Triad body.
        int third = -1, fifth = 7;
        switch (c.quality)
        {
            case Quality::major: third = 4; fifth = 7; break;
            case Quality::minor: third = 3; fifth = 7; break;
            case Quality::aug:   third = 4; fifth = 8; break;
            case Quality::dim:   third = 3; fifth = 6; break;
            case Quality::sus2:  third = 2; fifth = 7; break;
            case Quality::sus4:  third = 5; fifth = 7; break;
            case Quality::power: third = -1; fifth = 7; break;   // root + fifth only
            case Quality::bass:  third = -1; fifth = -1; break;  // root only
        }
        if (third >= 0) out.push_back (third);

        // Fifth alterations override the triad fifth.
        if (hasExt (c.extensions, ext_flat5))       fifth = 6;
        else if (hasExt (c.extensions, ext_sharp5))  fifth = 8;
        if (fifth >= 0) out.push_back (fifth);

        // Sevenths / sixths.
        if (hasExt (c.extensions, ext_6))    out.push_back (9);
        if (hasExt (c.extensions, ext_maj7)) out.push_back (11);
        else if (hasExt (c.extensions, ext_7)) out.push_back (10);

        // Upper tensions (across the octave).
        if (hasExt (c.extensions, ext_flat9))  out.push_back (13);
        if (hasExt (c.extensions, ext_9))      out.push_back (14);
        if (hasExt (c.extensions, ext_sharp9)) out.push_back (15);
        if (hasExt (c.extensions, ext_11))     out.push_back (17);
        if (hasExt (c.extensions, ext_sharp11))out.push_back (18);
        if (hasExt (c.extensions, ext_flat13)) out.push_back (20);
        if (hasExt (c.extensions, ext_13))     out.push_back (21);

        std::sort (out.begin(), out.end());
        out.erase (std::unique (out.begin(), out.end()), out.end());
        return out;
    }

    inline const char* rootName (int pc)
    {
        static constexpr std::array<const char*, 12> names {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return names[static_cast<std::size_t> (((pc % 12) + 12) % 12)];
    }

    // Compact chord label, e.g. "Dm", "Gmaj7", "Asus4".
    inline juce::String chordName (const ChordSpec& c)
    {
        juce::String s (rootName (c.rootPc));
        switch (c.quality)
        {
            case Quality::major: break;
            case Quality::minor: s << "m"; break;
            case Quality::aug:   s << "aug"; break;
            case Quality::dim:   s << "dim"; break;
            case Quality::sus2:  s << "sus2"; break;
            case Quality::sus4:  s << "sus4"; break;
            case Quality::power: s << "5"; break;
            case Quality::bass:  s << " (bass)"; break;
        }
        if (hasExt (c.extensions, ext_maj7)) s << "maj7";
        else if (hasExt (c.extensions, ext_7)) s << "7";
        if (hasExt (c.extensions, ext_6))    s << "6";
        if (hasExt (c.extensions, ext_9))    s << "9";
        if (hasExt (c.extensions, ext_11))   s << "11";
        if (hasExt (c.extensions, ext_13))   s << "13";
        if (hasExt (c.extensions, ext_flat5))  s << "b5";
        if (hasExt (c.extensions, ext_sharp5)) s << "#5";
        if (hasExt (c.extensions, ext_flat9))  s << "b9";
        if (hasExt (c.extensions, ext_sharp9)) s << "#9";
        if (hasExt (c.extensions, ext_sharp11))s << "#11";
        if (hasExt (c.extensions, ext_flat13)) s << "b13";
        if (c.bassPc >= 0 && (c.bassPc % 12) != (((c.rootPc % 12) + 12) % 12))
            s << "/" << rootName (c.bassPc);
        return s;
    }

    // Actual MIDI pitches for a chord, voiced from a base octave (default root near C3).
    inline std::vector<int> pitches (const ChordSpec& c, int rootMidi = 48)
    {
        const int base = rootMidi - (rootMidi % 12) + (((c.rootPc % 12) + 12) % 12);
        std::vector<int> out;
        for (int iv : intervals (c))
            out.push_back (juce::jlimit (0, 127, base + iv));
        if (c.bassPc >= 0)
        {
            const int bp = ((c.bassPc % 12) + 12) % 12;
            int bassMidi = (rootMidi - (rootMidi % 12)) - 12 + bp;
            if (bassMidi >= base) bassMidi -= 12;
            out.insert (out.begin(), juce::jlimit (0, 127, bassMidi));
        }
        return out;
    }

    // Key-aware voicing: the "plain" tensions (6/7/9/11/13) follow the project scale so a click on
    // "7" turns a diatonic chord into the seventh chord that actually belongs to the key (Imaj7, V7,
    // iim7, …) instead of a fixed dominant seventh. Explicit alterations (maj7/b5/#5/b9/#9/#11/b13)
    // still override. Falls back to the fixed intervals() offsets when the chord root is out of key.
    inline std::vector<int> pitchesInKey (const ChordSpec& c, int keyRootPc,
                                          const std::array<int, 7>& pattern, int rootMidi = 48)
    {
        const int rootPc = ((c.rootPc % 12) + 12) % 12;
        keyRootPc = ((keyRootPc % 12) + 12) % 12;

        // Locate the chord root as a scale degree of the key.
        int deg = -1;
        for (int i = 0; i < 7; ++i)
            if (((keyRootPc + pattern[static_cast<std::size_t> (i)]) % 12 + 12) % 12 == rootPc) { deg = i; break; }

        if (deg < 0)
            return pitches (c, rootMidi);   // chromatic root → fixed offsets

        // Semitone offset (from the chord root) of the scale tone `stepsUp` diatonic steps above it.
        auto tone = [&] (int stepsUp)
        {
            const int idx = deg + stepsUp;
            const int oct = idx / 7;
            return pattern[static_cast<std::size_t> (idx % 7)] + 12 * oct - pattern[static_cast<std::size_t> (deg)];
        };

        std::vector<int> iv;
        iv.push_back (0);

        int third = -1, fifth = 7;
        switch (c.quality)
        {
            case Quality::major: third = 4; fifth = 7; break;
            case Quality::minor: third = 3; fifth = 7; break;
            case Quality::aug:   third = 4; fifth = 8; break;
            case Quality::dim:   third = 3; fifth = 6; break;
            case Quality::sus2:  third = 2; fifth = 7; break;
            case Quality::sus4:  third = 5; fifth = 7; break;
            case Quality::power: third = -1; fifth = 7; break;
            case Quality::bass:  third = -1; fifth = -1; break;
        }
        if (third >= 0) iv.push_back (third);
        if (hasExt (c.extensions, ext_flat5))       fifth = 6;
        else if (hasExt (c.extensions, ext_sharp5))  fifth = 8;
        if (fifth >= 0) iv.push_back (fifth);

        if (hasExt (c.extensions, ext_6))    iv.push_back (tone (5));           // diatonic 6th
        if (hasExt (c.extensions, ext_maj7)) iv.push_back (11);                 // explicit major 7
        else if (hasExt (c.extensions, ext_7)) iv.push_back (tone (6));         // diatonic 7th

        if (hasExt (c.extensions, ext_flat9))  iv.push_back (13);
        if (hasExt (c.extensions, ext_9))      iv.push_back (tone (8));         // diatonic 9th
        if (hasExt (c.extensions, ext_sharp9)) iv.push_back (15);
        if (hasExt (c.extensions, ext_11))     iv.push_back (tone (10));        // diatonic 11th
        if (hasExt (c.extensions, ext_sharp11))iv.push_back (18);
        if (hasExt (c.extensions, ext_flat13)) iv.push_back (20);
        if (hasExt (c.extensions, ext_13))     iv.push_back (tone (12));        // diatonic 13th

        std::sort (iv.begin(), iv.end());
        iv.erase (std::unique (iv.begin(), iv.end()), iv.end());

        const int base = rootMidi - (rootMidi % 12) + rootPc;
        std::vector<int> out;
        for (int s : iv)
            out.push_back (juce::jlimit (0, 127, base + s));

        // Slash-chord bass: place the bass pitch class an octave below the chord body.
        if (c.bassPc >= 0)
        {
            const int bp = ((c.bassPc % 12) + 12) % 12;
            int bassMidi = (base - rootPc) - 12 + bp;      // pc bp in the octave below the root's C
            if (bassMidi >= base) bassMidi -= 12;
            out.insert (out.begin(), juce::jlimit (0, 127, bassMidi));
        }
        return out;
    }

    // The seven diatonic triads of a key, built by stacking scale thirds. `pattern` is the scale's
    // 7 pitch classes relative to its root (as in the editor's scalePatterns).
    inline std::array<ChordSpec, 7> diatonicTriads (int keyRootPc, const std::array<int, 7>& pattern)
    {
        std::array<ChordSpec, 7> out {};
        for (int i = 0; i < 7; ++i)
        {
            const int rootPc = ((keyRootPc + pattern[static_cast<std::size_t> (i)]) % 12 + 12) % 12;
            const int third = ((pattern[static_cast<std::size_t> ((i + 2) % 7)] - pattern[static_cast<std::size_t> (i)]) % 12 + 12) % 12;
            const int fifth = ((pattern[static_cast<std::size_t> ((i + 4) % 7)] - pattern[static_cast<std::size_t> (i)]) % 12 + 12) % 12;

            Quality q = Quality::major;
            if      (third == 4 && fifth == 7) q = Quality::major;
            else if (third == 3 && fifth == 7) q = Quality::minor;
            else if (third == 3 && fifth == 6) q = Quality::dim;
            else if (third == 4 && fifth == 8) q = Quality::aug;
            else if (third == 3)               q = Quality::minor;
            else                               q = Quality::major;

            out[static_cast<std::size_t> (i)] = ChordSpec { rootPc, q, ext_none };
        }
        return out;
    }
}
