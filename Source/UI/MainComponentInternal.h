#pragma once

// Shared palette + layout constants for MainComponent's translation units.
//
// These used to live in an anonymous namespace inside MainComponent.cpp. Once that file was
// split up (MainComponentWiring.cpp and friends), several units needed the same values, so they
// moved here rather than being duplicated. Internal to the MainComponent implementation — not
// part of any public interface.

#include "OrionTheme.h"
#include "TransportBarComponent.h"

#include <set>

namespace orion
{
// Pure MIDI/chord display helpers, shared by MainComponent's translation units (the live-MIDI
// routing lives in MainComponentMidi.cpp, the rest in MainComponent.cpp). Stateless — safe as
// inline free functions.
inline juce::String midiNoteName(int midiNote)
{
    static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto note = juce::jlimit(0, 127, midiNote);
    return juce::String(names[note % 12]) + juce::String(note / 12 - 1);
}

inline juce::String pitchClassName(int pitchClass)
{
    static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[(pitchClass % 12 + 12) % 12];
}

inline bool containsInterval(const std::set<int>& pitchClasses, int root, int interval)
{
    return pitchClasses.count((root + interval) % 12) > 0;
}

inline juce::String chordNameForPitchClasses(const std::set<int>& pitchClasses)
{
    if (pitchClasses.size() < 2)
        return {};

    for (const auto root : pitchClasses)
    {
        const bool min3 = containsInterval(pitchClasses, root, 3);
        const bool maj3 = containsInterval(pitchClasses, root, 4);
        const bool p5   = containsInterval(pitchClasses, root, 7);
        const bool dim5 = containsInterval(pitchClasses, root, 6);
        const bool aug5 = containsInterval(pitchClasses, root, 8);
        const bool min7 = containsInterval(pitchClasses, root, 10);
        const bool maj7 = containsInterval(pitchClasses, root, 11);
        const bool sus2 = containsInterval(pitchClasses, root, 2);
        const bool sus4 = containsInterval(pitchClasses, root, 5);

        if (min3 && dim5)  return pitchClassName(root) + (min7 ? "m7b5" : "dim");
        if (maj3 && aug5)  return pitchClassName(root) + "aug";
        if (min3 && p5)    return pitchClassName(root) + (min7 ? "m7" : (maj7 ? "mMaj7" : "m"));
        if (maj3 && p5)    return pitchClassName(root) + (min7 ? "7" : (maj7 ? "maj7" : ""));
        if (sus4 && p5)    return pitchClassName(root) + (min7 ? "7sus4" : "sus4");
        if (sus2 && p5)    return pitchClassName(root) + "sus2";
    }
    return {};
}

inline juce::String liveMidiDisplayText(const std::set<int>& notes)
{
    if (notes.empty())
        return "No MIDI";
    if (notes.size() == 1)
        return midiNoteName(*notes.begin());

    std::set<int> pitchClasses;
    for (const auto note : notes)
        pitchClasses.insert(note % 12);

    if (const auto chord = chordNameForPitchClasses(pitchClasses); chord.isNotEmpty())
        return chord;

    juce::StringArray parts;
    for (const auto pitchClass : pitchClasses)
        parts.add(pitchClassName(pitchClass));
    return parts.joinIntoString("+");
}

// Stable key for an MPC CC (channel + controller) used by the command-learn map.
inline int mpcCcKey(int channel, int controller) noexcept
{
    return juce::jlimit(1, 16, channel) * 128 + juce::jlimit(0, 127, controller);
}

inline const auto backgroundColour       = theme::core::canvas;
inline const auto panelColour            = theme::core::studio;
inline const auto accentColour           = theme::accent::activeCoral;
inline const auto panelStroke            = theme::line::subtle;
inline const auto mutedText              = theme::text::muted;
inline const auto transportShelfColour   = theme::core::voidBlack;
inline const auto transportShelfStroke   = theme::line::subtle;
inline const auto transportButtonColour  = theme::core::studio;
inline const auto transportButtonText    = theme::text::secondary;
inline const auto transportDarkPanel     = theme::core::voidBlack;
inline const auto transportSectionFill   = theme::core::canvas;
inline const auto transportSectionStroke = theme::line::subtle.withAlpha(0.45f);
inline const auto recordAccent           = theme::status::error;

inline constexpr int minBrowserPanelWidth = 220;
inline constexpr int maxBrowserPanelWidth = 520;
inline constexpr int browserResizeHandleWidth = 10;
inline constexpr int transportShelfHeight = TransportBarComponent::preferredHeight;
// The transport panel is already vertically centred inside the shelf with an 8 px inset.
// Keep the workspace flush to the shelf so the panel has the same visual air above and below.
inline constexpr int workspaceTopGap = 0;
inline constexpr int transportBrandWidth = 210;
inline constexpr int transportClusterWidth = 264;
inline constexpr int transportTempoWidth = 178;   // BPM + KEY combined card
inline constexpr int transportModeWidth = 152;
inline constexpr int transportUtilityWidth = 302;
inline constexpr int transportSectionGap = 12;
inline constexpr int transportControlHeight = 46;
inline constexpr int transportSectionHeight = 54;
inline constexpr int transportContentVerticalNudge = 0;
inline constexpr int samplerPanelHeight = 350;   // shared by the sampler and clip editor
inline constexpr const char* sidebarFoldersSettingsKey = "sidebar.customFolders";
} // namespace orion
