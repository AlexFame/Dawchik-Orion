#include "SamplerPanelComponent.h"

#include <array>
#include <cmath>

namespace
{
const auto panelBackground = juce::Colour(0xff151c23);
const auto deviceHeader = juce::Colour(0xff202832);
const auto waveformBackground = juce::Colour(0xff0e141a);
const auto panelStroke = juce::Colour(0xff31404d);
const auto accentColour = juce::Colour(0xffeb6f3a);
const auto mutedText = juce::Colours::white.withAlpha(0.62f);
const auto controlBackground = juce::Colour(0xff202a33);

void drawKnob(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title, const juce::String& value, float normalized)
{
    auto knob = bounds.removeFromTop(58).withSizeKeepingCentre(48, 48);
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.fillEllipse(knob.toFloat());
    g.setColour(juce::Colours::white.withAlpha(0.18f));
    g.drawEllipse(knob.toFloat(), 1.0f);

    const auto angle = juce::jmap(juce::jlimit(0.0f, 1.0f, normalized),
                                 -2.35f,
                                 2.35f);
    const auto centre = knob.toFloat().getCentre();
    const auto radius = knob.getWidth() * 0.34f;
    g.setColour(accentColour);
    g.drawLine(centre.x,
               centre.y,
               centre.x + std::sin(angle) * radius,
               centre.y - std::cos(angle) * radius,
               2.4f);

    g.setColour(mutedText);
    g.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    g.drawText(title, bounds.removeFromTop(16), juce::Justification::centred);
    g.setColour(juce::Colours::white.withAlpha(0.88f));
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText(value, bounds.removeFromTop(18), juce::Justification::centred);
}
}

namespace orion
{
SamplerPanelComponent::SamplerPanelComponent()
{
    setWantsKeyboardFocus(true);
    setVisible(false);
}

void SamplerPanelComponent::openTrack(TrackState& trackState)
{
    releaseTypingPianoNotes();
    activeTrack = &trackState;
    activeTrackIndex = -1;
    setVisible(true);
    toFront(true);
    startTimerHz(90);
    grabKeyboardFocus();
    repaint();
}

void SamplerPanelComponent::openTrackIndex(int trackIndex)
{
    releaseTypingPianoNotes();
    activeTrack = nullptr;
    activeTrackIndex = trackIndex;
    setVisible(true);
    toFront(true);
    startTimerHz(90);
    grabKeyboardFocus();
    repaint();
}

void SamplerPanelComponent::closePanel()
{
    releaseTypingPianoNotes();
    stopTimer();
    setVisible(false);
    activeTrack = nullptr;
    activeTrackIndex = -1;

    if (onClose)
        onClose();
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

    auto waveform = content.removeFromTop(152);
    g.setColour(waveformBackground);
    g.fillRoundedRectangle(waveform.toFloat(), 12.0f);
    g.setColour(panelStroke);
    g.drawRoundedRectangle(waveform.toFloat(), 12.0f, 1.0f);

    auto waveInner = waveform.reduced(18, 18);
    const auto centerY = waveInner.getCentreY();
    const auto bars = 96;
    for (int i = 0; i < bars; ++i)
    {
        const auto x = juce::jmap(i, 0, bars - 1, waveInner.getX(), waveInner.getRight());
        const auto phase = static_cast<float>(i) * 0.23f;
        const auto amp = 0.2f + 0.8f * std::abs(std::sin(phase) * std::cos(phase * 0.37f));
        const auto height = juce::jmax(5.0f, static_cast<float>(waveInner.getHeight()) * 0.72f * amp);
        g.setColour(accentColour.withAlpha(0.72f));
        g.drawLine(static_cast<float>(x),
                   static_cast<float>(centerY) - height * 0.5f,
                   static_cast<float>(x),
                   static_cast<float>(centerY) + height * 0.5f,
                   2.0f);
    }

    if (track != nullptr && track->samplerMode == SamplerPlaybackMode::slice)
    {
        const auto slices = juce::jlimit(1, 64, track->samplerSliceCount);
        const auto sliceWidth = static_cast<float>(waveInner.getWidth()) / static_cast<float>(slices);

        for (int slice = 0; slice < slices; ++slice)
        {
            const auto sliceX = waveInner.getX() + sliceWidth * static_cast<float>(slice);
            auto sliceArea = juce::Rectangle<float>(sliceX,
                                                   static_cast<float>(waveInner.getY()),
                                                   sliceWidth,
                                                   static_cast<float>(waveInner.getHeight()));
            const auto isPlayingSlice = playbackSliceIndex.has_value() && *playbackSliceIndex == slice;
            const auto isActiveSlice = activeSliceIndices.contains(slice) || isPlayingSlice;

            if ((slice % 2) == 1)
            {
                g.setColour(juce::Colours::white.withAlpha(0.035f));
                g.fillRect(sliceArea);
            }

            if (isActiveSlice)
            {
                g.setColour(accentColour.withAlpha(0.28f));
                g.fillRect(sliceArea);
                g.setColour(accentColour.withAlpha(0.95f));
                g.drawRoundedRectangle(sliceArea.reduced(2.0f), 7.0f, 1.5f);
            }

            g.setColour(isActiveSlice ? juce::Colours::white : juce::Colours::white.withAlpha(0.78f));
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(juce::String(slice + 1),
                       sliceArea.removeFromTop(18.0f).reduced(5.0f, 0.0f),
                       juce::Justification::centredLeft);

            if (slice > 0)
            {
                g.setColour(accentColour.withAlpha(0.95f));
                g.drawVerticalLine(static_cast<int>(std::round(sliceX)),
                                   static_cast<float>(waveInner.getY()),
                                   static_cast<float>(waveInner.getBottom()));
            }
        }

        g.setColour(accentColour.withAlpha(0.9f));
        g.drawRoundedRectangle(waveInner.toFloat(), 8.0f, 1.2f);

        if (playbackSliceIndex.has_value() && playbackSliceDurationMs > 0.0)
        {
            const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - playbackSliceStartedMs;
            const auto progress = juce::jlimit(0.0, 1.0, elapsedMs / playbackSliceDurationMs);
            const auto sliceStartX = static_cast<float>(waveInner.getX())
                + sliceWidth * static_cast<float>(*playbackSliceIndex);
            const auto playheadX = sliceStartX + sliceWidth * static_cast<float>(progress);

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

    auto keyboardHelp = content.removeFromTop(74).reduced(0, 12);
    g.setColour(juce::Colours::white.withAlpha(0.88f));
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText("Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P [ ] +", keyboardHelp.removeFromTop(24), juce::Justification::centred);
    g.drawText("Z S X D C V G B H N J M , L . ; / '", keyboardHelp.removeFromTop(24), juce::Justification::centred);
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    g.drawText("Arrows: octave left/right, transpose up/down. Z/X stay as playable notes.",
               content.removeFromTop(24),
               juce::Justification::centred);

    auto sliceRow = content.removeFromTop(44).withSizeKeepingCentre(210, 34);
    const std::array<int, 3> sliceCounts { 4, 8, 16 };
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

    auto controlGrid = rightControls;
    auto topKnobs = controlGrid.removeFromTop(108);
    drawKnob(g, topKnobs.removeFromLeft(78), "Transpose", "0 st", 0.5f);
    topKnobs.removeFromLeft(6);
    drawKnob(g, topKnobs.removeFromLeft(78), "Root", "C3", 0.5f);
    topKnobs.removeFromLeft(6);
    drawKnob(g, topKnobs.removeFromLeft(78), "Gain", "0 dB", 0.62f);

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

bool SamplerPanelComponent::hitTest(int x, int y)
{
    return getPanelBounds().contains(x, y);
}

void SamplerPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    auto* track = getActiveTrack();
    if (track != nullptr)
    {
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

        const std::array<int, 3> sliceCounts { 4, 8, 16 };
        for (std::size_t i = 0; i < sliceButtonBounds.size(); ++i)
        {
            if (sliceButtonBounds[i].contains(event.getPosition()))
            {
                releaseTypingPianoNotes();
                track->samplerSliceCount = sliceCounts[i];
                track->samplerMode = SamplerPlaybackMode::slice;
                repaint();
                grabKeyboardFocus();
                return;
            }
        }

        if (samplerWarpButtonBounds.contains(event.getPosition()))
        {
            releaseTypingPianoNotes();
            track->samplerWarpEnabled = ! track->samplerWarpEnabled;
            repaint();
            grabKeyboardFocus();
            return;
        }
    }

    grabKeyboardFocus();
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
            releaseTypingPianoNotes();
            track->samplerTransposeSemitones = juce::jlimit(-24,
                                                                  24,
                                                                  track->samplerTransposeSemitones
                                                                      + (key == juce::KeyPress::upKey ? 1 : -1));
            repaint();
            return true;
        }
    }

    if (pitchForKeyCode(key.getKeyCode()).has_value())
    {
        return updateTypingPianoNotes();
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
    if (! isVisible())
    {
        stopTimer();
        releaseTypingPianoNotes();
    }
}

void SamplerPanelComponent::timerCallback()
{
    if (isVisible())
    {
        updateTypingPianoNotes();

        if (playbackSliceIndex.has_value())
        {
            const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - playbackSliceStartedMs;
            if (playbackSliceDurationMs <= 0.0 || elapsedMs >= playbackSliceDurationMs)
                playbackSliceIndex.reset();

            repaint();
        }
    }
}

juce::Rectangle<int> SamplerPanelComponent::getPanelBounds() const
{
    return getLocalBounds().withTrimmedLeft(14).withTrimmedRight(14).withTrimmedBottom(14);
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

    auto findSlice = [lowerKeyCode, safeSliceCount](const auto& keys) -> std::optional<int>
    {
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (lowerKeyCode == keys[i])
            {
                if (static_cast<int>(i) >= safeSliceCount)
                    return std::nullopt;

                return static_cast<int>(i);
            }
        }

        return std::nullopt;
    };

    if (auto lowerSlice = findSlice(lowerRow))
        return lowerSlice;

    return findSlice(upperRow);
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

bool SamplerPanelComponent::updateTypingPianoNotes()
{
    auto* track = getActiveTrack();
    if (track == nullptr || track->samplerSourcePath.isEmpty())
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

                sliceIndex = *mappedSlice;
                playablePitch = juce::jlimit(0, 127, track->samplerRootMidiNote + sliceIndex);
                noteIdentity = 10000 + keyCode;
                currentlyActiveSlices.insert(sliceIndex);
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
                    startSlicePlaybackIndicator(sliceIndex);
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
    repaint();
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
    repaint();
}
}  // namespace orion
