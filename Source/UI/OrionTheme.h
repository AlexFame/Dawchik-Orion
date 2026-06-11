#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// ORION full color system ("Codex Handoff v1").
//
// All values are sRGB hex from the design spec. The old namespace/token names are
// kept (core/surface/line/warm/cool/text/status) and remapped onto the new system
// so every existing call site shifts to the new palette without edits; new code
// should prefer the explicit `accent`, `states` and `tracks` sections.
//
// Usage rules from the spec:
//  - Track clips are the most saturated layer; the waveform is darker than the body.
//  - Cyan is for brand, focus and selection. Red is reserved for record/warning/live.
//  - Neutrals stay quiet behind the content.
namespace orion::theme
{
// 1. FOUNDATION / NEUTRALS -----------------------------------------------------
namespace core
{
static const juce::Colour canvas     { 0xff0a0f15 };  // Background
static const juce::Colour deepSpace  { 0xff0a0f15 };  // Background (transport shelf)
static const juce::Colour studio     { 0xff121823 };  // Surface 1
static const juce::Colour voidBlack  { 0xff06090d };  // below-background well (panels/insets)
}  // namespace core

namespace surface
{
static const juce::Colour primary  { 0xff121823 };    // Surface 1
static const juce::Colour elevated { 0xff1a2230 };    // Surface 2
static const juce::Colour panel    { 0xff232c3b };    // Surface 3
static const juce::Colour hover    { 0xff2a3544 };    // Pressed / Active step
}  // namespace surface

namespace line
{
static const juce::Colour subtle  { 0xff1f2733 };     // Grid Line
static const juce::Colour normal  { 0xff2f3947 };     // Border / Line
static const juce::Colour soft    { 0xff3e4e61 };     // derived step above border
static const juce::Colour strong  { 0xff52647b };     // derived step above border
}  // namespace line

namespace text
{
static const juce::Colour primary   { 0xffe6edf3 };
static const juce::Colour secondary { 0xffa9b4c2 };
static const juce::Colour tertiary  { 0xff8b97a6 };
static const juce::Colour muted     { 0xff6e7a89 };
static const juce::Colour disabled  { 0xff515e6c };
static const juce::Colour inverse   { 0xff0a0f15 };
}  // namespace text

// 2. SYSTEM ACCENTS --------------------------------------------------------------
namespace accent
{
static const juce::Colour brandCyan    { 0xff0ad4e5 };
static const juce::Colour brandCyanDim { 0xff00a8b8 };
static const juce::Colour focusGlow    { 0xff33f6ff };
static const juce::Colour recordRed    { 0xffff3b4d };
static const juce::Colour recordRedDim { 0xffb61f2e };
static const juce::Colour warningAmber { 0xffffb020 };
static const juce::Colour successGreen { 0xff22c55e };
static const juce::Colour infoBlue     { 0xff4da3ff };
}  // namespace accent

// Legacy warm/cool families, remapped onto the new track palette bases so old call
// sites pick up the new system automatically. Red maps to Record Red per the spec
// ("red is reserved for record, warnings, and live states").
namespace warm
{
static const juce::Colour red      { 0xffff3b4d };    // Record Red
static const juce::Colour coral    { 0xffff5a5a };    // Coral Red base
static const juce::Colour salmon   { 0xffff7b73 };    // Coral Red highlight
static const juce::Colour pink     { 0xfffb7185 };    // Rose Pink base
static const juce::Colour peach    { 0xffffa45a };    // Warm Orange highlight
static const juce::Colour amber    { 0xffffb020 };    // Warning Amber
}  // namespace warm

namespace cool
{
static const juce::Colour cyan      { 0xff0ad4e5 };   // Brand Cyan
static const juce::Colour turquoise { 0xff14b8a6 };   // Teal base
static const juce::Colour aqua      { 0xff22d3ee };   // Aqua Cyan base
static const juce::Colour blue      { 0xff4f8bff };   // Studio Blue base
static const juce::Colour indigo    { 0xff6366f1 };   // Deep Blue base
static const juce::Colour violet    { 0xffa855f7 };   // Violet base
}  // namespace cool

namespace status
{
static const juce::Colour success { 0xff22c55e };
static const juce::Colour warning { 0xffffb020 };
static const juce::Colour error   { 0xffff3b4d };
static const juce::Colour info    { 0xff4da3ff };
static const juce::Colour focus   { 0xff33f6ff };
static const juce::Colour off     { 0xff2f3947 };
}  // namespace status

// 5. UI STATES --------------------------------------------------------------------
namespace states
{
static const juce::Colour selectedBorder   { 0xff00d4e5 };
static const juce::Colour hoverOverlay     { 0x1fffffff };  // white 12%
static const juce::Colour pressed          { 0xff2a3544 };
static const juce::Colour mutedClipOverlay { 0x66000000 };  // black 40%
static const juce::Colour disabled         { 0xff8b97a6 };  // use withAlpha(0.4f)
static const juce::Colour playhead         { 0xffff3b4d };
static const juce::Colour masterMeterRed   { 0xffff3b4d };
}  // namespace states

// 3. FULL TRACK PALETTE (12 colors × 4 variants) ------------------------------------
namespace tracks
{
// One track/clip colour family: body fill is a vertical gradient (top → bottom),
// the waveform inside the clip is drawn in the darker `waveform` variant.
struct Variant
{
    juce::Colour base;          // clip body / track identity
    juce::Colour gradientTop;   // highlight (top of the vertical gradient)
    juce::Colour gradientBottom;// shade (bottom of the vertical gradient)
    juce::Colour waveform;      // darker waveform / inner detail
};

static const Variant palette12[] {
    { juce::Colour(0xffff5a5a), juce::Colour(0xffff7b73), juce::Colour(0xffe33e3e), juce::Colour(0xffb21f1f) },  // 1 Coral Red
    { juce::Colour(0xffff8a3d), juce::Colour(0xffffa45a), juce::Colour(0xffe66f22), juce::Colour(0xffb3450f) },  // 2 Warm Orange
    { juce::Colour(0xffffc23a), juce::Colour(0xffffd866), juce::Colour(0xffe0a21a), juce::Colour(0xffb37a0c) },  // 3 Amber Gold
    { juce::Colour(0xffa6e22e), juce::Colour(0xffbdf15a), juce::Colour(0xff8ecc1e), juce::Colour(0xff5f8b12) },  // 4 Lime Green
    { juce::Colour(0xff34d399), juce::Colour(0xff6ee7b7), juce::Colour(0xff10b981), juce::Colour(0xff0e7c59) },  // 5 Mint Green
    { juce::Colour(0xff14b8a6), juce::Colour(0xff2dd4bf), juce::Colour(0xff0d9488), juce::Colour(0xff0b6e65) },  // 6 Teal
    { juce::Colour(0xff22d3ee), juce::Colour(0xff67e8f9), juce::Colour(0xff0ea5e9), juce::Colour(0xff0b6d99) },  // 7 Aqua Cyan
    { juce::Colour(0xff4f8bff), juce::Colour(0xff79b1ff), juce::Colour(0xff3a6ee5), juce::Colour(0xff1e4db4) },  // 8 Studio Blue
    { juce::Colour(0xff6366f1), juce::Colour(0xff818cf8), juce::Colour(0xff4f46e5), juce::Colour(0xff3730a3) },  // 9 Deep Blue
    { juce::Colour(0xffa855f7), juce::Colour(0xffc084fc), juce::Colour(0xff9333ea), juce::Colour(0xff6b21a8) },  // 10 Violet
    { juce::Colour(0xffec4899), juce::Colour(0xfff472b6), juce::Colour(0xffdb2777), juce::Colour(0xff9d174d) },  // 11 Magenta
    { juce::Colour(0xfffb7185), juce::Colour(0xfffb92a7), juce::Colour(0xffe11d48), juce::Colour(0xffbe123c) },  // 12 Rose Pink
};

constexpr auto paletteSize = static_cast<int>(sizeof(palette12) / sizeof(palette12[0]));

inline juce::Colour colourForIndex(int index) noexcept
{
    return palette12[static_cast<std::size_t>(juce::jlimit(0, paletteSize - 1, index % paletteSize))].base;
}

// Returns the 4-variant family for a clip/track base colour. Exact palette bases get
// the designed gradient/waveform values; any other colour (legacy projects, custom
// colours) gets variants derived from the base so rendering never breaks.
inline Variant variantsFor(juce::Colour base) noexcept
{
    for (const auto& v : palette12)
        if (v.base.getARGB() == base.getARGB())
            return v;

    return Variant {
        base,
        base.brighter(0.18f),
        base.darker(0.18f),
        base.darker(0.55f),
    };
}
}  // namespace tracks
}  // namespace orion::theme
