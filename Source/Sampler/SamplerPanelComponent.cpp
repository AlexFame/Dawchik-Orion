#include "SamplerPanelComponent.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
const auto panelBackground = juce::Colour(0xff151c23);
const auto deviceHeader = juce::Colour(0xff202832);
const auto waveformBackground = juce::Colour(0xff0e141a);
const auto panelStroke = juce::Colour(0xff31404d);
const auto accentColour = juce::Colour(0xffe8401f);
// Match the mixer's rotary controls so the sampler knobs share one visual style:
// a thin turquoise ring with a short red pointer (theme::cool::turquoise / warm::red).
const auto knobRingColour    = juce::Colour(0xff14b8a6);
const auto knobPointerColour = juce::Colour(0xffff3b4d);
const auto mutedText = juce::Colours::white.withAlpha(0.62f);
const auto controlBackground = juce::Colour(0xff202a33);
constexpr double padFlashDurationMs = 180.0;

// Parse a root entry: a plain MIDI number (0..127) OR a note name like "C3", "F#4",
// "Bb2" (middle C = C3 = 60, matching the Root display). Returns nullopt if unparseable.
std::optional<int> parseRootEntry(const juce::String& raw)
{
    const auto s = raw.trim();
    if (s.isEmpty())
        return std::nullopt;

    // Pure integer → treat as a MIDI note number.
    if (s.containsOnly("+-0123456789"))
    {
        const auto n = s.getIntValue();
        if (n >= 0 && n <= 127)
            return n;
        return std::nullopt;
    }

    static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };  // A B C D E F G
    const auto upper = s.toUpperCase();
    const auto letter = upper[0];
    if (letter < 'A' || letter > 'G')
        return std::nullopt;
    int semitone = semis[letter - 'A'];

    int i = 1;
    if (i < upper.length() && (upper[i] == '#' || upper[i] == 'S')) { semitone += 1; ++i; }
    else if (i < upper.length() && upper[i] == 'B')                 { semitone -= 1; ++i; }

    const auto octaveStr = upper.substring(i).trim();
    if (octaveStr.isEmpty() || ! octaveStr.containsOnly("+-0123456789"))
        return std::nullopt;
    const auto octave = octaveStr.getIntValue();
    const auto note = (octave - 3) * 12 + 60 + semitone;   // C3 = 60
    if (note < 0 || note > 127)
        return std::nullopt;
    return note;
}

juce::Colour slicePadColourFor(int padIndex)
{
    static constexpr std::array<juce::uint32, 16> colours {{
        0xffd9d63b, 0xff18a36a, 0xff0aa68f, 0xff218fd0,
        0xff8cc9d5, 0xff4a94bd, 0xffa9bcc5, 0xff0d72aa,
        0xff36c27d, 0xffd64a12, 0xffd69a13, 0xffb8a51f,
        0xff68e8b6, 0xff58cbd9, 0xff8ccc4e, 0xff6a4fd1,
    }};

    return juce::Colour(colours[static_cast<std::size_t>(juce::jlimit(0, 15, padIndex))]);
}

void drawKeyboardStrip(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const std::array<const char*, 20> topKeys {
        "Q", "2", "W", "3", "E", "R", "5", "T", "6", "Y",
        "7", "U", "I", "9", "O", "0", "P", "[", "]", "+"
    };
    const std::array<const char*, 19> bottomKeys {
        "Z", "S", "X", "D", "C", "V", "G", "B", "H", "N",
        "J", "M", ",", "L", ".", ";", "/", "'", "\\"
    };

    const auto drawRow = [&](juce::Rectangle<int> rowBounds, const auto& keys)
    {
        constexpr int keyWidth = 20;
        constexpr int keyHeight = 17;
        constexpr int gap = 4;
        const auto keyCount = static_cast<int>(keys.size());
        const auto rowWidth = keyCount * keyWidth + (keyCount - 1) * gap;
        auto x = rowBounds.getCentreX() - rowWidth / 2;
        const auto y = rowBounds.getCentreY() - keyHeight / 2;

        for (const auto* key : keys)
        {
            const juce::Rectangle<int> keyBox(x, y, keyWidth, keyHeight);
            g.setColour(controlBackground.withAlpha(0.62f));
            g.fillRoundedRectangle(keyBox.toFloat(), 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.drawRoundedRectangle(keyBox.toFloat().reduced(0.5f), 4.0f, 1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.58f));
            g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
            g.drawText(key, keyBox, juce::Justification::centred);
            x += keyWidth + gap;
        }
    };

    auto rows = bounds.withSizeKeepingCentre(bounds.getWidth(), 42);
    drawRow(rows.removeFromTop(20), topKeys);
    drawRow(rows.removeFromTop(20), bottomKeys);
}

void drawKnob(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title, const juce::String& value, float normalized, bool selected = false, bool hideValue = false)
{
    auto knob = bounds.removeFromTop(58).withSizeKeepingCentre(48, 48).toFloat().reduced(3.0f);
    const auto radius = juce::jmin(knob.getWidth(), knob.getHeight()) * 0.5f;
    const auto centre = knob.getCentre();
    // Same 270° sweep as the mixer's pan knobs (pi*1.25 .. pi*2.75).
    const auto startAngle = juce::MathConstants<float>::pi * 1.25f;
    const auto endAngle   = juce::MathConstants<float>::pi * 2.75f;
    const auto angle = startAngle + juce::jlimit(0.0f, 1.0f, normalized) * (endAngle - startAngle);

    // Ring.
    juce::Path ring;
    ring.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour(knobRingColour.withAlpha(selected ? 1.0f : 0.8f));
    g.strokePath(ring, juce::PathStrokeType(selected ? 2.2f : 1.6f));

    // Pointer (short red line from centre) + centre dot.
    const juce::Point<float> tip(centre.x + radius * 0.86f * std::sin(angle),
                                 centre.y - radius * 0.86f * std::cos(angle));
    g.setColour(knobPointerColour);
    g.drawLine({ centre, tip }, 2.6f);
    g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(centre));

    g.setColour(mutedText);
    g.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    g.drawText(title, bounds.removeFromTop(16), juce::Justification::centred);
    auto valueBand = bounds.removeFromTop(18);
    if (! hideValue)  // hidden while an inline editor is shown over this value
    {
        g.setColour(juce::Colours::white.withAlpha(0.88f));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(value, valueBand, juce::Justification::centred);
    }
}
}

namespace orion
{
SamplerPanelComponent::SamplerPanelComponent()
{
    waveformFormatManager.registerBasicFormats();
    setWantsKeyboardFocus(true);
    setVisible(false);
}

void SamplerPanelComponent::openTrack(TrackState& trackState)
{
    releaseTypingPianoNotes();
    waveViewStart = 0.0f; waveViewSpan = 1.0f;
    activeTrack = &trackState;
    activeTrackIndex = -1;
    setVisible(true);
    toFront(true);
    startTimerHz(90);
    pushSlicePointsToEngine();
    grabKeyboardFocus();
    repaint();
}

void SamplerPanelComponent::openTrackIndex(int trackIndex)
{
    releaseTypingPianoNotes();
    waveViewStart = 0.0f; waveViewSpan = 1.0f;
    activeTrack = nullptr;
    activeTrackIndex = trackIndex;
    setVisible(true);
    toFront(true);
    startTimerHz(90);
    pushSlicePointsToEngine();
    grabKeyboardFocus();
    repaint();
}

void SamplerPanelComponent::closePanel()
{
    // Don't release notes or drop the active track — we keep the sampler "armed"
    // so the laptop keyboard continues to trigger the last selected track even
    // after the visible panel goes away. Notes only get released when another
    // track is opened or when the user explicitly disarms the keyboard.
    setVisible(false);

    if (onClose)
        onClose();
}

void SamplerPanelComponent::disarmKeyboard()
{
    releaseTypingPianoNotes();
    stopTimer();
    activeTrack = nullptr;
    activeTrackIndex = -1;
}

void SamplerPanelComponent::paint(juce::Graphics& g)
{
    auto* track = getActiveTrack();
    auto panel = getPanelBounds();
    g.setColour(panelBackground);
    g.fillRoundedRectangle(panel.toFloat(), 18.0f);
    g.setColour(panelStroke);
    g.drawRoundedRectangle(panel.toFloat(), 18.0f, 1.2f);

    auto header = panel.removeFromTop(58);
    g.setColour(deviceHeader);
    g.fillRoundedRectangle(header.toFloat(), 18.0f);
    g.setColour(panelBackground);
    g.fillRect(header.withTrimmedTop(30));

    auto headerContent = header.reduced(18, 10);
    g.setColour(juce::Colours::white.withAlpha(0.96f));
    g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    auto titleArea = headerContent.removeFromLeft(180);
    g.drawText("ORION SIMPLER", titleArea.removeFromTop(24), juce::Justification::centredLeft);
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    const auto modeName = track != nullptr ? samplerModeName(track->samplerMode).toUpperCase() : juce::String("CLASSIC");
    g.drawText(modeName + " / OCT "
                   + (track != nullptr && track->samplerKeyboardOctaveOffset >= 0 ? "+" : "")
                   + juce::String(track != nullptr ? track->samplerKeyboardOctaveOffset : 0)
                   + " / " + juce::String(track != nullptr ? track->samplerTransposeSemitones : 0) + " ST",
               titleArea,
               juce::Justification::centredLeft);

    const auto sampleName = track != nullptr && track->samplerSourcePath.isNotEmpty()
        ? juce::File(track->samplerSourcePath).getFileName()
        : juce::String("No sample loaded");
    g.setColour(accentColour);
    g.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    g.drawText(sampleName, headerContent.removeFromLeft(headerContent.getWidth() - 120), juce::Justification::centredLeft, true);
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText("ESC CLOSE", headerContent, juce::Justification::centredRight);

    auto content = panel.reduced(18);
    auto leftModeColumn = content.removeFromLeft(112);
    auto rightControls = content.removeFromRight(250);
    content.removeFromRight(16);

    const std::array<std::pair<SamplerPlaybackMode, juce::String>, 3> modes {{
        { SamplerPlaybackMode::classic, "Classic" },
        { SamplerPlaybackMode::oneShot, "One-Shot" },
        { SamplerPlaybackMode::slice, "Slice" },
    }};

    for (std::size_t i = 0; i < modes.size(); ++i)
    {
        const auto& [mode, name] = modes[i];
        auto modeButton = leftModeColumn.removeFromTop(42).reduced(0, 4);
        modeButtonBounds[i] = modeButton;
        const auto active = track != nullptr && track->samplerMode == mode;
        g.setColour(active ? accentColour : controlBackground);
        g.fillRoundedRectangle(modeButton.toFloat(), 8.0f);
        g.setColour(active ? juce::Colours::white : juce::Colours::white.withAlpha(0.72f));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(name, modeButton, juce::Justification::centred);
    }

    samplerWarpButtonBounds = leftModeColumn.removeFromTop(42).reduced(0, 4);
    const auto warpActive = track != nullptr && track->samplerWarpEnabled;
    g.setColour(warpActive ? accentColour : controlBackground);
    g.fillRoundedRectangle(samplerWarpButtonBounds.toFloat(), 8.0f);
    g.setColour(warpActive ? juce::Colours::white : juce::Colours::white.withAlpha(0.72f));
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText("Warp", samplerWarpButtonBounds, juce::Justification::centred);
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(10.0f, juce::Font::plain));
    auto warpInfo = leftModeColumn.removeFromTop(28);
    const auto sourceBpmText = track != nullptr && track->samplerSourceBpm > 0.0
        ? juce::String(track->samplerSourceBpm, 1) + " BPM"
        : juce::String("No BPM");
    g.drawText(sourceBpmText, warpInfo, juce::Justification::centred);

    const bool sliceMode = track != nullptr && track->samplerMode == SamplerPlaybackMode::slice;
    // Slices are triggered from the keyboard (octave switches reach slices beyond the
    // visible range), so the waveform keeps full height in every mode.
    auto waveform = content.removeFromTop(152);
    g.setColour(waveformBackground);
    g.fillRoundedRectangle(waveform.toFloat(), 12.0f);
    g.setColour(panelStroke);
    g.drawRoundedRectangle(waveform.toFloat(), 12.0f, 1.0f);

    auto waveInner = waveform.reduced(18, 18);
    waveformBounds = waveInner;
    drawWaveform(g, waveInner, track);

    if (track != nullptr && track->samplerMode == SamplerPlaybackMode::slice)
    {
        // Transient chop: slice boundaries follow detected points; otherwise equal divisions.
        const bool usePoints = ! track->samplerSlicePoints.empty();
        const auto sliceN = usePoints ? static_cast<int>(track->samplerSlicePoints.size())
                                      : juce::jlimit(1, 64, track->samplerSliceCount);
        const auto w = static_cast<float>(waveInner.getWidth());
        const auto viewStart = waveViewStart;
        const auto viewSpan  = juce::jmax(0.0001f, waveViewSpan);
        const auto startRatioOf = [&](int i) -> float
        {
            return usePoints ? static_cast<float>(track->samplerSlicePoints[static_cast<std::size_t>(i)])
                             : static_cast<float>(i) / static_cast<float>(sliceN);
        };
        const auto endRatioOf = [&](int i) -> float
        {
            if (usePoints)
                return i + 1 < sliceN ? static_cast<float>(track->samplerSlicePoints[static_cast<std::size_t>(i + 1)]) : 1.0f;
            return static_cast<float>(i + 1) / static_cast<float>(sliceN);
        };
        // Map a 0..1 sample ratio to screen x through the zoom window.
        const auto ratioToX = [&](float r) { return waveInner.getX() + ((r - viewStart) / viewSpan) * w; };

        // Clip everything to the waveform box so zoomed-out slices don't draw past it.
        g.saveState();
        g.reduceClipRegion(waveInner);

        const auto topY = static_cast<float>(waveInner.getY());
        const auto botY = static_cast<float>(waveInner.getBottom());

        // Highlight only the currently playing/held slice region (clean, Ableton-like).
        for (int slice = 0; slice < sliceN; ++slice)
        {
            const auto isActiveSlice = (playbackSliceIndex.has_value() && *playbackSliceIndex == slice)
                                       || activeSliceIndices.contains(slice);
            if (! isActiveSlice)
                continue;
            const auto sliceX = ratioToX(startRatioOf(slice));
            const auto sliceEndX = ratioToX(endRatioOf(slice));
            if (sliceEndX < waveInner.getX() || sliceX > waveInner.getRight())
                continue;
            g.setColour(accentColour.withAlpha(0.22f));
            g.fillRect(juce::Rectangle<float>(sliceX, topY, juce::jmax(1.0f, sliceEndX - sliceX), botY - topY));
        }

        // Thin slice markers with a small grab handle at the top (no per-slice numbers).
        for (int slice = 1; slice < sliceN; ++slice)
        {
            const auto sliceX = std::round(ratioToX(startRatioOf(slice)));
            if (sliceX < waveInner.getX() - 1.0f || sliceX > waveInner.getRight() + 1.0f)
                continue;
            const bool boundaryHot = draggingSliceIndex == slice;
            g.setColour(boundaryHot ? accentColour : juce::Colours::white.withAlpha(0.32f));
            g.drawVerticalLine(static_cast<int>(sliceX), topY, botY);

            juce::Path handle;  // small downward triangle tab
            handle.addTriangle(sliceX - 4.0f, topY, sliceX + 4.0f, topY, sliceX, topY + 6.0f);
            g.setColour(boundaryHot ? accentColour : juce::Colours::white.withAlpha(0.6f));
            g.fillPath(handle);
        }
        g.restoreState();

        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.drawRect(waveInner.toFloat(), 1.0f);

        if (playbackSliceIndex.has_value() && playbackSliceDurationMs > 0.0
            && *playbackSliceIndex >= 0 && *playbackSliceIndex < sliceN)
        {
            const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - playbackSliceStartedMs;
            const auto progress = juce::jlimit(0.0, 1.0, elapsedMs / playbackSliceDurationMs);
            const auto sliceStartX = ratioToX(startRatioOf(*playbackSliceIndex));
            const auto sliceEndX   = ratioToX(endRatioOf(*playbackSliceIndex));
            const auto playheadX = juce::jlimit(static_cast<float>(waveInner.getX()),
                                                static_cast<float>(waveInner.getRight()),
                                                sliceStartX + (sliceEndX - sliceStartX) * static_cast<float>(progress));

            g.setColour(juce::Colours::white.withAlpha(0.96f));
            g.drawLine(playheadX,
                       static_cast<float>(waveInner.getY()),
                       playheadX,
                       static_cast<float>(waveInner.getBottom()),
                       2.0f);
            g.setColour(accentColour);
            g.fillEllipse(playheadX - 4.0f, static_cast<float>(waveInner.getY()) - 4.0f, 8.0f, 8.0f);
        }
    }
    else if (track != nullptr && playbackLinearStartedMs.has_value() && playbackLinearDurationMs > 0.0)
    {
        // Classic / One-Shot playback line sweeping the waveform.
        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - *playbackLinearStartedMs;
        const auto progress = juce::jlimit(0.0, 1.0, elapsedMs / playbackLinearDurationMs);
        const auto playheadX = static_cast<float>(waveInner.getX())
            + static_cast<float>(waveInner.getWidth()) * static_cast<float>(progress);

        g.setColour(juce::Colours::white.withAlpha(0.96f));
        g.drawLine(playheadX,
                   static_cast<float>(waveInner.getY()),
                   playheadX,
                   static_cast<float>(waveInner.getBottom()),
                   2.0f);
        g.setColour(accentColour);
        g.fillEllipse(playheadX - 4.0f, static_cast<float>(waveInner.getY()) - 4.0f, 8.0f, 8.0f);
    }

    if (! sliceMode)
    {
        auto keyboardHelp = content.removeFromTop(58).reduced(0, 8);
        drawKeyboardStrip(g, keyboardHelp);
    }

    auto sliceRow = content.removeFromTop(36).withSizeKeepingCentre(272, 30);
    const std::array<int, 4> sliceCounts { 4, 8, 16, 32 };
    for (std::size_t i = 0; i < sliceCounts.size(); ++i)
    {
        auto sliceButton = sliceRow.removeFromLeft(62).reduced(4, 3);
        sliceButtonBounds[i] = sliceButton;
        const auto active = track != nullptr && track->samplerSliceCount == sliceCounts[i];
        g.setColour(active && track->samplerMode == SamplerPlaybackMode::slice ? accentColour : controlBackground);
        g.fillRoundedRectangle(sliceButton.toFloat(), 8.0f);
        g.setColour(active ? juce::Colours::white : juce::Colours::white.withAlpha(0.7f));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(juce::String(sliceCounts[i]), sliceButton, juce::Justification::centred);
        sliceRow.removeFromLeft(4);
    }

    // "Detect transients" button + Sensitivity slider — auto-chop at onsets.
    detectTransientsButtonBounds = {};
    sensSliderBounds = {};
    if (sliceMode)
    {
        auto detRow = content.removeFromTop(32).withSizeKeepingCentre(330, 26);
        auto btn = detRow.removeFromLeft(178);
        detectTransientsButtonBounds = btn;
        const bool onTransients = track != nullptr && ! track->samplerSlicePoints.empty();
        g.setColour(onTransients ? accentColour : controlBackground);
        g.fillRoundedRectangle(btn.toFloat(), 7.0f);
        g.setColour(onTransients ? juce::Colours::white : juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(onTransients ? "TRANSIENTS (" + juce::String(track->samplerSlicePoints.size()) + ")  ✕"
                                : "DETECT TRANSIENTS",
                   btn, juce::Justification::centred);

        detRow.removeFromLeft(10);
        sensSliderBounds = detRow;
        const auto sens = track != nullptr ? track->samplerSliceSensitivity : 0.5;
        g.setColour(controlBackground);
        g.fillRoundedRectangle(sensSliderBounds.toFloat(), 7.0f);
        auto fill = sensSliderBounds.toFloat().reduced(3.0f);
        fill = fill.withWidth(juce::jmax(0.0f, fill.getWidth() * static_cast<float>(sens)));
        g.setColour(accentColour.withAlpha(0.80f));
        g.fillRoundedRectangle(fill, 4.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText("SENS " + juce::String(juce::roundToInt(sens * 100.0)) + "%",
                   sensSliderBounds, juce::Justification::centred);
    }

    // Chop pads were removed: slices are triggered from the computer/MIDI keyboard, and
    // switching octave (←/→) reaches slices beyond the keyboard's current range.

    auto controlGrid = rightControls;
    auto topKnobs = controlGrid.removeFromTop(108);

    const auto transposeVal = track != nullptr ? track->samplerTransposeSemitones : 0;
    const auto rootVal      = track != nullptr ? track->samplerRootMidiNote : 60;
    const auto gainVal      = track != nullptr ? track->volumeDb : 0.0;

    const auto transposeText = (transposeVal > 0 ? "+" : "") + juce::String(transposeVal) + " st";
    const auto rootText      = juce::MidiMessage::getMidiNoteName(rootVal, true, true, 3);
    const auto gainText      = (gainVal > 0.0 ? "+" : "") + juce::String(gainVal, 1) + " dB";

    transposeKnobBounds = topKnobs.removeFromLeft(78);
    drawKnob(g, transposeKnobBounds, "Transpose", transposeText,
             juce::jmap(static_cast<float>(transposeVal), -24.0f, 24.0f, 0.0f, 1.0f),
             selectedKnob == Knob::transpose, editingKnob == Knob::transpose);
    topKnobs.removeFromLeft(6);
    rootKnobBounds = topKnobs.removeFromLeft(78);
    drawKnob(g, rootKnobBounds, "Root", rootText,
             juce::jmap(static_cast<float>(rootVal), 0.0f, 127.0f, 0.0f, 1.0f),
             selectedKnob == Knob::root, editingKnob == Knob::root);
    topKnobs.removeFromLeft(6);
    gainKnobBounds = topKnobs.removeFromLeft(78);
    drawKnob(g, gainKnobBounds, "Gain", gainText,
             juce::jmap(static_cast<float>(gainVal), -60.0f, 6.0f, 0.0f, 1.0f),
             selectedKnob == Knob::gain, editingKnob == Knob::gain);

    controlGrid.removeFromTop(18);
    auto envelope = controlGrid.removeFromTop(104);
    g.setColour(controlBackground);
    g.fillRoundedRectangle(envelope.toFloat(), 12.0f);
    auto envelopeContent = envelope.reduced(14);
    g.setColour(juce::Colours::white.withAlpha(0.9f));
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText("Playback", envelopeContent.removeFromTop(20), juce::Justification::centredLeft);
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    g.drawText("Mode: " + modeName, envelopeContent.removeFromTop(20), juce::Justification::centredLeft);
    g.drawText(track != nullptr && track->samplerMode == SamplerPlaybackMode::classic
                   ? "Voices: Poly / chords"
                   : "Voices: Mono retrigger",
               envelopeContent.removeFromTop(20),
               juce::Justification::centredLeft);
    g.drawText(track != nullptr && track->samplerMode == SamplerPlaybackMode::slice
                   ? "Slices: " + juce::String(track->samplerSliceCount)
                   : "Warp: " + juce::String(track != nullptr && track->samplerWarpEnabled ? "On" : "Off"),
               envelopeContent.removeFromTop(20),
               juce::Justification::centredLeft);
}

void SamplerPanelComponent::ensureWaveformPeaksFor(const juce::String& sourcePath)
{
    if (sourcePath.isEmpty() || waveformPeaks.sourcePath == sourcePath)
        return;

    waveformPeaks = {};
    waveformPeaks.sourcePath = sourcePath;

    const juce::File sourceFile(sourcePath);
    std::unique_ptr<juce::AudioFormatReader> reader(waveformFormatManager.createReaderFor(sourceFile));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return;

    constexpr int bucketCount = 720;
    constexpr int maxReadBlock = 4096;
    const auto totalSamples = reader->lengthInSamples;
    const auto channelCount = static_cast<int>(reader->numChannels);

    waveformPeaks.minValues.assign(bucketCount, 0.0f);
    waveformPeaks.maxValues.assign(bucketCount, 0.0f);

    juce::AudioBuffer<float> block(channelCount, maxReadBlock);
    float globalPeak = 1.0e-6f;

    for (int bucket = 0; bucket < bucketCount; ++bucket)
    {
        const auto bucketStart = (totalSamples * bucket) / bucketCount;
        const auto bucketEnd = (totalSamples * (bucket + 1)) / bucketCount;
        float minValue = 0.0f;
        float maxValue = 0.0f;
        bool hasSamples = false;

        for (auto readPosition = bucketStart; readPosition < bucketEnd; readPosition += maxReadBlock)
        {
            const auto samplesToRead = static_cast<int>(juce::jmin<juce::int64>(maxReadBlock, bucketEnd - readPosition));
            block.clear();
            reader->read(&block, 0, samplesToRead, readPosition, true, true);

            for (int sample = 0; sample < samplesToRead; ++sample)
            {
                float monoSample = 0.0f;

                for (int channel = 0; channel < channelCount; ++channel)
                    monoSample += block.getSample(channel, sample);

                monoSample /= static_cast<float>(channelCount);

                if (! hasSamples)
                {
                    minValue = monoSample;
                    maxValue = monoSample;
                    hasSamples = true;
                }
                else
                {
                    minValue = juce::jmin(minValue, monoSample);
                    maxValue = juce::jmax(maxValue, monoSample);
                }
            }
        }

        waveformPeaks.minValues[static_cast<std::size_t>(bucket)] = minValue;
        waveformPeaks.maxValues[static_cast<std::size_t>(bucket)] = maxValue;
        globalPeak = juce::jmax(globalPeak, std::abs(minValue), std::abs(maxValue));
    }

    for (auto& value : waveformPeaks.minValues)
        value /= globalPeak;

    for (auto& value : waveformPeaks.maxValues)
        value /= globalPeak;
}

void SamplerPanelComponent::drawWaveform(juce::Graphics& g, juce::Rectangle<int> waveInner, const TrackState* track)
{
    if (track != nullptr && track->samplerSourcePath.isNotEmpty())
        ensureWaveformPeaksFor(track->samplerSourcePath);

    if (waveformPeaks.minValues.empty()
        || waveformPeaks.maxValues.empty()
        || waveformPeaks.minValues.size() != waveformPeaks.maxValues.size())
    {
        g.setColour(mutedText.withAlpha(0.45f));
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(track != nullptr && track->samplerSourcePath.isNotEmpty() ? "Waveform unavailable" : "Load a sample",
                   waveInner,
                   juce::Justification::centred);
        return;
    }

    const auto centerY = static_cast<float>(waveInner.getCentreY());
    const auto halfHeight = static_cast<float>(waveInner.getHeight()) * 0.46f;
    const auto bucketCount = static_cast<int>(waveformPeaks.minValues.size());

    g.setColour(accentColour.withAlpha(0.72f));

    const auto viewStart = waveViewStart;
    const auto viewSpan  = juce::jmax(0.0001f, waveViewSpan);
    const auto widthF    = static_cast<float>(juce::jmax(1, waveInner.getWidth()));
    for (int x = 0; x < waveInner.getWidth(); x += 2)
    {
        const auto r0 = viewStart + (static_cast<float>(x)       / widthF) * viewSpan;
        const auto r1 = viewStart + (static_cast<float>(x + 2)   / widthF) * viewSpan;
        const auto startBucket = juce::jlimit(0, bucketCount - 1, static_cast<int>(r0 * static_cast<float>(bucketCount)));
        const auto endBucket = juce::jlimit(startBucket + 1,
                                           bucketCount,
                                           static_cast<int>(r1 * static_cast<float>(bucketCount)) + 1);
        float minValue = 1.0f;
        float maxValue = -1.0f;

        for (int bucket = startBucket; bucket < endBucket; ++bucket)
        {
            minValue = juce::jmin(minValue, waveformPeaks.minValues[static_cast<std::size_t>(bucket)]);
            maxValue = juce::jmax(maxValue, waveformPeaks.maxValues[static_cast<std::size_t>(bucket)]);
        }

        const auto drawX = static_cast<float>(waveInner.getX() + x);
        const auto y1 = centerY - maxValue * halfHeight;
        const auto y2 = centerY - minValue * halfHeight;
        g.drawLine(drawX, y1, drawX, y2, 1.6f);
    }

    g.setColour(accentColour.withAlpha(0.18f));
    g.drawHorizontalLine(waveInner.getCentreY(), static_cast<float>(waveInner.getX()), static_cast<float>(waveInner.getRight()));
}

bool SamplerPanelComponent::hitTest(int x, int y)
{
    return getPanelBounds().contains(x, y);
}

void SamplerPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    draggingSens = false;
    draggingSliceIndex = -1;
    auto* track = getActiveTrack();
    if (track != nullptr)
    {
        // Manual slice-marker editing on the waveform (Slice mode): drag a marker to move
        // the point exactly onto a transient; double-click adds a marker, double-click on a
        // marker removes it. Equal divisions are materialised into editable points on first edit.
        if (track->samplerMode == SamplerPlaybackMode::slice
            && ! waveformBounds.isEmpty() && waveformBounds.contains(event.getPosition()))
        {
            const auto w = static_cast<float>(juce::jmax(1, waveformBounds.getWidth()));
            const auto viewStart = waveViewStart;
            const auto viewSpan  = juce::jmax(0.0001f, waveViewSpan);
            const auto xToRatio = [&](int x) {
                return juce::jlimit(0.0, 1.0, static_cast<double>(viewStart) + static_cast<double>(x - waveformBounds.getX()) / w * static_cast<double>(viewSpan));
            };
            const auto ratioToX = [&](double r) {
                return waveformBounds.getX() + static_cast<int>(std::round(((r - viewStart) / viewSpan) * w));
            };

            const bool hasPts = ! track->samplerSlicePoints.empty();
            const int boundaryCount = hasPts ? static_cast<int>(track->samplerSlicePoints.size())
                                             : juce::jlimit(1, 64, track->samplerSliceCount);
            const auto boundaryRatio = [&](int i) {
                return hasPts ? track->samplerSlicePoints[static_cast<std::size_t>(i)]
                              : static_cast<double>(i) / static_cast<double>(boundaryCount);
            };

            int nearest = -1, nearestDx = 7;
            for (int i = 1; i < boundaryCount; ++i)
            {
                const auto dx = std::abs(event.getPosition().x - ratioToX(boundaryRatio(i)));
                if (dx < nearestDx) { nearestDx = dx; nearest = i; }
            }

            const bool doubleClick = event.getNumberOfClicks() >= 2;
            if (doubleClick || nearest >= 1)
            {
                // Materialise equal divisions so points become editable.
                if (! hasPts)
                {
                    track->samplerSlicePoints.clear();
                    for (int i = 0; i < boundaryCount; ++i)
                        track->samplerSlicePoints.push_back(static_cast<double>(i) / static_cast<double>(boundaryCount));
                }
                auto& pts = track->samplerSlicePoints;

                if (doubleClick)
                {
                    if (nearest >= 1 && nearest < static_cast<int>(pts.size()))
                    {
                        pts.erase(pts.begin() + nearest);  // remove this marker
                    }
                    else
                    {
                        const auto r = xToRatio(event.getPosition().x);
                        if (r > 0.005 && r < 0.995)
                            pts.insert(std::upper_bound(pts.begin(), pts.end(), r), r);
                    }
                    track->samplerSliceCount = static_cast<int>(pts.size());
                    padBank = 0;
                    pushSlicePointsToEngine();
                    repaint();
                    grabKeyboardFocus();
                    return;
                }

                draggingSliceIndex = nearest;  // begin dragging the marker
                grabKeyboardFocus();
                return;
            }
        }

        const std::array<SamplerPlaybackMode, 3> modes {
            SamplerPlaybackMode::classic,
            SamplerPlaybackMode::oneShot,
            SamplerPlaybackMode::slice
        };

        for (std::size_t i = 0; i < modeButtonBounds.size(); ++i)
        {
            if (modeButtonBounds[i].contains(event.getPosition()))
            {
                releaseTypingPianoNotes();
                track->samplerMode = modes[i];
                repaint();
                grabKeyboardFocus();
                return;
            }
        }

        const std::array<int, 4> sliceCounts { 4, 8, 16, 32 };
        for (std::size_t i = 0; i < sliceButtonBounds.size(); ++i)
        {
            if (sliceButtonBounds[i].contains(event.getPosition()))
            {
                releaseTypingPianoNotes();
                track->samplerSliceCount = sliceCounts[i];
                track->samplerSlicePoints.clear();   // equal divisions → leave transient mode
                track->samplerMode = SamplerPlaybackMode::slice;
                if (track->samplerSliceCount <= 16)
                    padBank = 0;
                pushSlicePointsToEngine();
                repaint();
                grabKeyboardFocus();
                return;
            }
        }

        // Detect / clear transients.
        if (! detectTransientsButtonBounds.isEmpty() && detectTransientsButtonBounds.contains(event.getPosition()))
        {
            releaseTypingPianoNotes();
            if (! track->samplerSlicePoints.empty())
            {
                track->samplerSlicePoints.clear();   // toggle off → back to equal divisions
                pushSlicePointsToEngine();
            }
            else
            {
                detectTransients();
            }
            padBank = 0;
            repaint();
            grabKeyboardFocus();
            return;
        }

        // Sensitivity slider: drag to set the value (live %), re-detect on release.
        if (! sensSliderBounds.isEmpty() && sensSliderBounds.contains(event.getPosition()))
        {
            draggingSens = true;
            track->samplerSliceSensitivity = juce::jlimit(0.0, 1.0,
                static_cast<double>(event.getPosition().x - sensSliderBounds.getX()) / juce::jmax(1, sensSliderBounds.getWidth()));
            repaint();
            grabKeyboardFocus();
            return;
        }


        if (samplerWarpButtonBounds.contains(event.getPosition()))
        {
            releaseTypingPianoNotes();
            track->samplerWarpEnabled = ! track->samplerWarpEnabled;
            repaint();
            grabKeyboardFocus();
            return;
        }

        // Knobs: click selects (so arrow keys edit it) and arms a vertical drag.
        const auto pickKnob = [&](juce::Rectangle<int> bounds, Knob which, double value)
        {
            if (! bounds.contains(event.getPosition()))
                return false;
            selectedKnob = which;
            knobDragStartY = event.getPosition().y;
            knobDragStartValue = value;
            repaint();
            grabKeyboardFocus();
            return true;
        };
        if (pickKnob(transposeKnobBounds, Knob::transpose, track->samplerTransposeSemitones)) return;
        if (pickKnob(rootKnobBounds,      Knob::root,      track->samplerRootMidiNote))        return;
        if (pickKnob(gainKnobBounds,      Knob::gain,      track->volumeDb))                   return;
    }

    selectedKnob = Knob::none;  // click elsewhere deselects
    repaint();
    grabKeyboardFocus();
}

void SamplerPanelComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    auto* track = getActiveTrack();
    if (track == nullptr)
        return;

    const auto pos = event.getPosition();

    // Double-click directly on the NUMBER (the value text under a knob) to type a value.
    struct KnobRef { Knob which; juce::Rectangle<int> col; juce::String current; };
    const KnobRef knobs[] = {
        { Knob::transpose, transposeKnobBounds, juce::String(track->samplerTransposeSemitones) },
        { Knob::root,      rootKnobBounds,      juce::MidiMessage::getMidiNoteName(track->samplerRootMidiNote, true, true, 3) },
        { Knob::gain,      gainKnobBounds,      juce::String(track->volumeDb, 1) },
    };
    for (const auto& k : knobs)
    {
        // Value text sits below the 48px knob (drawn in a 58px band) and the 16px title.
        if (knobValueBoundsFor(k.col).contains(pos))
        {
            beginKnobTextEntry(k.which, k.current);
            return;
        }
    }

    // Double-click the knob itself resets it to its default (mixer-style).
    if (transposeKnobBounds.contains(pos))
        track->samplerTransposeSemitones = 0;
    else if (rootKnobBounds.contains(pos))
        track->samplerRootMidiNote = 60;          // C3
    else if (gainKnobBounds.contains(pos))
        track->volumeDb = 0.0;
    else
        return;

    repaint();
    grabKeyboardFocus();
}

juce::Rectangle<int> SamplerPanelComponent::knobValueBoundsFor(juce::Rectangle<int> col) const
{
    // Mirror drawKnob's layout exactly: 58px knob band, 16px title, then the 18px value text.
    return col.withTrimmedTop(74).withHeight(18);
}

juce::Rectangle<int> SamplerPanelComponent::knobBoundsFor(Knob which) const
{
    switch (which)
    {
        case Knob::transpose: return transposeKnobBounds;
        case Knob::root:      return rootKnobBounds;
        case Knob::gain:      return gainKnobBounds;
        case Knob::none:      break;
    }
    return {};
}

void SamplerPanelComponent::beginKnobTextEntry(Knob which, const juce::String& seed)
{
    auto* track = getActiveTrack();
    if (track == nullptr || which == Knob::none)
        return;

    const auto col = knobBoundsFor(which);
    if (col.isEmpty())
        return;
    const auto valueArea = knobValueBoundsFor(col).reduced(4, 0);

    if (knobEditor == nullptr)
    {
        knobEditor = std::make_unique<juce::TextEditor>();
        knobEditor->setJustification(juce::Justification::centred);
        // No frame and no background box — just the number, matching the painted value.
        knobEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        knobEditor->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        knobEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        knobEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
        knobEditor->setColour(juce::TextEditor::highlightColourId, knobRingColour.withAlpha(0.4f));
        knobEditor->setBorder(juce::BorderSize<int>(0));
        knobEditor->setIndents(0, 0);
        knobEditor->onReturnKey = [this] { commitKnobTextEntry(); };
        knobEditor->onEscapeKey = [this] { cancelKnobTextEntry(); };
        knobEditor->onFocusLost = [this] { commitKnobTextEntry(); };
    }

    editingKnob = which;
    selectedKnob = which;
    knobEditor->setFont(juce::FontOptions(12.0f, juce::Font::bold));  // match the painted value
    knobEditor->setBounds(valueArea);
    addAndMakeVisible(*knobEditor);
    knobEditor->setText(seed, juce::dontSendNotification);
    if (seed.isEmpty())
        knobEditor->setCaretPosition(0);
    else
        knobEditor->selectAll();
    knobEditor->grabKeyboardFocus();
    repaint();
}

void SamplerPanelComponent::commitKnobTextEntry()
{
    if (knobEditor == nullptr || editingKnob == Knob::none)
        return;

    const auto which = editingKnob;
    const auto text = knobEditor->getText();
    // Clear the editing flag first so the focus-lost callback (fired when we remove the
    // editor below) doesn't re-enter this function.
    editingKnob = Knob::none;

    if (auto* track = getActiveTrack(); track != nullptr && text.trim().isNotEmpty())
    {
        switch (which)
        {
            case Knob::transpose:
                track->samplerTransposeSemitones = juce::jlimit(-24, 24, text.getIntValue());
                break;
            case Knob::root:
                if (const auto note = parseRootEntry(text))
                    track->samplerRootMidiNote = *note;
                break;
            case Knob::gain:
                track->volumeDb = juce::jlimit(-60.0, 6.0, text.getDoubleValue());
                break;
            case Knob::none:
                break;
        }
    }

    removeChildComponent(knobEditor.get());
    grabKeyboardFocus();
    repaint();
}

void SamplerPanelComponent::cancelKnobTextEntry()
{
    if (knobEditor == nullptr)
        return;
    editingKnob = Knob::none;
    removeChildComponent(knobEditor.get());
    grabKeyboardFocus();
    repaint();
}

void SamplerPanelComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (draggingSliceIndex >= 1)
    {
        auto* track = getActiveTrack();
        if (track != nullptr && draggingSliceIndex < static_cast<int>(track->samplerSlicePoints.size())
            && ! waveformBounds.isEmpty())
        {
            const auto w = static_cast<float>(juce::jmax(1, waveformBounds.getWidth()));
            const auto viewSpan = juce::jmax(0.0001f, waveViewSpan);
            const auto r = juce::jlimit(0.0, 1.0,
                static_cast<double>(waveViewStart) + static_cast<double>(event.getPosition().x - waveformBounds.getX()) / w * static_cast<double>(viewSpan));
            auto& pts = track->samplerSlicePoints;
            const auto lo = pts[static_cast<std::size_t>(draggingSliceIndex - 1)] + 0.0005;
            const auto hi = draggingSliceIndex + 1 < static_cast<int>(pts.size())
                ? pts[static_cast<std::size_t>(draggingSliceIndex + 1)] - 0.0005 : 0.9995;
            pts[static_cast<std::size_t>(draggingSliceIndex)] = juce::jlimit(lo, hi, r);
            pushSlicePointsToEngine();
            repaint();
        }
        return;
    }

    if (draggingSens)
    {
        if (auto* track = getActiveTrack(); track != nullptr && ! sensSliderBounds.isEmpty())
        {
            track->samplerSliceSensitivity = juce::jlimit(0.0, 1.0,
                static_cast<double>(event.getPosition().x - sensSliderBounds.getX()) / juce::jmax(1, sensSliderBounds.getWidth()));
            repaint();
        }
        return;
    }

    if (selectedKnob == Knob::none || getActiveTrack() == nullptr)
        return;

    // Drag up = increase. ~6 px per step for fine control.
    const auto deltaPx = static_cast<double>(knobDragStartY - event.getPosition().y);
    const auto steps = deltaPx / 6.0;

    switch (selectedKnob)
    {
        case Knob::transpose:
            getActiveTrack()->samplerTransposeSemitones = juce::jlimit(-24, 24,
                static_cast<int>(std::round(knobDragStartValue + steps)));
            break;
        case Knob::root:
            getActiveTrack()->samplerRootMidiNote = juce::jlimit(0, 127,
                static_cast<int>(std::round(knobDragStartValue + steps)));
            break;
        case Knob::gain:
            getActiveTrack()->volumeDb = juce::jlimit(-60.0, 6.0,
                knobDragStartValue + steps * 0.5);  // 0.5 dB per step
            break;
        case Knob::none:
            break;
    }
    repaint();
}

void SamplerPanelComponent::mouseUp(const juce::MouseEvent&)
{
    if (draggingSliceIndex >= 0)
    {
        draggingSliceIndex = -1;
        if (auto* track = getActiveTrack(); track != nullptr)
            pushSlicePointsToEngine();
    }

    if (draggingSens)
    {
        draggingSens = false;
        // Apply the new sensitivity by re-detecting (only while transient mode is on).
        if (auto* track = getActiveTrack(); track != nullptr && ! track->samplerSlicePoints.empty())
        {
            detectTransients();
            repaint();
        }
    }
}

void SamplerPanelComponent::mouseMagnify(const juce::MouseEvent& event, float scaleFactor)
{
    // Two-finger pinch zooms the slice waveform horizontally around the cursor.
    if (waveformBounds.isEmpty() || ! waveformBounds.contains(event.getPosition()) || scaleFactor <= 0.0f)
        return;

    const auto w = static_cast<float>(juce::jmax(1, waveformBounds.getWidth()));
    const auto cursorFrac  = juce::jlimit(0.0f, 1.0f, static_cast<float>(event.getPosition().x - waveformBounds.getX()) / w);
    const auto cursorRatio = waveViewStart + cursorFrac * waveViewSpan;

    const auto newSpan = juce::jlimit(0.02f, 1.0f, waveViewSpan / scaleFactor);  // pinch out → zoom in → smaller span
    waveViewStart = juce::jlimit(0.0f, 1.0f - newSpan, cursorRatio - cursorFrac * newSpan);
    waveViewSpan  = newSpan;
    repaint();
}

void SamplerPanelComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    // Two-finger horizontal swipe pans the zoomed slice waveform.
    if (waveformBounds.isEmpty() || ! waveformBounds.contains(event.getPosition()) || waveViewSpan >= 0.999f)
        return;

    const auto delta = std::abs(wheel.deltaX) >= std::abs(wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
    waveViewStart = juce::jlimit(0.0f, 1.0f - waveViewSpan, waveViewStart - delta * waveViewSpan);
    repaint();
}

bool SamplerPanelComponent::keyPressed(const juce::KeyPress& key)
{
    auto* track = getActiveTrack();
    if (key == juce::KeyPress::escapeKey)
    {
        closePanel();
        return true;
    }

    if (key == juce::KeyPress::spaceKey)
    {
        releaseTypingPianoNotes();
        return false;
    }

    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown() || key.getModifiers().isAltDown())
        return false;

    if (track != nullptr)
    {
        // Direct numeric entry on the selected knob: Return opens an editor pre-filled with
        // the current value; typing a digit / '-' / '.' opens it seeded with that character.
        if (selectedKnob != Knob::none && editingKnob == Knob::none)
        {
            const auto c = key.getTextCharacter();
            const bool isNumeric = (c >= '0' && c <= '9') || c == '-' || c == '.';
            if (key == juce::KeyPress::returnKey)
            {
                juce::String current;
                switch (selectedKnob)
                {
                    case Knob::transpose: current = juce::String(track->samplerTransposeSemitones); break;
                    case Knob::root:      current = juce::MidiMessage::getMidiNoteName(track->samplerRootMidiNote, true, true, 3); break;
                    case Knob::gain:      current = juce::String(track->volumeDb, 1); break;
                    case Knob::none:      break;
                }
                beginKnobTextEntry(selectedKnob, current);
                return true;
            }
            if (isNumeric)
            {
                beginKnobTextEntry(selectedKnob, juce::String::charToString(c));
                return true;
            }
        }

        if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
        {
            releaseTypingPianoNotes();
            track->samplerKeyboardOctaveOffset = juce::jlimit(-4,
                                                                    4,
                                                                    track->samplerKeyboardOctaveOffset
                                                                        + (key == juce::KeyPress::rightKey ? 1 : -1));
            repaint();
            return true;
        }

        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        {
            const auto dir = (key == juce::KeyPress::upKey ? 1 : -1);

            // Ableton-style: if a knob is selected, arrows edit it (Shift = big step).
            if (selectedKnob != Knob::none)
            {
                nudgeSelectedKnob(dir, key.getModifiers().isShiftDown());
                return true;
            }

            // Otherwise up/down still transpose globally (legacy shortcut).
            releaseTypingPianoNotes();
            track->samplerTransposeSemitones = juce::jlimit(-24, 24,
                track->samplerTransposeSemitones + dir);
            repaint();
            return true;
        }
    }

    if (pitchForKeyCode(key.getKeyCode()).has_value())
    {
        const auto handled = updateTypingPianoNotes();
        // Even if the async key-state poll missed this exact event, consume musical
        // keys while a playable track is armed so macOS does not emit its system beep.
        return handled
            || (track != nullptr
                && juce::Process::isForegroundProcess()
                && (track->samplerSourcePath.isNotEmpty() || track->instrumentPluginId.isNotEmpty()));
    }

    return false;
}

bool SamplerPanelComponent::keyStateChanged(bool)
{
    return updateTypingPianoNotes();
}

void SamplerPanelComponent::focusLost(FocusChangeType)
{
}

void SamplerPanelComponent::visibilityChanged()
{
    // Keyboard polling timer keeps running even when the panel is hidden, so the
    // user can keep playing the active track from anywhere in the app. The
    // timer stops only when the active track is fully cleared.
    if (! isVisible())
        cancelKnobTextEntry();
}

void SamplerPanelComponent::timerCallback()
{
    // Always poll the keyboard while a track is armed, regardless of panel
    // visibility — so the user can play notes from anywhere in the app.
    if (getActiveTrack() != nullptr)
        updateTypingPianoNotes();

    if (isVisible() && playbackSliceIndex.has_value())
    {
        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - playbackSliceStartedMs;
        if (playbackSliceDurationMs <= 0.0 || elapsedMs >= playbackSliceDurationMs)
            playbackSliceIndex.reset();

        repaint();
    }

    if (isVisible() && padFlashSliceIndex.has_value())
    {
        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - padFlashStartedMs;
        if (elapsedMs >= padFlashDurationMs)
            padFlashSliceIndex.reset();

        repaint();
    }

    if (isVisible() && playbackLinearStartedMs.has_value())
    {
        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - *playbackLinearStartedMs;
        if (playbackLinearDurationMs <= 0.0 || elapsedMs >= playbackLinearDurationMs)
            playbackLinearStartedMs.reset();

        repaint();
    }
}

juce::Rectangle<int> SamplerPanelComponent::getPanelBounds() const
{
    return getLocalBounds();
}

std::optional<int> SamplerPanelComponent::pitchForKeyCode(int keyCode)
{
    static constexpr int lowerRowBasePitch = 48;
    static constexpr int upperRowBasePitch = 60;
    static constexpr std::array<std::pair<int, int>, 39> mapping {{
        { 'z', lowerRowBasePitch + 0 }, { 's', lowerRowBasePitch + 1 }, { 'x', lowerRowBasePitch + 2 },
        { 'd', lowerRowBasePitch + 3 }, { 'c', lowerRowBasePitch + 4 }, { 'v', lowerRowBasePitch + 5 },
        { 'g', lowerRowBasePitch + 6 }, { 'b', lowerRowBasePitch + 7 }, { 'h', lowerRowBasePitch + 8 },
        { 'n', lowerRowBasePitch + 9 }, { 'j', lowerRowBasePitch + 10 }, { 'm', lowerRowBasePitch + 11 },
        { ',', lowerRowBasePitch + 12 }, { 'l', lowerRowBasePitch + 13 }, { '.', lowerRowBasePitch + 14 },
        { ';', lowerRowBasePitch + 15 }, { '/', lowerRowBasePitch + 16 }, { '\'', lowerRowBasePitch + 17 },
        { 'q', upperRowBasePitch + 0 }, { '2', upperRowBasePitch + 1 }, { 'w', upperRowBasePitch + 2 },
        { '3', upperRowBasePitch + 3 }, { 'e', upperRowBasePitch + 4 }, { 'r', upperRowBasePitch + 5 },
        { '5', upperRowBasePitch + 6 }, { 't', upperRowBasePitch + 7 }, { '6', upperRowBasePitch + 8 },
        { 'y', upperRowBasePitch + 9 }, { '7', upperRowBasePitch + 10 }, { 'u', upperRowBasePitch + 11 },
        { 'i', upperRowBasePitch + 12 }, { '9', upperRowBasePitch + 13 }, { 'o', upperRowBasePitch + 14 },
        { '0', upperRowBasePitch + 15 }, { 'p', upperRowBasePitch + 16 }, { '[', upperRowBasePitch + 17 },
        { ']', upperRowBasePitch + 18 }, { '+', upperRowBasePitch + 19 }, { '=', upperRowBasePitch + 19 },
    }};

    const auto lowerKeyCode = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(keyCode));
    for (const auto& [mappedKey, pitch] : mapping)
    {
        if (lowerKeyCode == mappedKey)
            return pitch;
    }

    return std::nullopt;
}

std::optional<int> SamplerPanelComponent::sliceIndexForKeyCode(int keyCode, int sliceCount)
{
    const auto lowerKeyCode = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(keyCode));
    const auto safeSliceCount = juce::jlimit(1, 64, sliceCount);

    static constexpr std::array<int, 18> lowerRow {
        'z', 's', 'x', 'd', 'c', 'v', 'g', 'b', 'h', 'n', 'j', 'm', ',', 'l', '.', ';', '/', '\''
    };
    static constexpr std::array<int, 21> upperRow {
        'q', '2', 'w', '3', 'e', 'r', '5', 't', '6', 'y', '7', 'u', 'i', '9', 'o', '0', 'p', '[', ']', '+', '='
    };

    auto findSlice = [lowerKeyCode, safeSliceCount](const auto& keys, int offset) -> std::optional<int>
    {
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (lowerKeyCode == keys[i])
            {
                const auto idx = static_cast<int>(i) + offset;
                if (idx >= safeSliceCount)
                    return std::nullopt;

                return idx;
            }
        }

        return std::nullopt;
    };

    // With more than 16 slices, split across two rows like MPC banks: the lower row plays
    // slices 1-16, the upper row plays 17-32. With 16 or fewer, both rows play 1-16.
    const auto upperOffset = safeSliceCount > 16 ? 16 : 0;

    if (auto lowerSlice = findSlice(lowerRow, 0))
        return lowerSlice;

    return findSlice(upperRow, upperOffset);
}

juce::String SamplerPanelComponent::samplerModeName(SamplerPlaybackMode mode)
{
    switch (mode)
    {
        case SamplerPlaybackMode::classic: return "Classic";
        case SamplerPlaybackMode::oneShot: return "One-Shot";
        case SamplerPlaybackMode::slice: return "Slice";
    }

    return "Classic";
}

TrackState* SamplerPanelComponent::getActiveTrack() const
{
    if (activeTrackIndex >= 0 && onResolveTrack)
        return onResolveTrack(activeTrackIndex);

    return activeTrack;
}

bool SamplerPanelComponent::getActiveTrackGainDb(double& outDb) const
{
    if (auto* track = getActiveTrack(); track != nullptr)
    {
        outDb = track->volumeDb + track->trackGainDb;
        return true;
    }
    return false;
}

bool SamplerPanelComponent::updateTypingPianoNotes()
{
    if (! juce::Process::isForegroundProcess())
    {
        releaseTypingPianoNotes();
        return false;
    }

    auto* track = getActiveTrack();
    // The typing keyboard drives either a loaded sampler sample OR a hosted VST
    // instrument. Tracks with neither produce no sound, so bail out.
    if (track == nullptr || (track->samplerSourcePath.isEmpty() && track->instrumentPluginId.isEmpty()))
    {
        releaseTypingPianoNotes();
        return false;
    }

    const auto modifiers = juce::ModifierKeys::getCurrentModifiers();
    if (modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown())
        return false;

    static constexpr std::array<int, 39> keyCodes {
        'q', '2', 'w', '3', 'e', 'r', '5', 't', '6', 'y', '7', 'u', 'i', '9', 'o', '0', 'p', '[', ']', '+', '=',
        'z', 's', 'x', 'd', 'c', 'v', 'g', 'b', 'h', 'n', 'j', 'm', ',', 'l', '.', ';', '/', '\''
    };

    bool consumed = false;
    std::set<int> currentlyDown;
    std::set<int> currentlyActiveSlices;
    for (const auto keyCode : keyCodes)
    {
        const auto pitch = pitchForKeyCode(keyCode);
        if (! pitch.has_value())
            continue;

        if (juce::KeyPress::isKeyCurrentlyDown(keyCode))
        {
            int playablePitch = juce::jlimit(0,
                                             127,
                                             *pitch
                                                 + track->samplerKeyboardOctaveOffset * 12
                                                 + track->samplerTransposeSemitones);
            auto sliceIndex = 0;
            auto noteIdentity = playablePitch;

            if (track->samplerMode == SamplerPlaybackMode::slice)
            {
                const auto mappedSlice = sliceIndexForKeyCode(keyCode, track->samplerSliceCount);
                if (! mappedSlice.has_value())
                    continue;

                // The keyboard maps ~32 slices at once; each octave step pages to the next
                // block, so slices beyond 32 are reached by switching octave (←/→).
                constexpr int slicesPerOctavePage = 32;
                sliceIndex = *mappedSlice + track->samplerKeyboardOctaveOffset * slicesPerOctavePage;
                if (sliceIndex < 0 || sliceIndex >= juce::jlimit(1, 64, track->samplerSliceCount))
                    continue;
                playablePitch = juce::jlimit(0, 127, track->samplerRootMidiNote + sliceIndex);
                noteIdentity = 10000 + keyCode;
                currentlyActiveSlices.insert(sliceIndex);
            }
            else
            {
                // Diatonic mapping for pitched playback modes when scale lock is on:
                // each "white" laptop key plays the NEXT scale degree (no duplicates).
                // "Black key" positions (numbers + s/d/g/h/j/l/; etc.) are skipped.
                const auto lockEnabled = onRequestScaleLockEnabled && onRequestScaleLockEnabled();
                if (lockEnabled && onRequestProjectKeyRoot && onRequestProjectKeyIsMinor)
                {
                    static const std::array<std::pair<int, int>, 12> upperRow {{
                        {'q', 0}, {'w', 1}, {'e', 2}, {'r', 3}, {'t', 4}, {'y', 5},
                        {'u', 6}, {'i', 7}, {'o', 8}, {'p', 9}, {'[', 10}, {']', 11},
                    }};
                    static const std::array<std::pair<int, int>, 11> lowerRow {{
                        {'z', 0}, {'x', 1}, {'c', 2}, {'v', 3}, {'b', 4}, {'n', 5},
                        {'m', 6}, {',', 7}, {'.', 8}, {'/', 9}, {'\'', 10},
                    }};

                    int diatonicIdx = -1;
                    bool isLowerRow = false;
                    for (const auto& [k, idx] : upperRow)
                        if (k == keyCode) { diatonicIdx = idx; break; }
                    if (diatonicIdx < 0)
                    {
                        for (const auto& [k, idx] : lowerRow)
                            if (k == keyCode) { diatonicIdx = idx; isLowerRow = true; break; }
                    }
                    if (diatonicIdx < 0) continue; // chromatic accidental key — skip silently

                    static const std::array<int, 7> majorScale { 0, 2, 4, 5, 7, 9, 11 };
                    static const std::array<int, 7> minorScale { 0, 2, 3, 5, 7, 8, 10 };
                    const auto& pattern = onRequestProjectKeyIsMinor() ? minorScale : majorScale;
                    const auto root     = onRequestProjectKeyRoot();
                    const auto rowBase  = isLowerRow ? 48 : 60;
                    const auto octShift = diatonicIdx / 7;
                    const auto degree   = diatonicIdx % 7;

                    playablePitch = juce::jlimit(0, 127,
                                                 rowBase + root + octShift * 12 + pattern[static_cast<std::size_t>(degree)]
                                                     + track->samplerKeyboardOctaveOffset * 12
                                                     + track->samplerTransposeSemitones);
                    noteIdentity  = playablePitch;
                }
            }

            currentlyDown.insert(noteIdentity);
            if (! activeNotes.contains(noteIdentity))
            {
                activeNotes.insert(noteIdentity);
                activeNotePitches[noteIdentity] = playablePitch;
                if (onNoteOn)
                    onNoteOn(track->samplerSourcePath,
                             playablePitch,
                             110,
                             track->samplerRootMidiNote,
                             track->volumeDb,
                             track->samplerMode,
                             sliceIndex,
                             track->samplerSliceCount,
                             track->samplerWarpEnabled,
                             track->samplerSourceBpm);

                if (track->samplerMode == SamplerPlaybackMode::slice)
                {
                    startSlicePlaybackIndicator(sliceIndex);
                    padBank = sliceIndex / 16;  // pad view follows the played slice's bank
                }
                else
                    startLinearPlaybackIndicator(playablePitch);
            }

            consumed = true;
        }
    }

    std::vector<int> releasedNotes;
    for (const auto activeNote : activeNotes)
    {
        if (! currentlyDown.contains(activeNote))
            releasedNotes.push_back(activeNote);
    }

    for (const auto releasedNote : releasedNotes)
    {
        const auto pitchIt = activeNotePitches.find(releasedNote);
        const auto releasedPitch = pitchIt != activeNotePitches.end() ? pitchIt->second : releasedNote;
        activeNotes.erase(releasedNote);
        activeNotePitches.erase(releasedNote);
        if (onNoteOff)
            onNoteOff(releasedPitch, track != nullptr ? track->samplerMode : SamplerPlaybackMode::classic);
    }

    if (activeSliceIndices != currentlyActiveSlices)
    {
        activeSliceIndices = currentlyActiveSlices;
        repaint();
    }

    return consumed || ! releasedNotes.empty();
}

void SamplerPanelComponent::releaseTypingPianoNotes()
{
    if (onAllNotesOff)
        onAllNotesOff();

    activeNotes.clear();
    activeNotePitches.clear();
    activeSliceIndices.clear();
    playbackSliceIndex.reset();
    padFlashSliceIndex.reset();
    playbackLinearStartedMs.reset();
    repaint();
}

void SamplerPanelComponent::nudgeSelectedKnob(int direction, bool large)
{
    auto* track = getActiveTrack();
    if (track == nullptr || selectedKnob == Knob::none)
        return;

    switch (selectedKnob)
    {
        case Knob::transpose:
            track->samplerTransposeSemitones = juce::jlimit(-24, 24,
                track->samplerTransposeSemitones + direction * (large ? 12 : 1));
            break;
        case Knob::root:
            track->samplerRootMidiNote = juce::jlimit(0, 127,
                track->samplerRootMidiNote + direction * (large ? 12 : 1));
            break;
        case Knob::gain:
            track->volumeDb = juce::jlimit(-60.0, 6.0,
                track->volumeDb + direction * (large ? 6.0 : 1.0));
            break;
        case Knob::none:
            break;
    }
    repaint();
}

void SamplerPanelComponent::stopPreviewPlayback()
{
    // Release the audition voice and clear BOTH playhead indicators so the simpler
    // preview halts in lock-step with the transport.
    releaseTypingPianoNotes();
}

void SamplerPanelComponent::triggerSlicePad(int sliceIndex)
{
    auto* track = getActiveTrack();
    if (track == nullptr || track->samplerSourcePath.isEmpty())
        return;

    const auto sliceCount = juce::jlimit(1, 64, track->samplerSliceCount);
    if (sliceIndex < 0 || sliceIndex >= sliceCount)
        return;

    padBank = sliceIndex / 16;
    const auto pitch = juce::jlimit(0, 127, track->samplerRootMidiNote + sliceIndex);
    if (onNoteOn)
        onNoteOn(track->samplerSourcePath, pitch, 110, track->samplerRootMidiNote,
                 track->volumeDb, track->samplerMode, sliceIndex, sliceCount,
                 track->samplerWarpEnabled, track->samplerSourceBpm);

    startSlicePlaybackIndicator(sliceIndex);  // lights the pad (auto-clears on timer)
    repaint();
}

void SamplerPanelComponent::pushSlicePointsToEngine()
{
    if (! onSlicePointsChanged)
        return;
    auto* track = getActiveTrack();
    onSlicePointsChanged(track != nullptr ? track->samplerSlicePoints : std::vector<double>{});
}

void SamplerPanelComponent::detectTransients()
{
    auto* track = getActiveTrack();
    if (track == nullptr || track->samplerSourcePath.isEmpty())
        return;

    juce::File file(track->samplerSourcePath);
    std::unique_ptr<juce::AudioFormatReader> reader(waveformFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return;

    const auto total = static_cast<int>(juce::jmin<juce::int64>(reader->lengthInSamples, 16'000'000));
    juce::AudioBuffer<float> buffer(static_cast<int>(juce::jmax(1u, reader->numChannels)), total);
    reader->read(&buffer, 0, total, 0, true, true);

    // Mono onset envelope: positive frame-to-frame rise in short-window RMS energy.
    const int hop = 256;
    const int win = 1024;
    const int numCh = buffer.getNumChannels();
    std::vector<float> flux;
    flux.reserve(static_cast<std::size_t>(total / hop + 1));
    float prevEnergy = 0.0f;
    for (int pos = 0; pos < total; pos += hop)
    {
        const int end = juce::jmin(total, pos + win);
        float e = 0.0f;
        for (int i = pos; i < end; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < numCh; ++c)
                s += buffer.getSample(c, i);
            s /= static_cast<float>(numCh);
            e += s * s;
        }
        e = std::sqrt(e / static_cast<float>(juce::jmax(1, end - pos)));
        flux.push_back(juce::jmax(0.0f, e - prevEnergy));
        prevEnergy = e;
    }
    if (flux.size() < 3)
        return;

    // Adaptive threshold: mean + k·stddev of the flux.
    double mean = 0.0;
    for (auto f : flux) mean += f;
    mean /= static_cast<double>(flux.size());
    double var = 0.0;
    for (auto f : flux) var += (f - mean) * (f - mean);
    var /= static_cast<double>(flux.size());
    const auto stdv = std::sqrt(var);

    // Collect ALL onset candidates (local maxima above a low noise floor, lightly spaced).
    const auto minSpacingFrames = juce::jmax(1, static_cast<int>(0.04 * reader->sampleRate / hop)); // ~40 ms
    const auto floorLevel = static_cast<float>(mean + 0.2 * stdv);
    struct Peak { int frame; float strength; };
    std::vector<Peak> peaks;
    int lastPeak = -minSpacingFrames;
    for (int i = 1; i + 1 < static_cast<int>(flux.size()); ++i)
    {
        const auto v = flux[static_cast<std::size_t>(i)];
        if (v > floorLevel
            && v >= flux[static_cast<std::size_t>(i - 1)]
            && v >= flux[static_cast<std::size_t>(i + 1)]
            && i - lastPeak >= minSpacingFrames)
        {
            const auto ratio = static_cast<double>(i * hop) / static_cast<double>(total);
            if (ratio > 0.01 && ratio < 0.995)
                peaks.push_back({ i, v });
            lastPeak = i;
        }
    }

    // Sensitivity 0..1 directly controls density: keep that fraction of the strongest
    // candidates. 0% → no extra slices (whole sample), 100% → all (capped to 4 pad banks).
    const auto sens = juce::jlimit(0.0, 1.0, track->samplerSliceSensitivity);
    const auto keep = juce::jlimit(0, 63, static_cast<int>(std::round(sens * static_cast<double>(peaks.size()))));
    if (static_cast<int>(peaks.size()) > keep)
    {
        std::nth_element(peaks.begin(), peaks.begin() + keep, peaks.end(),
                         [](const Peak& a, const Peak& b) { return a.strength > b.strength; });
        peaks.resize(static_cast<std::size_t>(keep));
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.frame < b.frame; });

    std::vector<double> points { 0.0 };  // first slice always starts at 0
    for (const auto& pk : peaks)
        points.push_back(static_cast<double>(pk.frame * hop) / static_cast<double>(total));

    track->samplerSlicePoints = points;
    track->samplerSliceCount = static_cast<int>(points.size());
    track->samplerMode = SamplerPlaybackMode::slice;
    pushSlicePointsToEngine();
}

void SamplerPanelComponent::startSlicePlaybackIndicator(int sliceIndex)
{
    auto* track = getActiveTrack();
    if (track == nullptr || track->samplerMode != SamplerPlaybackMode::slice)
        return;

    const auto safeSliceCount = juce::jlimit(1, 64, track->samplerSliceCount);
    if (track->samplerSourceDurationSeconds <= 0.0)
        return;

    auto playbackDurationSeconds = track->samplerSourceDurationSeconds;
    if (track->samplerWarpEnabled && track->samplerSourceBpm > 0.0 && onRequestProjectTempoBpm)
    {
        const auto projectTempoBpm = onRequestProjectTempoBpm();
        if (projectTempoBpm > 0.0)
            playbackDurationSeconds *= track->samplerSourceBpm / projectTempoBpm;
    }

    playbackSliceIndex = juce::jlimit(0, safeSliceCount - 1, sliceIndex);
    playbackSliceStartedMs = juce::Time::getMillisecondCounterHiRes();
    playbackSliceDurationMs = (playbackDurationSeconds / static_cast<double>(safeSliceCount)) * 1000.0;
    padFlashSliceIndex = playbackSliceIndex;
    padFlashStartedMs = playbackSliceStartedMs;
    repaint();
}

void SamplerPanelComponent::startLinearPlaybackIndicator(int playablePitch)
{
    auto* track = getActiveTrack();
    if (track == nullptr || track->samplerMode == SamplerPlaybackMode::slice)
        return;
    if (track->samplerSourceDurationSeconds <= 0.0)
        return;

    // The note plays the whole sample once (Classic & One-Shot both play through). Warp
    // stretches it to the project tempo, and pitch (vs the root note) scales the speed —
    // mirror the engine so the playhead lands exactly where the audio is.
    auto playbackDurationSeconds = track->samplerSourceDurationSeconds;
    if (track->samplerWarpEnabled && track->samplerSourceBpm > 0.0 && onRequestProjectTempoBpm)
    {
        const auto projectTempoBpm = onRequestProjectTempoBpm();
        if (projectTempoBpm > 0.0)
            playbackDurationSeconds *= track->samplerSourceBpm / projectTempoBpm;
    }
    const auto pitchRatio = std::pow(2.0, static_cast<double>(playablePitch - track->samplerRootMidiNote) / 12.0);
    if (pitchRatio > 0.0)
        playbackDurationSeconds /= pitchRatio;

    playbackLinearStartedMs = juce::Time::getMillisecondCounterHiRes();
    playbackLinearDurationMs = playbackDurationSeconds * 1000.0;
    repaint();
}
}  // namespace orion
