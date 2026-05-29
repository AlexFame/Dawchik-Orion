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

	std::optional<int> pitchForTypingKeyCode(int keyCode)
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
	        if (lowerKeyCode == mappedKey)
	            return pitch;

	    return std::nullopt;
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

    quantizeButton.setButtonText("Quantize");
    quantizeButton.setTooltip("Quantize selected notes (or all if none selected) to the Snap grid  -  Option+Q");
    quantizeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1b232b));
    quantizeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    quantizeButton.addListener(this);
    addAndMakeVisible(quantizeButton);

    scaleLockLabel.setText("Scale Lock", juce::dontSendNotification);
    scaleLockLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    scaleLockLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(scaleLockLabel);

    scaleLockToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    scaleLockToggle.setToggleState(scaleLockEnabled, juce::dontSendNotification);
    scaleLockToggle.onClick = [this]
    {
        scaleLockEnabled = scaleLockToggle.getToggleState();
        if (onScaleLockChanged)
            onScaleLockChanged(scaleLockEnabled);
        repaint();
    };
    addAndMakeVisible(scaleLockToggle);

    focusToggle.setVisible(false);

    closeButton.setButtonText("Back To Arrangement");
    closeButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffeb6f3a));
    closeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    closeButton.addListener(this);
    addAndMakeVisible(closeButton);
}

MidiEditorOverlayComponent::~MidiEditorOverlayComponent() = default;

void MidiEditorOverlayComponent::openClip(TrackState& trackState, TimelineClip& clipState,
                                          int projectKeyRoot, bool projectKeyIsMinor,
                                          bool initialScaleLock)
{
    activeTrack = &trackState;
    activeClip = &clipState;
    trackName = trackState.name;
    clipName = clipState.name;
    trackColour = trackState.colour;
    clearSelection();
    liveKeyboardPitches.clear();
    hoverNote.reset();
    noteDragState.reset();
    marqueeState.reset();
    velocityDragState.reset();
    liveKeyboardPitches.clear();
    undoStack.clear();
    redoStack.clear();
    horizontalZoom = 1.0;
    verticalZoom = 1.0;
    scrollX = 0.0;
    scrollY = 0.0;
    pendingMagnifyDelta = 0.0;
    ignoreWheelUntilMs = 0.0;
    // Inherit scale from project key. scalePatterns[0]=Minor, [1]=Major.
    scaleRoot         = ((projectKeyRoot % 12) + 12) % 12;
    scalePatternIndex = projectKeyIsMinor ? 0 : 1;
    scaleLockEnabled  = initialScaleLock;
    scaleLockToggle.setToggleState(scaleLockEnabled, juce::dontSendNotification);
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

void MidiEditorOverlayComponent::setProjectKey(int rootSemitones, bool minor)
{
    scaleRoot         = ((rootSemitones % 12) + 12) % 12;
    scalePatternIndex = minor ? 0 : 1;
    updateSubtitle();
    repaint();
}

void MidiEditorOverlayComponent::setScaleLockExternally(bool enabled)
{
    scaleLockEnabled = enabled;
    scaleLockToggle.setToggleState(enabled, juce::dontSendNotification);
    repaint();
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

    const auto livePlayheadBeat = onRequestPlayheadBeat ? onRequestPlayheadBeat() : 0.0;
    const auto livePlayingState = onRequestPlayingState ? onRequestPlayingState() : false;
    const auto clipStart  = activeClip != nullptr ? activeClip->startBeat     : 0.0;
    const auto clipLength = activeClip != nullptr ? activeClip->lengthInBeats : 8.0;
    const auto localBeat  = livePlayheadBeat - clipStart;
    const bool playheadInsideClip = localBeat >= 0.0 && localBeat <= clipLength + 0.0001;

    std::set<int> activePlaybackPitches;
    if (activeClip != nullptr && livePlayingState && playheadInsideClip)
    {
        for (const auto& note : activeClip->midiNotes)
        {
            const auto noteEnd = note.startBeat + juce::jmax(0.01, note.lengthInBeats);
            if (localBeat >= note.startBeat && localBeat < noteEnd)
                activePlaybackPitches.insert(note.pitch);
        }
    }
    activePlaybackPitches.insert(liveKeyboardPitches.begin(), liveKeyboardPitches.end());

    for (int lane = firstVisibleLane; lane <= lastVisibleLane; ++lane)
    {
        const auto pitch = laneIndexToPitch(lane);
        auto keyRow = keyboardArea.withY(static_cast<int>(std::round(gridArea.getY() + lane * laneHeight)))
                          .withHeight(static_cast<int>(std::ceil(laneHeight)));

        if (keyRow.getBottom() < keyboardArea.getY() || keyRow.getY() > keyboardArea.getBottom())
            continue;

        keyRow = keyRow.getIntersection(keyboardArea);
        const auto blackKey = isBlackKey(pitch);
        const auto pitchClass = ((pitch % 12) + 12) % 12;
        const bool isRoot   = pitchClass == scaleRoot;
        const bool inScale  = isPitchInScale(pitch);
        const bool isActive = activePlaybackPitches.contains(pitch);
        auto keyFill = keyRow.reduced(6, 1);

        if (blackKey && ! isActive)
        {
            keyFill = keyFill.withX(keyboardArea.getX() + keyboardArea.getWidth() / 3)
                             .withWidth(keyboardArea.getWidth() - keyboardArea.getWidth() / 3 - 8);
        }

        auto keyShape = keyFill.toFloat().reduced(isActive ? 0.0f : 0.6f, isActive ? 0.0f : 0.4f);
        if (isActive)
            keyShape = keyShape.withTrimmedRight(1.0f);

        g.setColour(juce::Colours::black.withAlpha(blackKey ? 0.48f : 0.20f));
        g.fillRoundedRectangle(keyShape.translated(1.4f, 1.0f), blackKey ? 3.0f : 4.0f);

        juce::Colour baseTop = blackKey ? juce::Colour(0xff29313a) : juce::Colour(0xfff8fbfd);
        juce::Colour baseBottom = blackKey ? juce::Colour(0xff070a0e) : juce::Colour(0xffb8c2cb);
        if (isRoot || inScale)
        {
            const auto tint = trackColour.withAlpha(isRoot ? 0.34f : 0.16f);
            baseTop = baseTop.interpolatedWith(tint, isRoot ? 0.28f : 0.14f);
            baseBottom = baseBottom.interpolatedWith(tint, isRoot ? 0.24f : 0.10f);
        }
        if (isActive)
        {
            baseTop = trackColour.brighter(0.55f);
            baseBottom = trackColour.darker(0.25f);
        }

        juce::ColourGradient keyGradient(baseTop,
                                         keyShape.getX(), keyShape.getY(),
                                         baseBottom,
                                         keyShape.getX(), keyShape.getBottom(),
                                         false);
        keyGradient.addColour(0.50, blackKey && ! isActive ? baseTop.darker(0.24f) : baseTop.darker(0.05f));
        g.setGradientFill(keyGradient);
        g.fillRoundedRectangle(keyShape, isActive ? 4.0f : (blackKey ? 3.0f : 4.0f));

        if (isActive)
        {
            juce::ColourGradient pressGlow(trackColour.brighter(0.85f).withAlpha(0.78f),
                                           keyShape.getX(), keyShape.getY(),
                                           trackColour.darker(0.10f).withAlpha(0.78f),
                                           keyShape.getRight(), keyShape.getY(),
                                           false);
            g.setGradientFill(pressGlow);
            g.fillRoundedRectangle(keyShape.reduced(1.0f, 1.0f), 3.5f);
        }

        g.setColour(isActive ? juce::Colours::white.withAlpha(0.42f)
                             : juce::Colours::white.withAlpha(blackKey ? 0.12f : 0.68f));
        g.drawLine(keyShape.getX() + 2.0f, keyShape.getY() + 1.0f,
                   keyShape.getRight() - 2.0f, keyShape.getY() + 1.0f, isActive ? 1.4f : 1.0f);

        g.setColour(isActive ? juce::Colours::white.withAlpha(0.78f)
                             : juce::Colours::black.withAlpha(blackKey ? 0.72f : 0.22f));
        g.drawRoundedRectangle(keyShape, isActive ? 4.0f : (blackKey ? 3.0f : 4.0f), isActive ? 1.6f : 1.0f);

        g.setColour(isActive ? juce::Colours::white
                             : (blackKey ? juce::Colours::white.withAlpha(0.86f) : juce::Colours::black.withAlpha(0.76f)));
        g.setFont(juce::FontOptions(isActive ? 13.0f : 12.0f, (isRoot || isActive) ? juce::Font::bold : juce::Font::plain));
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

        // Scale highlight: tonic row strongest, in-scale rows tinted with the track
        // colour, out-of-scale rows darkened so the usable lanes visually pop.
        const auto pitchClass = ((pitch % 12) + 12) % 12;
        if (pitchClass == scaleRoot)
            g.setColour(trackColour.withAlpha(0.20f));
        else if (isPitchInScale(pitch))
            g.setColour(trackColour.withAlpha(0.075f));
        else
            g.setColour(juce::Colours::black.withAlpha(isBlackKey(pitch) ? 0.24f : 0.13f));
        g.fillRect(row);

        if (activePlaybackPitches.contains(pitch))
        {
            auto laneGlow = row.toFloat().reduced(0.0f, 1.0f);
            juce::ColourGradient glow(trackColour.brighter(0.65f).withAlpha(0.30f),
                                      laneGlow.getX(), laneGlow.getCentreY(),
                                      trackColour.darker(0.45f).withAlpha(0.015f),
                                      laneGlow.getRight(), laneGlow.getCentreY(),
                                      false);
            glow.addColour(0.18, trackColour.withAlpha(0.22f));
            glow.addColour(0.48, trackColour.withAlpha(0.085f));
            g.setGradientFill(glow);
            g.fillRect(laneGlow);

            juce::ColourGradient edge(trackColour.brighter(0.85f).withAlpha(0.42f),
                                      laneGlow.getX(), laneGlow.getY(),
                                      trackColour.withAlpha(0.02f),
                                      laneGlow.getRight(), laneGlow.getY(),
                                      false);
            g.setGradientFill(edge);
            g.fillRect(laneGlow.withHeight(1.4f));
            g.fillRect(laneGlow.withY(laneGlow.getBottom() - 1.4f).withHeight(1.4f));
        }
    }

    // Dim the area BEYOND the clip end so the user can see where the pattern stops.
    const auto activeClipLengthBeats = activeClip != nullptr ? activeClip->lengthInBeats : 0.0;
    if (activeClipLengthBeats > 0.0)
    {
        const auto clipEndX = static_cast<float>(gridArea.getX() + (activeClipLengthBeats * pixelsPerBeat) - scrollX);
        if (clipEndX < static_cast<float>(visibleGrid.getRight()))
        {
            const auto dimX = juce::jmax(static_cast<float>(visibleGrid.getX()), clipEndX);
            g.setColour(juce::Colours::black.withAlpha(0.45f));
            g.fillRect(juce::Rectangle<float>(dimX,
                                              static_cast<float>(visibleGrid.getY()),
                                              static_cast<float>(visibleGrid.getRight()) - dimX,
                                              static_cast<float>(visibleGrid.getHeight())));
        }
    }

    const auto stepsPerBeat = juce::jmax(1, static_cast<int>(std::round(1.0 / juce::jmax(0.001, snapSizeInBeats))));
    const auto beatsPerBar  = 4; // visual bar — independent of project's time-sig numerator for now
    const auto stepsPerBar  = stepsPerBeat * beatsPerBar;

    for (int step = 0; step <= steps; ++step)
    {
        const auto beat = static_cast<double>(step) * snapSizeInBeats;
        const auto x = static_cast<float>(gridArea.getX() + (beat * pixelsPerBeat) - scrollX);
        if (x < static_cast<float>(visibleGrid.getX() - 8) || x > static_cast<float>(visibleGrid.getRight() + 8))
            continue;

        const bool isBarLine  = (step % stepsPerBar)  == 0;
        const bool isBeatLine = (step % stepsPerBeat) == 0;

        if (isBarLine)
            g.setColour(juce::Colours::white.withAlpha(0.32f));
        else if (isBeatLine)
            g.setColour(juce::Colours::white.withAlpha(0.14f));
        else
            g.setColour(juce::Colours::white.withAlpha(0.045f));

        g.drawLine(x, static_cast<float>(visibleGrid.getY()), x, static_cast<float>(visibleGrid.getBottom()),
                   isBarLine ? 2.0f : (isBeatLine ? 1.4f : 1.0f));
    }

    // Bar numbers along the top of the grid.
    {
        const auto rulerHeight = 16.0f;
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRect(juce::Rectangle<float>(static_cast<float>(visibleGrid.getX()),
                                          static_cast<float>(visibleGrid.getY()),
                                          static_cast<float>(visibleGrid.getWidth()),
                                          rulerHeight));
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.setColour(juce::Colours::white.withAlpha(0.78f));
        for (int step = 0; step <= steps; step += stepsPerBar)
        {
            const auto beat = static_cast<double>(step) * snapSizeInBeats;
            const auto x = static_cast<float>(gridArea.getX() + (beat * pixelsPerBeat) - scrollX);
            if (x < static_cast<float>(visibleGrid.getX() - 30) || x > static_cast<float>(visibleGrid.getRight() + 4))
                continue;
            const auto barNumber = step / stepsPerBar + 1; // 1-based for display
            g.drawText(juce::String(barNumber),
                       static_cast<int>(x + 3),
                       static_cast<int>(visibleGrid.getY() + 1),
                       40, static_cast<int>(rulerHeight) - 2,
                       juce::Justification::topLeft);
        }
    }

    // Solid clip-end line so the boundary is crystal-clear.
    if (activeClipLengthBeats > 0.0)
    {
        const auto clipEndX = static_cast<float>(gridArea.getX() + (activeClipLengthBeats * pixelsPerBeat) - scrollX);
        if (clipEndX >= static_cast<float>(visibleGrid.getX() - 4)
            && clipEndX <= static_cast<float>(visibleGrid.getRight() + 4))
        {
            g.setColour(juce::Colour(0xffeb6f3a).withAlpha(0.85f)); // accent
            g.drawLine(clipEndX, static_cast<float>(visibleGrid.getY()),
                       clipEndX, static_cast<float>(visibleGrid.getBottom()), 2.0f);
        }
    }

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

    // Playhead in clip-local coordinates: project playhead minus clip start.
    // Only draw when the project playhead is INSIDE this clip's range — otherwise
    // the piano-roll cursor used to mirror the project loop wrap-around and slide
    // silently back to bar 1 as soon as the clip ended.
    if (playheadInsideClip)
    {
        const auto playheadX = static_cast<float>(gridArea.getX())
            + static_cast<float>((localBeat * pixelsPerBeat) - scrollX);

        juce::ColourGradient gradient(playheadColour.withAlpha(0.0f), playheadX - 8.0f, 0.0f,
                                      playheadColour.withAlpha(0.0f), playheadX + 8.0f, 0.0f, false);
        gradient.addColour(0.5, playheadColour.withAlpha(livePlayingState ? 0.35f : 0.15f));
        g.setGradientFill(gradient);
        g.fillRect(playheadX - 8.0f, static_cast<float>(visibleGrid.getY()), 16.0f, static_cast<float>(visibleGrid.getHeight()));

        g.setColour(playheadColour.withAlpha(livePlayingState ? 0.95f : 0.75f));
        g.drawLine(playheadX, static_cast<float>(visibleGrid.getY()), playheadX, static_cast<float>(visibleGrid.getBottom()), 2.0f);
    }

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
    controlsArea.removeFromLeft(6);
    quantizeButton.setBounds(controlsArea.removeFromLeft(86).reduced(0, 2));
    controlsArea.removeFromLeft(12);
    scaleLockToggle.setBounds(controlsArea.removeFromLeft(22).reduced(0, 4));
    scaleLockLabel.setBounds(controlsArea.removeFromLeft(78).reduced(0, 2));
    contextLabel.setBounds(centerArea.reduced(8, 4));
    closeButton.setBounds(topBar.removeFromRight(190).reduced(0, 8));
    clampScrollOffsets();
}

bool MidiEditorOverlayComponent::keyPressed(const juce::KeyPress& key)
{
    if (! key.getModifiers().isCommandDown()
        && ! key.getModifiers().isCtrlDown()
        && ! key.getModifiers().isAltDown()
        && pitchForTypingKeyCode(key.getKeyCode()).has_value())
    {
        updateLiveKeyboardPitches();
        return false;
    }

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
        // FL-style: space toggles playback; if it's already playing, stop AND rewind
        // to the start of THIS clip (not project zero). So the second tap of space
        // sends the cursor back to the beginning of the pattern.
        const auto isPlayingNow = onRequestPlayingState && onRequestPlayingState();
        if (isPlayingNow)
        {
            if (onStopAndRewindToClipStart) onStopAndRewindToClipStart();
            else if (onTogglePlayback)       onTogglePlayback();
        }
        else if (onTogglePlayback)
        {
            onTogglePlayback();
        }
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

    // Option+Q — quick quantize. Q alone would collide with the laptop-keyboard
    // note input (QWERTY row plays notes), and Cmd+Q is the OS quit shortcut.
    if (key == juce::KeyPress('q', juce::ModifierKeys::altModifier, 0))
    {
        quantizeSelectedNotes();
        return true;
    }

    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::escapeKey)
    {
        closeEditor();
        return true;
    }

    return false;
}

bool MidiEditorOverlayComponent::keyStateChanged(bool)
{
    updateLiveKeyboardPitches();
    return false;
}

void MidiEditorOverlayComponent::focusLost(FocusChangeType)
{
    ignoreNextMouseDown = true;
    liveKeyboardPitches.clear();
    repaint();
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
            auto newPitch = juce::jlimit(lowestPitch, highestPitch, originalNote.pitch + pitchDelta);
            if (scaleLockEnabled && ! isPitchInScale(newPitch))
                newPitch = snapPitchToScale(newPitch);
            movedNote.pitch = newPitch;
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
        auto pitch = yToPitch(marqueeState->origin.y);
        // Scale lock: snap new notes to the closest in-scale pitch when enabled.
        if (scaleLockEnabled && ! isPitchInScale(pitch))
            pitch = snapPitchToScale(pitch);
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
    else if (button == &quantizeButton)
        quantizeSelectedNotes();
}

bool MidiEditorOverlayComponent::updateLiveKeyboardPitches()
{
    static constexpr std::array<int, 39> keyCodes {
        'q', '2', 'w', '3', 'e', 'r', '5', 't', '6', 'y', '7', 'u', 'i', '9', 'o', '0', 'p', '[', ']', '+', '=',
        'z', 's', 'x', 'd', 'c', 'v', 'g', 'b', 'h', 'n', 'j', 'm', ',', 'l', '.', ';', '/', '\''
    };

    std::set<int> nextPitches;
    const auto modifiers = juce::ModifierKeys::getCurrentModifiers();
    if (! modifiers.isCommandDown() && ! modifiers.isCtrlDown() && ! modifiers.isAltDown())
    {
        for (const auto keyCode : keyCodes)
        {
            if (! juce::KeyPress::isKeyCurrentlyDown(keyCode))
                continue;

            auto pitch = pitchForTypingKeyCode(keyCode);
            if (! pitch.has_value())
                continue;

            auto playablePitch = *pitch;
            if (activeTrack != nullptr)
                playablePitch += activeTrack->samplerKeyboardOctaveOffset * 12
                               + activeTrack->samplerTransposeSemitones;

            if (scaleLockEnabled)
            {
                if (! isPitchInScale(playablePitch))
                    playablePitch = snapPitchToScale(playablePitch);
            }

            nextPitches.insert(juce::jlimit(lowestPitch, highestPitch, playablePitch));
        }
    }

    if (nextPitches == liveKeyboardPitches)
        return false;

    liveKeyboardPitches = std::move(nextPitches);
    repaint();
    return true;
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

// Snap a chromatic pitch to the closest in-scale pitch. Ties prefer the lower one
// (musically the diatonic neighbour below is usually a safer guess).
int MidiEditorOverlayComponent::snapPitchToScale(int pitch) const noexcept
{
    if (isPitchInScale(pitch)) return pitch;
    for (int dist = 1; dist <= 6; ++dist)
    {
        if (isPitchInScale(pitch - dist)) return pitch - dist;
        if (isPitchInScale(pitch + dist)) return pitch + dist;
    }
    return pitch;
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

void MidiEditorOverlayComponent::quantizeSelectedNotes()
{
    if (activeClip == nullptr || activeClip->midiNotes.empty())
        return;

    // Simple Ableton-style quick quantize:
    //   * Grid     — current Snap value
    //   * Start    — pulled to the NEAREST grid line (round, not floor)
    //   * Length   — preserved; end rides with start
    //   * Target   — selected notes; if no selection, ALL notes in the clip.
    const auto snap    = juce::jmax(0.001, snapSizeInBeats);
    const auto clipLen = activeClip->lengthInBeats;
    const bool quantizeAll = selectedNotes.empty();

    auto computeNewStart = [&](const MidiNote& n)
    {
        const auto snapped = std::round(n.startBeat / snap) * snap;
        const auto maxStart = juce::jmax(0.0, clipLen - n.lengthInBeats);
        return juce::jlimit(0.0, maxStart, snapped);
    };

    bool anyChange = false;
    for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
    {
        if (! quantizeAll && ! selectedNotes.contains(i))
            continue;
        const auto& n = activeClip->midiNotes[static_cast<std::size_t>(i)];
        if (std::abs(computeNewStart(n) - n.startBeat) > 1.0e-6)
        {
            anyChange = true;
            break;
        }
    }
    if (! anyChange)
        return;

    pushUndoSnapshot();

    for (int i = 0; i < static_cast<int>(activeClip->midiNotes.size()); ++i)
    {
        if (! quantizeAll && ! selectedNotes.contains(i))
            continue;
        auto& n = activeClip->midiNotes[static_cast<std::size_t>(i)];
        n.startBeat = computeNewStart(n);
    }

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
    const auto visible = getVisibleGridViewport();

    if (activeClip == nullptr || activeClip->midiNotes.empty())
    {
        const auto laneHeight = juce::jmax(10.0, baseLaneHeightPx * verticalZoom);
        const auto defaultCenterPitch = 60; // C4: useful middle register for new empty MIDI clips.
        scrollY = (static_cast<double>(pitchToLane(defaultCenterPitch)) * laneHeight)
                - (static_cast<double>(visible.getHeight()) * 0.5);
        scrollX = 0.0;
        return;
    }

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
