#include "ChordWheelComponent.h"
#include "../UI/OrionTheme.h"

#include <cmath>

namespace orion::chordwheel
{
Component::Component()
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void Component::setKey (int rootPc, const std::array<int, 7>& pattern)
{
    model.setKey (rootPc, pattern);
    repaint();
}

void Component::setChord (const chords::ChordSpec& chord)
{
    model.setChord (chord);
    repaint();
}

int Component::pitchClassAt (juce::Point<float> p) const noexcept
{
    const auto area = getLocalBounds().toFloat().reduced (4.0f);
    const auto centre = area.getCentre();
    const auto outer = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    return model.pitchClassAt (p.x, p.y, centre.x, centre.y, outer, outer * 0.45f);
}

void Component::updatePointing (juce::Point<float> p, bool notify)
{
    const int pc = pitchClassAt (p);
    const bool wasInside = pointingInside;
    pointingInside = pc >= 0;

    if (hoveredPc == pc && wasInside == pointingInside)
        return;

    hoveredPc = pc;
    if (pointingInside && notify && onRootPointed)
        onRootPointed (model.chordForRoot (hoveredPc));
    else if (! pointingInside && wasInside && onPointingExited)
        onPointingExited();
    repaint();
}

void Component::setPointingPosition (juce::Point<float> position)
{
    updatePointing (position, true);
}

void Component::clearPointing() noexcept
{
    if (! pointingInside && hoveredPc < 0)
        return;
    hoveredPc = -1;
    pointingInside = false;
    if (onPointingExited)
        onPointingExited();
    repaint();
}

void Component::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat().reduced (4.0f);
    const auto centre = area.getCentre();
    const auto outer = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    const auto inner = outer * 0.45f;
    const auto mid = (outer + inner) * 0.5f;

    std::array<bool, 12> inKey {};
    for (int offset : model.keyPattern())
        inKey[static_cast<std::size_t> ((model.keyRoot() + offset + 120) % 12)] = true;

    for (int pc = 0; pc < 12; ++pc)
    {
        const auto a0 = juce::degreesToRadians (pc * 30.0f - 15.0f);
        const auto a1 = juce::degreesToRadians (pc * 30.0f + 15.0f);
        juce::Path segment;
        segment.addPieSegment (centre.x - outer, centre.y - outer, outer * 2.0f, outer * 2.0f,
                               a0, a1, inner / outer);

        auto fill = inKey[static_cast<std::size_t> (pc)]
                  ? theme::surface::panel.brighter (0.08f)
                  : theme::surface::elevated;
        if (pc == hoveredPc)
            fill = theme::cool::cyan.withAlpha (0.45f);
        if (pc == model.chord().rootPc)
            fill = theme::cool::cyan.withAlpha (0.92f);

        g.setColour (fill);
        g.fillPath (segment);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.strokePath (segment, juce::PathStrokeType (1.0f));

        const auto theta = juce::degreesToRadians (pc * 30.0f);
        const auto labelX = centre.x + mid * std::sin (theta);
        const auto labelY = centre.y - mid * std::cos (theta);
        const bool emphasised = pc == model.chord().rootPc || pc == hoveredPc || inKey[static_cast<std::size_t> (pc)];
        g.setColour ((pc == model.chord().rootPc ? juce::Colours::black : theme::text::primary)
                         .withAlpha (emphasised ? 0.95f : 0.5f));
        g.setFont (juce::Font (13.0f, emphasised ? juce::Font::bold : juce::Font::plain));
        g.drawText (chords::rootName (pc), juce::Rectangle<float> (labelX - 15.0f, labelY - 10.0f, 30.0f, 20.0f),
                    juce::Justification::centred);
    }

    g.setColour (theme::core::canvas.withAlpha (0.96f));
    g.fillEllipse (centre.x - inner, centre.y - inner, inner * 2.0f, inner * 2.0f);
    g.setColour (theme::text::primary.withAlpha (0.85f));
    g.setFont (juce::Font (15.0f, juce::Font::bold));
    g.drawText (chords::rootName (model.chord().rootPc),
                juce::Rectangle<float> (centre.x - inner, centre.y - 12.0f, inner * 2.0f, 24.0f),
                juce::Justification::centred);
}

void Component::mouseMove (const juce::MouseEvent& e) { updatePointing (e.position, true); }

void Component::mouseExit (const juce::MouseEvent&) { clearPointing(); }

void Component::mouseDown (const juce::MouseEvent& e)
{
    const int pc = pitchClassAt (e.position);
    if (pc < 0 || ! onRootSelected)
        return;
    model.setChord (model.chordForRoot (pc));
    onRootSelected (model.chord());
    repaint();
}
}
