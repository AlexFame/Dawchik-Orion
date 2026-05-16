#include "MidiEditorOverlayComponent.h"

#include <algorithm>
#include <limits>

namespace
{
const auto overlayBackground = juce::Colour(0xee090d11);
const auto panelBackground = juce::Colour(0xff131a20);
const auto panelStroke = juce::Colour(0xff2a3743);
const auto mutedText = juce::Colours::white.withAlpha(0.64f);
const auto playheadColour = juce::Colour(0xfff5c451);
const auto velocityLaneBackground = juce::Colour(0xff151c23);
constexpr int resizeHandleWidth = 10;
constexpr double minimumNoteLengthInBeats = 0.25;
constexpr int minimumNoteWidthPx = 4;
constexpr int marqueeThresholdPx = 4;
constexpr int noteDragThresholdPx = 4;
constexpr int baseLaneHeightPx = 24;
constexpr int velocityLaneHeightPx = 112;
constexpr int totalPitchCount = 128;
constexpr int lowestPitch = 0;
constexpr int highestPitch = totalPitchCount - 1;
constexpr double beatEpsilon = 0.0001;
constexpr double minimumHorizontalZoom = 0.5;
constexpr double minimumVerticalZoom = 1.0;
constexpr double maximumHorizontalZoom = 6.0;
constexpr double maximumVerticalZoom = 3.0;

struct ScalePattern
{
    const char* name;
    std::array<int, 7> pitchClasses;
};

constexpr std::array<const char*, 12> rootNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
constexpr std::array<ScalePattern, 3> scalePatterns {{
    { "Minor", { 0, 2, 3, 5, 7, 8, 10 } },
    { "Major", { 0, 2, 4, 5, 7, 9, 11 } },
    { "Dorian", { 0, 2, 3, 5, 7, 9, 10 } },
}};

struct SnapSetting
{
    const char* name;
    double beats;
};

constexpr std::array<SnapSetting, 5> snapSettings {{
    { "1/4", 1.0 },
    { "1/8", 0.5 },
    { "1/16", 0.25 },
    { "1/32", 0.125 },
    { "1/64", 0.0625 },
}};

juce::String noteNameForPitch(int pitch)
{
    static constexpr const char* noteNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto pitchClass = ((pitch % 12) + 12) % 12;
    const auto octave = (pitch / 12) - 1;
    return juce::String(noteNames[pitchClass]) + juce::String(octave);
}
}  // namespace

namespace orion
{
MidiEditorOverlayComponent::MidiEditorOverlayComponent()
{
    setWantsKeyboardFocus(true);
    setVisible(false);
    startTimerHz(120);

    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    addAndMakeVisible(titleLabel);

    subtitleLabel.setColour(juce::Label::textColourId, mutedText);
    subtitleLabel.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    addAndMakeVisible(subtitleLabel);

    contextLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
    contextLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    contextLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(contextLabel);

    for (auto* label : { &scaleLabel, &snapLabel })
    {
        label->setColour(juce::Label::textColourId, mutedText);
        label->setFont(juce::FontOptions(12.0f, juce::Font::plain));
        addChildComponent(*label);
    }

    scaleLabel.setText("Scale", juce::dontSendNotification);
    snapLabel.setText("Snap", juce::dontSendNotification);

    scaleButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1b232b));
    scaleButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    scaleButton.addListener(this);
    addAndMakeVisible(scaleButton);

    snapButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1b232b));
    snapButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    snapButton.addListener(this);
    addAndMakeVisible(snapButton);

    focusToggle.setVisible(false);

    closeButton.setButtonText("Back To Arrangement");
    closeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffeb6f3a));
    closeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    closeButton.addListener(this);
    addAndMakeVisible(closeButton);
}

MidiEditorOverlayComponent::~MidiEditorOverlayComponent() = default;

void MidiEditorOverlayComponent::openClip(TrackState& trackState, TimelineClip& clipState)
{
    activeTrack = &trackState;
    activeClip = &clipState;
    trackName = trackState.name;
    clipName = clipState.name;
    trackColour = trackState.colour;
    clearSelection();
    hoverNote.reset();
    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();
    undoStack.clear();
    redoStack.clear();
    horizontalZoom = 1.0;
    verticalZoom = 1.0;
    scrollX = 0.0;
    scrollY = 0.0;
    pendingMagnifyDelta = 0.0;
    ignoreWheelUntilMs = 0.0;
    scaleRoot = 0;
    scalePatternIndex = 0;
    snapSizeInBeats = 0.25;
    focusModeEnabled = false;
    hasStoredViewportBeforeFocus = false;
    ignoreNextMouseDown = true;
    focusToggle.setToggleState(false, juce::dontSendNotification);

    titleLabel.setText("Piano Roll", juce::dontSendNotification);
    updateSubtitle();
    setVisible(true);
    toFront(true);
    grabKeyboardFocus();

    focusViewportAroundClipNotes();
    clampScrollOffsets();
    repaint();
}

void MidiEditorOverlayComponent::closeEditor()
{
    setVisible(false);
    clearSelection();
    hoverNote.reset();
    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();

    if (onClose)
        onClose();
}

void MidiEditorOverlayComponent::timerCallback()
{
    if (! isVisible())
        return;

    repaint();
}

void MidiEditorOverlayComponent::paint(juce::Graphics& g)
{
    g.fillAll(overlayBackground);

    const auto topBar = getTopBarBounds();
    const auto keyboardArea = getKeyboardBounds();
    const auto visibleGrid = getVisibleGridViewport();
    const auto gridArea = getGridBounds();
    const auto velocityLane = getVelocityLaneBounds();

    g.setColour(panelBackground);
    g.fillRoundedRectangle(topBar.toFloat(), 20.0f);
    g.fillRoundedRectangle(keyboardArea.toFloat(), 18.0f);
    g.fillRoundedRectangle(visibleGrid.toFloat(), 18.0f);
    g.setColour(velocityLaneBackground);
    g.fillRoundedRectangle(velocityLane.toFloat(), 16.0f);

    g.setColour(panelStroke);
    g.drawRoundedRectangle(topBar.toFloat(), 20.0f, 1.0f);
    g.drawRoundedRectangle(keyboardArea.toFloat(), 18.0f, 1.0f);
    g.drawRoundedRectangle(visibleGrid.toFloat(), 18.0f, 1.0f);
    g.drawRoundedRectangle(velocityLane.toFloat(), 16.0f, 1.0f);

    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto visibleBeatRange = getVisibleBeatRange();
    const auto pixelsPerBeat = getPixelsPerBeat();
    const auto steps = juce::jmax(16, static_cast<int>(std::ceil(visibleBeatRange / snapSizeInBeats)));
    const auto displayedLaneCount = getDisplayedLaneCount();

    const auto firstVisibleLane = juce::jmax(0, static_cast<int>(std::floor((visibleGrid.getY() - gridArea.getY()) / laneHeight)));
    const auto lastVisibleLane = juce::jmin(displayedLaneCount - 1, static_cast<int>(std::ceil((visibleGrid.getBottom() - gridArea.getY()) / laneHeight)));

    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        auto keyRow = keyboardArea.withY(static_cast<int>(std::round(gridArea.getY() + lane * laneHeight)))
                          .withHeight(static_cast<int>(std::ceil(laneHeight)));

        if (keyRow.getBottom() < keyboardArea.getY() || keyRow.getY() > keyboardArea.getBottom())
            continue;

        keyRow = keyRow.getIntersection(keyboardArea);
        const auto blackKey = isBlackKey(pitch);

        g.setColour(blackKey ? juce::Colour(0xff0e1318) : juce::Colour(0xfff2f4f6));
        g.fillRect(keyRow.reduced(8, 1));
        g.setColour(blackKey ? juce::Colours::white.withAlpha(0.84f) : juce::Colours::black.withAlpha(0.78f));
        g.setFont(juce::FontOptions(12.5f, juce::Font::plain));
        g.drawText(noteNameForPitch(pitch), keyRow.reduced(12, 0), juce::Justification::centredLeft);
    }

    g.reduceClipRegion(visibleGrid);

    for (int lane = firstVisibleLane; lane <= lastVisibleLane + 1; ++lane)
    {
        const auto y = static_cast<float>(gridArea.getY() + lane * laneHeight);
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.drawLine(static_cast<float>(visibleGrid.getX()), y, static_cast<float>(visibleGrid.getRight()), y, 1.0f);
    }

    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        const auto y = static_cast<int>(std::round(gridArea.getY() + lane * laneHeight));
        auto row = juce::Rectangle<int>(visibleGrid.getX(), y, visibleGrid.getWidth(), static_cast<int>(std::ceil(laneHeight)));
        row = row.getIntersection(visibleGrid);
        g.setColour(isBlackKey(pitch) ? juce::Colours::black.withAlpha(0.13f) : juce::Colours::white.withAlpha(0.015f));
        g.fillRect(row);
    }

    for (int step = 0; step <= steps; ++step)
    {
        const auto beat = static_cast<double>(step) * snapSizeInBeats;
        const auto x = static_cast<float>(gridArea.getX() + (beat * pixelsPerBeat) - scrollX);
        if (x < static_cast<float>(visibleGrid.getX() - 8) || x > static_cast<float>(visibleGrid.getRight() + 8))
            continue;
        g.setColour(step % 4 == 0 ? juce::Colours::white.withAlpha(0.12f) : juce::Colours::white.withAlpha(0.045f));
        g.drawLine(x, static_cast<float>(visibleGrid.getY()), x, static_cast<float>(visibleGrid.getBottom()), step % 4 == 0 ? 1.8f : 1.0f);
    }

    const auto livePlayheadBeat = onRequestPlayheadBeat ? onRequestPlayheadBeat() : 0.0;
    const auto livePlayingState = onRequestPlayingState ? onRequestPlayingState() : false;
    if (activeClip != nullptr)
    {
        for (int noteIndex = 0; noteIndex < static_cast<int>(activeClip->midiNotes.size()); ++noteIndex)
        {
            const auto& note = activeClip->midiNotes[static_cast<std::size_t>(noteIndex)];
            if (focusModeEnabled && ! isPitchInScale(note.pitch))
                continue;
            const auto noteBounds = getNoteBounds(note);
            const auto isSelected = isNoteSelected(noteIndex);

            g.setColour(trackColour.withSaturation(isSelected ? 0.92f : 0.82f));
            g.fillRoundedRectangle(noteBounds.toFloat(), 6.0f);

            if (isSelected)
            {
                g.setColour(juce::Colours::white.withAlpha(0.75f));
                g.drawRoundedRectangle(noteBounds.toFloat(), 6.0f, 1.4f);
            }
        }
    }

    const auto clipLength = activeClip != nullptr ? activeClip->lengthInBeats : 8.0;
    const auto loopedBeat = std::fmod(livePlayheadBeat, juce::jmax(1.0, clipLength));

    const auto playheadX = static_cast<float>(gridArea.getX())
        + static_cast<float>((loopedBeat * pixelsPerBeat) - scrollX);

    // Draw Playhead (Logic style - OVER EVERYTHING)
    juce::ColourGradient gradient(playheadColour.withAlpha(0.0f), playheadX - 8.0f, 0.0f,
                                  playheadColour.withAlpha(0.0f), playheadX + 8.0f, 0.0f, false);
    gradient.addColour(0.5, playheadColour.withAlpha(livePlayingState ? 0.35f : 0.15f));
    g.setGradientFill(gradient);
    g.fillRect(playheadX - 8.0f, static_cast<float>(visibleGrid.getY()), 16.0f, static_cast<float>(visibleGrid.getHeight()));

    g.setColour(playheadColour.withAlpha(livePlayingState ? 0.95f : 0.75f));
    g.drawLine(playheadX, static_cast<float>(visibleGrid.getY()), playheadX, static_cast<float>(visibleGrid.getBottom()), 2.0f);

    if (marqueeState.has_value() && marqueeState->movedEnough)
    {
        const auto clippedMarquee = marqueeState->bounds.getIntersection(visibleGrid);
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillRect(clippedMarquee);
        g.setColour(juce::Colours::white.withAlpha(0.62f));
        g.drawRect(clippedMarquee, 1);
    }

    g.excludeClipRegion(visibleGrid);
    g.reduceClipRegion(velocityLane);

    for (int step = 0; step <= steps; ++step)
    {
        const auto beat = static_cast<double>(step) * snapSizeInBeats;
        const auto x = static_cast<float>(gridArea.getX() + (beat * pixelsPerBeat) - scrollX);
        if (x < static_cast<float>(velocityLane.getX() - 8) || x > static_cast<float>(velocityLane.getRight() + 8))
            continue;
        g.setColour(step % 4 == 0 ? juce::Colours::white.withAlpha(0.10f) : juce::Colours::white.withAlpha(0.035f));
        g.drawLine(x, static_cast<float>(velocityLane.getY()), x, static_cast<float>(velocityLane.getBottom()), step % 4 == 0 ? 1.4f : 1.0f);
    }

    g.setColour(mutedText);
    g.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    g.drawText("Velocity", velocityLane.reduced(12, 8).removeFromTop(18), juce::Justification::centredLeft);

    if (activeClip != nullptr)
    {
        for (int noteIndex = 0; noteIndex < static_cast<int>(activeClip->midiNotes.size()); ++noteIndex)
        {
            if (focusModeEnabled && ! isPitchInScale(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].pitch))
                continue;
            const auto barBounds = getVelocityBarBounds(noteIndex);
            const auto selected = isNoteSelected(noteIndex);
            g.setColour(trackColour.withAlpha(selected ? 0.95f : 0.55f));
            g.fillRoundedRectangle(barBounds.toFloat(), 4.0f);
        }
    }
}

void MidiEditorOverlayComponent::resized()
{
    auto bounds = getLocalBounds().reduced(8);
    auto topBar = bounds.removeFromTop(88).reduced(20, 16);

    auto leftArea = topBar.removeFromLeft(topBar.proportionOfWidth(0.42f));
    auto centerArea = topBar.removeFromLeft(topBar.proportionOfWidth(0.34f));
    auto titleArea = leftArea;
    titleLabel.setBounds(titleArea.removeFromTop(30));
    subtitleLabel.setBounds(0, 0, 0, 0);
    auto controlsArea = titleArea.removeFromTop(28);
    scaleButton.setBounds(controlsArea.removeFromLeft(128).reduced(0, 2));
    controlsArea.removeFromLeft(8);
    snapButton.setBounds(controlsArea.removeFromLeft(74).reduced(0, 2));
    contextLabel.setBounds(centerArea.reduced(8, 4));
    closeButton.setBounds(topBar.removeFromRight(190).reduced(0, 8));
    clampScrollOffsets();
}

bool MidiEditorOverlayComponent::keyPressed(const juce::KeyPress& key)
{
    if ((key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) && ! undoStack.empty())
    {
        redoStack.push_back(NoteSnapshot { activeClip != nullptr ? activeClip->midiNotes : std::vector<MidiNote> {}, selectedNotes, horizontalZoom, verticalZoom, scaleRoot, scalePatternIndex, snapSizeInBeats, false });
        restoreSnapshot(undoStack.back());
        undoStack.pop_back();
        repaint();
        return true;
    }

    if ((key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
         || key == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0))
        && ! redoStack.empty())
    {
        undoStack.push_back(NoteSnapshot { activeClip != nullptr ? activeClip->midiNotes : std::vector<MidiNote> {}, selectedNotes, horizontalZoom, verticalZoom, scaleRoot, scalePatternIndex, snapSizeInBeats, false });
        restoreSnapshot(redoStack.back());
        redoStack.pop_back();
        repaint();
        return true;
    }

    if (key == juce::KeyPress::spaceKey)
    {
        if (onTogglePlayback)
            onTogglePlayback();
        return true;
    }

    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0) && activeClip != nullptr)
    {
        selectedNotes.clear();
        for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
            selectedNotes.insert(i);
        repaint();
        return true;
    }

    if (key == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0) && ! selectedNotes.empty())
    {
        duplicateSelectedNotes();
        return true;
    }

    if ((key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey) && ! selectedNotes.empty())
    {
        deleteSelectedNotes();
        return true;
    }

    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::escapeKey)
    {
        closeEditor();
        return true;
    }

    return false;
}

void MidiEditorOverlayComponent::focusLost(FocusChangeType)
{
    ignoreNextMouseDown = true;
}

void MidiEditorOverlayComponent::mouseMove(const juce::MouseEvent& event)
{
    hoverNote = hitTestNote(event.getPosition());
    setMouseCursor(hoverNote.has_value() && hoverNote->overResizeHandle
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : juce::MouseCursor::NormalCursor);
}

void MidiEditorOverlayComponent::mouseExit(const juce::MouseEvent&)
{
    hoverNote.reset();
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void MidiEditorOverlayComponent::mouseDown(const juce::MouseEvent& event)
{
    if (activeClip == nullptr)
        return;

    if (ignoreNextMouseDown || shouldConsumeFocusClick())
    {
        ignoreNextMouseDown = false;
        grabKeyboardFocus();
        return;
    }

    if (getVelocityLaneBounds().contains(event.getPosition()))
    {
        const auto hitVelocityIndex = hitTestVelocityBar(event.getPosition());
        if (hitVelocityIndex.has_value() && ! isNoteSelected(*hitVelocityIndex))
            selectSingleNote(*hitVelocityIndex);

        std::vector<int> targetIndices;
        if (selectedNotes.empty())
        {
            if (! hitVelocityIndex.has_value())
                return;

            targetIndices.push_back(*hitVelocityIndex);
        }
        else
        {
            targetIndices.assign(selectedNotes.begin(), selectedNotes.end());
        }

        std::vector<int> originalVelocities;
        originalVelocities.reserve(targetIndices.size());
        for (const auto index : targetIndices)
            originalVelocities.push_back(activeClip->midiNotes[static_cast<std::size_t>(index)].velocity);

        velocityDragState = VelocityDragState { targetIndices, originalVelocities, event.getPosition(), false };
        pushUndoSnapshot();
        velocityDragState->historyCaptured = true;
        updateVelocityFromPosition(event.getPosition().y);
        grabKeyboardFocus();
        repaint();
        return;
    }

    if (! getVisibleGridViewport().contains(event.getPosition()))
        return;

    const auto hit = hitTestNote(event.getPosition());

    if (event.mods.isRightButtonDown())
    {
        if (hit.has_value())
        {
            selectSingleNote(hit->selected.noteIndex);
            deleteSelectedNotes();
        }
        return;
    }

    if (hit.has_value())
    {
        if (! isNoteSelected(hit->selected.noteIndex))
            selectSingleNote(hit->selected.noteIndex);

        std::vector<int> selectedIndices(selectedNotes.begin(), selectedNotes.end());
        std::vector<MidiNote> originalSelectedNotes;
        originalSelectedNotes.reserve(selectedIndices.size());
        for (const auto index : selectedIndices)
            originalSelectedNotes.push_back(activeClip->midiNotes[static_cast<std::size_t>(index)]);

        const auto& note = activeClip->midiNotes[static_cast<std::size_t>(hit->selected.noteIndex)];
        noteDragState = NoteDragState {
            hit->selected,
            hit->overResizeHandle ? NoteDragMode::resizeRight : NoteDragMode::move,
            event.getPosition(),
            note.startBeat,
            note.lengthInBeats,
            note.pitch,
            originalSelectedNotes,
            selectedIndices,
            false
        };
        grabKeyboardFocus();
        repaint();
        return;
    }

    clearSelection();
    marqueeState = MarqueeState { event.getPosition(), juce::Rectangle<int>(event.getPosition(), { 1, 1 }), false };
    grabKeyboardFocus();
    repaint();
}

void MidiEditorOverlayComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (velocityDragState.has_value())
    {
        updateVelocityFromPosition(event.getPosition().y);
        repaint();
        return;
    }

    if (! noteDragState.has_value() || activeClip == nullptr)
    {
        if (marqueeState.has_value())
        {
            marqueeState->bounds = juce::Rectangle<int>(marqueeState->origin, event.getPosition()).getSmallestIntegerContainer();
            marqueeState->movedEnough = marqueeState->bounds.getWidth() > marqueeThresholdPx || marqueeState->bounds.getHeight() > marqueeThresholdPx;
            selectedNotes = hitTestNotesInRect(marqueeState->bounds);
            repaint();
        }
        return;
    }

    const auto dragDistanceX = event.getDistanceFromDragStartX();
    const auto dragDistanceY = event.getDistanceFromDragStartY();
    if ((dragDistanceX * dragDistanceX) + (dragDistanceY * dragDistanceY) < noteDragThresholdPx * noteDragThresholdPx)
        return;

    if (! noteDragState->historyCaptured)
    {
        pushUndoSnapshot();
        noteDragState->historyCaptured = true;
    }

    auto& note = activeClip->midiNotes[static_cast<std::size_t>(noteDragState->selected.noteIndex)];
    const auto pixelsPerBeat = getPixelsPerBeat();
    const auto beatDelta = snapBeat((event.position.x - noteDragState->mouseDownPosition.x) / pixelsPerBeat);
    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);

    if (noteDragState->mode == NoteDragMode::move)
    {
        const auto pitchDelta = static_cast<int>((noteDragState->mouseDownPosition.y - event.position.y) / laneHeight);
        for (std::size_t i = 0; i < noteDragState->selectedIndices.size(); ++i)
        {
            auto& movedNote = activeClip->midiNotes[static_cast<std::size_t>(noteDragState->selectedIndices[i])];
            const auto& originalNote = noteDragState->originalSelectedNotes[i];
            movedNote.startBeat = juce::jlimit(0.0, activeClip->lengthInBeats - movedNote.lengthInBeats, snapBeat(originalNote.startBeat + beatDelta));
            movedNote.pitch = juce::jlimit(lowestPitch, highestPitch, originalNote.pitch + pitchDelta);
        }
    }
    else
    {
        note.lengthInBeats = juce::jlimit(
            minimumNoteLengthInBeats,
            activeClip->lengthInBeats - note.startBeat,
            snapBeat(noteDragState->originalLengthInBeats + beatDelta));
    }

    repaint();
}

void MidiEditorOverlayComponent::mouseUp(const juce::MouseEvent&)
{
    if (activeClip != nullptr && marqueeState.has_value() && ! marqueeState->movedEnough)
    {
        pushUndoSnapshot();
        const auto snappedBeat = snapBeat(xToBeat(static_cast<double>(marqueeState->origin.x)));
        const auto pitch = yToPitch(marqueeState->origin.y);
        activeClip->midiNotes.push_back(MidiNote { pitch, snappedBeat, 1.0, 100 });
        selectSingleNote(static_cast<int>(activeClip->midiNotes.size()) - 1);
    }

    if (noteDragState.has_value() && noteDragState->historyCaptured && ! undoStack.empty() && ! notesChangedSince(undoStack.back()))
        undoStack.pop_back();

    if (velocityDragState.has_value() && velocityDragState->historyCaptured && ! undoStack.empty() && ! notesChangedSince(undoStack.back()))
        undoStack.pop_back();

    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();
    repaint();
}

void MidiEditorOverlayComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (! getVisibleGridViewport().contains(event.getPosition()))
        return;

    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto totalDelta = std::abs(wheel.deltaX) + std::abs(wheel.deltaY);

    if (nowMs < ignoreWheelUntilMs && totalDelta < 0.045f)
        return;

    if (std::abs(wheel.deltaX) > 0.0001f || std::abs(wheel.deltaY) > 0.0001f)
    {
        scrollX -= static_cast<double>(wheel.deltaX) * 600.0;
        scrollY -= static_cast<double>(wheel.deltaY) * 600.0;
        clampScrollOffsets();
        repaint();
    }
}

void MidiEditorOverlayComponent::mouseMagnify(const juce::MouseEvent& event, float scaleFactor)
{
    if (! getVisibleGridViewport().contains(event.getPosition()))
        return;

    const auto rawScale = juce::jlimit(0.85, 1.15, static_cast<double>(scaleFactor));
    pendingMagnifyDelta = pendingMagnifyDelta == 0.0 ? rawScale : (pendingMagnifyDelta * rawScale);
    ignoreWheelUntilMs = juce::Time::getMillisecondCounterHiRes() + 120.0;

    if (std::abs(pendingMagnifyDelta - 1.0) < 0.003)
        return;

    const auto visible = getVisibleGridViewport();
    const auto oldContentWidth = static_cast<double>(visible.getWidth()) * horizontalZoom;
    const auto focusXInView = juce::jlimit(0.0, static_cast<double>(visible.getWidth()), static_cast<double>(event.getPosition().x - visible.getX()));
    const auto focusXRatio = oldContentWidth > 0.0 ? (scrollX + focusXInView) / oldContentWidth : 0.5;

    horizontalZoom = juce::jlimit(minimumHorizontalZoom, maximumHorizontalZoom, horizontalZoom * pendingMagnifyDelta);

    const auto newContentWidth = static_cast<double>(visible.getWidth()) * horizontalZoom;
    scrollX = (focusXRatio * newContentWidth) - focusXInView;
    pendingMagnifyDelta = 1.0;
    clampScrollOffsets();
    updateSubtitle();
    repaint();
}

void MidiEditorOverlayComponent::buttonClicked(juce::Button* button)
{
    if (button == &closeButton)
        closeEditor();
    else if (button == &scaleButton)
        showScaleMenu();
    else if (button == &snapButton)
        showSnapMenu();
}

juce::Rectangle<int> MidiEditorOverlayComponent::getTopBarBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(24);
    return bounds.removeFromTop(88);
}

juce::Rectangle<int> MidiEditorOverlayComponent::getKeyboardBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(24);
    bounds.removeFromTop(88);
    bounds.removeFromBottom(velocityLaneHeightPx + 10);
    return bounds.removeFromLeft(92);
}

juce::Rectangle<int> MidiEditorOverlayComponent::getVisibleGridViewport() const noexcept
{
    auto bounds = getLocalBounds().reduced(24);
    bounds.removeFromTop(88);
    bounds.removeFromBottom(velocityLaneHeightPx + 10);
    bounds.removeFromLeft(92);
    return bounds;
}

juce::Rectangle<int> MidiEditorOverlayComponent::getVelocityLaneBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(24);
    bounds.removeFromTop(88);
    auto velocityArea = bounds.removeFromBottom(velocityLaneHeightPx);
    velocityArea.removeFromLeft(92);
    return velocityArea;
}

juce::Rectangle<int> MidiEditorOverlayComponent::getGridBounds() const noexcept
{
    auto bounds = getVisibleGridViewport();
    const auto fullWidth = bounds.getWidth();
    const auto zoomedWidth = fullWidth;
    const auto fullHeight = bounds.getHeight();
    const auto naturalHeight = getDisplayedLaneCount() * baseLaneHeightPx;
    const auto zoomedHeight = juce::jmax(fullHeight, static_cast<int>(std::round(static_cast<double>(naturalHeight) * verticalZoom)));
    return bounds.withWidth(zoomedWidth).withHeight(zoomedHeight).translated(-static_cast<int>(std::round(scrollX)), -static_cast<int>(std::round(scrollY)));
}

double MidiEditorOverlayComponent::getPixelsPerBeat() const noexcept
{
    const auto visible = getVisibleGridViewport();
    const auto clipLength = activeClip != nullptr ? activeClip->lengthInBeats : 8.0;
    return (static_cast<double>(visible.getWidth()) * horizontalZoom) / juce::jmax(1.0, clipLength);
}

double MidiEditorOverlayComponent::getVisibleBeatRange() const noexcept
{
    const auto clipLength = activeClip != nullptr ? activeClip->lengthInBeats : 8.0;
    return juce::jmax(clipLength, juce::jmax(1.0, clipLength) / juce::jmax(minimumHorizontalZoom, horizontalZoom));
}

juce::Rectangle<int> MidiEditorOverlayComponent::getNoteBounds(const MidiNote& note) const noexcept
{
    const auto grid = getGridBounds();
    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto pixelsPerBeat = getPixelsPerBeat();
    const auto lane = pitchToLane(note.pitch);
    return juce::Rectangle<int>(
        grid.getX() + static_cast<int>(std::round((note.startBeat * pixelsPerBeat) - scrollX)) + 1,
        grid.getY() + static_cast<int>(std::round(lane * laneHeight)) + 3,
        juce::jmax(minimumNoteWidthPx, static_cast<int>(std::round(note.lengthInBeats * pixelsPerBeat)) - 2),
        juce::jmax(10, static_cast<int>(std::round(laneHeight)) - 6));
}

std::optional<MidiEditorOverlayComponent::NoteHit> MidiEditorOverlayComponent::hitTestNote(juce::Point<int> position) const
{
    if (activeClip == nullptr)
        return std::nullopt;

    for (int noteIndex = static_cast<int>(activeClip->midiNotes.size()) - 1; noteIndex >= 0; --noteIndex)
    {
        if (focusModeEnabled && ! isPitchInScale(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].pitch))
            continue;
        const auto bounds = getNoteBounds(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)]);
        if (! bounds.contains(position))
            continue;

        return NoteHit {
            SelectedNote { noteIndex },
            bounds,
            position.x >= bounds.getRight() - resizeHandleWidth
        };
    }

    return std::nullopt;
}

juce::Rectangle<int> MidiEditorOverlayComponent::getVelocityBarBounds(int noteIndex) const noexcept
{
    const auto velocityLane = getVelocityLaneBounds().reduced(10, 10);
    const auto note = activeClip->midiNotes[static_cast<std::size_t>(noteIndex)];
    const auto grid = getGridBounds();
    const auto pixelsPerBeat = getPixelsPerBeat();
    const auto x = grid.getX() + static_cast<int>(std::round((note.startBeat * pixelsPerBeat) - scrollX)) + 1;
    const auto width = juce::jmax(6, static_cast<int>(std::round(note.lengthInBeats * pixelsPerBeat)) - 2);
    const auto normalisedVelocity = juce::jlimit(0.0, 1.0, static_cast<double>(note.velocity) / 127.0);
    const auto height = juce::jmax(8, static_cast<int>(std::round(normalisedVelocity * static_cast<double>(velocityLane.getHeight() - 20))));

    return juce::Rectangle<int>(x, velocityLane.getBottom() - height, width, height).getIntersection(getVelocityLaneBounds().reduced(8, 8));
}

std::optional<int> MidiEditorOverlayComponent::hitTestVelocityBar(juce::Point<int> position) const
{
    if (activeClip == nullptr)
        return std::nullopt;

    for (int noteIndex = static_cast<int>(activeClip->midiNotes.size()) - 1; noteIndex >= 0; --noteIndex)
    {
        if (focusModeEnabled && ! isPitchInScale(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].pitch))
            continue;
        if (getVelocityBarBounds(noteIndex).contains(position))
            return noteIndex;
    }

    return std::nullopt;
}

std::set<int> MidiEditorOverlayComponent::hitTestNotesInRect(juce::Rectangle<int> selection) const
{
    std::set<int> hits;

    if (activeClip == nullptr)
        return hits;

    for (int noteIndex = 0; noteIndex < static_cast<int>(activeClip->midiNotes.size()); ++noteIndex)
    {
        if (focusModeEnabled && ! isPitchInScale(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)].pitch))
            continue;
        if (selection.intersects(getNoteBounds(activeClip->midiNotes[static_cast<std::size_t>(noteIndex)])))
            hits.insert(noteIndex);
    }

    return hits;
}

int MidiEditorOverlayComponent::yToPitch(int y) const noexcept
{
    const auto grid = getGridBounds();
    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto lane = juce::jlimit(0, getDisplayedLaneCount() - 1, static_cast<int>((y - grid.getY()) / laneHeight));
    return laneIndexToPitch(lane);
}

int MidiEditorOverlayComponent::laneIndexToPitch(int laneIndex) const noexcept
{
    laneIndex = juce::jlimit(0, getDisplayedLaneCount() - 1, laneIndex);
    return highestPitch - laneIndex;
}

double MidiEditorOverlayComponent::xToBeat(double x) const noexcept
{
    const auto grid = getGridBounds();
    const auto pixelsPerBeat = getPixelsPerBeat();
    return juce::jmax(0.0, (x - static_cast<double>(grid.getX()) + scrollX) / juce::jmax(1.0, pixelsPerBeat));
}

double MidiEditorOverlayComponent::snapBeat(double beat) const noexcept
{
    return std::floor(beat / snapSizeInBeats) * snapSizeInBeats;
}

int MidiEditorOverlayComponent::pitchToLane(int pitch) const noexcept
{
    return juce::jlimit(0, totalPitchCount - 1, highestPitch - pitch);
}

int MidiEditorOverlayComponent::getDisplayedLaneCount() const noexcept
{
    return totalPitchCount;
}

bool MidiEditorOverlayComponent::isPitchInScale(int pitch) const noexcept
{
    const int pitchClass = ((pitch % 12) + 12) % 12;
    const int scaleRelative = (pitchClass - scaleRoot + 12) % 12;
    const auto& pattern = scalePatterns[static_cast<std::size_t>(scalePatternIndex)].pitchClasses;
    return std::find(pattern.begin(), pattern.end(), scaleRelative) != pattern.end();
}

bool MidiEditorOverlayComponent::isBlackKey(int pitch) const noexcept
{
    const int pitchClass = ((pitch % 12) + 12) % 12;
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;
}

bool MidiEditorOverlayComponent::isNoteSelected(int noteIndex) const noexcept
{
    return selectedNotes.contains(noteIndex);
}

void MidiEditorOverlayComponent::selectSingleNote(int noteIndex)
{
    selectedNotes.clear();
    selectedNotes.insert(noteIndex);
}

void MidiEditorOverlayComponent::duplicateSelectedNotes()
{
    if (activeClip == nullptr || selectedNotes.empty())
        return;

    pushUndoSnapshot();

    std::vector<int> indices(selectedNotes.begin(), selectedNotes.end());
    double minStart = std::numeric_limits<double>::max();
    double maxEnd = 0.0;

    for (const auto index : indices)
    {
        const auto& note = activeClip->midiNotes[static_cast<std::size_t>(index)];
        minStart = juce::jmin(minStart, note.startBeat);
        maxEnd = juce::jmax(maxEnd, note.startBeat + note.lengthInBeats);
    }

    const auto offset = juce::jmax(snapSizeInBeats, snapBeat(maxEnd - minStart));
    std::vector<int> duplicatedIndices;

    for (const auto index : indices)
    {
        auto note = activeClip->midiNotes[static_cast<std::size_t>(index)];
        note.startBeat = juce::jlimit(0.0, activeClip->lengthInBeats - note.lengthInBeats, note.startBeat + offset);
        activeClip->midiNotes.push_back(note);
        duplicatedIndices.push_back(static_cast<int>(activeClip->midiNotes.size()) - 1);
    }

    selectedNotes.clear();
    selectedNotes.insert(duplicatedIndices.begin(), duplicatedIndices.end());
    repaint();
}

void MidiEditorOverlayComponent::deleteSelectedNotes()
{
    if (activeClip == nullptr || selectedNotes.empty())
        return;

    pushUndoSnapshot();

    std::vector<MidiNote> keptNotes;
    keptNotes.reserve(activeClip->midiNotes.size());

    for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
    {
        if (! selectedNotes.contains(i))
            keptNotes.push_back(activeClip->midiNotes[static_cast<std::size_t>(i)]);
    }

    activeClip->midiNotes = std::move(keptNotes);
    clearSelection();
    hoverNote.reset();
    noteDragState.reset();
    repaint();
}

void MidiEditorOverlayComponent::updateSubtitle()
{
    scaleButton.setButtonText(getScaleName());
    snapButton.setButtonText(getSnapName());
    contextLabel.setText(trackName, juce::dontSendNotification);
}

void MidiEditorOverlayComponent::clearSelection()
{
    selectedNotes.clear();
}

void MidiEditorOverlayComponent::pushUndoSnapshot()
{
    if (activeClip == nullptr)
        return;

    undoStack.push_back(NoteSnapshot { activeClip->midiNotes, selectedNotes, horizontalZoom, verticalZoom, scaleRoot, scalePatternIndex, snapSizeInBeats, false });
    redoStack.clear();
}

void MidiEditorOverlayComponent::restoreSnapshot(const NoteSnapshot& snapshot)
{
    if (activeClip == nullptr)
        return;

    activeClip->midiNotes = snapshot.midiNotes;
    selectedNotes = snapshot.selectedNotes;
    horizontalZoom = snapshot.horizontalZoom;
    verticalZoom = snapshot.verticalZoom;
    scaleRoot = snapshot.scaleRoot;
    scalePatternIndex = snapshot.scalePatternIndex;
    snapSizeInBeats = snapshot.snapSizeInBeats;
    focusModeEnabled = false;
    hoverNote.reset();
    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();
    clampScrollOffsets();
    focusToggle.setToggleState(false, juce::dontSendNotification);
    updateSubtitle();
}

bool MidiEditorOverlayComponent::notesChangedSince(const NoteSnapshot& snapshot) const noexcept
{
    if (activeClip == nullptr)
        return false;

    if (activeClip->midiNotes.size() != snapshot.midiNotes.size())
        return true;

    for (std::size_t i = 0; i < activeClip->midiNotes.size(); ++i)
    {
        const auto& current = activeClip->midiNotes[i];
        const auto& previous = snapshot.midiNotes[i];

        if (current.pitch != previous.pitch
            || std::abs(current.startBeat - previous.startBeat) > beatEpsilon
            || std::abs(current.lengthInBeats - previous.lengthInBeats) > beatEpsilon
            || current.velocity != previous.velocity)
        {
            return true;
        }
    }

    return false;
}

void MidiEditorOverlayComponent::updateVelocityFromPosition(int y)
{
    if (activeClip == nullptr || ! velocityDragState.has_value())
        return;

    const auto lane = getVelocityLaneBounds().reduced(10, 10);
    const auto clampedY = juce::jlimit(lane.getY(), lane.getBottom(), y);
    const auto ratio = 1.0 - (static_cast<double>(clampedY - lane.getY()) / juce::jmax(1.0, static_cast<double>(lane.getHeight())));
    const auto velocity = juce::jlimit(1, 127, static_cast<int>(std::round(ratio * 127.0)));

    for (const auto index : velocityDragState->targetIndices)
        activeClip->midiNotes[static_cast<std::size_t>(index)].velocity = velocity;
}

void MidiEditorOverlayComponent::focusViewportAroundClipNotes()
{
    if (activeClip == nullptr || activeClip->midiNotes.empty())
        return;

    const auto visible = getVisibleGridViewport();

    int minPitch = highestPitch;
    int maxPitch = lowestPitch;
    double minBeat = std::numeric_limits<double>::max();
    double maxBeat = 0.0;

    for (const auto& note : activeClip->midiNotes)
    {
        if (focusModeEnabled && ! isPitchInScale(note.pitch))
            continue;
        minPitch = juce::jmin(minPitch, note.pitch);
        maxPitch = juce::jmax(maxPitch, note.pitch);
        minBeat = juce::jmin(minBeat, note.startBeat);
        maxBeat = juce::jmax(maxBeat, note.startBeat + note.lengthInBeats);
    }

    if (minBeat > maxBeat)
        return;

    const auto pitchSpan = juce::jmax(1, (maxPitch - minPitch) + 1);
    const auto paddedPitchSpan = pitchSpan + 6;
    const auto targetVisibleLanes = juce::jlimit(10, totalPitchCount, paddedPitchSpan);
    verticalZoom = juce::jlimit(
        minimumVerticalZoom,
        maximumVerticalZoom,
        static_cast<double>(visible.getHeight()) / (static_cast<double>(targetVisibleLanes) * baseLaneHeightPx));

    const auto beatSpan = juce::jmax(snapSizeInBeats, maxBeat - minBeat);
    const auto paddedBeatSpan = beatSpan + juce::jmax(2.0 * snapSizeInBeats, beatSpan * 0.18);
    horizontalZoom = juce::jlimit(
        minimumHorizontalZoom,
        maximumHorizontalZoom,
        static_cast<double>(activeClip->lengthInBeats) / paddedBeatSpan);

    const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
    const auto pixelsPerBeat = (static_cast<double>(visible.getWidth()) * horizontalZoom) / juce::jmax(1.0, activeClip->lengthInBeats);
    const auto displayedLaneCount = getDisplayedLaneCount();
    const auto paddedTopLane = juce::jmax(0, pitchToLane(maxPitch) - 3);
    const auto paddedBottomLane = juce::jmin(displayedLaneCount - 1, pitchToLane(minPitch) + 3);
    const auto laneCenter = (static_cast<double>(paddedTopLane) + static_cast<double>(paddedBottomLane)) * 0.5;
    scrollY = (laneCenter * laneHeight) - (static_cast<double>(visible.getHeight()) * 0.5);

    const auto beatCenter = (minBeat + maxBeat) * 0.5;
    scrollX = (beatCenter * pixelsPerBeat) - (static_cast<double>(visible.getWidth()) * 0.5);
}

bool MidiEditorOverlayComponent::shouldConsumeFocusClick() const noexcept
{
    return ! hasKeyboardFocus(true);
}

void MidiEditorOverlayComponent::adjustZoom(double horizontalDelta, double verticalDelta, std::optional<juce::Point<int>> focusPoint)
{
    const auto visible = getVisibleGridViewport();
    const auto oldContentWidth = static_cast<double>(visible.getWidth()) * horizontalZoom;
    const auto oldContentHeight = juce::jmax(static_cast<double>(visible.getHeight()), static_cast<double>(getDisplayedLaneCount() * baseLaneHeightPx) * verticalZoom);

    double focusXInView = static_cast<double>(visible.getWidth()) * 0.5;
    double focusYInView = static_cast<double>(visible.getHeight()) * 0.5;
    double focusXRatio = 0.5;
    double focusYRatio = 0.5;

    if (focusPoint.has_value())
    {
        focusXInView = juce::jlimit(0.0, static_cast<double>(visible.getWidth()), static_cast<double>(focusPoint->x - visible.getX()));
        focusYInView = juce::jlimit(0.0, static_cast<double>(visible.getHeight()), static_cast<double>(focusPoint->y - visible.getY()));
        focusXRatio = oldContentWidth > 0.0 ? (scrollX + focusXInView) / oldContentWidth : 0.5;
        focusYRatio = oldContentHeight > 0.0 ? (scrollY + focusYInView) / oldContentHeight : 0.5;
    }

    horizontalZoom = juce::jlimit(minimumHorizontalZoom, maximumHorizontalZoom, horizontalZoom + horizontalDelta);
    verticalZoom = juce::jlimit(minimumVerticalZoom, maximumVerticalZoom, verticalZoom + verticalDelta);

    const auto newContentWidth = static_cast<double>(visible.getWidth()) * horizontalZoom;
    const auto newContentHeight = juce::jmax(static_cast<double>(visible.getHeight()), static_cast<double>(getDisplayedLaneCount() * baseLaneHeightPx) * verticalZoom);
    scrollX = (focusXRatio * newContentWidth) - focusXInView;
    scrollY = (focusYRatio * newContentHeight) - focusYInView;
    clampScrollOffsets();
    updateSubtitle();
    repaint();
}

void MidiEditorOverlayComponent::clampScrollOffsets()
{
    const auto visible = getVisibleGridViewport();
    const auto fullWidth = static_cast<double>(visible.getWidth());
    const auto fullHeight = static_cast<double>(visible.getHeight());
    const auto zoomedWidth = fullWidth * horizontalZoom;
    const auto zoomedHeight = juce::jmax(fullHeight, static_cast<double>(getDisplayedLaneCount() * baseLaneHeightPx) * verticalZoom);
    scrollX = juce::jlimit(0.0, juce::jmax(0.0, zoomedWidth - fullWidth), scrollX);
    scrollY = juce::jlimit(0.0, juce::jmax(0.0, zoomedHeight - fullHeight), scrollY);
}

juce::String MidiEditorOverlayComponent::getScaleName() const
{
    return juce::String(rootNames[static_cast<std::size_t>(scaleRoot)]) + " " + scalePatterns[static_cast<std::size_t>(scalePatternIndex)].name;
}

juce::String MidiEditorOverlayComponent::getSnapName() const
{
    for (const auto& setting : snapSettings)
    {
        if (std::abs(setting.beats - snapSizeInBeats) < beatEpsilon)
            return setting.name;
    }

    return juce::String(snapSizeInBeats, 3) + " beat";
}

void MidiEditorOverlayComponent::showScaleMenu()
{
    juce::PopupMenu menu;

    for (int rootIndex = 0; rootIndex < static_cast<int>(rootNames.size()); ++rootIndex)
    {
        for (int patternIndex = 0; patternIndex < static_cast<int>(scalePatterns.size()); ++patternIndex)
        {
            const auto itemId = 1 + rootIndex * static_cast<int>(scalePatterns.size()) + patternIndex;
            const auto selected = scaleRoot == rootIndex && scalePatternIndex == patternIndex;
            menu.addItem(itemId, juce::String(rootNames[static_cast<std::size_t>(rootIndex)]) + " " + scalePatterns[static_cast<std::size_t>(patternIndex)].name, true, selected);
        }
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&scaleButton),
                       [this](int selectedId)
                       {
                           if (selectedId <= 0)
                               return;

                           const auto index = selectedId - 1;
                           scaleRoot = index / static_cast<int>(scalePatterns.size());
                           scalePatternIndex = index % static_cast<int>(scalePatterns.size());
                           clampScrollOffsets();
                           updateSubtitle();
                           repaint();
                       });
}

void MidiEditorOverlayComponent::showSnapMenu()
{
    juce::PopupMenu menu;

    for (int i = 0; i < static_cast<int>(snapSettings.size()); ++i)
    {
        const auto selected = std::abs(snapSettings[static_cast<std::size_t>(i)].beats - snapSizeInBeats) < beatEpsilon;
        menu.addItem(i + 1, snapSettings[static_cast<std::size_t>(i)].name, true, selected);
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&snapButton),
                       [this](int selectedId)
                       {
                           if (selectedId <= 0)
                               return;

                           snapSizeInBeats = snapSettings[static_cast<std::size_t>(selectedId - 1)].beats;
                           updateSubtitle();
                           repaint();
                       });
}
}  // namespace orion
