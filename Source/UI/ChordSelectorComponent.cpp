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

juce::Rectangle<int> ChordSelectorComponent::keyLabelBounds() const
{
    // Right side of the header, left of the close button — holds the clickable "Key: …" text.
    return headerBounds().withTrimmedRight (40).removeFromRight (180);
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
    // Clickable key label (opens the project-key menu) — brighter + a ▾ so it reads as a control.
    g.setColour (theme::text::primary.withAlpha (0.78f));
    g.drawText ("Key: " + keyName + "  " + juce::String::fromUTF8 ("\xE2\x96\xBE"),
                keyLabelBounds(), juce::Justification::centredRight);

    // Camera toggle: point at chords with a fingertip.
    {
        const auto camB = cameraButtonBounds().toFloat();
        g.setColour (cameraOn ? theme::cool::cyan.withAlpha (0.90f)
                              : theme::surface::elevated.withAlpha (0.85f));
        g.fillRoundedRectangle (camB, 6.0f);
        g.setColour ((cameraOn ? theme::cool::cyan : juce::Colours::white).withAlpha (0.35f));
        g.drawRoundedRectangle (camB.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (cameraOn ? juce::Colours::black.withAlpha (0.9f) : theme::text::primary.withAlpha (0.85f));
        g.setFont (juce::Font (12.0f, juce::Font::bold));
        g.drawText (cameraOpening ? "Cam…" : (cameraOn ? "Cam \xE2\x97\x8F" : "\xF0\x9F\x93\xB7 Cam"),
                    camB, juce::Justification::centred);
    }

    const auto cb = closeButtonBounds();
    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.drawLine (cb.getX() + 9.0f, cb.getY() + 9.0f, cb.getRight() - 9.0f, cb.getBottom() - 9.0f, 1.6f);
    g.drawLine (cb.getRight() - 9.0f, cb.getY() + 9.0f, cb.getX() + 9.0f, cb.getBottom() - 9.0f, 1.6f);

    // Circle of Fifths: outer ring = 12 major chords by fifths, inner ring = their relative minors,
    // tonic (the key) in the centre, roman numerals on the chords diatonic to the current key.
    {
        const auto w = wheelBounds().toFloat();
        const auto cx = w.getCentreX(), cy = w.getCentreY();
        const auto rOuter = w.getWidth() * 0.5f;
        const auto rMid   = rOuter * 0.66f;   // boundary between the major (outer) and minor (inner) rings
        const auto rHub   = rOuter * 0.34f;   // centre hub (tonic)

        // Roman-numeral labels for the chords diatonic to the current key, keyed by (root, isMinorRing).
        std::array<juce::String, 12> outerRoman {}, innerRoman {};
        {
            const auto tri = diatonicTriads (keyRootPc, keyPattern);
            static constexpr std::array<const char*, 7> base { "I", "II", "III", "IV", "V", "VI", "VII" };
            for (int i = 0; i < 7; ++i)
            {
                const auto& d = tri[static_cast<std::size_t> (i)];
                const bool minorish = d.quality == Quality::minor || d.quality == Quality::dim;
                juce::String num (base[static_cast<std::size_t> (i)]);
                if (minorish) num = num.toLowerCase();
                if (d.quality == Quality::dim) num += juce::String::fromUTF8 ("\xC2\xB0");
                (minorish ? innerRoman : outerRoman)[static_cast<std::size_t> ((d.rootPc % 12 + 12) % 12)] = num;
            }
        }

        const auto drawRing = [&] (bool minor, float rIn, float rOut)
        {
            for (int pos = 0; pos < 12; ++pos)
            {
                const int root = minor ? innerRootAt (pos) : outerRootAt (pos);
                const float a0 = juce::degreesToRadians (pos * 30.0f - 15.0f);
                const float a1 = juce::degreesToRadians (pos * 30.0f + 15.0f);
                juce::Path seg;
                seg.addPieSegment (cx - rOut, cy - rOut, rOut * 2.0f, rOut * 2.0f, a0, a1, rIn / rOut);

                const auto& roman = (minor ? innerRoman : outerRoman)[static_cast<std::size_t> (root)];
                const bool diatonic = roman.isNotEmpty();
                const bool selected = root == spec.rootPc
                                   && ((spec.quality == Quality::minor) == minor);
                const bool hover    = root == hoverPc && minor == hoverMinor;

                juce::Colour fill = minor ? theme::surface::elevated : theme::surface::panel.brighter (0.03f);
                if (diatonic) fill = fill.brighter (0.12f);
                if (hover)    fill = fill.brighter (0.20f);
                if (selected) fill = theme::cool::cyan.withAlpha (0.92f);
                g.setColour (fill);
                g.fillPath (seg);
                g.setColour (juce::Colours::black.withAlpha (0.38f));
                g.strokePath (seg, juce::PathStrokeType (1.0f));

                const float theta = juce::degreesToRadians (pos * 30.0f);
                const float rLabel = (rIn + rOut) * 0.5f;
                const float lx = cx + rLabel * std::sin (theta);
                const float ly = cy - rLabel * std::cos (theta);
                const auto name = juce::String (rootName (root)) + (minor ? "m" : "");
                g.setColour (selected ? juce::Colours::black.withAlpha (0.95f)
                                      : theme::text::primary.withAlpha (diatonic ? 0.98f : 0.62f));
                g.setFont (juce::Font (minor ? 15.0f : 17.5f, selected || diatonic ? juce::Font::bold : juce::Font::plain));
                g.drawText (name, juce::Rectangle<float> (lx - 22, ly - (diatonic ? 13 : 10), 44, 20), juce::Justification::centred);
                if (diatonic)
                {
                    g.setColour ((selected ? juce::Colours::black : theme::cool::cyan).withAlpha (0.85f));
                    g.setFont (juce::Font (11.5f, juce::Font::bold));
                    g.drawText (roman, juce::Rectangle<float> (lx - 22, ly + 5, 44, 14), juce::Justification::centred);
                }
            }
        };

        drawRing (false, rMid, rOuter);   // outer: major
        drawRing (true,  rHub, rMid);     // inner: relative minor

        // Hub: the tonic / current key.
        g.setColour (theme::core::studio);
        g.fillEllipse (cx - rHub, cy - rHub, rHub * 2.0f, rHub * 2.0f);
        g.setColour (theme::cool::cyan.withAlpha (0.30f));
        g.drawEllipse (cx - rHub, cy - rHub, rHub * 2.0f, rHub * 2.0f, 1.0f);
        g.setColour (theme::text::primary.withAlpha (0.95f));
        g.setFont (juce::Font (24.0f, juce::Font::bold));
        g.drawText (juce::String (rootName (keyRootPc)) + (keyPattern[2] == 3 ? "m" : ""),
                    juce::Rectangle<float> (cx - rHub, cy - 20, rHub * 2, 26), juce::Justification::centred);
        g.setColour (theme::text::primary.withAlpha (0.55f));
        g.setFont (juce::Font (12.0f, juce::Font::bold));
        g.drawText ("I", juce::Rectangle<float> (cx - rHub, cy + 8, rHub * 2, 14), juce::Justification::centred);
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

void ChordSelectorComponent::resized()
{
    if (camOverlay != nullptr)
        camOverlay->setBounds (getLocalBounds());
}

// Fast top layer: only the camera cursor + self-view (see CamOverlay). Never touches the wheel.
void ChordSelectorComponent::paintCameraOverlay (juce::Graphics& g)
{
    // Camera overlay: on-wheel cursor + a live self-view so the user sees where the finger points.
    if (cameraOn)
    {
        // Fingertip cursor on the wheel (crosshair + ring).
        if (cursorValid)
        {
            const auto c = cursorPos;
            g.setColour (theme::accent::activeCoral.withAlpha (0.95f));
            g.drawEllipse (c.x - 11.0f, c.y - 11.0f, 22.0f, 22.0f, 2.4f);
            g.fillEllipse (c.x - 3.0f, c.y - 3.0f, 6.0f, 6.0f);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawLine (c.x - 15.0f, c.y, c.x - 6.0f, c.y, 1.4f);
            g.drawLine (c.x + 6.0f, c.y, c.x + 15.0f, c.y, 1.4f);
            g.drawLine (c.x, c.y - 15.0f, c.x, c.y - 6.0f, 1.4f);
            g.drawLine (c.x, c.y + 6.0f, c.x, c.y + 15.0f, 1.4f);
        }

        // Self-view thumbnail (mirrored) in the wheel's top-left corner + a dot on the fingertip.
        const auto wb = wheelBounds();
        juce::Rectangle<float> preview (static_cast<float> (wb.getX()), static_cast<float> (wb.getY()), 150.0f, 112.0f);
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.fillRoundedRectangle (preview, 8.0f);
        {
            const juce::ScopedLock lock (frameLock);
            if (latestFrame.isValid())
            {
                juce::Graphics::ScopedSaveState ss (g);
                juce::Path clip; clip.addRoundedRectangle (preview.reduced (2.0f), 7.0f);
                g.reduceClipRegion (clip);
                // Mirror horizontally so it reads like a mirror.
                g.addTransform (juce::AffineTransform::scale (-1.0f, 1.0f, preview.getCentreX(), preview.getCentreY()));
                g.drawImage (latestFrame, preview.reduced (2.0f), juce::RectanglePlacement::fillDestination);
            }
        }
        if (haveTip)
        {
            const float dx = preview.getX() + (1.0f - previewTip.x) * preview.getWidth();   // preview is mirrored
            const float dy = preview.getY() + previewTip.y * preview.getHeight();
            g.setColour (theme::accent::activeCoral);
            g.fillEllipse (dx - 4.0f, dy - 4.0f, 8.0f, 8.0f);
            g.setColour (juce::Colours::white);
            g.drawEllipse (dx - 4.0f, dy - 4.0f, 8.0f, 8.0f, 1.2f);
        }
        g.setColour (theme::cool::cyan.withAlpha (0.5f));
        g.drawRoundedRectangle (preview.reduced (0.5f), 8.0f, 1.0f);
    }
}

//========================================================================== input
ChordSelectorComponent::WheelChord ChordSelectorComponent::wheelChordAtPoint (juce::Point<int> p) const
{
    const auto w = wheelBounds().toFloat();
    const auto cx = w.getCentreX(), cy = w.getCentreY();
    const auto rOuter = w.getWidth() * 0.5f;
    const auto rMid   = rOuter * 0.66f;
    const auto rHub   = rOuter * 0.34f;
    const auto dx = p.x - cx, dy = p.y - cy;
    const auto dist = std::sqrt (dx * dx + dy * dy);
    if (dist < rHub || dist > rOuter)
        return {};   // hub or outside the rings
    auto angle = std::atan2 (dx, -dy);
    if (angle < 0) angle += juce::MathConstants<float>::twoPi;
    const int pos = static_cast<int> (std::round (angle / juce::MathConstants<float>::twoPi * 12.0f)) % 12;
    const bool minor = dist < rMid;   // inner ring
    return WheelChord { minor ? innerRootAt (pos) : outerRootAt (pos), minor };
}

void ChordSelectorComponent::mouseDown (const juce::MouseEvent& e)
{
    dragStarted = false;
    movingPanel = false;
    const auto p = e.getPosition();

    if (closeButtonBounds().contains (p)) { if (onClose) onClose(); return; }

    if (cameraButtonBounds().contains (p)) { toggleCamera(); return; }

    // Clicking the key label opens the project-key menu (host wires it to the transport key menu).
    if (keyLabelBounds().contains (p))
    {
        if (onRequestKeyMenu) onRequestKeyMenu (localAreaToGlobal (keyLabelBounds()));
        return;
    }

    // Dragging the header moves the whole panel (the header is the title bar / move handle).
    if (headerBounds().contains (p))
    {
        movingPanel = true;
        panelDragger.startDraggingComponent (this, e);
        return;
    }

    if (const auto wc = wheelChordAtPoint (p); wc.isValid())
    {
        spec.rootPc  = wc.rootPc;
        spec.quality = wc.minor ? Quality::minor : Quality::major;   // ring picks major/minor
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
    if (movingPanel)   // dragging the header moves the panel
    {
        panelDragger.dragComponent (this, e, nullptr);
        return;
    }

    // Drag a chord out of the wheel / keyboard / diatonic row to drop it on the grid.
    if (dragStarted || onDragChordOut == nullptr)
        return;
    const auto down = e.getMouseDownPosition();
    const bool fromDragZone = wheelBounds().contains (down)
                           || keyboardBounds().contains (down)
                           || diatonicRowBounds().contains (down);
    if (fromDragZone && e.getDistanceFromDragStart() > 8)
    {
        dragStarted = true;
        onDragChordOut (currentPitches());
    }
}

void ChordSelectorComponent::mouseMove (const juce::MouseEvent& e)
{
    const auto wc = wheelChordAtPoint (e.getPosition());
    if (wc.rootPc != hoverPc || wc.minor != hoverMinor)
    {
        hoverPc    = wc.rootPc;
        hoverMinor = wc.minor;
        repaint (wheelBounds());
    }
}

void ChordSelectorComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoverPc != -1 && ! cameraOn)   // don't fight the camera's own pointing
    {
        hoverPc = -1;
        repaint (wheelBounds());
    }
}

// ---- Camera (finger-pointing) mode ----------------------------------------------------------

ChordSelectorComponent::~ChordSelectorComponent() { stopCamera(); }

juce::Rectangle<int> ChordSelectorComponent::cameraButtonBounds() const
{
    // A pill in the header, just left of the "Key: …" label.
    auto h = headerBounds();
    return juce::Rectangle<int> (h.getRight() - 220 - 8 - 66, h.getY() + 7, 66, 26);
}

void ChordSelectorComponent::toggleCamera()
{
    if (cameraOn || cameraOpening) stopCamera();
    else                          startCamera();
    repaint();
}

void ChordSelectorComponent::startCamera()
{
    if (cameraOn || cameraOpening) return;
    if (juce::CameraDevice::getAvailableDevices().isEmpty()) return;
    cameraOpening = true;
    juce::Component::SafePointer<ChordSelectorComponent> safeThis (this);
    juce::CameraDevice::openDeviceAsync (0,
        [safeThis] (juce::CameraDevice* device, const juce::String& error)
        {
            if (safeThis != nullptr) safeThis->cameraOpened (device, error);
            else delete device;
        }, 640, 480, 1280, 720, false);
}

void ChordSelectorComponent::cameraOpened (juce::CameraDevice* device, const juce::String& /*error*/)
{
    cameraOpening = false;
    camera.reset (device);
    if (camera == nullptr) return;
    camera->addListener (this);
    cameraOn = true;
    if (camOverlay == nullptr)
    {
        camOverlay = std::make_unique<CamOverlay> (*this);
        addAndMakeVisible (*camOverlay);
    }
    camOverlay->setBounds (getLocalBounds());
    camOverlay->setVisible (true);
    camOverlay->toFront (false);
    worker = std::make_unique<FrameWorker> (*this);
    worker->startThread();
    repaint();
}

void ChordSelectorComponent::stopCamera()
{
    if (worker != nullptr) { worker->stopThread (500); worker.reset(); }
    if (camera != nullptr) { camera->removeListener (this); camera->stopRecording(); camera.reset(); }
    cameraOn = false;
    cameraOpening = false;
    pointHoverPc = -1;
    haveTip = false;
    cursorValid = false;
    if (camOverlay != nullptr) camOverlay->setVisible (false);
    if (hoverPc != -1) hoverPc = -1;
    repaint();
}

void ChordSelectorComponent::imageReceived (const juce::Image& image)
{
    const juce::ScopedLock lock (frameLock);
    latestFrame = image.createCopy();   // one owning copy; the worker shares this ref
}

void ChordSelectorComponent::detectionLoop (juce::Thread& self)
{
    juce::Component::SafePointer<ChordSelectorComponent> safeThis (this);
    while (! self.threadShouldExit())
    {
        juce::Image frame;
        { const juce::ScopedLock lock (frameLock); frame = latestFrame; }
        std::optional<juce::Point<float>> tip;
        if (frame.isValid())
            tip = orion::camera::detectIndexTip (frame);
        juce::MessageManager::callAsync ([safeThis, tip] { if (safeThis != nullptr) safeThis->applyPointing (tip); });
        self.wait (45);   // ~22 Hz
    }
}

void ChordSelectorComponent::applyPointing (const std::optional<juce::Point<float>>& normTip)
{
    if (! cameraOn) return;
    if (! normTip.has_value())
    {
        // Brief hold: a single dropped frame shouldn't blank the cursor (Vision misses now and then).
        if (haveTip && ++missedFrames < 8)
        {
            if (camOverlay != nullptr) camOverlay->repaint();
            return;
        }
        haveTip = false;
        cursorValid = false;
        const bool hadHover = hoverPc != -1;
        hoverPc = -1;
        if (hadHover) repaint (wheelBounds());     // clear the segment highlight (rare)
        if (camOverlay != nullptr) camOverlay->repaint();
        return;
    }
    missedFrames = 0;

    // Map the CENTRE of the camera view to the whole wheel (gain around 0.5) so every chord — even the
    // bottom of the circle — is reachable without pushing the hand to the extreme edge of the frame.
    constexpr float gain = 1.7f;
    const juce::Point<float> t { juce::jlimit (0.0f, 1.0f, 0.5f + (normTip->x - 0.5f) * gain),
                                 juce::jlimit (0.0f, 1.0f, 0.5f + (normTip->y - 0.5f) * gain) };

    // Exponential smoothing: kills the frame-to-frame jitter so the cursor reads cleanly.
    if (! haveTip) { smoothTip = t; previewTip = *normTip; haveTip = true; }
    else           { smoothTip += (t - smoothTip) * 0.5f; previewTip += (*normTip - previewTip) * 0.5f; }

    // Map the (mirrored) fingertip onto the wheel and hit-test a chord segment.
    const auto w = wheelBounds().toFloat();
    const float lx = w.getX() + (1.0f - smoothTip.x) * w.getWidth();
    const float ly = w.getY() + smoothTip.y * w.getHeight();
    cursorPos   = { lx, ly };
    cursorValid = true;
    const auto wc = wheelChordAtPoint ({ juce::roundToInt (lx), juce::roundToInt (ly) });

    if (wc.rootPc != hoverPc || wc.minor != hoverMinor)
    {
        hoverPc    = wc.rootPc;
        hoverMinor = wc.minor;
        repaint (wheelBounds());   // only when the pointed chord changes (heavy) — NOT every frame

        // Audition the pointed chord, throttled so it doesn't retrigger every frame.
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (wc.isValid() && now - lastPointAuditionMs > 220.0 && onAudition)
        {
            lastPointAuditionMs = now;
            ChordSpec probe = spec;
            probe.rootPc  = wc.rootPc;
            probe.quality = wc.minor ? Quality::minor : Quality::major;
            probe.extensions = ext_none;
            onAudition (pitchesInKey (probe, keyRootPc, keyPattern, 48));
        }
    }
    if (camOverlay != nullptr) camOverlay->repaint();   // cursor + preview move (cheap, transparent layer)
}
