#include "ClipEditorComponent.h"

#include "OrionTheme.h"

#include <algorithm>
#include <cmath>

namespace orion
{
namespace
{
const auto cardRadius = theme::metrics::panelRadius;
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
        // No full repaint here — the slider draws itself and the GAIN readout refreshes on the timer.
        // A full waveform repaint per drag tick is what made changing the level feel sluggish.
    };
    addAndMakeVisible(gainSlider);
}

void ClipEditorComponent::setState(const ClipEditorState& newState)
{
    // A live warp-marker drag owns the state — don't let the 60 Hz timer refresh clobber the marker
    // back to the clip's stored position (that was the flicker / snap-back while dragging).
    if (waveDragMode == WaveDragMode::warpMarker)
        return;

    const bool sourceChanged = (lastSourcePath != newState.sourcePath);
    if (sourceChanged)
    {
        lastSourcePath = newState.sourcePath;
        waveformZoom = 1.0;
        targetWaveformZoom = 1.0;
        waveformViewStart = 0.0;
        stopTimer();
    }
    // Recompute onsets when the displayed source changes (waveform arrives via setState).
    const bool needTransients = sourceChanged
        || (transientRatios.empty() && newState.waveform != nullptr && ! newState.waveform->maxValues.empty());

    // Fast path when nothing that affects the WAVEFORM changed (source/trim/warp/markers). The playhead
    // and the control readouts (gain/pitch) can still differ — those get cheap targeted repaints, never
    // a full waveform redraw. This is what keeps the playhead smooth AND makes dragging the gain instant
    // (a full waveform repaint per drag tick made changing the level feel sluggish).
    const bool waveformUnchanged =
        ! sourceChanged
        && state.hasSelection == newState.hasSelection
        && state.isAudioClip == newState.isAudioClip
        && state.sampleStartRatio == newState.sampleStartRatio
        && state.sampleEndRatio == newState.sampleEndRatio
        && state.warpEnabled == newState.warpEnabled
        && state.keyShiftEnabled == newState.keyShiftEnabled
        && state.sourceLengthBeats == newState.sourceLengthBeats
        && state.lengthInBeats == newState.lengthInBeats
        && state.warpMarkers.size() == newState.warpMarkers.size()
        && std::equal(state.warpMarkers.begin(), state.warpMarkers.end(), newState.warpMarkers.begin(),
                      [](const WarpMarker& a, const WarpMarker& b)
                      { return a.sourceRatio == b.sourceRatio && a.beat == b.beat; })
        && ! lastWaveformBounds.isEmpty();

    if (waveformUnchanged)
    {
        const bool controlsUnchanged = state.gainDb == newState.gainDb
            && state.transposeSemitones == newState.transposeSemitones
            && state.autoKeyShiftSemitones == newState.autoKeyShiftSemitones
            && state.autoKeyShiftActive == newState.autoKeyShiftActive
            && state.title == newState.title;

        const int oldX = state.playheadIsBeatTime ? beatNormToX(state.previewSourceRatio)
                                                  : waveRatioToX(juce::jlimit(0.0, 1.0, state.previewSourceRatio));
        state = newState;

        if (! controlsUnchanged)
        {
            updateControlsFromState();      // sync the gain slider / pitch (cheap)
            repaint(lastControlsBounds);    // redraw only the control + info cards, not the waveform
            repaint(lastInfoBounds);
        }

        const int newX = newState.playheadIsBeatTime ? beatNormToX(newState.previewSourceRatio)
                                                     : waveRatioToX(juce::jlimit(0.0, 1.0, newState.previewSourceRatio));
        const int lo = juce::jmin(oldX, newX) - 3;
        const int hi = juce::jmax(oldX, newX) + 3;
        repaint(lo, lastWaveformBounds.getY(), hi - lo, lastWaveformBounds.getHeight());
        return;
    }

    state = newState;
    if (needTransients)
        recomputeTransients();
    rebuildWarpMap();
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
        g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
        g.drawText("Select a clip", bounds, juce::Justification::centred);
        return;
    }

    if (! state.isAudioClip)
    {
        g.setColour(theme::text::muted);
        g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
        g.drawText("MIDI clip editor is next", bounds, juce::Justification::centred);
        return;
    }

    auto header = bounds.removeFromTop(30);
    g.setColour(theme::text::primary);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText(state.title, header.removeFromLeft(360), juce::Justification::centredLeft);
    g.setColour(theme::text::tertiary);
    g.setFont(juce::FontOptions(15.0f, juce::Font::plain));
    g.drawText(state.trackName, header.removeFromLeft(260), juce::Justification::centredLeft);

    bounds.removeFromTop(10);
    auto info = bounds.removeFromRight(250);
    bounds.removeFromRight(14);
    auto controls = bounds.removeFromBottom(64);
    bounds.removeFromBottom(12);

    const_cast<ClipEditorComponent*>(this)->lastControlsBounds = controls;
    const_cast<ClipEditorComponent*>(this)->lastInfoBounds = info;

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

void ClipEditorComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (state.hasSelection && state.isAudioClip && pitchValueBounds.contains(event.getPosition()))
    {
        beginPitchTextEntry();
        return;
    }

    // Warp editing: double-click a marker deletes it; double-click the waveform adds one that pins the
    // clicked source point to the nearest grid beat (quantise a transient to the grid, Ableton-style).
    if (state.warpEnabled && state.isAudioClip && lastWaveformBounds.expanded(0, 8).contains(event.getPosition()))
    {
        // Live's rules, verbatim from the manual:
        //   "Double-click anywhere in the upper half of the Sample Editor to add a Warp Marker at
        //    that location."
        //   "To delete Warp Markers you can double-click them."
        // So both gestures share the upper strip and are told apart purely by whether you hit an
        // existing marker. That makes the delete radius critical: the old 12 px grab radius (fine
        // for dragging) swallowed every nearby create, so markers could not be placed close
        // together and placement felt random. Use a tight radius for the delete test only.
        constexpr int deleteHitRadiusPx = 5;
        if (event.position.y > static_cast<float>(lastWaveformBounds.getCentreY()))
            return;   // lower half is not the warp-marker area in Live

        if (const int wm = warpMarkerAtX(static_cast<int>(event.position.x), deleteHitRadiusPx); wm >= 0)
        {
            if (onWarpMarkerRemoved)
                onWarpMarkerRemoved(wm);
        }
        else
        {
            // Live's rule: "Double-click anywhere in the upper half of the Sample Editor to add a
            // Warp Marker at that location" — the marker goes exactly where you click. Transients
            // are a separate affordance: hovering directly over one shows a pseudo-marker you can
            // activate. The old 14 px magnet pulled the marker visibly away from the cursor, which
            // is not what Live does and made precise placement impossible.
            double sr = juce::jlimit(0.001, 0.999, xToWaveRatio(event.x));
            const double maxDist = (4.0 / juce::jmax(1, lastWaveformBounds.getWidth())) * visibleWaveSpan();
            if (const double t = nearestTransient(sr, maxDist); t >= 0.0)
                sr = juce::jlimit(0.001, 0.999, t);   // clicked essentially ON a transient
            // Pin the point at the beat it ALREADY sits on (no grid snap) so adding a marker never shifts
            // the audio — Ableton-style. Warping only happens when the marker is then dragged.
            if (onWarpMarkerAdded)
                onWarpMarkerAdded(sr, srcRatioToBeat(sr));
        }
    }
}

void ClipEditorComponent::beginPitchTextEntry()
{
    pitchEditor = std::make_unique<juce::TextEditor>();
    pitchEditor->setBounds(pitchValueBounds.withTrimmedTop(16).reduced(4, 2));
    pitchEditor->setJustification(juce::Justification::centred);
    pitchEditor->setFont(juce::FontOptions(17.0f, juce::Font::bold));
    pitchEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    pitchEditor->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    pitchEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    pitchEditor->setInputRestrictions(4, "-0123456789");
    pitchEditor->setText(juce::String(state.transposeSemitones), juce::dontSendNotification);
    pitchEditor->selectAll();
    pitchEditor->onReturnKey = [this] { commitPitchTextEntry(); };
    pitchEditor->onEscapeKey = [this] { pitchEditor.reset(); repaint(); };
    pitchEditor->onFocusLost = [this] { commitPitchTextEntry(); };
    addAndMakeVisible(*pitchEditor);
    pitchEditor->grabKeyboardFocus();
    repaint();
}

void ClipEditorComponent::commitPitchTextEntry()
{
    if (pitchEditor == nullptr)
        return;
    const auto text = pitchEditor->getText().trim();
    pitchEditor.reset();   // remove before applying so the focus-lost callback can't re-enter
    if (text.isNotEmpty())
        setTransposeSemitones(juce::jlimit(-24, 24, text.getIntValue()));
    grabKeyboardFocus();
    repaint();
}

void ClipEditorComponent::mouseMove(const juce::MouseEvent& event)
{
    const bool active = state.warpEnabled && state.isAudioClip
                        && lastWaveformBounds.expanded(0, 14).contains(event.getPosition());
    const int wm = active ? warpMarkerAtX(static_cast<int>(event.position.x)) : -1;

    // No existing marker under the cursor → show a grey "ghost" marker at the nearest transient, the
    // candidate a double-click would activate (Ableton shows exactly this on hover).
    double cand = -1.0;
    if (active && wm < 0)
    {
        const double sr = xToWaveRatio(event.x);
        // Only when the cursor is essentially on the transient, like Live's pseudo-warp marker.
        // A wide magnet here drew the ghost far from the pointer, which read as "the marker
        // lands somewhere else".
        const double maxDist = (6.0 / juce::jmax(1, lastWaveformBounds.getWidth())) * visibleWaveSpan();
        cand = nearestTransient(sr, maxDist);
    }

    if (wm != hoveredWarpMarker || std::abs(cand - hoverCandidateSourceRatio) > 1.0e-9)
    {
        hoveredWarpMarker = wm;
        hoverCandidateSourceRatio = cand;
        setMouseCursor(wm >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                               : (cand >= 0.0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor));
        repaint();
    }
}

void ClipEditorComponent::mouseExit(const juce::MouseEvent&)
{
    if (hoveredWarpMarker != -1 || hoverCandidateSourceRatio >= 0.0)
    {
        hoveredWarpMarker = -1;
        hoverCandidateSourceRatio = -1.0;
        repaint();
    }
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
    snapBypass = event.mods.isAltDown();

    // Grabbing a warp-marker handle takes priority (it sits mid-waveform, away from START/END).
    if (const int wm = warpMarkerAtX(static_cast<int>(event.position.x)); wm >= 0)
    {
        activeWarpMarker = wm;
        waveDragMode = WaveDragMode::warpMarker;
        return;
    }

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

    snapBypass = event.mods.isAltDown();

    // Dragging a warp marker moves which source point is pinned to its beat (visual only until release;
    // rebuilding the warped preview every frame would be too heavy). Beat stays fixed.
    if (waveDragMode == WaveDragMode::warpMarker)
    {
        if (activeWarpMarker >= 0 && activeWarpMarker < static_cast<int>(state.warpMarkers.size()))
        {
            // Move the marker along the BEAT ruler; its pinned source point stays, so the audio around it
            // stretches (Ableton warp). Snap the beat to the grid unless Alt is held.
            double beat = juce::jmax(0.0, xToBeatNorm(event.x) * warpTotalBeats);
            if (! snapBypass)
            {
                const double step = currentGridStepBeats();
                if (step > 0.0)
                    beat = std::round(beat / step) * step;
            }
            // Keep the marker strictly between its source-neighbours' beats so the map stays monotonic.
            double lo = 0.001, hi = juce::jmax(0.002, warpTotalBeats - 0.001);
            for (int j = 0; j < static_cast<int>(state.warpMarkers.size()); ++j)
            {
                if (j == activeWarpMarker) continue;
                const auto& o = state.warpMarkers[static_cast<std::size_t>(j)];
                if (o.sourceRatio < state.warpMarkers[static_cast<std::size_t>(activeWarpMarker)].sourceRatio)
                    lo = juce::jmax(lo, o.beat + 0.001);
                else
                    hi = juce::jmin(hi, o.beat - 0.001);
            }
            state.warpMarkers[static_cast<std::size_t>(activeWarpMarker)].beat = juce::jlimit(lo, juce::jmax(lo, hi), beat);
            rebuildWarpMap();   // live: waveform + grid restretch under the drag
            repaint(lastWaveformBounds.expanded(18, 28));
        }
        return;
    }

    updateSampleMarker(waveDragMode, event.x, true);
}

void ClipEditorComponent::mouseUp(const juce::MouseEvent& event)
{
    const auto endedMode = waveDragMode;

    // Commit a warp-marker move to the host (which rebuilds the warped preview). Reset the drag state
    // BEFORE the callback so the refresh it triggers isn't blocked by the in-drag guard in setState.
    if (endedMode == WaveDragMode::warpMarker)
    {
        const int idx = activeWarpMarker;
        const double beat = (idx >= 0 && idx < static_cast<int>(state.warpMarkers.size()))
            ? state.warpMarkers[static_cast<std::size_t>(idx)].beat : -1.0;
        activeWarpMarker = -1;
        waveDragMode = WaveDragMode::none;
        if (beat >= 0.0 && onWarpMarkerMoved)
            onWarpMarkerMoved(idx, beat);
        return;
    }

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
    repaint(lastWaveformBounds.expanded(18, 28));
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
    const auto localX = juce::jlimit(0.0, 1.0,
        static_cast<double>(x - lastWaveformBounds.getX()) / juce::jmax(1.0, static_cast<double>(lastWaveformBounds.getWidth())));
    // Remember the point under the cursor and set a zoom target; the timer eases toward it and keeps
    // this point pinned, so the zoom feels organic instead of jumping a notch per event.
    zoomAnchorLocalX = localX;
    zoomAnchorRatio = waveformViewStart + localX * visibleWaveSpan();
    targetWaveformZoom = juce::jlimit(1.0, 96.0, targetWaveformZoom * zoomFactor);
    if (! isTimerRunning())
        startTimerHz(90);
    return;
}

void ClipEditorComponent::timerCallback()
{
    // Critically-damped-ish ease toward the target zoom, re-pinning the anchor each frame.
    const double diff = targetWaveformZoom - waveformZoom;
    if (std::abs(diff) < waveformZoom * 0.002)
    {
        waveformZoom = targetWaveformZoom;
        stopTimer();
    }
    else
    {
        waveformZoom += diff * 0.30;
    }
    waveformViewStart = clampWaveViewStart(zoomAnchorRatio - zoomAnchorLocalX * visibleWaveSpan());
    repaint(lastWaveformBounds.expanded(18, 28));
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

    if (state.waveform != nullptr
        && ! state.waveform->minValues.empty()
        && state.waveform->minValues.size() == state.waveform->maxValues.size())
    {
        const auto halfH = juce::jmax(2.0f, static_cast<float>(waveform.getHeight()) * 0.46f);
        const auto bucketCount = static_cast<int>(state.waveform->minValues.size());
        g.setColour(accent.withAlpha(0.82f));
        for (int px = 0; px < width; ++px)
        {
            // x is BEAT time → map each column's beat span to the source span it plays (warp view).
            const auto beatNormStart = visibleStart + (static_cast<double>(px) / juce::jmax(1.0, static_cast<double>(width))) * visibleSpan;
            const auto beatNormEnd = visibleStart + (static_cast<double>(px + 1) / juce::jmax(1.0, static_cast<double>(width))) * visibleSpan;
            const auto ratioStart = beatToSrcRatio(beatNormStart * warpTotalBeats);
            const auto ratioEnd = beatToSrcRatio(beatNormEnd * warpTotalBeats);
            const auto bStart = juce::jlimit(0, bucketCount - 1, static_cast<int>(std::floor(ratioStart * bucketCount)));
            const auto bEnd = juce::jlimit(bStart + 1, bucketCount, static_cast<int>(std::ceil(ratioEnd * bucketCount)));
            float minVal = 0.0f;
            float maxVal = 0.0f;
            for (int b = bStart; b < bEnd && b < bucketCount; ++b)
            {
                minVal = juce::jmin(minVal, state.waveform->minValues[static_cast<std::size_t>(b)]);
                maxVal = juce::jmax(maxVal, state.waveform->maxValues[static_cast<std::size_t>(b)]);
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

    // Beat / bar grid over the waveform so START/END read against musical positions (Ableton-style).
    if (const double totalBeats = state.sourceLengthBeats > 0.0 ? state.sourceLengthBeats : state.lengthInBeats;
        totalBeats > 0.0 && totalBeats < 100000.0)
    {
        const double step = currentGridStepBeats();
        constexpr int beatsPerBar = 4;
        if (step > 0.0)
        {
            for (double beat = 0.0; beat <= totalBeats + 0.001; beat += step)
            {
                // The axis IS beat time, so beat lines are uniform; the warped waveform slides under them.
                const int x = beatNormToX(beat / totalBeats);
                if (x < waveform.getX() - 1 || x > waveform.getRight() + 1)
                    continue;
                const double inBar = std::fmod(beat, static_cast<double>(beatsPerBar));
                const bool onBar = inBar < 0.001 || (beatsPerBar - inBar) < 0.001;
                g.setColour(juce::Colours::white.withAlpha(onBar ? 0.18f : 0.07f));
                g.drawVerticalLine(x, static_cast<float>(waveform.getY()), static_cast<float>(waveform.getBottom()));
                if (onBar)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.5f));
                    g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
                    g.drawText(juce::String(static_cast<int>(std::round(beat / beatsPerBar)) + 1),
                               x + 3, waveform.getBottom() - 13, 26, 11, juce::Justification::bottomLeft);
                }
            }
        }
    }

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

    // Only while playing — a stopped playhead line sitting next to a just-placed warp marker read as a
    // confusing stray "stripe".
    if (state.playing && state.lengthInBeats > 0.0)
    {
        const auto playheadRatio = juce::jlimit(0.0, 1.0, state.previewSourceRatio);
        // Preview playback reports position in warped output (beat) time → straight onto the beat axis;
        // otherwise it's a source ratio that must go through the warp map.
        const auto playheadX = state.playheadIsBeatTime ? beatNormToX(playheadRatio) : waveRatioToX(playheadRatio);
        if (waveform.contains(playheadX, waveform.getCentreY()))
        {
            g.setColour(theme::cool::cyan.withAlpha(0.95f));
            g.drawVerticalLine(playheadX, static_cast<float>(waveform.getY()), static_cast<float>(waveform.getBottom()));
        }
    }

    // Warp markers. Only shown when warp is on.
    if (state.warpEnabled)
    {
        // A warp flag: vertical line + a pennant cap ABOVE the waveform (the grab handle).
        const auto drawWarpFlag = [&](int x, juce::Colour colour, bool big)
        {
            if (x < waveform.getX() || x > waveform.getRight())
                return;
            g.setColour(colour);
            g.drawVerticalLine(x, static_cast<float>(waveform.getY()), static_cast<float>(waveform.getBottom()));
            const float fx = static_cast<float>(x);
            const float tipY = static_cast<float>(waveform.getY());
            const float capTop = tipY - (big ? 14.0f : 10.0f);
            const float capW = big ? 7.0f : 5.0f;
            juce::Path flag;
            flag.startNewSubPath(fx - capW, capTop);
            flag.lineTo(fx + capW, capTop);
            flag.lineTo(fx + capW, tipY - 5.0f);
            flag.lineTo(fx, tipY);                 // point down onto the waveform
            flag.lineTo(fx - capW, tipY - 5.0f);
            flag.closeSubPath();
            g.fillPath(flag);
        };

        // Faint transient ticks — the candidate positions a marker snaps to.
        for (const double t : transientRatios)
        {
            const int tx = waveRatioToX(t);
            if (tx < waveform.getX() || tx > waveform.getRight())
                continue;
            g.setColour(juce::Colours::white.withAlpha(0.20f));
            g.drawVerticalLine(tx, static_cast<float>(waveform.getY()), static_cast<float>(waveform.getY() + 6));
        }

        // Grey "ghost" marker under the cursor (double-click to activate it) — the Ableton hover cue.
        if (hoverCandidateSourceRatio >= 0.0)
            drawWarpFlag(waveRatioToX(hoverCandidateSourceRatio), juce::Colours::white.withAlpha(0.55f), true);

        // Active (amber) markers, pinned to their source point.
        for (int i = 0; i < static_cast<int>(state.warpMarkers.size()); ++i)
        {
            const auto& m = state.warpMarkers[static_cast<std::size_t>(i)];
            const int x = waveRatioToX(juce::jlimit(0.0, 1.0, m.sourceRatio));
            const bool hot = (i == hoveredWarpMarker || i == activeWarpMarker);
            drawWarpFlag(x, juce::Colour(0xffffc24d).withAlpha(hot ? 1.0f : 0.9f), hot);
            if (hot && x >= waveform.getX() && x <= waveform.getRight())
            {
                g.setColour(juce::Colours::black.withAlpha(0.85f));
                g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
                g.drawText(juce::String(m.beat + 1.0, 1), x + 9, waveform.getY() - 15, 40, 13,
                           juce::Justification::centredLeft);
            }
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
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    g.drawText("CLIP INFO", area.removeFromTop(20), juce::Justification::centredLeft);

    const auto drawRow = [&](const juce::String& label, const juce::String& value)
    {
        auto row = area.removeFromTop(22);
        g.setColour(theme::text::muted);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(label, row.removeFromLeft(78), juce::Justification::centredLeft);
        g.setColour(theme::text::secondary);
        g.setFont(juce::FontOptions(14.0f, juce::Font::plain));
        g.drawText(value, row, juce::Justification::centredRight);
    };

    drawRow("START", formatBeat(state.startBeat));
    drawRow("LENGTH", juce::String(state.lengthInBeats, 2));
    drawRow("GAIN", formatDb(state.gainDb));
    drawRow("PITCH", juce::String(state.transposeSemitones >= 0 ? "+" : "") + juce::String(state.transposeSemitones) + " st");
    drawRow("KEY SHIFT", state.autoKeyShiftActive
                             ? juce::String(state.autoKeyShiftSemitones >= 0 ? "+" : "") + juce::String(state.autoKeyShiftSemitones) + " st"
                             : juce::String("off"));
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
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText("PITCH", pitchText.removeFromTop(13), juce::Justification::centred);
    if (pitchEditor == nullptr)   // hidden while the inline numeric editor is shown
    {
        g.setColour(theme::text::primary);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(juce::String(state.transposeSemitones >= 0 ? "+" : "") + juce::String(state.transposeSemitones) + " st",
                   pitchText,
                   juce::Justification::centred);
    }
    gainLabel.removeFromLeft(18);
    gainLabel.removeFromTop(34);
    g.setColour(theme::text::secondary);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(formatDb(state.gainDb), gainLabel.removeFromLeft(80), juce::Justification::centredLeft);
}

void ClipEditorComponent::rebuildWarpMap()
{
    warpTotalBeats = state.sourceLengthBeats > 0.0 ? state.sourceLengthBeats : state.lengthInBeats;
    warpMap = warpControlPoints(state.warpMarkers, juce::jmax(1.0e-6, warpTotalBeats));
}

double ClipEditorComponent::srcRatioToBeat(double sourceRatio) const noexcept
{
    if (warpMap.size() < 2)
        return juce::jlimit(0.0, 1.0, sourceRatio) * warpTotalBeats;
    sourceRatio = juce::jlimit(0.0, 1.0, sourceRatio);
    for (std::size_t i = 1; i < warpMap.size(); ++i)
        if (sourceRatio <= warpMap[i].sourceRatio)
        {
            const auto span = juce::jmax(1.0e-9, warpMap[i].sourceRatio - warpMap[i - 1].sourceRatio);
            const auto t = (sourceRatio - warpMap[i - 1].sourceRatio) / span;
            return warpMap[i - 1].beat + t * (warpMap[i].beat - warpMap[i - 1].beat);
        }
    return warpMap.back().beat;
}

double ClipEditorComponent::beatToSrcRatio(double beat) const noexcept
{
    if (warpMap.size() < 2)
        return warpTotalBeats > 0.0 ? juce::jlimit(0.0, 1.0, beat / warpTotalBeats) : 0.0;
    beat = juce::jlimit(0.0, warpMap.back().beat, beat);
    for (std::size_t i = 1; i < warpMap.size(); ++i)
        if (beat <= warpMap[i].beat)
        {
            const auto span = juce::jmax(1.0e-9, warpMap[i].beat - warpMap[i - 1].beat);
            const auto t = (beat - warpMap[i - 1].beat) / span;
            return warpMap[i - 1].sourceRatio + t * (warpMap[i].sourceRatio - warpMap[i - 1].sourceRatio);
        }
    return warpMap.back().sourceRatio;
}

double ClipEditorComponent::xToBeatNorm(int x) const noexcept
{
    if (lastWaveformBounds.getWidth() <= 0)
        return 0.0;
    const auto local = juce::jlimit(0.0, 1.0, static_cast<double>(x - lastWaveformBounds.getX())
                                              / static_cast<double>(lastWaveformBounds.getWidth()));
    return juce::jlimit(0.0, 1.0, waveformViewStart + local * visibleWaveSpan());
}

int ClipEditorComponent::beatNormToX(double beatNorm) const noexcept
{
    if (lastWaveformBounds.getWidth() <= 0)
        return 0;
    const auto visible = (juce::jlimit(0.0, 1.0, beatNorm) - waveformViewStart) / visibleWaveSpan();
    return lastWaveformBounds.getX() + static_cast<int>(std::round(visible * lastWaveformBounds.getWidth()));
}

// A pixel maps to a beat; that beat maps (through the warp) to a source ratio.
double ClipEditorComponent::xToWaveRatio(int x) const noexcept
{
    return beatToSrcRatio(xToBeatNorm(x) * warpTotalBeats);
}

// A source ratio sits at the x of the beat it warps to.
int ClipEditorComponent::waveRatioToX(double ratio) const noexcept
{
    const double bn = warpTotalBeats > 0.0 ? srcRatioToBeat(ratio) / warpTotalBeats : juce::jlimit(0.0, 1.0, ratio);
    return beatNormToX(bn);
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

double ClipEditorComponent::currentGridStepBeats() const noexcept
{
    const double totalBeats = state.sourceLengthBeats > 0.0 ? state.sourceLengthBeats : state.lengthInBeats;
    if (totalBeats <= 0.0 || lastWaveformBounds.getWidth() <= 0)
        return 0.0;
    const double pxPerBeat = static_cast<double>(lastWaveformBounds.getWidth()) / (totalBeats * visibleWaveSpan());
    // Finest musical step whose lines are at least ~9px apart (no picket fence).
    static const double steps[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0 };
    for (const double s : steps)
        if (pxPerBeat * s >= 9.0)
            return s;
    return 256.0;
}

int ClipEditorComponent::warpMarkerAtX(int x, int radiusPx) const noexcept
{
    if (! state.warpEnabled)
        return -1;
    const int radius = radiusPx >= 0 ? radiusPx : markerHitRadius;
    int best = -1;
    int bestDist = radius + 1;
    for (int i = 0; i < static_cast<int>(state.warpMarkers.size()); ++i)
    {
        const int mx = waveRatioToX(juce::jlimit(0.0, 1.0, state.warpMarkers[static_cast<std::size_t>(i)].sourceRatio));
        const int d = std::abs(x - mx);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

void ClipEditorComponent::recomputeTransients()
{
    transientRatios.clear();
    if (state.waveform == nullptr)
        return;

    const auto& mx = state.waveform->maxValues;
    const auto& mn = state.waveform->minValues;
    const int n = static_cast<int>(std::min(mx.size(), mn.size()));
    if (n < 8)
        return;

    std::vector<float> env(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        env[static_cast<std::size_t>(i)] = std::max(std::abs(mx[static_cast<std::size_t>(i)]),
                                                    std::abs(mn[static_cast<std::size_t>(i)]));

    // Onset strength = positive envelope difference (a rise in energy). Peaks above a mean-relative
    // threshold, spaced apart, are the transients — the same idea Ableton uses for its transient marks.
    std::vector<float> flux(static_cast<std::size_t>(n), 0.0f);
    double mean = 0.0;
    for (int i = 1; i < n; ++i)
    {
        flux[static_cast<std::size_t>(i)] = std::max(0.0f, env[static_cast<std::size_t>(i)] - env[static_cast<std::size_t>(i - 1)]);
        mean += flux[static_cast<std::size_t>(i)];
    }
    mean /= std::max(1, n - 1);
    const float thr = static_cast<float>(mean) * 2.5f + 1.0e-4f;
    const int minSpacing = std::max(2, n / 256);
    int last = -minSpacing;
    for (int i = 1; i < n - 1; ++i)
    {
        const float f = flux[static_cast<std::size_t>(i)];
        if (f > thr && f >= flux[static_cast<std::size_t>(i - 1)] && f >= flux[static_cast<std::size_t>(i + 1)]
            && (i - last) >= minSpacing)
        {
            transientRatios.push_back(static_cast<double>(i) / static_cast<double>(n - 1));
            last = i;
        }
    }
    if (transientRatios.empty() || transientRatios.front() > 0.01)
        transientRatios.insert(transientRatios.begin(), 0.0);
}

double ClipEditorComponent::nearestTransient(double sourceRatio, double maxDistRatio) const noexcept
{
    double best = -1.0, bestDist = maxDistRatio;
    for (const double t : transientRatios)
    {
        const double d = std::abs(t - sourceRatio);
        if (d < bestDist) { bestDist = d; best = t; }
    }
    return best;
}

double ClipEditorComponent::snappedBeatForSourceRatio(double sourceRatio) const noexcept
{
    const double totalBeats = state.sourceLengthBeats > 0.0 ? state.sourceLengthBeats : state.lengthInBeats;
    const double step = currentGridStepBeats();
    if (totalBeats <= 0.0)
        return 0.0;
    const double beat = warpSourceRatioToBeat(state.warpMarkers, totalBeats, sourceRatio);
    const double snapped = step > 0.0 ? std::round(beat / step) * step : beat;
    return juce::jlimit(0.0, totalBeats, snapped);
}

void ClipEditorComponent::updateSampleMarker(WaveDragMode mode, int x, bool shouldSeek)
{
    auto ratio = xToWaveRatio(x);

    // Snap START/END to the beat grid (hold Alt to place freely). Makes trimming to bars/beats exact.
    // Snap the BEAT the point maps to, then convert back to a source ratio (matters once warped).
    if (! snapBypass && (mode == WaveDragMode::start || mode == WaveDragMode::end))
    {
        const double step = currentGridStepBeats();
        if (warpTotalBeats > 0.0 && step > 0.0)
        {
            const double snappedBeat = std::round(srcRatioToBeat(ratio) / step) * step;
            ratio = juce::jlimit(0.0, 1.0, beatToSrcRatio(snappedBeat));
        }
    }

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
