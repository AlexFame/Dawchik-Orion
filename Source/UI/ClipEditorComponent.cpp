#include "ClipEditorComponent.h"

#include "OrionTheme.h"

#include <cmath>

namespace orion
{
namespace
{
const auto cardRadius = 8.0f;
const auto markerHitRadius = 12;
const auto endMarkerHitRadius = 22;

juce::Colour mutedAccent(juce::Colour colour)
{
    return colour.withSaturation(0.78f).withBrightness(0.92f);
}
}  // namespace

ClipEditorComponent::ClipEditorComponent()
{
    setWantsKeyboardFocus(true);

    for (auto* button : { &warpButton, &keyShiftButton, &normalizeButton })
    {
        button->setColour(juce::TextButton::buttonColourId, theme::core::voidBlack);
        button->setColour(juce::TextButton::buttonOnColourId, theme::warm::red);
        button->setColour(juce::TextButton::textColourOffId, theme::text::primary);
        button->setColour(juce::TextButton::textColourOnId, theme::text::inverse);
        addAndMakeVisible(*button);
    }

    warpButton.setClickingTogglesState(true);
    warpButton.onClick = [this]
    {
        state.warpEnabled = warpButton.getToggleState();
        if (onWarpChanged)
            onWarpChanged(state.warpEnabled);
    };

    keyShiftButton.setClickingTogglesState(true);
    keyShiftButton.onClick = [this]
    {
        state.keyShiftEnabled = keyShiftButton.getToggleState();
        if (onKeyShiftChanged)
            onKeyShiftChanged(state.keyShiftEnabled);
    };

    normalizeButton.onClick = [this]
    {
        if (onNormalize)
            onNormalize();
    };

    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    gainSlider.setRange(-24.0, 12.0, 0.1);
    gainSlider.setColour(juce::Slider::backgroundColourId, theme::core::canvas);
    gainSlider.setColour(juce::Slider::trackColourId, theme::warm::red);
    gainSlider.setColour(juce::Slider::thumbColourId, theme::text::primary);
    gainSlider.onValueChange = [this]
    {
        const auto nextGain = gainSlider.getValue();
        if (std::abs(nextGain - state.gainDb) < 0.001)
            return;

        state.gainDb = nextGain;
        if (onGainChanged)
            onGainChanged(nextGain);
        repaint();
    };
    addAndMakeVisible(gainSlider);
}

void ClipEditorComponent::setState(const ClipEditorState& newState)
{
    if (lastSourcePath != newState.sourcePath)
    {
        lastSourcePath = newState.sourcePath;
        waveformZoom = 1.0;
        waveformViewStart = 0.0;
    }

    state = newState;
    waveformViewStart = clampWaveViewStart(waveformViewStart);
    updateControlsFromState();
    repaint();
}

void ClipEditorComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(10, 8);
    g.setColour(theme::core::voidBlack);
    g.fillRoundedRectangle(bounds.toFloat(), 10.0f);
    g.setColour(theme::line::subtle);
    g.drawRoundedRectangle(bounds.toFloat(), 10.0f, 1.0f);

    bounds.reduce(16, 14);

    if (! state.hasSelection)
    {
        g.setColour(theme::text::muted);
        g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        g.drawText("Select a clip", bounds, juce::Justification::centred);
        return;
    }

    if (! state.isAudioClip)
    {
        g.setColour(theme::text::muted);
        g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        g.drawText("MIDI clip editor is next", bounds, juce::Justification::centred);
        return;
    }

    auto header = bounds.removeFromTop(30);
    g.setColour(theme::text::primary);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText(state.title, header.removeFromLeft(360), juce::Justification::centredLeft);
    g.setColour(theme::text::tertiary);
    g.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    g.drawText(state.trackName, header.removeFromLeft(260), juce::Justification::centredLeft);

    bounds.removeFromTop(10);
    auto info = bounds.removeFromRight(250);
    bounds.removeFromRight(14);
    auto controls = bounds.removeFromBottom(64);
    bounds.removeFromBottom(12);

    drawWaveformPreview(g, bounds);
    drawControlCard(g, controls);
    drawInfoCard(g, info);
}

void ClipEditorComponent::resized()
{
    auto bounds = getLocalBounds().reduced(26, 22);
    if (! state.hasSelection)
    {
        warpButton.setBounds({});
        keyShiftButton.setBounds({});
        normalizeButton.setBounds({});
        transposeDownButton.setBounds({});
        transposeUpButton.setBounds({});
        gainSlider.setBounds({});
        pitchValueBounds = {};
        return;
    }

    bounds.removeFromTop(40);
    auto info = bounds.removeFromRight(250);
    juce::ignoreUnused(info);
    bounds.removeFromRight(14);
    auto controls = bounds.removeFromBottom(64).reduced(16, 12);

    warpButton.setBounds(controls.removeFromLeft(92));
    controls.removeFromLeft(10);
    keyShiftButton.setBounds(controls.removeFromLeft(120));
    controls.removeFromLeft(10);
    normalizeButton.setBounds(controls.removeFromLeft(124));
    controls.removeFromLeft(18);
    pitchValueBounds = controls.removeFromLeft(98).withHeight(42).withY(controls.getY() + 1);
    transposeDownButton.setBounds({});
    transposeUpButton.setBounds({});
    controls.removeFromLeft(18);
    gainSlider.setBounds(controls.removeFromLeft(180).withHeight(28).withY(controls.getY() + 6));
}

void ClipEditorComponent::mouseDown(const juce::MouseEvent& event)
{
    if (state.hasSelection && state.isAudioClip && pitchValueBounds.contains(event.getPosition()))
    {
        grabKeyboardFocus();
        repaint();
        return;
    }

    const auto waveformHitBounds = lastWaveformBounds.expanded(endMarkerHitRadius, 8);
    if (! state.hasSelection || ! state.isAudioClip || ! waveformHitBounds.contains(event.getPosition()))
        return;

    trimmedClipDragCandidate = false;
    trimmedClipDragStarted = false;

    const auto startX = waveRatioToX(state.sampleStartRatio);
    const auto endX = waveRatioToX(state.sampleEndRatio);
    const auto x = event.position.x;
    const auto startDistance = std::abs(x - static_cast<float>(startX));
    const auto endDistance = std::abs(x - static_cast<float>(endX));
    const auto startHit = startDistance <= static_cast<float>(markerHitRadius);
    const auto endHit = endDistance <= static_cast<float>(endMarkerHitRadius);

    if (endHit && (! startHit || endDistance <= startDistance))
    {
        waveDragMode = WaveDragMode::end;
    }
    else if (startHit)
    {
        waveDragMode = WaveDragMode::start;
    }
    else if (endHit)
    {
        waveDragMode = WaveDragMode::end;
    }
    else
    {
        waveDragMode = WaveDragMode::playhead;
        trimmedClipDragCandidate = true;
    }

    updateSampleMarker(waveDragMode, event.x, true);
}

void ClipEditorComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (trimmedClipDragCandidate && ! trimmedClipDragStarted)
    {
        const auto distanceSquared = event.getDistanceFromDragStartX() * event.getDistanceFromDragStartX()
                                   + event.getDistanceFromDragStartY() * event.getDistanceFromDragStartY();
        if (distanceSquared > 64)
        {
            beginTrimmedClipDrag(event);
            trimmedClipDragStarted = true;
            trimmedClipDragCandidate = false;
            waveDragMode = WaveDragMode::none;
            return;
        }
    }

    if (waveDragMode == WaveDragMode::none)
        return;

    updateSampleMarker(waveDragMode, event.x, true);
}

void ClipEditorComponent::mouseUp(const juce::MouseEvent& event)
{
    const auto endedMode = waveDragMode;
    if (endedMode != WaveDragMode::none)
        updateSampleMarker(endedMode, event.x, true);

    waveDragMode = WaveDragMode::none;
    trimmedClipDragCandidate = false;
    trimmedClipDragStarted = false;

    // Marker drag finished → tell the host so it can move the loop and (for a START move)
    // jump playback to the new start, AKAI MPC-style.
    if ((endedMode == WaveDragMode::start || endedMode == WaveDragMode::end) && onSampleRangeFinalized)
        onSampleRangeFinalized(state.sampleStartRatio, state.sampleEndRatio, endedMode == WaveDragMode::start);
}

bool ClipEditorComponent::keyPressed(const juce::KeyPress& key)
{
    if (! state.hasSelection || ! state.isAudioClip || ! hasKeyboardFocus(true))
        return false;

    if (key == juce::KeyPress::upKey)
    {
        setTransposeSemitones(state.transposeSemitones + 1);
        return true;
    }

    if (key == juce::KeyPress::downKey)
    {
        setTransposeSemitones(state.transposeSemitones - 1);
        return true;
    }

    return false;
}

void ClipEditorComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (! state.hasSelection || ! state.isAudioClip || ! lastWaveformBounds.contains(event.getPosition()))
    {
        juce::Component::mouseWheelMove(event, wheel);
        return;
    }

    // Hold Cmd/Ctrl to zoom (for mouse-wheel users); pinch already zooms.
    if (event.mods.isCommandDown() || event.mods.isCtrlDown())
    {
        const auto wheelDelta = std::abs(wheel.deltaY) > std::abs(wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
        if (std::abs(wheelDelta) < 0.001f)
            return;
        zoomWaveformAt(event.x, wheelDelta > 0.0f ? 1.18 : 1.0 / 1.18);
        return;
    }

    // Plain two-finger scroll = pan along the clip (horizontal), like a native
    // trackpad. Either axis pans the timeline since the waveform is 1-D.
    const float panDelta = std::abs(wheel.deltaX) >= std::abs(wheel.deltaY) ? wheel.deltaX : -wheel.deltaY;
    if (std::abs(panDelta) < 0.001f)
        return;
    waveformViewStart = clampWaveViewStart(waveformViewStart - static_cast<double>(panDelta) * visibleWaveSpan());
    repaint();
}

void ClipEditorComponent::mouseMagnify(const juce::MouseEvent& event, float scaleFactor)
{
    if (! state.hasSelection || ! state.isAudioClip || ! lastWaveformBounds.contains(event.getPosition()))
        return;

    const auto zoomFactor = juce::jlimit(0.82, 1.22, static_cast<double>(scaleFactor));
    if (std::abs(zoomFactor - 1.0) < 0.003)
        return;

    zoomWaveformAt(event.x, zoomFactor);
}

void ClipEditorComponent::zoomWaveformAt(int x, double zoomFactor)
{
    const auto oldSpan = visibleWaveSpan();
    const auto localX = juce::jlimit(0.0, 1.0,
        static_cast<double>(x - lastWaveformBounds.getX()) / juce::jmax(1.0, static_cast<double>(lastWaveformBounds.getWidth())));
    const auto anchorRatio = waveformViewStart + localX * oldSpan;

    waveformZoom = juce::jlimit(1.0, 96.0, waveformZoom * zoomFactor);
    const auto newSpan = visibleWaveSpan();
    waveformViewStart = clampWaveViewStart(anchorRatio - localX * newSpan);
    repaint();
}

juce::String ClipEditorComponent::formatBeat(double beat)
{
    return juce::String(beat + 1.0, 2);
}

juce::String ClipEditorComponent::formatDb(double db)
{
    return db <= -59.0 ? "-inf dB" : juce::String(db, 1) + " dB";
}

void ClipEditorComponent::drawWaveformPreview(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    const auto accent = mutedAccent(state.accent);
    g.setColour(theme::surface::primary);
    g.fillRoundedRectangle(bounds.toFloat(), cardRadius);
    g.setColour(theme::line::subtle);
    g.drawRoundedRectangle(bounds.toFloat(), cardRadius, 1.0f);

    auto waveform = bounds.reduced(14, 16);
    const_cast<ClipEditorComponent*>(this)->lastWaveformBounds = waveform;
    g.setColour(theme::core::canvas.withAlpha(0.82f));
    g.fillRoundedRectangle(waveform.toFloat(), 4.0f);
    const auto centreY = static_cast<float>(waveform.getCentreY());
    const auto width = waveform.getWidth();
    const auto visibleStart = waveformViewStart;
    const auto visibleSpan = visibleWaveSpan();

    if (! state.waveformMin.empty() && state.waveformMin.size() == state.waveformMax.size())
    {
        const auto halfH = juce::jmax(2.0f, static_cast<float>(waveform.getHeight()) * 0.46f);
        const auto bucketCount = static_cast<int>(state.waveformMin.size());
        g.setColour(accent.withAlpha(0.82f));
        for (int px = 0; px < width; ++px)
        {
            const auto ratioStart = visibleStart + (static_cast<double>(px) / juce::jmax(1.0, static_cast<double>(width))) * visibleSpan;
            const auto ratioEnd = visibleStart + (static_cast<double>(px + 1) / juce::jmax(1.0, static_cast<double>(width))) * visibleSpan;
            const auto bStart = juce::jlimit(0, bucketCount - 1, static_cast<int>(std::floor(ratioStart * bucketCount)));
            const auto bEnd = juce::jlimit(bStart + 1, bucketCount, static_cast<int>(std::ceil(ratioEnd * bucketCount)));
            float minVal = 0.0f;
            float maxVal = 0.0f;
            for (int b = bStart; b < bEnd && b < bucketCount; ++b)
            {
                minVal = juce::jmin(minVal, state.waveformMin[static_cast<std::size_t>(b)]);
                maxVal = juce::jmax(maxVal, state.waveformMax[static_cast<std::size_t>(b)]);
            }

            const auto x = waveform.getX() + px;
            g.drawVerticalLine(x, centreY + minVal * halfH, centreY + maxVal * halfH);
        }
    }
    else
    {
        juce::Path top;
        juce::Path bottom;
        const auto step = juce::jmax(2, width / 120);
        top.startNewSubPath(static_cast<float>(waveform.getX()), centreY);
        bottom.startNewSubPath(static_cast<float>(waveform.getX()), centreY);

        for (int x = 0; x <= width; x += step)
        {
            const auto phase = static_cast<float>(x) * 0.075f;
            const auto lfo = 0.42f + 0.28f * std::sin(phase) + 0.16f * std::sin(phase * 2.7f + 1.2f);
            const auto amp = juce::jlimit(0.12f, 0.86f, lfo);
            const auto y = amp * static_cast<float>(waveform.getHeight()) * 0.42f;
            const auto px = static_cast<float>(waveform.getX() + x);
            top.lineTo(px, centreY - y);
            bottom.lineTo(px, centreY + y);
        }

        g.setColour(accent.withAlpha(0.58f));
        g.strokePath(top, juce::PathStrokeType(2.0f));
        g.strokePath(bottom, juce::PathStrokeType(2.0f));
    }

    const auto startX = waveRatioToX(state.sampleStartRatio);
    const auto endX = waveRatioToX(state.sampleEndRatio);
    const auto visibleStartX = juce::jlimit(waveform.getX(), waveform.getRight(), startX);
    const auto visibleEndX = juce::jlimit(waveform.getX(), waveform.getRight(), endX);
    if (visibleEndX > visibleStartX)
    {
        auto activeRange = waveform.withX(visibleStartX).withRight(visibleEndX);
        g.setColour(accent.withAlpha(0.15f));
        g.fillRoundedRectangle(activeRange.toFloat(), 3.0f);
    }

    g.setColour(theme::core::voidBlack.withAlpha(0.55f));
    if (startX > waveform.getX())
        g.fillRect(waveform.withRight(juce::jmin(startX, waveform.getRight())));
    if (endX < waveform.getRight())
        g.fillRect(waveform.withX(juce::jmax(endX, waveform.getX())));

    const auto drawMarker = [&](int x, const juce::String& label)
    {
        if (x < waveform.getX() || x > waveform.getRight())
            return;

        g.setColour(theme::warm::red.withAlpha(0.92f));
        g.drawVerticalLine(x, static_cast<float>(waveform.getY()), static_cast<float>(waveform.getBottom()));
        juce::Path flag;
        flag.addTriangle(static_cast<float>(x - 7), static_cast<float>(waveform.getY()),
                         static_cast<float>(x + 7), static_cast<float>(waveform.getY()),
                         static_cast<float>(x), static_cast<float>(waveform.getY() + 10));
        g.fillPath(flag);
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText(label, x + 5, waveform.getY() + 4, 42, 14, juce::Justification::centredLeft);
    };

    if (state.lengthInBeats > 0.0)
    {
        const auto playheadRatio = juce::jlimit(0.0, 1.0, state.previewSourceRatio);
        const auto playheadX = waveRatioToX(playheadRatio);
        if (waveform.contains(playheadX, waveform.getCentreY()))
        {
            g.setColour(theme::cool::cyan.withAlpha(state.playing ? 0.95f : 0.42f));
            g.drawVerticalLine(playheadX, static_cast<float>(waveform.getY()), static_cast<float>(waveform.getBottom()));
        }
    }

    drawMarker(startX, "START");
    drawMarker(endX, "END");
}

void ClipEditorComponent::drawInfoCard(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    g.setColour(theme::surface::primary);
    g.fillRoundedRectangle(bounds.toFloat(), cardRadius);
    g.setColour(theme::line::subtle);
    g.drawRoundedRectangle(bounds.toFloat(), cardRadius, 1.0f);

    auto area = bounds.reduced(18, 14);
    g.setColour(theme::text::primary);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText("CLIP INFO", area.removeFromTop(20), juce::Justification::centredLeft);

    const auto drawRow = [&](const juce::String& label, const juce::String& value)
    {
        auto row = area.removeFromTop(22);
        g.setColour(theme::text::muted);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(label, row.removeFromLeft(78), juce::Justification::centredLeft);
        g.setColour(theme::text::secondary);
        g.setFont(juce::FontOptions(12.0f, juce::Font::plain));
        g.drawText(value, row, juce::Justification::centredRight);
    };

    drawRow("START", formatBeat(state.startBeat));
    drawRow("LENGTH", juce::String(state.lengthInBeats, 2));
    drawRow("GAIN", formatDb(state.gainDb));
    drawRow("PITCH", juce::String(state.transposeSemitones >= 0 ? "+" : "") + juce::String(state.transposeSemitones) + " st");
    drawRow("BPM", state.sourceBpm > 0.0 ? juce::String(state.sourceBpm, 1) : "-");
    drawRow("BARS", state.detectedBars > 0 ? juce::String(state.detectedBars) : "-");
    drawRow("FILE", state.fileName.isNotEmpty() ? state.fileName : "-");
}

void ClipEditorComponent::drawControlCard(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    g.setColour(theme::surface::primary);
    g.fillRoundedRectangle(bounds.toFloat(), cardRadius);
    g.setColour(theme::line::subtle);
    g.drawRoundedRectangle(bounds.toFloat(), cardRadius, 1.0f);

    auto gainLabel = bounds.reduced(16, 12);
    gainLabel.removeFromLeft(92 + 10 + 120 + 10 + 124 + 18);
    auto pitchLabel = gainLabel.removeFromLeft(98);
    const auto pitchBox = pitchLabel.toFloat().reduced(1.0f, 2.0f);
    g.setColour(hasKeyboardFocus(true) ? theme::warm::red.withAlpha(0.16f) : theme::core::canvas.withAlpha(0.48f));
    g.fillRoundedRectangle(pitchBox, 7.0f);
    g.setColour(hasKeyboardFocus(true) ? theme::warm::red.withAlpha(0.88f) : theme::line::subtle);
    g.drawRoundedRectangle(pitchBox, 7.0f, hasKeyboardFocus(true) ? 1.4f : 1.0f);
    auto pitchText = pitchLabel.reduced(6, 4);
    g.setColour(theme::text::muted);
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    g.drawText("PITCH", pitchText.removeFromTop(13), juce::Justification::centred);
    g.setColour(theme::text::primary);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(juce::String(state.transposeSemitones >= 0 ? "+" : "") + juce::String(state.transposeSemitones) + " st",
               pitchText,
               juce::Justification::centred);
    gainLabel.removeFromLeft(18);
    gainLabel.removeFromTop(34);
    g.setColour(theme::text::secondary);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(formatDb(state.gainDb), gainLabel.removeFromLeft(80), juce::Justification::centredLeft);
}

double ClipEditorComponent::xToWaveRatio(int x) const noexcept
{
    if (lastWaveformBounds.getWidth() <= 0)
        return 0.0;

    const auto local = juce::jlimit(0.0, 1.0, static_cast<double>(x - lastWaveformBounds.getX()) / static_cast<double>(lastWaveformBounds.getWidth()));
    return juce::jlimit(0.0, 1.0, waveformViewStart + local * visibleWaveSpan());
}

int ClipEditorComponent::waveRatioToX(double ratio) const noexcept
{
    if (lastWaveformBounds.getWidth() <= 0)
        return 0;

    const auto visibleRatio = (juce::jlimit(0.0, 1.0, ratio) - waveformViewStart) / visibleWaveSpan();
    return lastWaveformBounds.getX() + static_cast<int>(std::round(visibleRatio * lastWaveformBounds.getWidth()));
}

double ClipEditorComponent::visibleWaveSpan() const noexcept
{
    return juce::jlimit(1.0 / 64.0, 1.0, 1.0 / juce::jmax(1.0, waveformZoom));
}

double ClipEditorComponent::clampWaveViewStart(double start) const noexcept
{
    const auto span = visibleWaveSpan();
    return juce::jlimit(0.0, juce::jmax(0.0, 1.0 - span), start);
}

void ClipEditorComponent::updateSampleMarker(WaveDragMode mode, int x, bool shouldSeek)
{
    const auto ratio = xToWaveRatio(x);

    if (mode == WaveDragMode::start)
    {
        state.sampleStartRatio = juce::jlimit(0.0, state.sampleEndRatio - 0.001, ratio);
        if (onSampleRangeChanged)
            onSampleRangeChanged(state.sampleStartRatio, state.sampleEndRatio);
    }
    else if (mode == WaveDragMode::end)
    {
        state.sampleEndRatio = juce::jlimit(state.sampleStartRatio + 0.001, 1.0, ratio);
        if (onSampleRangeChanged)
            onSampleRangeChanged(state.sampleStartRatio, state.sampleEndRatio);
    }

    // Scrub-to-marker only makes sense while STOPPED. During playback, seeking on every
    // marker move yanks the playhead to the marker each frame while the playback timer
    // pulls it back to the real position — that fight is what made the line flicker (and
    // scrubbed the audio). Leave the playing playhead alone.
    if (shouldSeek && onPreviewSeek && state.lengthInBeats > 0.0 && ! state.playing)
        onPreviewSeek(ratio);

    repaint();
}

void ClipEditorComponent::setTransposeSemitones(int semitones)
{
    const auto nextValue = juce::jlimit(-24, 24, semitones);
    if (nextValue == state.transposeSemitones)
        return;

    state.transposeSemitones = nextValue;
    if (onTransposeChanged)
        onTransposeChanged(state.transposeSemitones);
    repaint();
}

void ClipEditorComponent::beginTrimmedClipDrag(const juce::MouseEvent& event)
{
    if (state.sourcePath.isEmpty())
        return;

    auto* dragContainer = juce::DragAndDropContainer::findParentDragContainerFor(this);
    if (dragContainer == nullptr)
        return;

    auto payload = juce::var(new juce::DynamicObject());
    auto* payloadObject = payload.getDynamicObject();
    payloadObject->setProperty("type", "clip-editor-audio");
    payloadObject->setProperty("name", state.title);
    payloadObject->setProperty("category", "Audio");
    payloadObject->setProperty("subtitle", state.fileName);
    payloadObject->setProperty("colour", static_cast<int>(state.accent.getARGB()));
    // Full source length (not the trimmed clip length) so the dropped region keeps the
    // original speed; fall back to lengthInBeats for safety.
    const auto fullSourceBeats = state.sourceLengthBeats > 0.0 ? state.sourceLengthBeats : state.lengthInBeats;
    payloadObject->setProperty("sourceLengthBeats", fullSourceBeats);
    payloadObject->setProperty("lengthBeats", fullSourceBeats * juce::jmax(0.001, state.sampleEndRatio - state.sampleStartRatio));
    payloadObject->setProperty("path", state.sourcePath);
    payloadObject->setProperty("sampleStartRatio", state.sampleStartRatio);
    payloadObject->setProperty("sampleEndRatio", state.sampleEndRatio);
    payloadObject->setProperty("gainDb", state.gainDb);
    payloadObject->setProperty("transposeSemitones", state.transposeSemitones);
    payloadObject->setProperty("warpEnabled", state.warpEnabled);
    payloadObject->setProperty("keyShiftEnabled", state.keyShiftEnabled);

    auto dragImage = juce::Image(juce::Image::ARGB, 220, 42, true);
    juce::Graphics g(dragImage);
    const auto accent = mutedAccent(state.accent);
    g.setColour(accent.withAlpha(0.95f));
    g.fillRoundedRectangle(dragImage.getBounds().toFloat(), 8.0f);
    g.setColour(accent.darker(0.42f).withAlpha(0.76f));
    g.fillRect(8, 23, dragImage.getWidth() - 16, 9);
    g.setColour(theme::text::primary);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(state.title, dragImage.getBounds().reduced(10, 0), juce::Justification::centredLeft, true);

    dragContainer->startDragging(payload, this, juce::ScaledImage(dragImage), true, nullptr, &event.source);
}

void ClipEditorComponent::setTransportButtonStyle(juce::Button& button)
{
    button.setColour(juce::TextButton::buttonColourId, theme::surface::elevated);
    button.setColour(juce::TextButton::buttonOnColourId, theme::warm::red);
    button.setColour(juce::TextButton::textColourOffId, theme::text::primary);
    button.setColour(juce::TextButton::textColourOnId, theme::text::inverse);
}

void ClipEditorComponent::updateControlsFromState()
{
    warpButton.setVisible(state.hasSelection && state.isAudioClip);
    keyShiftButton.setVisible(state.hasSelection && state.isAudioClip);
    normalizeButton.setVisible(state.hasSelection && state.isAudioClip);
    transposeDownButton.setVisible(false);
    transposeUpButton.setVisible(false);
    gainSlider.setVisible(state.hasSelection && state.isAudioClip);

    warpButton.setToggleState(state.warpEnabled, juce::dontSendNotification);
    keyShiftButton.setToggleState(state.keyShiftEnabled, juce::dontSendNotification);
    gainSlider.setValue(state.gainDb, juce::dontSendNotification);
}
}  // namespace orion
