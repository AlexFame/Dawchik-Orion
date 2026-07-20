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
// Shared geometry tokens. Components may use the small radius for indicators and the panel
// radius for large containers, but should not invent one-off corner values.
namespace metrics
{
constexpr int gridUnit = 4;
constexpr int controlHeight = 28;
constexpr int rowGap = gridUnit;
constexpr float smallRadius = 3.0f;
constexpr float controlRadius = 6.0f;
constexpr float panelRadius = 8.0f;
}  // namespace metrics

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
static const juce::Colour previewSlate { 0xff7182a8 };
static const juce::Colour orangeRed    { 0xffff4500 };
static const juce::Colour brightOrange { 0xffff6b32 };
static const juce::Colour activeCoral  { 0xffff5b68 };
static const juce::Colour activeCoralLight { 0xffff7b83 };
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
    { juce::Colour(0xffff3b30), juce::Colour(0xffff6258), juce::Colour(0xffe52d24), juce::Colour(0xffa91f1a) },  // 1 Red
    { juce::Colour(0xffff2d83), juce::Colour(0xffff5b9d), juce::Colour(0xffe51f70), juce::Colour(0xffa91452) },  // 2 Pink
    { juce::Colour(0xffe126ff), juce::Colour(0xffed63ff), juce::Colour(0xffc51ee3), juce::Colour(0xff83149b) },  // 3 Magenta
    { juce::Colour(0xff8e2de2), juce::Colour(0xffad5af0), juce::Colour(0xff7221bd), juce::Colour(0xff4b147d) },  // 4 Violet
    { juce::Colour(0xff304ffe), juce::Colour(0xff5b72ff), juce::Colour(0xff253fd3), juce::Colour(0xff18288c) },  // 5 Blue
    { juce::Colour(0xff1de9ff), juce::Colour(0xff63f0ff), juce::Colour(0xff12c4dc), juce::Colour(0xff087d91) },  // 6 Cyan
    { juce::Colour(0xff00c7b7), juce::Colour(0xff35dfd1), juce::Colour(0xff00a89b), juce::Colour(0xff006f68) },  // 7 Teal
    { juce::Colour(0xff00d973), juce::Colour(0xff42e59a), juce::Colour(0xff00b85f), juce::Colour(0xff007c41) },  // 8 Green
    { juce::Colour(0xff7be21b), juce::Colour(0xffa1ed4b), juce::Colour(0xff65bd14), juce::Colour(0xff3f790d) },  // 9 Lime
    { juce::Colour(0xffdce51a), juce::Colour(0xffe9ed57), juce::Colour(0xffb9c10e), juce::Colour(0xff777d08) },  // 10 Yellow
    { juce::Colour(0xffff9f1a), juce::Colour(0xffffba55), juce::Colour(0xffe7830d), juce::Colour(0xff9e5808) },  // 11 Orange
    { juce::Colour(0xffffb37a), juce::Colour(0xffffc99f), juce::Colour(0xffe9955d), juce::Colour(0xffa8603d) },  // 12 Peach
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
