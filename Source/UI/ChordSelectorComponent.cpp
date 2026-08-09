#include "ChordSelectorComponent.h"
#include "OrionTheme.h"

using namespace orion;
using namespace orion::chords;

namespace
{
    struct Layout
    {
        juce::Rectangle<int> header, wheel, quality, extension, diatonic, keyboard;
    };

    Layout computeLayout (juce::Rectangle<int> full)
    {
        Layout l;
        auto area = full.reduced (14);
        l.header   = area.removeFromTop (40);
        area.removeFromTop (8);
        l.keyboard = area.removeFromBottom (86);
        area.removeFromBottom (10);
        l.diatonic = area.removeFromBottom (40);
        area.removeFromBottom (12);
        l.wheel    = area.removeFromLeft (area.getHeight());   // square note ring
        area.removeFromLeft (16);
        l.quality  = area.removeFromTop (34);
        area.removeFromTop (10);
        l.extension = area;
        return l;
    }

    struct QualityDef { const char* label; Quality q; };
    constexpr std::array<QualityDef, 8> kQualities { {
        { "Maj", Quality::major }, { "Min", Quality::minor }, { "Aug", Quality::aug },
        { "Dim", Quality::dim },   { "sus4", Quality::sus4 }, { "sus2", Quality::sus2 },
        { "5th", Quality::power }, { "Bass", Quality::bass } } };

    struct ExtDef { const char* label; Extension e; };
    constexpr std::array<ExtDef, 12> kExtensions { {
        { "b5", ext_flat5 }, { "#5", ext_sharp5 }, { "6", ext_6 }, { "7", ext_7 },
        { "maj7", ext_maj7 }, { "b9", ext_flat9 }, { "9", ext_9 }, { "#9", ext_sharp9 },
        { "11", ext_11 }, { "#11", ext_sharp11 }, { "13", ext_13 }, { "b13", ext_flat13 } } };

    void drawPill (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool active, bool hover)
    {
        g.setColour (hover ? theme::surface::panel.brighter (0.10f)
                           : theme::surface::elevated.withAlpha (active ? 0.0f : 1.0f));
        if (active)
            g.setColour (theme::cool::cyan.withAlpha (0.90f));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (active ? theme::cool::cyan : juce::Colours::white.withAlpha (0.16f));
        g.drawRoundedRectangle (r.toFloat(), 6.0f, active ? 1.4f : 1.0f);
        g.setColour (active ? juce::Colours::black.withAlpha (0.92f) : theme::text::primary.withAlpha (0.90f));
        g.setFont (juce::Font (12.5f, juce::Font::bold));
        g.drawText (text, r, juce::Justification::centred);
    }
}

ChordSelectorComponent::ChordSelectorComponent()
{
    setWantsKeyboardFocus (false);
}

void ChordSelectorComponent::setProjectKey (int rootPc, const std::array<int, 7>& pattern, const juce::String& name)
{
    keyRootPc = ((rootPc % 12) + 12) % 12;
    keyPattern = pattern;
    keyName = name;
    // Seed with the tonic (I) chord of the key so opening the selector already offers something useful.
    spec = diatonicTriads (keyRootPc, keyPattern)[0];
    repaint();
}

void ChordSelectorComponent::setChord (const ChordSpec& newSpec, bool audition)
{
    spec = newSpec;
    commitChange (audition);
}

std::vector<int> ChordSelectorComponent::currentPitches() const
{
    return pitchesInKey (spec, keyRootPc, keyPattern, 48);
}

void ChordSelectorComponent::commitChange (bool audition)
{
    if (onChordChanged) onChordChanged (spec);
    if (audition && onAudition) onAudition (currentPitches());
    repaint();
}

//========================================================================== layout
juce::Rectangle<int> ChordSelectorComponent::panelBounds() const     { return getLocalBounds(); }
juce::Rectangle<int> ChordSelectorComponent::headerBounds() const    { return computeLayout (getLocalBounds()).header; }
juce::Rectangle<int> ChordSelectorComponent::wheelBounds() const     { return computeLayout (getLocalBounds()).wheel; }
juce::Rectangle<int> ChordSelectorComponent::qualityRowBounds() const{ return computeLayout (getLocalBounds()).quality; }
juce::Rectangle<int> ChordSelectorComponent::extensionGridBounds() const { return computeLayout (getLocalBounds()).extension; }
juce::Rectangle<int> ChordSelectorComponent::diatonicRowBounds() const   { return computeLayout (getLocalBounds()).diatonic; }
juce::Rectangle<int> ChordSelectorComponent::keyboardBounds() const  { return computeLayout (getLocalBounds()).keyboard; }

juce::Rectangle<int> ChordSelectorComponent::closeButtonBounds() const
{
    auto h = headerBounds();
    return juce::Rectangle<int> (h.getRight() - 30, h.getY() + 6, 28, 28);
}

std::array<juce::Rectangle<int>, 8> ChordSelectorComponent::qualityRects() const
{
    std::array<juce::Rectangle<int>, 8> out;
    auto row = qualityRowBounds();
    constexpr int gap = 6;
    const int w = (row.getWidth() - gap * 7) / 8;
    for (int i = 0; i < 8; ++i)
        out[static_cast<std::size_t> (i)] = juce::Rectangle<int> (row.getX() + i * (w + gap), row.getY(), w, row.getHeight());
    return out;
}

std::array<juce::Rectangle<int>, 12> ChordSelectorComponent::extensionRects() const
{
    std::array<juce::Rectangle<int>, 12> out;
    auto grid = extensionGridBounds();
    constexpr int cols = 6, rows = 2, gap = 6;
    const int w = (grid.getWidth() - gap * (cols - 1)) / cols;
    const int h = (grid.getHeight() - gap * (rows - 1)) / rows;
    for (int i = 0; i < 12; ++i)
    {
        const int c = i % cols, r = i / cols;
        out[static_cast<std::size_t> (i)] = juce::Rectangle<int> (grid.getX() + c * (w + gap),
                                                                  grid.getY() + r * (h + gap), w, h);
    }
    return out;
}

std::array<juce::Rectangle<int>, 7> ChordSelectorComponent::diatonicRects() const
{
    std::array<juce::Rectangle<int>, 7> out;
    auto row = diatonicRowBounds();
    constexpr int gap = 6;
    const int w = (row.getWidth() - gap * 6) / 7;
    for (int i = 0; i < 7; ++i)
        out[static_cast<std::size_t> (i)] = juce::Rectangle<int> (row.getX() + i * (w + gap), row.getY(), w, row.getHeight());
    return out;
}

//========================================================================== paint
void ChordSelectorComponent::paint (juce::Graphics& g)
{
    // Panel shell.
    g.setColour (theme::core::studio);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::metrics::panelRadius);
    g.setColour (theme::cool::cyan.withAlpha (0.28f));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.6f), theme::metrics::panelRadius, 1.2f);

    // Header: chord name + key context + close.
    const auto header = headerBounds();
    g.setColour (theme::text::primary);
    g.setFont (juce::Font (24.0f, juce::Font::bold));
    g.drawText (chordName (spec), header.withTrimmedRight (40), juce::Justification::centredLeft);
    g.setFont (juce::Font (14.0f, juce::Font::plain));
    g.setColour (theme::text::primary.withAlpha (0.55f));
    g.drawText ("Key: " + keyName, header.withTrimmedRight (40), juce::Justification::centredRight);

    const auto cb = closeButtonBounds();
    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.drawLine (cb.getX() + 9.0f, cb.getY() + 9.0f, cb.getRight() - 9.0f, cb.getBottom() - 9.0f, 1.6f);
    g.drawLine (cb.getRight() - 9.0f, cb.getY() + 9.0f, cb.getX() + 9.0f, cb.getBottom() - 9.0f, 1.6f);

    // Note wheel.
    {
        const auto w = wheelBounds().toFloat();
        const auto cx = w.getCentreX(), cy = w.getCentreY();
        const auto outer = w.getWidth() * 0.5f;
        const auto inner = outer * 0.46f;
        const auto midR = (outer + inner) * 0.5f;

        std::array<bool, 12> inKey { {} };
        for (int i = 0; i < 7; ++i)
            inKey[static_cast<std::size_t> (((keyRootPc + keyPattern[static_cast<std::size_t> (i)]) % 12 + 12) % 12)] = true;

        for (int pc = 0; pc < 12; ++pc)
        {
            const float a0 = juce::degreesToRadians (pc * 30.0f - 15.0f);
            const float a1 = juce::degreesToRadians (pc * 30.0f + 15.0f);
            juce::Path seg;
            seg.addPieSegment (cx - outer, cy - outer, outer * 2.0f, outer * 2.0f, a0, a1, inner / outer);

            const bool selected = pc == spec.rootPc;
            const bool hover = pc == hoverPc;
            juce::Colour fill = theme::surface::elevated;
            if (inKey[static_cast<std::size_t> (pc)]) fill = theme::surface::panel.brighter (0.05f);
            if (hover)    fill = fill.brighter (0.18f);
            if (selected) fill = theme::cool::cyan.withAlpha (0.92f);
            g.setColour (fill);
            g.fillPath (seg);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.strokePath (seg, juce::PathStrokeType (1.0f));

            const float theta = juce::degreesToRadians (pc * 30.0f);
            const float lx = cx + midR * std::sin (theta);
            const float ly = cy - midR * std::cos (theta);
            g.setColour (selected ? juce::Colours::black.withAlpha (0.92f)
                                  : theme::text::primary.withAlpha (inKey[static_cast<std::size_t> (pc)] ? 0.95f : 0.5f));
            g.setFont (juce::Font (13.0f, selected || inKey[static_cast<std::size_t> (pc)] ? juce::Font::bold : juce::Font::plain));
            g.drawText (rootName (pc), juce::Rectangle<float> (lx - 14, ly - 10, 28, 20), juce::Justification::centred);
        }
        // Hub label.
        g.setColour (theme::text::primary.withAlpha (0.85f));
        g.setFont (juce::Font (15.0f, juce::Font::bold));
        g.drawText (rootName (spec.rootPc), juce::Rectangle<float> (cx - inner, cy - 12, inner * 2, 24), juce::Justification::centred);
    }

    // Quality row.
    {
        const auto rects = qualityRects();
        for (std::size_t i = 0; i < kQualities.size(); ++i)
            drawPill (g, rects[i], kQualities[i].label, spec.quality == kQualities[i].q, false);
    }

    // Extension grid.
    {
        const auto rects = extensionRects();
        for (std::size_t i = 0; i < kExtensions.size(); ++i)
            drawPill (g, rects[i], kExtensions[i].label, hasExt (spec.extensions, kExtensions[i].e), false);
    }

    // Diatonic suggestion row.
    {
        const auto tri = diatonicTriads (keyRootPc, keyPattern);
        const auto rects = diatonicRects();
        static constexpr std::array<const char*, 7> roman { "I", "ii", "iii", "IV", "V", "vi", "vii" };
        for (int i = 0; i < 7; ++i)
        {
            const auto& d = tri[static_cast<std::size_t> (i)];
            const bool active = d.rootPc == spec.rootPc && d.quality == spec.quality && spec.extensions == ext_none;
            auto r = rects[static_cast<std::size_t> (i)];
            drawPill (g, r, chordName (d), active, false);
            g.setColour ((active ? juce::Colours::black : theme::text::primary).withAlpha (0.5f));
            g.setFont (juce::Font (9.0f, juce::Font::plain));
            g.drawText (roman[static_cast<std::size_t> (i)], r.removeFromTop (11), juce::Justification::centred);
        }
    }

    // Keyboard preview: two octaves from C3, chord tones lit.
    {
        const auto kb = keyboardBounds();
        g.setColour (theme::core::canvas);
        g.fillRoundedRectangle (kb.toFloat(), 6.0f);

        const int lowC = 48;                    // C3
        const int span = 24;                    // two octaves
        std::array<bool, 128> lit { {} };
        for (int p : currentPitches())
            if (p >= 0 && p < 128) lit[static_cast<std::size_t> (p)] = true;

        static constexpr std::array<int, 7> whiteSemis { { 0, 2, 4, 5, 7, 9, 11 } };
        const int whiteCount = 14;              // 2 octaves
        const float wKeyW = kb.getWidth() / static_cast<float> (whiteCount);
        const float wKeyH = static_cast<float> (kb.getHeight());

        // White keys.
        int wi = 0;
        for (int oct = 0; oct < 2; ++oct)
            for (int s = 0; s < 7; ++s, ++wi)
            {
                const int pitch = lowC + oct * 12 + whiteSemis[static_cast<std::size_t> (s)];
                juce::Rectangle<float> key (kb.getX() + wi * wKeyW, kb.getY(), wKeyW - 1.0f, wKeyH);
                g.setColour (lit[static_cast<std::size_t> (pitch)] ? theme::cool::cyan : juce::Colour (0xfff2f4f6));
                g.fillRoundedRectangle (key, 2.0f);
                g.setColour (juce::Colours::black.withAlpha (0.25f));
                g.drawRoundedRectangle (key, 2.0f, 0.8f);
            }

        // Black keys (over the gaps after C,D,F,G,A).
        static constexpr std::array<int, 5> blackAfterWhite { { 0, 1, 3, 4, 5 } };
        static constexpr std::array<int, 5> blackSemis { { 1, 3, 6, 8, 10 } };
        for (int oct = 0; oct < 2; ++oct)
            for (int b = 0; b < 5; ++b)
            {
                const int pitch = lowC + oct * 12 + blackSemis[static_cast<std::size_t> (b)];
                const int whiteIndex = oct * 7 + blackAfterWhite[static_cast<std::size_t> (b)];
                juce::Rectangle<float> key (kb.getX() + (whiteIndex + 1) * wKeyW - wKeyW * 0.32f,
                                            kb.getY(), wKeyW * 0.64f, wKeyH * 0.62f);
                g.setColour (lit[static_cast<std::size_t> (pitch)] ? theme::cool::cyan.darker (0.15f) : juce::Colour (0xff1a1f27));
                g.fillRoundedRectangle (key, 2.0f);
            }
    }
}

//========================================================================== input
int ChordSelectorComponent::wheelPcAtPoint (juce::Point<int> p) const
{
    const auto w = wheelBounds().toFloat();
    const auto cx = w.getCentreX(), cy = w.getCentreY();
    const auto outer = w.getWidth() * 0.5f;
    const auto inner = outer * 0.46f;
    const auto dx = p.x - cx, dy = p.y - cy;
    const auto dist = std::sqrt (dx * dx + dy * dy);
    if (dist < inner || dist > outer)
        return -1;
    auto angle = std::atan2 (dx, -dy);
    if (angle < 0) angle += juce::MathConstants<float>::twoPi;
    return static_cast<int> (std::round (angle / juce::MathConstants<float>::twoPi * 12.0f)) % 12;
}

void ChordSelectorComponent::mouseDown (const juce::MouseEvent& e)
{
    dragStarted = false;
    const auto p = e.getPosition();

    if (closeButtonBounds().contains (p)) { if (onClose) onClose(); return; }

    if (const int pc = wheelPcAtPoint (p); pc >= 0)
    {
        spec.rootPc = pc;
        commitChange (true);
        return;
    }

    {
        const auto rects = qualityRects();
        for (std::size_t i = 0; i < kQualities.size(); ++i)
            if (rects[i].contains (p)) { spec.quality = kQualities[i].q; commitChange (true); return; }
    }

    {
        const auto rects = extensionRects();
        for (std::size_t i = 0; i < kExtensions.size(); ++i)
            if (rects[i].contains (p))
            {
                const auto e2 = kExtensions[i].e;
                const bool wasOn = hasExt (spec.extensions, e2);
                // Mutually exclusive pairs so the chord stays coherent.
                if (e2 == ext_7 || e2 == ext_maj7)   spec.extensions &= ~(ext_7 | ext_maj7);
                if (e2 == ext_flat5 || e2 == ext_sharp5) spec.extensions &= ~(ext_flat5 | ext_sharp5);
                if (wasOn) spec.extensions &= ~e2;
                else       spec.extensions |= e2;
                commitChange (true);
                return;
            }
    }

    {
        const auto tri = diatonicTriads (keyRootPc, keyPattern);
        const auto rects = diatonicRects();
        for (int i = 0; i < 7; ++i)
            if (rects[static_cast<std::size_t> (i)].contains (p))
            {
                spec = tri[static_cast<std::size_t> (i)];
                commitChange (true);
                return;
            }
    }

    // Clicking the mini-keyboard auditions the current chord (the header/empty areas do not).
    if (keyboardBounds().contains (p))
    {
        if (onAudition) onAudition (currentPitches());
    }
}

void ChordSelectorComponent::mouseDrag (const juce::MouseEvent& e)
{
    // Drag a chord out of the keyboard/header to drop it on the grid.
    if (dragStarted || onDragChordOut == nullptr)
        return;
    const auto down = e.getMouseDownPosition();
    const bool fromDragZone = keyboardBounds().contains (down)
                           || headerBounds().contains (down)
                           || diatonicRowBounds().contains (down);
    if (fromDragZone && e.getDistanceFromDragStart() > 8)
    {
        dragStarted = true;
        onDragChordOut (currentPitches());
    }
}

void ChordSelectorComponent::mouseMove (const juce::MouseEvent& e)
{
    const int next = wheelPcAtPoint (e.getPosition());
    if (next != hoverPc)
    {
        hoverPc = next;
        repaint (wheelBounds());
    }
}

void ChordSelectorComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoverPc != -1)
    {
        hoverPc = -1;
        repaint (wheelBounds());
    }
}
