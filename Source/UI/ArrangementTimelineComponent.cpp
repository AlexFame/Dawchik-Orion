#include "ArrangementTimelineComponent.h"

#include "OrionTheme.h"
#include "../Audio/WarpEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
juce::AudioFormatManager& getSharedWaveformFormatManager()
{
    static juce::AudioFormatManager manager;
    static std::once_flag flag;
    std::call_once(flag, [&]() { manager.registerBasicFormats(); });
    return manager;
}

const auto timelineBackground = juce::Colour(0xff20252a);
const auto majorGridColour = juce::Colour(0xff56616b);
const auto minorGridColour = juce::Colour(0xff3b444c);
const auto markerColour = juce::Colours::white.withAlpha(0.64f);
const auto textColour = juce::Colours::white.withAlpha(0.88f);
const auto playheadColour = juce::Colour(0xfff25454);
const auto loopRangeColour = juce::Colour(0xff7ecb6f);
constexpr auto resizeHandleWidth = 12;
constexpr auto fadeHandleHitRadius = 8;
constexpr auto fadeHandleBandHeight = 18;
constexpr auto minimumClipLengthInBeats = 1.0;
constexpr auto snapSizeInBeats = 0.25;
constexpr auto minTrackHeaderWidth = 176;
constexpr auto maxTrackHeaderWidth = 360;
constexpr auto beatEpsilon = 0.0001;
constexpr auto defaultLaneHeight = 78;
constexpr auto minimumLaneHeight = 42;
constexpr auto maximumLaneHeight = 176;
constexpr auto minimumVerticalZoom = 0.54;
constexpr auto maximumVerticalZoom = 2.26;
constexpr auto timelineRulerHeight = 30;
constexpr auto loopLaneHeight = 11;
constexpr auto loopHandleHitWidth = 8;
constexpr auto playheadHitWidth = 8;
constexpr auto newTrackDropZoneHeight = 64;
constexpr auto maxExpandedLaneHeight = 240;
constexpr auto minPixelsPerBeat = 0.12;
constexpr auto maxPixelsPerBeat = 160.0;
constexpr auto minTimelineLengthInBeats = 512.0;
constexpr auto timelinePaddingInBeats = 64.0;

int chooseGridStepBeats(double pixelsPerBeat, int beatsPerBar, double minimumSpacingPixels)
{
    const auto safeBeatsPerBar = juce::jmax(1, beatsPerBar);

    if (pixelsPerBeat <= 0.0)
        return safeBeatsPerBar;

    const auto targetBeats = minimumSpacingPixels / pixelsPerBeat;
    static constexpr std::array<int, 10> barMultipliers { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };

    for (const auto bars : barMultipliers)
    {
        const auto stepBeats = bars * safeBeatsPerBar;
        if (static_cast<double>(stepBeats) >= targetBeats)
            return stepBeats;
    }

    return barMultipliers.back() * safeBeatsPerBar;
}

juce::Rectangle<int> getTimelineContentBounds(const juce::Component& component)
{
    auto bounds = component.getLocalBounds();
    bounds.removeFromLeft(6);
    bounds.removeFromTop(4);
    return bounds;
}

juce::Rectangle<int> getVisibleTrackAreaBounds(const juce::Component& component)
{
    auto bounds = getTimelineContentBounds(component);
    bounds.removeFromTop(timelineRulerHeight);
    return bounds.getIntersection(component.getLocalBounds());
}

struct AudioImportAnalysis
{
    double durationSeconds { 0.0 };
    double clipLengthInBeats { minimumClipLengthInBeats };
    double sourceBpm { 0.0 };
    int detectedBars { 0 };
    juce::String bpmSource { "none" };
    double bpmConfidence { 0.0 };
    bool bpmGuessed { false };
    int  sourceKeyRoot { -1 };
    bool sourceKeyIsMinor { false };
    // True when the (fast) drop-time analysis couldn't get key/tempo from the filename,
    // so the background worker should run the full signal analysis to fill them in.
    bool needsSignalAnalysis { false };
};


double fallbackClipLengthInBeats(const juce::DynamicObject& payload)
{
    return juce::jmax(minimumClipLengthInBeats, static_cast<double>(payload.getProperty("lengthBeats")));
}

AudioImportAnalysis analyzeImportedAudioClip(const juce::File& file, double tempoBpm, int numerator, double fallbackLengthBeats)
{
    AudioImportAnalysis result;
    result.clipLengthInBeats = fallbackLengthBeats;

    if (! file.existsAsFile() || tempoBpm <= 0.0)
        return result;

    // Fast pass first (filename + duration heuristics, no audio decode) — gives the
    // duration instantly. For SHORT files the full signal analysis (chroma key +
    // autocorrelation tempo) is cheap (~0.1s), so we run it synchronously right here so
    // loops snap to tempo/key immediately on drop. Only LONG files (full tracks, where
    // decoding/stretching is expensive) defer to the background worker.
    auto warp = orion::analyzeAudioWarpMetadata(file, tempoBpm, numerator, false);
    const bool incomplete = (warp.sourceKeyRoot < 0) || (warp.bpmSource != "filename");
    const bool shortFile = warp.durationSeconds > 0.0 && warp.durationSeconds <= 20.0;
    if (incomplete && shortFile)
        warp = orion::analyzeAudioWarpMetadata(file, tempoBpm, numerator, true);   // cheap for short clips
    result.needsSignalAnalysis = incomplete && ! shortFile;   // only long files use the background pass
    result.durationSeconds  = warp.durationSeconds;
    result.sourceBpm        = warp.sourceBpm;
    result.detectedBars     = warp.detectedBars;
    result.bpmSource        = warp.bpmSource;
    result.bpmConfidence    = warp.bpmConfidence;
    result.bpmGuessed       = warp.bpmGuessed;
    result.sourceKeyRoot    = warp.sourceKeyRoot;
    result.sourceKeyIsMinor = warp.sourceKeyIsMinor;

    const auto beatsPerBar = static_cast<double>(juce::jmax(1, numerator));
    const auto durationInBeats = warp.durationSeconds * (tempoBpm / 60.0);
    result.clipLengthInBeats = juce::jmax(minimumClipLengthInBeats, durationInBeats);

    if (warp.detectedBars > 0)
    {
        result.clipLengthInBeats = juce::jmax(minimumClipLengthInBeats,
                                              static_cast<double>(warp.detectedBars) * beatsPerBar);
    }
    else if (warp.sourceBpm > 0.0 && warp.durationSeconds > 0.0)
    {
        // Musical length (in beats) of the source — snapped to the grid. This is what the
        // clip occupies once warped to the project tempo (source-beats map 1:1 to project-beats).
        const auto sourceBeats = warp.durationSeconds * (warp.sourceBpm / 60.0);
        const auto snappedBeats = std::round(sourceBeats / snapSizeInBeats) * snapSizeInBeats;
        result.clipLengthInBeats = juce::jmax(minimumClipLengthInBeats, snappedBeats);
    }

    return result;
}

juce::String clipNameForImportedFile(const juce::File& file, const juce::DynamicObject& payload)
{
    if (file.existsAsFile())
        return file.getFileNameWithoutExtension();

    const auto category = payload.getProperty("category").toString();
    const auto name = payload.getProperty("name").toString();
    return category.isNotEmpty() ? category + " / " + name : name;
}
}  // namespace

namespace orion
{
ArrangementTimelineComponent::ArrangementTimelineComponent(ProjectState& projectState, TransportEngine& transportEngine)
    : project(projectState),
      transport(transportEngine)
{
    setWantsKeyboardFocus(true);
    addChildComponent(trackVolumeInlineEditor);
    trackVolumeInlineEditor.setJustification(juce::Justification::centredRight);
    trackVolumeInlineEditor.setSelectAllWhenFocused(true);
    trackVolumeInlineEditor.setInputRestrictions(8, "-.0123456789");
    trackVolumeInlineEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    trackVolumeInlineEditor.setColour(juce::TextEditor::textColourId, juce::Colour(0xfff2f4f7));
    trackVolumeInlineEditor.setColour(juce::TextEditor::highlightColourId, juce::Colour(0xffff5a4d).withAlpha(0.42f));
    trackVolumeInlineEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    trackVolumeInlineEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    trackVolumeInlineEditor.setFont(juce::FontOptions(13.5f, juce::Font::bold));
    trackVolumeInlineEditor.onReturnKey = [this] { commitTrackVolumeEditor(true); };
    trackVolumeInlineEditor.onEscapeKey = [this] { commitTrackVolumeEditor(false); };
    trackVolumeInlineEditor.onFocusLost = [this] { commitTrackVolumeEditor(true); };
    startTimerHz(120);
}

ArrangementTimelineComponent::~ArrangementTimelineComponent() = default;

void ArrangementTimelineComponent::captureUndoSnapshot()
{
    pushUndoSnapshot();
}

void ArrangementTimelineComponent::dropLastUndoSnapshot()
{
    if (! undoStack.empty())
        undoStack.pop_back();
}

bool ArrangementTimelineComponent::canUndo() const noexcept
{
    return ! undoStack.empty();
}

std::optional<int> ArrangementTimelineComponent::getSelectedTrackIndex() const noexcept
{
    return selectedTrackIndex;
}

void ArrangementTimelineComponent::selectTrack(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= static_cast<int>(project.getTracks().size()))
        return;
    setSingleSelection(std::nullopt);
    selectedTrackIndex = trackIndex;
    notifyClipSelectionChanged();
    repaint();
}

bool ArrangementTimelineComponent::canRedo() const noexcept
{
    return ! redoStack.empty();
}

void ArrangementTimelineComponent::setFitTrackLanesToVisibleArea(bool shouldFit)
{
    if (fitTrackLanesToVisibleArea == shouldFit)
        return;

    fitTrackLanesToVisibleArea = shouldFit;
    clampScrollOffsets();
    repaint();
}

bool ArrangementTimelineComponent::undo()
{
    if (undoStack.empty())
        return false;

    redoStack.push_back(TimelineSnapshot { project.getTracks(), selectedClip });
    restoreSnapshot(undoStack.back());
    undoStack.pop_back();
    repaint();
    return true;
}

bool ArrangementTimelineComponent::redo()
{
    if (redoStack.empty())
        return false;

    undoStack.push_back(TimelineSnapshot { project.getTracks(), selectedClip });
    restoreSnapshot(redoStack.back());
    redoStack.pop_back();
    repaint();
    return true;
}

void ArrangementTimelineComponent::resetForNewProject()
{
    selectedClip.reset();
    lastClickedClip.reset();
    selectedTrackIndex.reset();
    selectedClips.clear();
    dragState.reset();
    loopSelectionState.reset();
    hoverClip.reset();
    undoStack.clear();
    redoStack.clear();
    scrollX = 0.0;
    customTrackHeights.clear();
    repaint();
}

void ArrangementTimelineComponent::setLiveRecordingWaveform(int trackIndex, int clipIndex,
                                                            std::vector<float> mins, std::vector<float> maxs)
{
    liveWaveformTrack = trackIndex;
    liveWaveformClip  = clipIndex;
    liveWaveformMin   = std::move(mins);
    liveWaveformMax   = std::move(maxs);
}

void ArrangementTimelineComponent::clearLiveRecordingWaveform()
{
    liveWaveformTrack = -1;
    liveWaveformClip  = -1;
    liveWaveformMin.clear();
    liveWaveformMax.clear();
}

void ArrangementTimelineComponent::addAudioTrack()
{
    pushUndoSnapshot();
    const auto index = static_cast<int>(project.getTracks().size());
    {
        TrackState t;
        t.name = makeUniqueTrackName("Audio Track");
        t.isMidiTrack = false;
        t.colour = theme::tracks::colourForIndex(index);
        project.getTracks().push_back(std::move(t));
    }

    setSingleSelection(std::nullopt);
    selectedTrackIndex = index;
    notifyClipSelectionChanged();
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::addMidiTrack()
{
    pushUndoSnapshot();
    const auto index = static_cast<int>(project.getTracks().size());
    {
        TrackState t;
        t.name = makeUniqueTrackName("MIDI Track");
        t.isMidiTrack = true;
        t.colour = theme::tracks::colourForIndex(index);
        project.getTracks().push_back(std::move(t));
    }

    setSingleSelection(std::nullopt);
    // Auto-select the freshly created track so subsequent actions (double-click on a
    // browser sample, etc.) target it instead of creating yet another track.
    selectedTrackIndex = index;
    notifyClipSelectionChanged();
    clampScrollOffsets();
    repaint();
}

int ArrangementTimelineComponent::insertTrackAt(int atIndex, bool isMidi, const juce::String& name, juce::Colour colour, bool autoColour)
{
    pushUndoSnapshot();
    auto& tracks = project.getTracks();
    atIndex = juce::jlimit(0, static_cast<int>(tracks.size()), atIndex);

    TrackState t;
    t.name = name.isNotEmpty() ? makeUniqueTrackName(name) : makeUniqueTrackName(isMidi ? "MIDI Track" : "Audio Track");
    t.isMidiTrack = isMidi;
    t.colour = autoColour ? theme::tracks::colourForIndex(atIndex) : colour;
    tracks.insert(tracks.begin() + atIndex, std::move(t));

    // Structural change shifts indices; custom lane heights keyed by index would point at
    // the wrong track, so drop them (they revert to default height — acceptable & rare).
    customTrackHeights.clear();

    setSingleSelection(std::nullopt);
    selectedTrackIndex = atIndex;
    notifyClipSelectionChanged();
    clampScrollOffsets();
    repaint();
    return atIndex;
}

int ArrangementTimelineComponent::folderChildInsertIndex(int folderIndex) const noexcept
{
    const auto& tracks = project.getTracks();
    if (folderIndex < 0 || folderIndex >= static_cast<int>(tracks.size()) || ! tracks[static_cast<std::size_t>(folderIndex)].isFolder)
        return folderIndex + 1;
    const auto gid = tracks[static_cast<std::size_t>(folderIndex)].groupId;
    int i = folderIndex + 1;
    while (i < static_cast<int>(tracks.size()) && tracks[static_cast<std::size_t>(i)].parentGroup == gid)
        ++i;
    return i;
}

int ArrangementTimelineComponent::owningFolderIndex(int trackIndex) const noexcept
{
    const auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return -1;
    const auto& t = tracks[static_cast<std::size_t>(trackIndex)];
    if (t.isFolder)
        return trackIndex;
    if (t.parentGroup < 0)
        return -1;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<std::size_t>(i)].isFolder && tracks[static_cast<std::size_t>(i)].groupId == t.parentGroup)
            return i;
    return -1;
}

bool ArrangementTimelineComponent::isTrackHidden(int trackIndex) const noexcept
{
    const auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return false;
    const auto& t = tracks[static_cast<std::size_t>(trackIndex)];
    if (t.parentGroup < 0)
        return false;
    for (const auto& f : tracks)
        if (f.isFolder && f.groupId == t.parentGroup)
            return f.folderCollapsed;
    return false;
}

void ArrangementTimelineComponent::toggleFolderCollapsed(int folderIndex)
{
    auto& tracks = project.getTracks();
    if (folderIndex < 0 || folderIndex >= static_cast<int>(tracks.size()) || ! tracks[static_cast<std::size_t>(folderIndex)].isFolder)
        return;
    tracks[static_cast<std::size_t>(folderIndex)].folderCollapsed = ! tracks[static_cast<std::size_t>(folderIndex)].folderCollapsed;
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::paint(juce::Graphics& g)
{
    const juce::Graphics::ScopedSaveState scopedState(g);
    const auto localBounds = getLocalBounds();
    g.reduceClipRegion(localBounds);
    g.fillAll(timelineBackground);

    auto bounds = getTimelineContentBounds(*this);
    auto rulerArea = bounds.removeFromTop(timelineRulerHeight);
    auto tracksArea = bounds;
    auto gridArea = tracksArea;
    gridArea.removeFromLeft(trackHeaderWidth);
    const auto visibleTracksArea = getVisibleTrackAreaBounds(*this);
    auto visibleGridArea = visibleTracksArea;
    visibleGridArea.removeFromLeft(trackHeaderWidth);
    auto rulerGridArea = rulerArea;
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto trackCount = static_cast<int>(project.getTracks().size());
    g.setColour(juce::Colours::black.withAlpha(0.18f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 18.0f);

    const auto beatsPerBar = static_cast<double>(project.getNumerator());
    const auto totalBeats = getTimelineEndBeats();
    auto loopLane = rulerGridArea.removeFromTop(loopLaneHeight);
    auto markerLane = rulerGridArea;
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.fillRoundedRectangle(loopLane.toFloat(), 4.0f);

    const auto addTrackButton = getAddTrackButtonBounds().toFloat();
    g.setColour(theme::surface::primary.withAlpha(0.34f));
    g.fillRoundedRectangle(addTrackButton, 7.0f);
    g.setColour(theme::warm::red.withAlpha(0.62f));
    g.drawRoundedRectangle(addTrackButton.reduced(0.5f), 7.0f, 1.2f);
    auto plus = addTrackButton.reduced(8.0f);
    g.setColour(theme::text::primary.withAlpha(0.90f));
    g.drawLine(plus.getCentreX(), plus.getY(), plus.getCentreX(), plus.getBottom(), 2.0f);
    g.drawLine(plus.getX(), plus.getCentreY(), plus.getRight(), plus.getCentreY(), 2.0f);

    if (project.hasLoopRange())
    {
        g.saveState();
        g.reduceClipRegion(loopLane);
        const auto loopStartBeat = project.getLoopStartBeat();
        const auto loopEndBeat = project.getLoopEndBeat();
        const auto loopStartX = beatToX(loopStartBeat, gridArea);
        const auto loopEndX = beatToX(loopEndBeat, gridArea);
        const auto loopBarWidth = juce::jmax(12.0f, loopEndX - loopStartX);
        auto loopBar = juce::Rectangle<float>(loopStartX, static_cast<float>(loopLane.getY() + 1),
                                              loopBarWidth, static_cast<float>(loopLane.getHeight() - 2));
        g.setColour(loopRangeColour.withAlpha(0.88f));
        g.fillRoundedRectangle(loopBar, 4.0f);
        g.setColour(loopRangeColour.brighter(0.12f));
        g.drawRoundedRectangle(loopBar, 4.0f, 1.0f);

        const auto handleWidth = 4.0f;
        g.setColour(loopRangeColour.brighter(0.35f));
        g.fillRoundedRectangle(loopBar.withWidth(handleWidth), 2.0f);
        g.fillRoundedRectangle(loopBar.withX(loopBar.getRight() - handleWidth).withWidth(handleWidth), 2.0f);
        g.restoreState();
    }

    // Calculate the visible beat range from the independent timeline zoom.
    const auto gridWidth = static_cast<double>(gridArea.getWidth());
    const int firstVisibleBeat = pixelsPerBeat > 0.0 ? juce::jmax(0, static_cast<int>(scrollX / pixelsPerBeat)) : 0;
    const int lastVisibleBeat = pixelsPerBeat > 0.0 ? static_cast<int>((scrollX + gridWidth) / pixelsPerBeat) + 1 : static_cast<int>(totalBeats);
    const int maxBeat = juce::jmax(lastVisibleBeat, static_cast<int>(std::ceil(totalBeats)));

    const auto beatsPerBarInt = juce::jmax(1, static_cast<int>(beatsPerBar));
    const auto minGridSpacingPixels = 12.0;
    const auto fineStepBeats = chooseGridStepBeats(pixelsPerBeat, beatsPerBarInt, 18.0);
    const auto barStepBeats = chooseGridStepBeats(pixelsPerBeat, beatsPerBarInt, 42.0);
    const auto majorStepBeats = chooseGridStepBeats(pixelsPerBeat, beatsPerBarInt, 86.0);
    const auto sectionStepBeats = chooseGridStepBeats(pixelsPerBeat, beatsPerBarInt, 170.0);
    const auto labelStepBeats = chooseGridStepBeats(pixelsPerBeat, beatsPerBarInt, 58.0);

        auto drawGridLayer = [&](juce::Rectangle<int> verticalArea, int stepBeats, juce::Colour colour, float thickness, bool forceVisible = false)
        {
            if (stepBeats <= 0 || (! forceVisible && pixelsPerBeat * static_cast<double>(stepBeats) < minGridSpacingPixels))
                return;

        const auto firstBeat = (firstVisibleBeat / stepBeats) * stepBeats;
        g.setColour(colour);
        for (int beat = firstBeat; beat <= maxBeat; beat += stepBeats)
        {
            const auto x = beatToX(static_cast<double>(beat), gridArea);
            if (x > static_cast<float>(gridArea.getRight() + 50))
                break;
            if (x < static_cast<float>(gridArea.getX() - 50))
                continue;

            g.drawLine(x, static_cast<float>(verticalArea.getY()), x, static_cast<float>(verticalArea.getBottom()), thickness);
        }
    };

    g.saveState();
    g.reduceClipRegion(markerLane);
    drawGridLayer(rulerGridArea, fineStepBeats, minorGridColour.withAlpha(0.22f), 1.0f);
    drawGridLayer(rulerGridArea, barStepBeats, majorGridColour.withAlpha(0.36f), 1.0f);
    drawGridLayer(rulerGridArea, majorStepBeats, majorGridColour.withAlpha(0.56f), 1.3f);
    drawGridLayer(rulerGridArea, sectionStepBeats, majorGridColour.withAlpha(0.72f), 1.6f);
    drawGridLayer(rulerGridArea, labelStepBeats, majorGridColour.withAlpha(0.82f), 1.7f, true);

    const auto firstLabelBeat = (firstVisibleBeat / labelStepBeats) * labelStepBeats;
    for (int beat = firstLabelBeat; beat <= maxBeat; beat += labelStepBeats)
    {
        const auto x = beatToX(static_cast<double>(beat), gridArea);
        if (x > static_cast<float>(gridArea.getRight() + 50))
            break;
        if (x < static_cast<float>(gridArea.getX() - 50))
            continue;

        const auto barNumber = 1 + beat / beatsPerBarInt;
        g.setColour(markerColour);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(juce::String(barNumber), static_cast<int>(x) + 6, markerLane.getY(), 48, markerLane.getHeight(), juce::Justification::centredLeft);
    }
    g.restoreState();

    const auto& tracks = project.getTracks();

    g.saveState();
    g.reduceClipRegion(visibleTracksArea);

    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        const auto trackArrayIndex = static_cast<std::size_t>(trackIndex);
        auto lane = getTrackLaneBounds(trackIndex);
        if (! lane.intersects(visibleTracksArea))
            continue;

        const auto rowBounds = lane;

        auto trackNameArea = lane.removeFromLeft(trackHeaderWidth);
        g.setColour(juce::Colours::white.withAlpha(0.095f));
        g.drawHorizontalLine(rowBounds.getBottom(), static_cast<float>(rowBounds.getX()), static_cast<float>(rowBounds.getRight()));

        // Live output level for this track (0..1) — drives the header meter and the
        // "this track is playing" border glow.
        const auto trackLevel = onRequestTrackLevel
            ? juce::jlimit(0.0f, 1.0f, onRequestTrackLevel(trackIndex))
            : 0.0f;
        const auto isAudible = trackLevel > 0.015f;
        const auto isSelectedTrack = (selectedTrackIndex.has_value() && *selectedTrackIndex == trackIndex)
            || std::any_of(selectedClips.begin(), selectedClips.end(), [trackIndex](const auto& selected)
            {
                return selected.trackIndex == trackIndex;
            });

        juce::ignoreUnused(trackNameArea);
        const auto layout = computeHeaderLayout(trackIndex);
        const auto trackColour = tracks[trackArrayIndex].colour.withSaturation(0.70f).darker(0.03f);
        const auto cardBounds = layout.card.toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.32f));
        g.fillRoundedRectangle(cardBounds.translated(0.0f, 2.0f), 14.0f);
        g.setColour(isSelectedTrack
            ? trackColour.darker(0.72f).withAlpha(0.96f)
            : juce::Colour(0xff141821).withAlpha(0.92f));
        g.fillRoundedRectangle(cardBounds, 14.0f);
        if (isSelectedTrack)
        {
            juce::ColourGradient selectedGlow(trackColour.withAlpha(0.16f), cardBounds.getX(), cardBounds.getY(),
                                              trackColour.withAlpha(0.035f), cardBounds.getRight(), cardBounds.getBottom(), false);
            g.setGradientFill(selectedGlow);
            g.fillRoundedRectangle(cardBounds.reduced(1.0f), 13.0f);
        }
        // Border brightens with the signal so it's obvious which track is sounding.
        const auto borderAlpha = isSelectedTrack ? 0.88f : (isAudible ? juce::jlimit(0.58f, 0.86f, 0.58f + trackLevel * 0.36f) : 0.58f);
        g.setColour(trackColour.withAlpha(borderAlpha));
        g.drawRoundedRectangle(cardBounds.reduced(1.0f), 14.0f, isSelectedTrack ? 2.8f : (isAudible ? 1.6f + trackLevel * 1.4f : 1.6f));

        // Folder collapse triangle (▾ open / ▸ collapsed) + a coloured spine down children.
        if (tracks[trackArrayIndex].isFolder && ! layout.collapseTriangle.isEmpty())
        {
            const auto tri = layout.collapseTriangle.toFloat().reduced(2.0f);
            const auto cx = tri.getCentreX(), cy = tri.getCentreY();
            const auto s = juce::jmin(tri.getWidth(), tri.getHeight()) * 0.5f;
            juce::Path p;
            if (tracks[trackArrayIndex].folderCollapsed)
            {
                p.startNewSubPath(cx - s * 0.5f, cy - s);
                p.lineTo(cx + s * 0.7f, cy);
                p.lineTo(cx - s * 0.5f, cy + s);
            }
            else
            {
                p.startNewSubPath(cx - s, cy - s * 0.5f);
                p.lineTo(cx + s, cy - s * 0.5f);
                p.lineTo(cx, cy + s * 0.7f);
            }
            p.closeSubPath();
            g.setColour(trackColour.withAlpha(0.95f));
            g.fillPath(p);
        }
        else if (tracks[trackArrayIndex].parentGroup >= 0)
        {
            // Coloured spine on the left edge of a child card, tinted with the folder's colour.
            juce::Colour spine = trackColour;
            for (const auto& f : tracks)
                if (f.isFolder && f.groupId == tracks[trackArrayIndex].parentGroup) { spine = f.colour; break; }
            auto sp = layout.card.toFloat();
            g.setColour(spine.withAlpha(0.75f));
            g.fillRoundedRectangle(sp.getX() - 6.0f, sp.getY() + 2.0f, 3.0f, sp.getHeight() - 4.0f, 1.5f);
        }

        g.setColour(textColour.withAlpha(0.94f));
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawFittedText(tracks[trackArrayIndex].name, layout.title, juce::Justification::centredLeft, 1);

        const std::array<std::pair<juce::String, juce::Rectangle<int>>, 3> buttons {{
            { "M", layout.muteButton },
            { "S", layout.soloButton },
            { "R", layout.recordButton }
        }};
        for (const auto& [buttonText, buttonBounds] : buttons)
        {
            const auto active = (buttonText == "M" && tracks[trackArrayIndex].muted)
                || (buttonText == "S" && tracks[trackArrayIndex].solo)
                || (buttonText == "R" && tracks[trackArrayIndex].recordArmed);
            g.setColour(active ? trackColour.withAlpha(0.74f) : juce::Colours::black.withAlpha(0.42f));
            g.fillRoundedRectangle(buttonBounds.toFloat(), 6.0f);
            g.setColour(juce::Colours::white.withAlpha(active ? 0.98f : 0.84f));
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            g.drawText(buttonText, buttonBounds, juce::Justification::centred);
        }

        // Instrument button (MIDI tracks only): a tiny piano-keys glyph. The background is
        // tinted in the track colour when an instrument is loaded so it reads as "active".
        if (! layout.instrumentButton.isEmpty())
        {
            const auto hasInstrument = tracks[trackArrayIndex].instrumentPluginId.isNotEmpty();
            auto box = layout.instrumentButton.toFloat();
            g.setColour(hasInstrument ? trackColour.withAlpha(0.74f) : juce::Colours::black.withAlpha(0.42f));
            g.fillRoundedRectangle(box, 6.0f);

            // Mini keyboard: a white-key base with three black-key ticks.
            auto keys = box.reduced(box.getWidth() * 0.24f, box.getHeight() * 0.30f);
            g.setColour(juce::Colours::white.withAlpha(hasInstrument ? 0.98f : 0.82f));
            g.fillRoundedRectangle(keys, 1.5f);
            g.setColour(hasInstrument ? trackColour.darker(0.6f) : juce::Colours::black.withAlpha(0.8f));
            const auto bkW = keys.getWidth() * 0.16f;
            const auto bkH = keys.getHeight() * 0.58f;
            for (int k = 0; k < 3; ++k)
            {
                const auto cx = keys.getX() + keys.getWidth() * (0.28f + 0.22f * static_cast<float>(k));
                g.fillRect(cx - bkW * 0.5f, keys.getY(), bkW, bkH);
            }
        }

        auto sliderF = layout.slider.toFloat();
        const auto sliderRadius = sliderF.getHeight() * 0.5f;
        g.setColour(juce::Colours::black.withAlpha(0.62f));
        g.fillRoundedRectangle(sliderF, sliderRadius);
        const auto volumeRatio = juce::jmap(static_cast<float>(tracks[trackArrayIndex].volumeDb), -24.0f, 12.0f, 0.0f, 1.0f);
        auto volumeFill = sliderF.removeFromLeft(juce::jmax(sliderF.getHeight(), sliderF.getWidth() * juce::jlimit(0.0f, 1.0f, volumeRatio)));
        g.setColour(trackColour.withAlpha(0.84f));
        g.fillRoundedRectangle(volumeFill, sliderRadius);

        if (! (volumeEditorTrackIndex.has_value() && *volumeEditorTrackIndex == trackIndex))
        {
            g.setColour(juce::Colours::white.withAlpha(0.88f));
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            g.drawText(juce::String(tracks[trackArrayIndex].volumeDb, 1) + " dB",
                       layout.volumeValue, juce::Justification::centredRight);
        }

        // Stereo level meter on the right edge: two bars (L/R), one fixed colour scheme
        // for every track (green → amber → red, bottom-to-top), like Studio One.
        {
            auto levelL = trackLevel;
            auto levelR = trackLevel;
            if (onRequestTrackLevelStereo)
            {
                const auto stereo = onRequestTrackLevelStereo(trackIndex);
                levelL = stereo.first;
                levelR = stereo.second;
            }

            const auto meterF = layout.meter.toFloat();
            const auto gap = 2.0f;
            const auto barW = (meterF.getWidth() - gap) * 0.5f;
            auto leftBar  = meterF.withWidth(barW);
            auto rightBar = meterF.withWidth(barW).withX(meterF.getRight() - barW);

            const auto drawBar = [&](juce::Rectangle<float> bar, float level)
            {
                const auto radius = bar.getWidth() * 0.5f;
                g.setColour(juce::Colours::black.withAlpha(0.55f));
                g.fillRoundedRectangle(bar, radius);
                if (level <= 0.001f)
                    return;

                // Gradient anchored to the full bar so a given height is always the same
                // colour, regardless of how full the bar is.
                juce::ColourGradient grad(juce::Colour(0xff39d36b), bar.getX(), bar.getBottom(),
                                          orion::theme::warm::red, bar.getX(), bar.getY(), false);
                grad.addColour(0.72, juce::Colour(0xffe7c93a));
                auto fill = bar.removeFromBottom(juce::jmax(bar.getWidth(), bar.getHeight() * level));
                g.setGradientFill(grad);
                g.fillRoundedRectangle(fill, radius);
            };

            drawBar(leftBar, levelL);
            drawBar(rightBar, levelR);
        }
    }

    g.reduceClipRegion(gridArea);

    drawGridLayer(visibleTracksArea, fineStepBeats, minorGridColour.withAlpha(0.20f), 1.0f);
    drawGridLayer(visibleTracksArea, barStepBeats, majorGridColour.withAlpha(0.30f), 1.0f);
    drawGridLayer(visibleTracksArea, majorStepBeats, majorGridColour.withAlpha(0.48f), 1.15f);
    drawGridLayer(visibleTracksArea, sectionStepBeats, majorGridColour.withAlpha(0.62f), 1.4f);
    drawGridLayer(visibleTracksArea, labelStepBeats, majorGridColour.withAlpha(0.72f), 1.65f, true);

    // Draw Clips
    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        const auto trackArrayIndex = static_cast<std::size_t>(trackIndex);
        if (! getTrackLaneBounds(trackIndex).intersects(visibleTracksArea))
            continue;

        for (const auto& clip : tracks[trackArrayIndex].clips)
        {
            const auto clipIndex = static_cast<int>(&clip - tracks[trackArrayIndex].clips.data());
            const auto clipBoundsInt = getClipBounds(clip, trackIndex);
            const auto clipBounds = clipBoundsInt.toFloat();
            auto clipContentBounds = clipBoundsInt.reduced(0, 6);
            const auto shouldDrawLabel = clipBoundsInt.getWidth() > 36;
            const auto shouldDrawWaveform = clipBoundsInt.getWidth() > 16;
            const auto clipHeaderHeight = juce::jmin(20, juce::jmax(15, clipContentBounds.getHeight() / 5));
            auto clipHeaderBounds = clipContentBounds.removeFromTop(clipHeaderHeight);
            clipContentBounds.removeFromTop(2);
            auto clipBodyBounds = clipContentBounds;
            const auto headerStrip = juce::Rectangle<float>(clipBounds.getX(),
                                                            clipBounds.getY(),
                                                            clipBounds.getWidth(),
                                                            static_cast<float>(clipHeaderHeight));

            const auto isSelected = isClipSelected(SelectedClip { trackIndex, clipIndex });
            g.saveState();
            g.reduceClipRegion(clipBoundsInt);

            // Body fill. Selected clips get a separate, lighter state like Studio One:
            // same hue, clearer focus, without relying on the outline alone.
            const auto clipBase = clip.colour.withSaturation(0.68f).darker(0.02f);
            const auto clipFill = isSelected
                ? clipBase.brighter(0.18f).withSaturation(0.58f)
                : clipBase.withAlpha(0.90f);
            const auto waveformColour = isSelected
                ? clipBase.darker(0.66f).withAlpha(0.72f)
                : clipBase.darker(0.44f).withAlpha(clip.colour.getPerceivedBrightness() > 0.62f ? 0.48f : 0.58f);

            g.setColour(clipFill);
            g.fillRoundedRectangle(clipBounds, 10.0f);

            // Studio One-style header strip: a darker band at the top where the clip
            // name lives. Gives every clip a visible top edge even when adjacent clips
            // touch — together with the dark outline below this creates a clear seam
            // between flush clips.
            if (clipBounds.getHeight() > 18.0f)
            {
                juce::Path headerPath;
                // Round only the top corners — the bottom of the header is a flat seam.
                headerPath.addRoundedRectangle(headerStrip.getX(), headerStrip.getY(),
                                               headerStrip.getWidth(), headerStrip.getHeight(),
                                               10.0f, 10.0f,
                                               true, true, false, false);
                g.setColour((isSelected ? clipFill.darker(0.28f) : clipBase.darker(0.42f)).withAlpha(0.92f));
                g.fillPath(headerPath);
                juce::ColourGradient headerShade((isSelected ? clipFill.brighter(0.08f) : clipBase.brighter(0.12f)).withAlpha(0.36f), headerStrip.getX(), headerStrip.getY(),
                                                 (isSelected ? clipFill.darker(0.16f) : clipBase.darker(0.28f)).withAlpha(0.76f), headerStrip.getRight(), headerStrip.getBottom(), false);
                g.setGradientFill(headerShade);
                g.fillPath(headerPath);
            }

            if (clip.type == ClipType::audio && shouldDrawWaveform)
            {
                // Use the live capture buffer while this clip is being recorded (the file
                // isn't readable yet); otherwise read peaks from the source file.
                const auto isLive = (trackIndex == liveWaveformTrack && clipIndex == liveWaveformClip
                                     && ! liveWaveformMin.empty());
                const std::vector<float>* minVals = nullptr;
                const std::vector<float>* maxVals = nullptr;
                if (isLive)
                {
                    minVals = &liveWaveformMin;
                    maxVals = &liveWaveformMax;
                }
                else if (const auto* peaks = getOrComputePeaks(clip.sourcePath); peaks != nullptr && ! peaks->minVals.empty())
                {
                    minVals = &peaks->minVals;
                    maxVals = &peaks->maxVals;
                }

                if (minVals != nullptr)
                {
                    g.saveState();
                    g.reduceClipRegion(clipBoundsInt);
                    g.setColour(waveformColour);

                    const auto bodyX     = clipBodyBounds.getX();
                    const auto bodyWidth = juce::jmax(1, clipBodyBounds.getWidth());
                    const auto centerY   = static_cast<float>(clipBodyBounds.getY()) + clipBodyBounds.getHeight() * 0.5f;
                    const auto halfH     = juce::jmax(2.0f, static_cast<float>(clipBodyBounds.getHeight()) * 0.45f);
                    const auto numBuckets = static_cast<int>(minVals->size());

                    // Live capture is full (no trim); otherwise draw only the trimmed
                    // region [sampleStartRatio..sampleEndRatio] of the source.
                    const auto trimStartR = isLive ? 0.0 : juce::jlimit(0.0, 0.999, clip.sampleStartRatio);
                    const auto trimEndR   = isLive ? 1.0 : juce::jlimit(trimStartR + 0.001, 1.0, clip.sampleEndRatio);
                    const auto bucketStart = trimStartR * numBuckets;
                    const auto bucketSpan  = juce::jmax(1.0, (trimEndR - trimStartR) * numBuckets);

                    for (int px = 0; px < bodyWidth; ++px)
                    {
                        const auto bStart = static_cast<int>(bucketStart + static_cast<double>(px) * bucketSpan / bodyWidth);
                        const auto bEnd   = static_cast<int>(bucketStart + static_cast<double>(px + 1) * bucketSpan / bodyWidth);
                        const auto safeEnd = juce::jmax(bStart + 1, bEnd);

                        float minVal = 0.0f, maxVal = 0.0f;
                        for (int b = bStart; b < safeEnd && b < numBuckets; ++b)
                        {
                            minVal = juce::jmin(minVal, (*minVals)[static_cast<size_t>(b)]);
                            maxVal = juce::jmax(maxVal, (*maxVals)[static_cast<size_t>(b)]);
                        }

                        const auto x       = static_cast<float>(bodyX + px);
                        const auto top     = centerY + minVal * halfH;
                        const auto bottom  = centerY + maxVal * halfH;
                        if (bottom - top >= 0.5f)
                            g.drawLine(x, top, x, bottom, 1.0f);
                        else
                            g.fillRect(x, centerY - 0.5f, 1.0f, 1.0f);
                    }
                    g.restoreState();
                }
            }
            else if (! clip.midiNotes.empty() && clipBodyBounds.getWidth() > 12 && clipBodyBounds.getHeight() > 12)
            {
                int minPitch = clip.midiNotes.front().pitch;
                int maxPitch = clip.midiNotes.front().pitch;
                for (const auto& note : clip.midiNotes)
                {
                    minPitch = juce::jmin(minPitch, note.pitch);
                    maxPitch = juce::jmax(maxPitch, note.pitch);
                }

                const auto displayedPitchCount = juce::jmax(1, maxPitch - minPitch + 1);
                const auto midiLaneHeight = static_cast<float>(clipBodyBounds.getHeight()) / static_cast<float>(displayedPitchCount);
                g.setColour(juce::Colours::white.withAlpha(0.42f));
                for (const auto& note : clip.midiNotes)
                {
                    const auto startRatio = clip.lengthInBeats > 0.0 ? note.startBeat / clip.lengthInBeats : 0.0;
                    const auto endRatio = clip.lengthInBeats > 0.0 ? (note.startBeat + note.lengthInBeats) / clip.lengthInBeats : 0.1;
                    const auto noteX = clipBodyBounds.getX() + static_cast<int>(std::round(startRatio * clipBodyBounds.getWidth()));
                    const auto noteRight = clipBodyBounds.getX() + static_cast<int>(std::round(endRatio * clipBodyBounds.getWidth()));
                    const auto noteWidth = juce::jmax(6, noteRight - noteX);
                    const auto laneIndexFromTop = maxPitch - note.pitch;
                    const auto noteHeight = juce::jmax(4.0f, midiLaneHeight * 0.68f);
                    const auto noteY = static_cast<float>(clipBodyBounds.getY()) + (static_cast<float>(laneIndexFromTop) * midiLaneHeight)
                        + juce::jmax(1.0f, (midiLaneHeight - noteHeight) * 0.5f);
                    g.fillRoundedRectangle(static_cast<float>(noteX),
                                           juce::jlimit(static_cast<float>(clipBodyBounds.getY()),
                                                        static_cast<float>(clipBodyBounds.getBottom()) - noteHeight,
                                                        noteY),
                                           static_cast<float>(noteWidth),
                                           noteHeight,
                                           2.5f);
                }
            }

            // Always draw a 1px darker outline around every clip. This is what makes two
            // flush clips visually separable — without it adjacent clips read as one
            // continuous block (the issue raised when we removed the 2px inset gap).
            g.setColour(clipBase.darker(0.58f).withAlpha(isSelected ? 0.95f : 0.85f));
            g.drawRoundedRectangle(clipBounds.reduced(0.5f, 0.5f), 9.5f, isSelected ? 1.4f : 1.0f);

            if (isSelected)
            {
                g.setColour(juce::Colours::white.withAlpha(0.86f));
                g.drawRoundedRectangle(clipBounds.reduced(1.4f, 1.4f), 8.8f, 2.8f);
            }

            // Fade in/out overlays (Studio One style). The curve shows the gain ramp;
            // the region above it is dimmed to read as "quieter here". A mid-point
            // handle on the curve bends the curvature.
            {
                const auto isHovered = hoverClip.has_value()
                    && hoverClip->clip.trackIndex == trackIndex
                    && hoverClip->clip.clipIndex == clipIndex;
                const auto topY    = static_cast<float>(clipBounds.getY());
                const auto bottomY = static_cast<float>(clipBounds.getBottom());
                const auto leftX   = static_cast<float>(clipBounds.getX());
                const auto rightX  = static_cast<float>(clipBounds.getRight());
                const auto heightF = juce::jmax(1.0f, bottomY - topY);
                const auto maxFadePx = juce::jmax(0.0f, static_cast<float>(clipBounds.getWidth()));

                const auto fadeInPx = juce::jlimit(0.0f, maxFadePx,
                    static_cast<float>(clip.fadeInBeats * pixelsPerBeat));
                const auto fadeOutPx = juce::jlimit(0.0f, maxFadePx,
                    static_cast<float>(clip.fadeOutBeats * pixelsPerBeat));

                const auto handleVisible = isHovered || isSelected;
                const auto curveY = [&](double gain) { return bottomY - static_cast<float>(gain) * heightF; };

                // Clip the fade fills/strokes to the clip's rounded shape so they don't
                // poke past the rounded corners (the outer clip region is rectangular).
                juce::Path clipShape;
                clipShape.addRoundedRectangle(clipBounds, 10.0f);
                g.saveState();
                g.reduceClipRegion(clipShape);

                if (fadeInPx > 0.5f)
                {
                    const auto endX = leftX + fadeInPx;
                    const auto steps = juce::jmax(2, static_cast<int>(fadeInPx / 3.0f));
                    juce::Path curve;
                    curve.startNewSubPath(leftX, curveY(fadeCurveGain(0.0, clip.fadeInCurve)));
                    for (int s = 1; s <= steps; ++s)
                    {
                        const auto t = static_cast<double>(s) / steps;
                        curve.lineTo(leftX + static_cast<float>(t) * fadeInPx, curveY(fadeCurveGain(t, clip.fadeInCurve)));
                    }

                    auto region = curve;
                    region.lineTo(endX, topY);
                    region.lineTo(leftX, topY);
                    region.closeSubPath();
                    g.setColour(juce::Colours::black.withAlpha(0.42f));
                    g.fillPath(region);
                    g.setColour(clipBase.darker(0.55f).withAlpha(0.85f));
                    g.strokePath(curve, juce::PathStrokeType(1.2f));
                }

                if (fadeOutPx > 0.5f)
                {
                    const auto startX = rightX - fadeOutPx;
                    const auto steps = juce::jmax(2, static_cast<int>(fadeOutPx / 3.0f));
                    juce::Path curve;
                    curve.startNewSubPath(startX, curveY(fadeCurveGain(1.0, clip.fadeOutCurve)));
                    for (int s = 1; s <= steps; ++s)
                    {
                        const auto t = static_cast<double>(s) / steps;          // 0..1 across width
                        const auto remaining = 1.0 - t;                         // gain proportion
                        curve.lineTo(startX + static_cast<float>(t) * fadeOutPx, curveY(fadeCurveGain(remaining, clip.fadeOutCurve)));
                    }

                    auto region = curve;
                    region.lineTo(rightX, topY);
                    region.lineTo(startX, topY);
                    region.closeSubPath();
                    g.setColour(juce::Colours::black.withAlpha(0.42f));
                    g.fillPath(region);
                    g.setColour(clipBase.darker(0.55f).withAlpha(0.85f));
                    g.strokePath(curve, juce::PathStrokeType(1.2f));
                }

                g.restoreState();

                // Grab handles: the two top corners (length) plus a mid-point handle on
                // each fade curve (curvature). Inset so they aren't clipped to half-dots.
                if (handleVisible && clipBounds.getWidth() > 18.0f)
                {
                    const auto drawHandle = [&](float cx, float cy, float r)
                    {
                        g.setColour(juce::Colours::black.withAlpha(0.55f));
                        g.fillEllipse(cx - r - 1.0f, cy - r - 1.0f, (r + 1.0f) * 2.0f, (r + 1.0f) * 2.0f);
                        g.setColour(orion::theme::warm::red.brighter(0.4f));
                        g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
                        g.setColour(juce::Colours::white);
                        g.fillEllipse(cx - 1.8f, cy - 1.8f, 3.6f, 3.6f);
                    };

                    const auto cornerR = 5.0f;
                    const auto inHandleX  = juce::jlimit(leftX + cornerR, rightX - cornerR, leftX + fadeInPx);
                    const auto outHandleX = juce::jlimit(leftX + cornerR, rightX - cornerR, rightX - fadeOutPx);
                    const auto cornerY = topY + cornerR + 2.0f;
                    drawHandle(inHandleX, cornerY, cornerR);
                    drawHandle(outHandleX, cornerY, cornerR);

                    // Mid-curve handles (only when the fade is long enough to grab).
                    if (fadeInPx > 18.0f)
                        drawHandle(leftX + fadeInPx * 0.5f, curveY(fadeCurveGain(0.5, clip.fadeInCurve)), 4.0f);
                    if (fadeOutPx > 18.0f)
                        drawHandle(rightX - fadeOutPx * 0.5f, curveY(fadeCurveGain(0.5, clip.fadeOutCurve)), 4.0f);
                }
            }

            if (shouldDrawLabel)
            {
                juce::ignoreUnused(clipHeaderBounds);
                const auto headerArea = headerStrip.getSmallestIntegerContainer().reduced(6, 1);
                const auto adaptiveSize = juce::jlimit(11.0f,
                                                       15.0f,
                                                       juce::jmin(static_cast<float>(clipHeaderBounds.getHeight()) - 2.0f,
                                                                  11.0f + static_cast<float>(clipBoundsInt.getWidth()) / 150.0f));
                g.setFont(juce::FontOptions(adaptiveSize, juce::Font::bold));

                // Keep clip names readable by pinning them to a dedicated top header strip.
                g.setColour(juce::Colours::black.withAlpha(0.46f));
                g.drawFittedText(clip.name,
                                 headerArea.translated(1, 1),
                                 juce::Justification::topLeft,
                                 1,
                                 0.90f);

                g.setColour(theme::text::primary.withAlpha(0.96f));
                g.drawFittedText(clip.name,
                                 headerArea,
                                 juce::Justification::topLeft,
                                 1,
                                 0.90f);
            }
            g.restoreState();
        }
    }

    g.restoreState();

    // Draw Playhead with a top cap in the ruler
    g.saveState();
    g.reduceClipRegion(visibleGridArea);
    const auto playheadX = beatToX(transport.getPlayheadBeat(), gridArea);
    const auto isPlaying = transport.isPlaying();
    juce::ColourGradient gradient(playheadColour.withAlpha(0.0f), playheadX - 8.0f, 0.0f,
                                  playheadColour.withAlpha(0.0f), playheadX + 8.0f, 0.0f, false);
    gradient.addColour(0.5, playheadColour.withAlpha(isPlaying ? 0.35f : 0.15f));
    g.setGradientFill(gradient);
    g.fillRect(playheadX - 8.0f, static_cast<float>(visibleGridArea.getY()), 16.0f, static_cast<float>(visibleGridArea.getHeight()));

    g.setColour(playheadColour.withAlpha(isPlaying ? 0.95f : 0.78f));
    g.drawLine(playheadX, static_cast<float>(visibleGridArea.getY()), playheadX, static_cast<float>(visibleGridArea.getBottom()), 2.0f);
    g.fillEllipse(playheadX - 5.0f, static_cast<float>(visibleGridArea.getY()) - 5.0f, 10.0f, 10.0f);
    g.restoreState();

    // Empty "new track" lane that frees space below the last track when the playlist
    // is full and you drag a sample in. Same rounded shape as the clips.
    if (browserAppendAnim > 0.01f)
    {
        const auto trackCount = static_cast<int>(project.getTracks().size());
        const auto area = getVisibleTrackAreaBounds(*this);
        const int laneTop = trackCount > 0 ? getTrackLaneBounds(trackCount - 1).getBottom() : area.getY();
        const int laneH = juce::roundToInt(defaultLaneHeight * browserAppendAnim);
        auto lane = juce::Rectangle<int>(area.getX() + trackHeaderWidth, laneTop, area.getWidth() - trackHeaderWidth, laneH)
                        .reduced(2, 2)
                        .toFloat();

        if (lane.getHeight() > 1.0f)
        {
            g.saveState();
            g.reduceClipRegion(area);
            g.setColour(theme::cool::cyan.withAlpha(0.16f * browserAppendAnim));
            g.fillRoundedRectangle(lane, 10.0f);
            g.setColour(theme::cool::cyan.withAlpha(0.6f * browserAppendAnim));
            g.drawRoundedRectangle(lane, 10.0f, 1.5f);
            g.restoreState();
        }
    }

    if (browserDropPreviewBounds.has_value())
    {
        g.saveState();
        g.reduceClipRegion(visibleGridArea);
        if (browserDropCreatesNewTrack)
        {
            auto ghostLane = *browserDropPreviewBounds;
            ghostLane.setX(visibleGridArea.getX());
            ghostLane.setWidth(visibleGridArea.getWidth());
            g.setColour(browserDropPreviewColour.withAlpha(0.10f));
            g.fillRect(ghostLane);
            g.setColour(browserDropPreviewColour.withAlpha(0.36f));
            g.drawHorizontalLine(ghostLane.getY(), static_cast<float>(ghostLane.getX()), static_cast<float>(ghostLane.getRight()));
        }

        g.setColour(browserDropPreviewColour.withAlpha(0.28f));
        g.fillRoundedRectangle(browserDropPreviewBounds->toFloat(), 10.0f);
        g.setColour(browserDropPreviewColour.withAlpha(0.95f));
        g.drawRoundedRectangle(browserDropPreviewBounds->toFloat(), 10.0f, 1.5f);
        g.restoreState();
    }

    if (selectionBoxState.active)
    {
        const auto selectionBounds = getSelectionBoxBounds();
        if (! selectionBounds.isEmpty())
        {
            g.saveState();
            g.reduceClipRegion(visibleGridArea);
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.fillRect(selectionBounds);
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.drawRect(selectionBounds, 1);
            g.restoreState();
        }
    }

    paintToolPalette(g);
}

juce::Rectangle<int> ArrangementTimelineComponent::getToolButtonBounds(int index) const noexcept
{
    const auto bounds = getTimelineContentBounds(*this);
    constexpr int w = 28, h = 24, gap = 6, padX = 10, padY = 3;
    return juce::Rectangle<int>(bounds.getX() + padX + index * (w + gap), bounds.getY() + padY, w, h);
}

void ArrangementTimelineComponent::paintToolPalette(juce::Graphics& g)
{
    // Tool palette hidden for now — placement is being reconsidered.
    juce::ignoreUnused(g);
    return;

    const std::array<const char*, 2> tips { "Select / Move (V)", "Knife / Split (B)" };
    juce::ignoreUnused(tips);
    for (int i = 0; i < 2; ++i)
    {
        const auto b = getToolButtonBounds(i).toFloat();
        const auto active = (i == 0 && currentTool == ToolMode::pointer)
                         || (i == 1 && currentTool == ToolMode::knife);
        g.setColour(active ? orion::theme::warm::red.withAlpha(0.92f) : juce::Colours::black.withAlpha(0.42f));
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(juce::Colours::white.withAlpha(active ? 0.85f : 0.16f));
        g.drawRoundedRectangle(b, 6.0f, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(active ? 0.98f : 0.72f));
        if (i == 0)
        {
            // Pointer arrow.
            juce::Path p;
            const auto x = b.getX() + b.getWidth() * 0.34f;
            const auto y = b.getY() + b.getHeight() * 0.24f;
            const auto s = juce::jmin(b.getWidth(), b.getHeight());
            p.startNewSubPath(x, y);
            p.lineTo(x, y + s * 0.62f);
            p.lineTo(x + s * 0.18f, y + s * 0.44f);
            p.lineTo(x + s * 0.40f, y + s * 0.44f);
            p.closeSubPath();
            g.fillPath(p);
        }
        else
        {
            // Knife: a blade (filled triangle) with a handle line.
            const auto cx = b.getCentreX();
            const auto cy = b.getCentreY();
            const auto s  = juce::jmin(b.getWidth(), b.getHeight());
            juce::Path blade;
            blade.startNewSubPath(cx - s * 0.30f, cy - s * 0.28f);
            blade.lineTo(cx + s * 0.10f, cy + s * 0.30f);
            blade.lineTo(cx - s * 0.30f, cy + s * 0.30f);
            blade.closeSubPath();
            g.fillPath(blade);
            g.drawLine(cx + s * 0.10f, cy + s * 0.30f, cx + s * 0.34f, cy - s * 0.06f, 2.0f);
        }
    }
}

void ArrangementTimelineComponent::resized()
{
    clampScrollOffsets();
    if (volumeEditorTrackIndex.has_value())
        trackVolumeInlineEditor.setBounds(getTrackVolumeValueBounds(*volumeEditorTrackIndex));
    repaint();
}

void ArrangementTimelineComponent::mouseDown(const juce::MouseEvent& event)
{
    if (volumeEditorTrackIndex.has_value() && ! trackVolumeInlineEditor.getBounds().contains(event.getPosition()))
        commitTrackVolumeEditor(true);

    if (getAddTrackButtonBounds().contains(event.getPosition()))
    {
        if (onAddTrackRequested)
            onAddTrackRequested();        // open the full Add Track dialog
        else
            showAddTrackMenu();           // fallback: inline popup
        return;
    }

    // Tool palette hidden for now — click handling disabled.
    if ((false) && getToolButtonBounds(0).contains(event.getPosition()))
    {
        currentTool = ToolMode::pointer;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }
    if ((false) && getToolButtonBounds(1).contains(event.getPosition()))
    {
        currentTool = ToolMode::knife;
        repaint();
        return;
    }

    auto bounds = getTimelineContentBounds(*this);
    auto rulerArea = bounds.removeFromTop(timelineRulerHeight);
    auto tracksArea = bounds;
    auto gridArea = tracksArea;
    gridArea.removeFromLeft(trackHeaderWidth);

    const auto headerResizeX = tracksArea.getX() + trackHeaderWidth;
    if (tracksArea.contains(event.getPosition())
        && std::abs(event.getPosition().x - headerResizeX) <= 6)
    {
        trackHeaderWidthResizeState.active = true;
        trackHeaderWidthResizeState.mouseDownX = event.getPosition().x;
        trackHeaderWidthResizeState.originalWidth = trackHeaderWidth;
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    const auto trackCount = static_cast<int>(project.getTracks().size());
    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        auto lane = getTrackLaneBounds(trackIndex);
        auto headerArea = lane.removeFromLeft(trackHeaderWidth);
        if (headerArea.contains(event.getPosition())
            && std::abs(event.getPosition().y - lane.getBottom()) <= 6)
        {
            inspectorResizeState.active = true;
            inspectorResizeState.trackIndex = trackIndex;
            inspectorResizeState.mouseDownY = event.getPosition().y;
            inspectorResizeState.originalHeight = getLaneHeightForTrack(trackIndex);
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            return;
        }
    }

    // The playhead can only be grabbed from the ruler (top bar with bar numbers).
    // Dragging it from the tracks area is disabled so it doesn't steal clicks meant
    // for clip handles (e.g. a fade handle right at the clip start).
    const auto playheadX = beatToX(transport.getPlayheadBeat(), gridArea);
    if (rulerArea.contains(event.getPosition())
        && std::abs(static_cast<float>(event.getPosition().x) - playheadX) <= static_cast<float>(playheadHitWidth))
    {
        playheadDragState.active = true;
        transport.setPlayheadBeat(snapBeatValue(xToBeatPosition(event.getPosition().x)));
        repaint();
        return;
    }

    auto rulerGridArea = rulerArea;
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto loopLane = rulerGridArea.removeFromTop(loopLaneHeight);

    if (project.hasLoopRange() && loopLane.contains(event.getPosition()))
    {
        const auto beat = juce::jlimit(0.0, project.getLoopLengthInBeats(), snapBeatValue(xToBeatPosition(event.getPosition().x)));
        auto mode = LoopSelectionState::Mode::create;
        double originalStart = beat;
        double originalEnd = juce::jmin(project.getLoopLengthInBeats(), beat + static_cast<double>(project.getNumerator()));

        if (project.hasLoopRange())
        {
            const auto loopStartX = beatToX(project.getLoopStartBeat(), gridArea);
            const auto loopEndX = beatToX(project.getLoopEndBeat(), gridArea);
            originalStart = project.getLoopStartBeat();
            originalEnd = project.getLoopEndBeat();

            if (std::abs(event.getPosition().x - loopStartX) <= loopHandleHitWidth)
                mode = LoopSelectionState::Mode::resizeStart;
            else if (std::abs(event.getPosition().x - loopEndX) <= loopHandleHitWidth)
                mode = LoopSelectionState::Mode::resizeEnd;
            else if (beat >= originalStart && beat <= originalEnd)
                mode = LoopSelectionState::Mode::move;
        }

        loopSelectionState = LoopSelectionState { mode, beat, originalStart, originalEnd };

        if (mode == LoopSelectionState::Mode::create)
            project.setLoopRange(originalStart, originalEnd);

        repaint();
        return;
    }

    // Click anywhere on the ruler (the bar-number strip / empty loop lane) jumps the
    // playhead to that position and starts a scrub-drag — so you can move to any point.
    if (rulerArea.contains(event.getPosition()) && event.getPosition().x >= gridArea.getX())
    {
        playheadDragState.active = true;
        transport.setPlayheadBeat(juce::jmax(0.0, snapBeatValue(xToBeatPosition(event.getPosition().x))));
        if (onTransportSeek)
            onTransportSeek();
        repaint();
        return;
    }

    const auto trackHeaderHit = hitTestTrackHeader(event.getPosition());
    if (trackHeaderHit.has_value())
    {
        // Folder collapse triangle: toggle show/hide of the folder's children.
        {
            const auto idx = trackHeaderHit->trackIndex;
            if (idx >= 0 && idx < static_cast<int>(project.getTracks().size())
                && project.getTracks()[static_cast<std::size_t>(idx)].isFolder)
            {
                const auto tri = computeHeaderLayout(idx).collapseTriangle;
                if (! tri.isEmpty() && tri.expanded(4, 4).contains(event.getPosition()))
                {
                    toggleFolderCollapsed(idx);
                    selectedTrackIndex = idx;
                    notifyClipSelectionChanged();
                    return;
                }
            }
        }

        // Right-click anywhere on the header body (not on a control) opens the
        // track context menu (used for loading / managing VST instruments).
        if (event.mods.isPopupMenu() && trackHeaderHit->control == TrackHeaderControl::none)
        {
            selectedTrackIndex = trackHeaderHit->trackIndex;
            notifyClipSelectionChanged();
            grabKeyboardFocus();
            repaint();
            if (onTrackHeaderRightClick)
                onTrackHeaderRightClick(trackHeaderHit->trackIndex);
            return;
        }

        auto& track = project.getTracks()[static_cast<std::size_t>(trackHeaderHit->trackIndex)];
        selectedTrackIndex = trackHeaderHit->trackIndex;
        setSingleSelection(std::nullopt);

        if (trackHeaderHit->control == TrackHeaderControl::mute)
            track.muted = ! track.muted;
        else if (trackHeaderHit->control == TrackHeaderControl::solo)
            track.solo = ! track.solo;
        else if (trackHeaderHit->control == TrackHeaderControl::record)
            track.recordArmed = ! track.recordArmed;
        else if (trackHeaderHit->control == TrackHeaderControl::instrument)
        {
            notifyClipSelectionChanged();
            grabKeyboardFocus();
            repaint();
            if (onTrackInstrumentClicked)
                onTrackInstrumentClicked(trackHeaderHit->trackIndex);
            return;
        }
        else if (trackHeaderHit->control == TrackHeaderControl::volume)
        {
            trackVolumeDragState = TrackVolumeDragState { true, trackHeaderHit->trackIndex, trackHeaderHit->bounds };
            updateTrackVolumeFromPoint(trackHeaderHit->trackIndex, trackHeaderHit->bounds, event.getPosition().x);
        }
        else if (trackHeaderHit->control == TrackHeaderControl::volumeValue)
        {
            showTrackVolumeEditor(trackHeaderHit->trackIndex);
            notifyClipSelectionChanged();
            repaint();
            return;
        }

        // FL-style: clicking a track header arms it for laptop keyboard input
        // (no separate "arm" button) — fires selection callback with clipIndex=-1.
        notifyClipSelectionChanged();
        grabKeyboardFocus();
        repaint();
        return;
    }

    const auto hit = hitTestClipDetailed(event.getPosition(), false);

    // Knife tool: clicking a clip splits it at the clicked position (snapped).
    if (currentTool == ToolMode::knife && hit.has_value())
    {
        const auto splitBeat = snapBeatValue(xToBeatPosition(event.getPosition().x));
        pushUndoSnapshot();
        if (splitClipAtBeat(hit->clip.trackIndex, hit->clip.clipIndex, splitBeat))
        {
            setSingleSelection(std::nullopt);
            notifyClipSelectionChanged();
        }
        else if (! undoStack.empty())
        {
            undoStack.pop_back();   // nothing split — drop the snapshot
        }
        grabKeyboardFocus();
        repaint();
        return;
    }

    if (hit.has_value())
    {
        if (event.mods.isShiftDown() && ! hit->overResizeHandle)
            selectRangeTo(hit->clip);
        else
            setSingleSelection(hit->clip);
    }
    else if (gridArea.contains(event.getPosition()))
    {
        if (! event.mods.isShiftDown())
        {
            selectedTrackIndex.reset();
            setSingleSelection(std::nullopt);
        }

        selectionBoxState = SelectionBoxState { true, event.getPosition(), event.getPosition() };
        dragState.reset();
        grabKeyboardFocus();
        repaint();
        return;
    }
    else if (tracksArea.contains(event.getPosition()) && selectedClip.has_value())
    {
        selectedTrackIndex.reset();
        setSingleSelection(std::nullopt);
    }
    dragState.reset();

    if (hit.has_value())
    {
        const auto& clip = project.getTracks()[static_cast<std::size_t>(hit->clip.trackIndex)].clips[static_cast<std::size_t>(hit->clip.clipIndex)];
        // Edge drags: plain = trim (constant speed), Alt = time-stretch.
        const auto stretch = event.mods.isAltDown();
        const auto dragMode = hit->overFadeInCurveHandle
            ? DragMode::fadeInCurve
            : hit->overFadeOutCurveHandle
                ? DragMode::fadeOutCurve
                : hit->overFadeInHandle
                    ? DragMode::fadeIn
                    : hit->overFadeOutHandle
                        ? DragMode::fadeOut
                        : hit->overResizeHandle
                            ? (stretch ? DragMode::stretchRight : DragMode::resizeRight)
                            : hit->overLeftResizeHandle
                                ? (stretch ? DragMode::stretchLeft : DragMode::resizeLeft)
                                : DragMode::move;
        dragState = DragState {
            hit->clip,
            dragMode,
            event.getPosition(),
            clip.startBeat,
            clip.lengthInBeats,
            clip.warpTargetLengthInBeats,   // raw (0 = unset); fullLen derived in mouseDrag
            clip.sampleStartRatio,
            clip.sampleEndRatio,
            clip.fadeInBeats,
            clip.fadeOutBeats,
            clip.fadeInCurve,
            clip.fadeOutCurve,
            hit->clip.trackIndex,
            false
        };
    }

    grabKeyboardFocus();
    repaint();
}

void ArrangementTimelineComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (trackVolumeDragState.active)
    {
        updateTrackVolumeFromPoint(trackVolumeDragState.trackIndex, trackVolumeDragState.bounds, event.getPosition().x);
        repaint();
        return;
    }

    if (trackHeaderWidthResizeState.active)
    {
        const auto deltaX = event.getPosition().x - trackHeaderWidthResizeState.mouseDownX;
        trackHeaderWidth = juce::jlimit(minTrackHeaderWidth,
                                        maxTrackHeaderWidth,
                                        trackHeaderWidthResizeState.originalWidth + deltaX);
        clampScrollOffsets();
        repaint();
        return;
    }

    if (inspectorResizeState.active)
    {
        const auto deltaY = event.getPosition().y - inspectorResizeState.mouseDownY;
        if (inspectorResizeState.trackIndex >= 0)
        {
            customTrackHeights[inspectorResizeState.trackIndex] = juce::jlimit(
                minimumLaneHeight,
                maxExpandedLaneHeight,
                inspectorResizeState.originalHeight + deltaY);
        }
        clampScrollOffsets();
        repaint();
        return;
    }

    if (playheadDragState.active)
    {
        transport.setPlayheadBeat(juce::jmax(0.0, snapBeatValue(xToBeatPosition(event.getPosition().x))));
        if (onTransportSeek)
            onTransportSeek();
        repaint();
        return;
    }

    if (loopSelectionState.has_value())
    {
        const auto beat = juce::jlimit(0.0, project.getLoopLengthInBeats(), snapBeatValue(xToBeatPosition(event.getPosition().x)));
        if (loopSelectionState->mode == LoopSelectionState::Mode::create)
        {
            const auto startBeat = juce::jmin(loopSelectionState->anchorBeat, beat);
            const auto endBeat = juce::jmax(loopSelectionState->anchorBeat + snapSizeInBeats, beat + snapSizeInBeats);
            project.setLoopRange(startBeat, endBeat);
        }
        else if (loopSelectionState->mode == LoopSelectionState::Mode::resizeStart)
        {
            project.setLoopRange(juce::jmin(beat, loopSelectionState->originalEndBeat - snapSizeInBeats),
                                 loopSelectionState->originalEndBeat);
        }
        else if (loopSelectionState->mode == LoopSelectionState::Mode::resizeEnd)
        {
            project.setLoopRange(loopSelectionState->originalStartBeat,
                                 juce::jmax(loopSelectionState->originalStartBeat + snapSizeInBeats, beat));
        }
        else if (loopSelectionState->mode == LoopSelectionState::Mode::move)
        {
            const auto loopSpan = loopSelectionState->originalEndBeat - loopSelectionState->originalStartBeat;
            const auto delta = beat - loopSelectionState->anchorBeat;
            const auto newStart = juce::jlimit(0.0, project.getLoopLengthInBeats() - loopSpan, loopSelectionState->originalStartBeat + delta);
            project.setLoopRange(newStart, newStart + loopSpan);
        }

        repaint();
        return;
    }

    if (selectionBoxState.active)
    {
        updateSelectionBox(event.getPosition());
        repaint();
        return;
    }

    if (! dragState.has_value())
        return;

    if (! dragState->historyCaptured)
    {
        pushUndoSnapshot();
        dragState->historyCaptured = true;
    }

    if (dragState->mode == DragMode::move)
    {
        const auto hoveredTrackIndex = trackIndexFromY(event.getPosition().y);
        if (hoveredTrackIndex >= 0 && hoveredTrackIndex != dragState->clip.trackIndex)
        {
            moveSelectedClipToTrack(hoveredTrackIndex);
        }
    }

    auto& tracks = project.getTracks();
    auto& clip = tracks[static_cast<std::size_t>(dragState->clip.trackIndex)].clips[static_cast<std::size_t>(dragState->clip.clipIndex)];
    const auto beatDelta = snapBeatValue(xToBeatDelta(event.getDistanceFromDragStartX()));

    if (dragState->mode == DragMode::fadeIn)
    {
        // Fine (unsnapped) fade dragging. Drag right to lengthen the fade-in.
        const auto rawDelta = xToBeatDelta(event.getDistanceFromDragStartX());
        clip.fadeInBeats = juce::jlimit(0.0,
                                        juce::jmax(0.0, clip.lengthInBeats - clip.fadeOutBeats),
                                        dragState->originalFadeInBeats + rawDelta);
        repaint();
        return;
    }

    if (dragState->mode == DragMode::fadeOut)
    {
        // Drag left to lengthen the fade-out.
        const auto rawDelta = xToBeatDelta(event.getDistanceFromDragStartX());
        clip.fadeOutBeats = juce::jlimit(0.0,
                                         juce::jmax(0.0, clip.lengthInBeats - clip.fadeInBeats),
                                         dragState->originalFadeOutBeats - rawDelta);
        repaint();
        return;
    }

    if (dragState->mode == DragMode::fadeInCurve || dragState->mode == DragMode::fadeOutCurve)
    {
        // Map the vertical mouse position to the gain at the fade's mid-point,
        // then derive the curvature exponent the same way fadeCurveGain does.
        const auto bounds = getClipBounds(clip, dragState->clip.trackIndex);
        const auto height = juce::jmax(1, bounds.getHeight());
        const auto midGain = juce::jlimit(0.02, 0.98,
            static_cast<double>(bounds.getBottom() - event.getPosition().y) / static_cast<double>(height));
        const auto exponent = std::log(midGain) / std::log(0.5);
        const auto curve = juce::jlimit(-1.0, 1.0, std::log(exponent) / std::log(4.0));
        if (dragState->mode == DragMode::fadeInCurve)
            clip.fadeInCurve = curve;
        else
            clip.fadeOutCurve = curve;
        repaint();
        return;
    }

    if (dragState->mode == DragMode::move)
    {
        clip.startBeat = juce::jlimit(0.0, juce::jmax(0.0, getTimelineEndBeats() - clip.lengthInBeats), snapBeatValue(dragState->originalStartBeat + beatDelta));
        repaint();
        return;
    }

    // ---- Trim / time-stretch (left or right edge) ----
    const auto origStart = dragState->originalStartBeat;
    const auto origLen   = dragState->originalLengthInBeats;
    const auto origEnd   = origStart + origLen;
    const auto trimStart0 = juce::jlimit(0.0, 0.999, dragState->originalSampleStartRatio);
    const auto trimEnd0   = juce::jlimit(trimStart0 + 0.001, 1.0, dragState->originalSampleEndRatio);
    const auto trimSpan0  = juce::jmax(0.001, trimEnd0 - trimStart0);
    // Full source length in beats at 1:1 speed (constant-speed reference).
    const auto fullLen = dragState->originalWarpTargetLengthInBeats > 0.0
        ? dragState->originalWarpTargetLengthInBeats
        : origLen / trimSpan0;
    const auto minLen = minimumClipLengthInBeats;

    switch (dragState->mode)
    {
        case DragMode::resizeRight: // trim right edge — constant speed, reveal/hide source
        {
            const auto maxLenSource   = fullLen * (1.0 - trimStart0);
            const auto maxLenTimeline = getTimelineEndBeats() - origStart;
            const auto newLen = juce::jlimit(minLen, juce::jmax(minLen, juce::jmin(maxLenSource, maxLenTimeline)),
                                             snapBeatValue(origLen + beatDelta));
            clip.lengthInBeats   = newLen;
            clip.sampleStartRatio = trimStart0;
            clip.sampleEndRatio   = juce::jlimit(trimStart0 + 0.001, 1.0, trimStart0 + newLen / fullLen);
            clip.warpTargetLengthInBeats = fullLen;
            break;
        }
        case DragMode::resizeLeft: // trim left edge — constant speed
        {
            const auto maxLenLeft = fullLen * trimEnd0;          // can't reveal before source start
            const auto minStart   = juce::jmax(0.0, origEnd - maxLenLeft);
            const auto newStart   = juce::jlimit(minStart, origEnd - minLen, snapBeatValue(origStart + beatDelta));
            const auto newLen     = origEnd - newStart;
            clip.startBeat        = newStart;
            clip.lengthInBeats    = newLen;
            clip.sampleEndRatio   = trimEnd0;
            clip.sampleStartRatio = juce::jlimit(0.0, trimEnd0 - 0.001, trimEnd0 - newLen / fullLen);
            clip.warpTargetLengthInBeats = fullLen;
            break;
        }
        case DragMode::stretchRight: // time-stretch from right edge — sample fills new length
        {
            const auto maxLenTimeline = getTimelineEndBeats() - origStart;
            const auto newLen = juce::jlimit(minLen, juce::jmax(minLen, maxLenTimeline), snapBeatValue(origLen + beatDelta));
            clip.lengthInBeats = newLen;
            clip.sampleStartRatio = trimStart0;
            clip.sampleEndRatio   = trimEnd0;
            clip.warpTargetLengthInBeats = newLen / trimSpan0;   // scale full-source warp length
            break;
        }
        case DragMode::stretchLeft: // time-stretch from left edge
        {
            const auto newStart = juce::jlimit(0.0, origEnd - minLen, snapBeatValue(origStart + beatDelta));
            const auto newLen   = origEnd - newStart;
            clip.startBeat        = newStart;
            clip.lengthInBeats    = newLen;
            clip.sampleStartRatio = trimStart0;
            clip.sampleEndRatio   = trimEnd0;
            clip.warpTargetLengthInBeats = newLen / trimSpan0;
            break;
        }
        default:
            break;
    }

    // Keep fades within the (possibly shortened) clip.
    clip.fadeInBeats  = juce::jlimit(0.0, clip.lengthInBeats, clip.fadeInBeats);
    clip.fadeOutBeats = juce::jlimit(0.0, juce::jmax(0.0, clip.lengthInBeats - clip.fadeInBeats), clip.fadeOutBeats);

    repaint();
}

void ArrangementTimelineComponent::mouseMove(const juce::MouseEvent& event)
{
    if (getAddTrackButtonBounds().contains(event.getPosition()))
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        return;
    }

    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(timelineRulerHeight);
    const auto tracksArea = bounds;
    const auto headerResizeX = tracksArea.getX() + trackHeaderWidth;
    if (tracksArea.contains(event.getPosition())
        && std::abs(event.getPosition().x - headerResizeX) <= 6)
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    const auto trackCount = static_cast<int>(project.getTracks().size());
    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        auto lane = getTrackLaneBounds(trackIndex);
        auto headerArea = lane.removeFromLeft(trackHeaderWidth);
        if (headerArea.contains(event.getPosition())
            && std::abs(event.getPosition().y - lane.getBottom()) <= 6)
        {
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            return;
        }
    }

    hoverClip = hitTestClipDetailed(event.getPosition(), false);
    const auto overFade = hoverClip.has_value()
        && (hoverClip->overFadeInHandle || hoverClip->overFadeOutHandle
            || hoverClip->overFadeInCurveHandle || hoverClip->overFadeOutCurveHandle);
    const auto overEdge = hoverClip.has_value()
        && (hoverClip->overResizeHandle || hoverClip->overLeftResizeHandle);
    if (currentTool == ToolMode::knife)
        setMouseCursor(hoverClip.has_value() ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
    else
        setMouseCursor(overFade
                           ? juce::MouseCursor::PointingHandCursor
                           : (overEdge ? juce::MouseCursor::LeftRightResizeCursor
                                       : juce::MouseCursor::NormalCursor));
    repaint();
}

void ArrangementTimelineComponent::mouseExit(const juce::MouseEvent&)
{
    hoverClip.reset();
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

void ArrangementTimelineComponent::mouseUp(const juce::MouseEvent&)
{
    inspectorResizeState.active = false;
    inspectorResizeState.trackIndex = -1;
    trackHeaderWidthResizeState.active = false;
    playheadDragState.active = false;
    trackVolumeDragState.active = false;
    loopSelectionState.reset();
    selectionBoxState.active = false;

    if (dragState.has_value() && dragState->historyCaptured && ! undoStack.empty() && ! hasTimelineChangedSince(undoStack.back()))
    {
        undoStack.pop_back();
    }

    dragState.reset();
    const auto overFade = hoverClip.has_value()
        && (hoverClip->overFadeInHandle || hoverClip->overFadeOutHandle
            || hoverClip->overFadeInCurveHandle || hoverClip->overFadeOutCurveHandle);
    const auto overEdge = hoverClip.has_value()
        && (hoverClip->overResizeHandle || hoverClip->overLeftResizeHandle);
    setMouseCursor(overFade
                       ? juce::MouseCursor::PointingHandCursor
                       : (overEdge ? juce::MouseCursor::LeftRightResizeCursor
                                   : juce::MouseCursor::NormalCursor));
}

void ArrangementTimelineComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    auto bounds = getTimelineContentBounds(*this);
    auto rulerArea = bounds.removeFromTop(timelineRulerHeight);
    auto rulerGridArea = rulerArea;
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto loopLane = rulerGridArea.removeFromTop(loopLaneHeight);
    const auto beatsPerBar = static_cast<double>(project.getNumerator());

    if (loopLane.contains(event.getPosition()))
    {
        const auto clickedBeat = juce::jlimit(0.0, project.getLoopLengthInBeats(), xToBeatPosition(event.getPosition().x));
        if (project.hasLoopRange() && clickedBeat >= project.getLoopStartBeat() && clickedBeat <= project.getLoopEndBeat())
        {
            project.clearLoopRange();
            transport.setLoopEnabled(false);
        }
        else
        {
            const auto barStart = juce::jlimit(0.0,
                                               project.getLoopLengthInBeats() - static_cast<double>(project.getNumerator()),
                                               std::floor(clickedBeat / beatsPerBar) * beatsPerBar);
            project.setLoopRange(barStart, juce::jmin(project.getLoopLengthInBeats(), barStart + beatsPerBar));
            transport.setLoopEnabled(true);
        }
        repaint();
        return;
    }

    const auto trackHeaderHit = hitTestTrackHeader(event.getPosition());
    if (trackHeaderHit.has_value() && trackHeaderHit->control == TrackHeaderControl::none)
    {
        selectedTrackIndex = trackHeaderHit->trackIndex;
        setSingleSelection(std::nullopt);
        grabKeyboardFocus();
        repaint();

        if (onTrackHeaderDoubleClick)
            onTrackHeaderDoubleClick(trackHeaderHit->trackIndex);

        return;
    }

    const auto hit = hitTestClip(event.getPosition(), false);
    if (hit.has_value())
    {
        setSingleSelection(hit);
        grabKeyboardFocus();
        repaint();

        const auto& tracks = project.getTracks();
        if (hit->trackIndex >= 0 && hit->trackIndex < static_cast<int>(tracks.size()))
        {
            const auto& track = tracks[static_cast<std::size_t>(hit->trackIndex)];
            if (hit->clipIndex >= 0 && hit->clipIndex < static_cast<int>(track.clips.size()))
            {
                const auto& clip = track.clips[static_cast<std::size_t>(hit->clipIndex)];
                if (clip.type == ClipType::midi)
                {
                    if (onMidiClipDoubleClick)
                        onMidiClipDoubleClick(hit->trackIndex, hit->clipIndex);
                }
                else if (onAudioClipDoubleClick)
                {
                    onAudioClipDoubleClick(hit->trackIndex, hit->clipIndex);
                }
            }
        }
        return;
    }

    auto tracksArea = bounds;
    auto gridArea = tracksArea;
    gridArea.removeFromLeft(trackHeaderWidth);
    if (gridArea.contains(event.getPosition()))
    {
        const auto trackIndex = trackIndexFromY(event.getPosition().y);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(project.getTracks().size())
            && project.getTracks()[static_cast<std::size_t>(trackIndex)].isMidiTrack)
        {
            createMidiClipAt(trackIndex, snapBeatValue(xToBeatPosition(event.getPosition().x)));
            return;
        }
    }
}

bool ArrangementTimelineComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        if (onTogglePlayback)
        {
            onTogglePlayback();
            return true;
        }
        return false;
    }

    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0) && undo())
        return true;

    if ((key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
         || key == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0)) && redo())
        return true;

    if (selectedClip.has_value() && (key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey))
    {
        pushUndoSnapshot();
        auto& tracks = project.getTracks();
        auto clipsToDelete = selectedClips;
        std::sort(clipsToDelete.begin(), clipsToDelete.end(), [](const auto& a, const auto& b)
        {
            if (a.trackIndex != b.trackIndex)
                return a.trackIndex > b.trackIndex;

            return a.clipIndex > b.clipIndex;
        });

        for (const auto& clipToDelete : clipsToDelete)
        {
            if (clipToDelete.trackIndex < 0 || clipToDelete.trackIndex >= static_cast<int>(tracks.size()))
                continue;

            auto& clips = tracks[static_cast<std::size_t>(clipToDelete.trackIndex)].clips;
            if (clipToDelete.clipIndex >= 0 && clipToDelete.clipIndex < static_cast<int>(clips.size()))
                clips.erase(clips.begin() + clipToDelete.clipIndex);
        }

        selectedClip.reset();
        selectedClips.clear();
        lastClickedClip.reset();
        notifyClipSelectionChanged();
        hoverClip.reset();
        dragState.reset();
        repaint();
        return true;
    }

    if (! selectedClip.has_value() && selectedTrackIndex.has_value()
        && (key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey))
    {
        deleteSelectedTrack();
        return true;
    }

    if (selectedClip.has_value() && key == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0))
    {
        pushUndoSnapshot();
        auto& tracks = project.getTracks();
        auto& clips = tracks[static_cast<std::size_t>(selectedClip->trackIndex)].clips;
        auto duplicateClip = clips[static_cast<std::size_t>(selectedClip->clipIndex)];
        duplicateClip.startBeat = juce::jlimit(
            0.0,
            juce::jmax(0.0, getTimelineEndBeats() - duplicateClip.lengthInBeats),
            duplicateClip.startBeat + duplicateClip.lengthInBeats);
        duplicateClip.name += " Copy";
        clips.push_back(duplicateClip);
        selectedClip = SelectedClip { selectedClip->trackIndex, static_cast<int>(clips.size()) - 1 };
        notifyClipSelectionChanged();
        repaint();
        return true;
    }

    // Split (knife) at the playhead — 'B' (blade) or 'S'.
    if (key == juce::KeyPress('b', 0, 0) || key == juce::KeyPress('s', 0, 0))
    {
        splitSelectionAtPlayhead();
        return true;
    }

    if (selectedClip.has_value() && (key == juce::KeyPress('l', 0, 0)
                                     || key == juce::KeyPress('l', juce::ModifierKeys::commandModifier, 0)))
    {
        const auto& clip = project.getTracks()[static_cast<std::size_t>(selectedClip->trackIndex)]
                               .clips[static_cast<std::size_t>(selectedClip->clipIndex)];
        project.setLoopRange(clip.startBeat, clip.startBeat + clip.lengthInBeats);
        transport.setLoopEnabled(true);
        repaint();
        return true;
    }

    if (key != juce::KeyPress::returnKey || ! selectedClip.has_value())
        return false;

    const auto& clip = project.getTracks()[static_cast<std::size_t>(selectedClip->trackIndex)]
                           .clips[static_cast<std::size_t>(selectedClip->clipIndex)];

    if (clip.type == ClipType::midi)
    {
        if (! onMidiClipDoubleClick)
            return false;
        onMidiClipDoubleClick(selectedClip->trackIndex, selectedClip->clipIndex);
    }
    else
    {
        if (! onAudioClipDoubleClick)
            return false;
        onAudioClipDoubleClick(selectedClip->trackIndex, selectedClip->clipIndex);
    }
    return true;
}

std::optional<ArrangementTimelineComponent::SelectedClip> ArrangementTimelineComponent::hitTestClip(juce::Point<int> position, bool midiOnly) const
{
    const auto hit = hitTestClipDetailed(position, midiOnly);
    return hit.has_value() ? std::optional<SelectedClip>(hit->clip) : std::nullopt;
}

std::optional<ArrangementTimelineComponent::ClipHit> ArrangementTimelineComponent::hitTestClipDetailed(juce::Point<int> position, bool midiOnly) const
{
    const auto& tracks = project.getTracks();

    for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex)
    {
        const auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;

        // Iterate top-most first (clips are painted in ascending index order, so the
        // highest index sits on top). This makes overlapping clips selectable/deletable
        // in the order the user actually sees them.
        for (int clipIndex = static_cast<int>(clips.size()) - 1; clipIndex >= 0; --clipIndex)
        {
            const auto& clip = clips[static_cast<std::size_t>(clipIndex)];
            const auto clipBounds = getClipBounds(clip, trackIndex);

            if (! clipBounds.contains(position))
                continue;

            if (midiOnly && clip.type != ClipType::midi)
                return std::nullopt;

            // Fade handles live in a thin band at the very top of the clip, at the
            // X where each fade currently ends (Studio One style). They take priority
            // over the right-edge resize handle while inside that top band.
            const auto inFadeBand = position.y <= clipBounds.getY() + fadeHandleBandHeight;
            const auto fadeInHandleX = clipBounds.getX()
                + juce::roundToInt(clip.fadeInBeats * pixelsPerBeat);
            const auto fadeOutHandleX = clipBounds.getRight()
                - juce::roundToInt(clip.fadeOutBeats * pixelsPerBeat);
            const auto overFadeIn = inFadeBand
                && std::abs(position.x - fadeInHandleX) <= fadeHandleHitRadius;
            const auto overFadeOut = inFadeBand
                && std::abs(position.x - fadeOutHandleX) <= fadeHandleHitRadius;

            const auto overResize = position.x >= clipBounds.getRight() - resizeHandleWidth;
            const auto overResizeLeft = position.x <= clipBounds.getX() + resizeHandleWidth;

            // Mid-curve handles sit on the fade curve at its half-way point and bend
            // the curvature. Only present when the fade is long enough to grab.
            const auto fadeInPx = juce::jlimit(0.0, static_cast<double>(clipBounds.getWidth()),
                                               clip.fadeInBeats * pixelsPerBeat);
            const auto fadeOutPx = juce::jlimit(0.0, static_cast<double>(clipBounds.getWidth()),
                                                clip.fadeOutBeats * pixelsPerBeat);
            const auto curveHandlePoint = [&](double endXpx, double curve) -> juce::Point<int>
            {
                const auto midGain = fadeCurveGain(0.5, curve);
                const auto midY = clipBounds.getBottom() - midGain * clipBounds.getHeight();
                return { juce::roundToInt(endXpx), juce::roundToInt(midY) };
            };

            auto overFadeInCurve = false;
            auto overFadeOutCurve = false;
            if (fadeInPx > 18.0)
            {
                const auto p = curveHandlePoint(clipBounds.getX() + fadeInPx * 0.5, clip.fadeInCurve);
                overFadeInCurve = position.getDistanceFrom(p) <= fadeHandleHitRadius;
            }
            if (fadeOutPx > 18.0)
            {
                const auto p = curveHandlePoint(clipBounds.getRight() - fadeOutPx * 0.5, clip.fadeOutCurve);
                overFadeOutCurve = position.getDistanceFrom(p) <= fadeHandleHitRadius;
            }

            const auto fadeBusy = overFadeIn || overFadeOut || overFadeInCurve || overFadeOutCurve;
            return ClipHit {
                SelectedClip { trackIndex, clipIndex },
                clipBounds,
                overResize && ! fadeBusy,
                overResizeLeft && ! overResize && ! fadeBusy,
                overFadeIn,
                overFadeOut && ! overFadeIn,
                overFadeInCurve && ! overFadeIn && ! overFadeOut,
                overFadeOutCurve && ! overFadeIn && ! overFadeOut && ! overFadeInCurve
            };
        }
    }

    return std::nullopt;
}

std::optional<ArrangementTimelineComponent::TrackHeaderHit> ArrangementTimelineComponent::hitTestTrackHeader(juce::Point<int> position) const
{
    const auto& tracks = project.getTracks();
    for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex)
    {
        const auto layout = computeHeaderLayout(trackIndex);
        if (! layout.card.contains(position))
            continue;

        if (layout.muteButton.contains(position))
            return TrackHeaderHit { trackIndex, TrackHeaderControl::mute, layout.muteButton };
        if (layout.soloButton.contains(position))
            return TrackHeaderHit { trackIndex, TrackHeaderControl::solo, layout.soloButton };
        if (layout.recordButton.contains(position))
            return TrackHeaderHit { trackIndex, TrackHeaderControl::record, layout.recordButton };
        if (! layout.instrumentButton.isEmpty() && layout.instrumentButton.contains(position))
            return TrackHeaderHit { trackIndex, TrackHeaderControl::instrument, layout.instrumentButton };

        // Generous vertical hit zone around the thin slider so it's easy to grab.
        if (layout.slider.expanded(0, 6).contains(position))
            return TrackHeaderHit { trackIndex, TrackHeaderControl::volume, layout.slider };
        if (layout.volumeValue.expanded(6, 5).contains(position))
            return TrackHeaderHit { trackIndex, TrackHeaderControl::volumeValue, layout.volumeValue };

        return TrackHeaderHit { trackIndex, TrackHeaderControl::none, layout.card };
    }

    return std::nullopt;
}

bool ArrangementTimelineComponent::isClipSelected(const SelectedClip& clip) const noexcept
{
    return std::any_of(selectedClips.begin(), selectedClips.end(), [&clip](const auto& selected)
    {
        return selected.trackIndex == clip.trackIndex && selected.clipIndex == clip.clipIndex;
    });
}

void ArrangementTimelineComponent::setSingleSelection(std::optional<SelectedClip> clip)
{
    selectedClip = clip;
    if (clip.has_value())
        selectedTrackIndex.reset();
    selectedClips.clear();

    if (clip.has_value())
    {
        selectedClips.push_back(*clip);
        lastClickedClip = clip;
    }

    notifyClipSelectionChanged();
}

void ArrangementTimelineComponent::selectRangeTo(const SelectedClip& targetClip)
{
    if (! lastClickedClip.has_value())
    {
        setSingleSelection(targetClip);
        return;
    }

    std::vector<SelectedClip> allClips;
    const auto& tracks = project.getTracks();
    for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex)
    {
        const auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
        for (int clipIndex = 0; clipIndex < static_cast<int>(clips.size()); ++clipIndex)
            allClips.push_back(SelectedClip { trackIndex, clipIndex });
    }

    const auto findClip = [&allClips](const SelectedClip& needle)
    {
        return std::find_if(allClips.begin(), allClips.end(), [&needle](const auto& candidate)
        {
            return candidate.trackIndex == needle.trackIndex && candidate.clipIndex == needle.clipIndex;
        });
    };

    const auto anchorIt = findClip(*lastClickedClip);
    const auto targetIt = findClip(targetClip);
    if (anchorIt == allClips.end() || targetIt == allClips.end())
    {
        setSingleSelection(targetClip);
        return;
    }

    auto firstIndex = static_cast<int>(std::distance(allClips.begin(), anchorIt));
    auto lastIndex = static_cast<int>(std::distance(allClips.begin(), targetIt));
    if (firstIndex > lastIndex)
        std::swap(firstIndex, lastIndex);

    selectedClips.clear();
    for (int index = firstIndex; index <= lastIndex; ++index)
        selectedClips.push_back(allClips[static_cast<std::size_t>(index)]);

    selectedClip = targetClip;
    notifyClipSelectionChanged();
}

juce::Rectangle<int> ArrangementTimelineComponent::getSelectionBoxBounds() const noexcept
{
    return juce::Rectangle<int>::leftTopRightBottom(
        juce::jmin(selectionBoxState.anchor.x, selectionBoxState.current.x),
        juce::jmin(selectionBoxState.anchor.y, selectionBoxState.current.y),
        juce::jmax(selectionBoxState.anchor.x, selectionBoxState.current.x),
        juce::jmax(selectionBoxState.anchor.y, selectionBoxState.current.y));
}

void ArrangementTimelineComponent::updateSelectionBox(const juce::Point<int>& position)
{
    selectionBoxState.current = position;
    const auto selectionBounds = getSelectionBoxBounds();
    selectedClips.clear();

    const auto& tracks = project.getTracks();
    for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex)
    {
        const auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
        for (int clipIndex = 0; clipIndex < static_cast<int>(clips.size()); ++clipIndex)
        {
            const auto& clip = clips[static_cast<std::size_t>(clipIndex)];
            if (selectionBounds.intersects(getClipBounds(clip, trackIndex)))
                selectedClips.push_back(SelectedClip { trackIndex, clipIndex });
        }
    }

    selectedClip = selectedClips.empty() ? std::optional<SelectedClip>() : std::optional<SelectedClip>(selectedClips.back());
    if (selectedClip.has_value())
        lastClickedClip = selectedClip;

    notifyClipSelectionChanged();
}

juce::String ArrangementTimelineComponent::makeUniqueTrackName(const juce::String& baseName) const
{
    auto candidate = baseName;
    auto suffix = 1;
    const auto& tracks = project.getTracks();

    while (std::any_of(tracks.begin(), tracks.end(), [&candidate](const auto& track) { return track.name == candidate; }))
        candidate = baseName + " " + juce::String(++suffix);

    return candidate;
}

juce::Rectangle<int> ArrangementTimelineComponent::getAddTrackButtonBounds() const noexcept
{
    auto bounds = getTimelineContentBounds(*this);
    auto rulerArea = bounds.removeFromTop(timelineRulerHeight);
    auto headerArea = rulerArea.removeFromLeft(trackHeaderWidth);
    return headerArea.removeFromRight(44).withSizeKeepingCentre(30, 30);
}

void ArrangementTimelineComponent::showAddTrackMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Audio Track");
    menu.addItem(2, "MIDI Track");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(getAddTrackButtonBounds())),
                       [this](int result)
                       {
                           if (result == 1)
                               addAudioTrack();
                           else if (result == 2)
                               addMidiTrack();
                       });
}

void ArrangementTimelineComponent::createMidiClipAt(int trackIndex, double startBeat)
{
    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()) || ! tracks[static_cast<std::size_t>(trackIndex)].isMidiTrack)
        return;

    pushUndoSnapshot();
    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    const auto clipLength = static_cast<double>(juce::jmax(1, project.getNumerator()));
    const auto clippedStart = juce::jlimit(0.0, juce::jmax(0.0, getTimelineEndBeats() - clipLength), startBeat);
    track.clips.push_back(TimelineClip {
        "MIDI Clip",
        ClipType::midi,
        clippedStart,
        clipLength,
        track.colour,
        {},
        {},
        {},
        0.0,
        false,
        false,
        0.0,
        0.0,
        0,
        false
    });

    setSingleSelection(SelectedClip { trackIndex, static_cast<int>(track.clips.size()) - 1 });
    repaint();
}

void ArrangementTimelineComponent::updateTrackVolumeFromPoint(int trackIndex, juce::Rectangle<int> sliderBounds, int x)
{
    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()) || sliderBounds.getWidth() <= 0)
        return;

    const auto ratio = juce::jlimit(0.0f,
                                    1.0f,
                                    static_cast<float>(x - sliderBounds.getX())
                                        / static_cast<float>(sliderBounds.getWidth()));
    tracks[static_cast<std::size_t>(trackIndex)].volumeDb = juce::jmap(ratio, 0.0f, 1.0f, -24.0f, 12.0f);
}

ArrangementTimelineComponent::HeaderLayout ArrangementTimelineComponent::computeHeaderLayout(int trackIndex) const noexcept
{
    HeaderLayout layout;

    auto lane = getTrackLaneBounds(trackIndex);
    auto headerArea = lane.removeFromLeft(trackHeaderWidth);
    // Rounded header card, sized to the full lane height (clips inset ~1-2px) so it matches
    // the track rather than looking shorter.
    layout.card = headerArea.reduced(8, 2);

    // Children of a folder are indented to the right so the grouping reads visually.
    const auto& tracks = project.getTracks();
    if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())
        && tracks[static_cast<std::size_t>(trackIndex)].parentGroup >= 0)
        layout.card.removeFromLeft(folderChildIndentPx);

    // Content is anchored to the top of the card with consistent gaps, so it looks
    // identical regardless of how tall the track lane is. A thin vertical level meter
    // runs down the right edge (space-efficient on the default 78px lane).
    auto inner = layout.card.reduced(13, 6);
    const bool isFolderTrack = trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())
                            && tracks[static_cast<std::size_t>(trackIndex)].isFolder;

    layout.meter = inner.removeFromRight(9);
    inner.removeFromRight(10);

    layout.title = inner.removeFromTop(18);
    if (isFolderTrack)
        layout.collapseTriangle = layout.title.removeFromLeft(folderTriangleGutterPx);
    inner.removeFromTop(4);

    constexpr int buttonSize = 18;
    constexpr int buttonGap  = 6;
    auto controlsRow = inner.removeFromTop(buttonSize);
    layout.muteButton = controlsRow.removeFromLeft(buttonSize);
    controlsRow.removeFromLeft(buttonGap);
    layout.soloButton = controlsRow.removeFromLeft(buttonSize);
    controlsRow.removeFromLeft(buttonGap);
    layout.recordButton = controlsRow.removeFromLeft(buttonSize);

    // Instrument button on the right side of the controls row — MIDI tracks only,
    // since audio / folder tracks have no hosted VST instrument.
    const bool isMidiTrack = trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())
                          && tracks[static_cast<std::size_t>(trackIndex)].isMidiTrack;
    if (isMidiTrack && controlsRow.getWidth() >= buttonSize)
        layout.instrumentButton = controlsRow.removeFromRight(buttonSize);   // square, matches M/S/R

    inner.removeFromTop(5);
    auto volumeRow = inner.removeFromTop(14);
    layout.volumeValue = volumeRow.removeFromRight(46);
    volumeRow.removeFromRight(8);
    constexpr int sliderHeight = 8;
    volumeRow.removeFromTop(juce::jmax(0, (volumeRow.getHeight() - sliderHeight) / 2));
    layout.slider = volumeRow.removeFromTop(sliderHeight);

    return layout;
}

juce::Rectangle<int> ArrangementTimelineComponent::getTrackVolumeValueBounds(int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= static_cast<int>(project.getTracks().size()))
        return {};

    return computeHeaderLayout(trackIndex).volumeValue;
}

void ArrangementTimelineComponent::showTrackVolumeEditor(int trackIndex)
{
    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    volumeEditorTrackIndex = trackIndex;
    trackVolumeInlineEditor.setText(juce::String(tracks[static_cast<std::size_t>(trackIndex)].volumeDb, 1), juce::dontSendNotification);
    trackVolumeInlineEditor.setBounds(getTrackVolumeValueBounds(trackIndex));
    trackVolumeInlineEditor.setVisible(true);
    trackVolumeInlineEditor.toFront(false);
    trackVolumeInlineEditor.grabKeyboardFocus();
    trackVolumeInlineEditor.selectAll();
}

void ArrangementTimelineComponent::commitTrackVolumeEditor(bool applyChanges)
{
    if (! volumeEditorTrackIndex.has_value())
    {
        trackVolumeInlineEditor.setVisible(false);
        return;
    }

    const auto trackIndex = *volumeEditorTrackIndex;
    volumeEditorTrackIndex.reset();

    if (applyChanges)
    {
        auto& tracks = project.getTracks();
        if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
        {
            const auto text = trackVolumeInlineEditor.getText().toLowerCase().replace("db", "").trim();
            const auto value = text.getDoubleValue();
            tracks[static_cast<std::size_t>(trackIndex)].volumeDb = juce::jlimit(-24.0, 12.0, value);
        }
    }

    trackVolumeInlineEditor.setVisible(false);
    repaint();
}

void ArrangementTimelineComponent::deleteSelectedTrack()
{
    if (! selectedTrackIndex.has_value())
        return;

    auto& tracks = project.getTracks();
    const auto trackIndex = *selectedTrackIndex;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    pushUndoSnapshot();
    tracks.erase(tracks.begin() + trackIndex);
    std::map<int, int> reindexedHeights;
    for (const auto& [index, height] : customTrackHeights)
    {
        if (index < trackIndex)
            reindexedHeights[index] = height;
        else if (index > trackIndex)
            reindexedHeights[index - 1] = height;
    }
    customTrackHeights = std::move(reindexedHeights);
    selectedTrackIndex.reset();
    selectedClip.reset();
    selectedClips.clear();
    lastClickedClip.reset();
    notifyClipSelectionChanged();
    hoverClip.reset();
    dragState.reset();
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(timelineRulerHeight);

    const auto visibleTracksArea = getVisibleTrackAreaBounds(*this);
    auto visibleGridArea = visibleTracksArea;
    visibleGridArea.removeFromLeft(trackHeaderWidth);

    if (! visibleTracksArea.contains(event.getPosition()))
        return;

    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto totalDelta = std::abs(wheel.deltaX) + std::abs(wheel.deltaY);

    if (nowMs < ignoreWheelUntilMs && totalDelta < 0.045f)
        return;

    const auto horizontalDelta = static_cast<double>(wheel.deltaX);
    const auto verticalDelta = static_cast<double>(wheel.deltaY);
    const auto absHorizontal = std::abs(horizontalDelta);
    const auto absVertical = std::abs(verticalDelta);

    if (absHorizontal <= 0.0001 && absVertical <= 0.0001)
        return;

    const auto isExplicitHorizontal = absHorizontal > absVertical * 1.35;
    const auto isVerticalIntent = absVertical >= absHorizontal;
    const auto wantsVerticalZoom = (event.mods.isAltDown() || event.mods.isCommandDown()) && isVerticalIntent;

    if (wantsVerticalZoom)
    {
        adjustZoom(0.0, verticalDelta * 2.0, event.getPosition());
        return;
    }

    if (isExplicitHorizontal && visibleGridArea.contains(event.getPosition()))
    {
        const auto previousScrollX = scrollX;
        scrollX -= horizontalDelta * 600.0;
        clampScrollOffsets();

        if (std::abs(scrollX - previousScrollX) > 0.01)
            repaint();
    }
    else if (isVerticalIntent)
    {
        const auto previousScrollY = scrollY;
        scrollY -= verticalDelta * 600.0;
        clampScrollOffsets();

        if (std::abs(scrollY - previousScrollY) > 0.01)
            repaint();
    }
}

void ArrangementTimelineComponent::mouseMagnify(const juce::MouseEvent& event, float scaleFactor)
{
    auto bounds = getTimelineContentBounds(*this);
    auto tracksArea = bounds.withTrimmedTop(42);
    auto headerArea = tracksArea.removeFromLeft(trackHeaderWidth);
    auto gridArea = tracksArea;

    if (! headerArea.contains(event.getPosition()) && ! gridArea.contains(event.getPosition()))
        return;

    const auto clampedScale = juce::jlimit(0.35, 2.85, static_cast<double>(scaleFactor));
    const auto rawDelta = std::log(clampedScale) / std::log(1.2);
    pendingMagnifyDelta += rawDelta;
    ignoreWheelUntilMs = juce::Time::getMillisecondCounterHiRes() + 120.0;

    if (std::abs(pendingMagnifyDelta) < 0.005)
        return;

    const auto stableDelta = pendingMagnifyDelta;
    pendingMagnifyDelta = 0.0;
    const auto verticalDelta = headerArea.contains(event.getPosition()) ? stableDelta : 0.0;
    const auto horizontalDelta = gridArea.contains(event.getPosition()) ? stableDelta : 0.0;
    adjustZoom(horizontalDelta, verticalDelta, event.getPosition());
}

bool ArrangementTimelineComponent::isInterestedInDragSource(const SourceDetails& dragSourceDetails)
{
    const auto* payload = dragSourceDetails.description.getDynamicObject();
    if (payload == nullptr)
        return false;

    const auto type = payload->getProperty("type").toString();
    return type == "browser-item" || type == "clip-editor-audio";
}

void ArrangementTimelineComponent::itemDragEnter(const SourceDetails& dragSourceDetails)
{
    updateBrowserDropPreview(dragSourceDetails.localPosition, dragSourceDetails.description);
}

void ArrangementTimelineComponent::itemDragMove(const SourceDetails& dragSourceDetails)
{
    updateBrowserDropPreview(dragSourceDetails.localPosition, dragSourceDetails.description);
}

void ArrangementTimelineComponent::itemDragExit(const SourceDetails&)
{
    clearBrowserDropPreview();
}

void ArrangementTimelineComponent::itemDropped(const SourceDetails& dragSourceDetails)
{
    const auto* payload = dragSourceDetails.description.getDynamicObject();
    if (payload == nullptr)
    {
        clearBrowserDropPreview();
        return;
    }

    auto targetTrackIndex = trackIndexFromY(dragSourceDetails.localPosition.y);
    bool createNewTrack = false;
    const auto trackCountBeforeDrop = static_cast<int>(project.getTracks().size());

    if (trackCountBeforeDrop > 0)
    {
        const auto visibleTracksArea = getVisibleTrackAreaBounds(*this);
        const auto needsBottomDropZone = getTotalTrackHeight() + defaultLaneHeight > visibleTracksArea.getHeight();
        if (needsBottomDropZone)
        {
            const auto bottomDropZoneTop = visibleTracksArea.getBottom() - juce::jmin(newTrackDropZoneHeight, visibleTracksArea.getHeight());
            const auto lastLane = getTrackLaneBounds(trackCountBeforeDrop - 1);
            if (dragSourceDetails.localPosition.y >= juce::jmax(lastLane.getCentreY(), bottomDropZoneTop)
                && visibleTracksArea.contains(visibleTracksArea.getX() + 1, dragSourceDetails.localPosition.y))
            {
                targetTrackIndex = trackCountBeforeDrop;
                createNewTrack = true;
            }
        }
    }

    if (targetTrackIndex < 0)
    {
        auto tracksBounds = getTimelineContentBounds(*this);
        tracksBounds.removeFromTop(timelineRulerHeight);
        if (tracksBounds.contains(tracksBounds.getX() + 1, dragSourceDetails.localPosition.y))
        {
            targetTrackIndex = static_cast<int>(project.getTracks().size());
            createNewTrack = true;
        }
    }

    if (targetTrackIndex < 0)
    {
        clearBrowserDropPreview();
        return;
    }

    auto& tracks = project.getTracks();
    if (! createNewTrack)
    {
        auto& track = tracks[static_cast<std::size_t>(targetTrackIndex)];
        if (track.isMidiTrack)
        {
            clearBrowserDropPreview();
            return;
        }
    }

    const auto sourceFile = juce::File(payload->getProperty("path").toString());
    const auto dragType = payload->getProperty("type").toString();
    const auto isClipEditorDrop = dragType == "clip-editor-audio";
    const auto getPayloadDouble = [&](const char* propertyName, double fallback)
    {
        return payload->hasProperty(propertyName) ? static_cast<double>(payload->getProperty(propertyName)) : fallback;
    };
    const auto getPayloadInt = [&](const char* propertyName, int fallback)
    {
        return payload->hasProperty(propertyName) ? static_cast<int>(payload->getProperty(propertyName)) : fallback;
    };
    const auto getPayloadBool = [&](const char* propertyName, bool fallback)
    {
        return payload->hasProperty(propertyName) ? static_cast<bool>(payload->getProperty(propertyName)) : fallback;
    };

    const auto sampleStartRatio = juce::jlimit(0.0, 0.999, getPayloadDouble("sampleStartRatio", 0.0));
    const auto sampleEndRatio = juce::jlimit(sampleStartRatio + 0.001, 1.0, getPayloadDouble("sampleEndRatio", 1.0));
    const auto sampleTrimSpan = juce::jmax(0.001, sampleEndRatio - sampleStartRatio);
    const auto fallbackLengthBeats = fallbackClipLengthInBeats(*payload);
    const auto analysis = analyzeImportedAudioClip(sourceFile, project.getTempoBpm(), project.getNumerator(), fallbackLengthBeats);
    const auto sourceLengthBeats = isClipEditorDrop
        ? juce::jmax(minimumClipLengthInBeats, getPayloadDouble("sourceLengthBeats", analysis.clipLengthInBeats))
        : analysis.clipLengthInBeats;
    const auto lengthBeats = isClipEditorDrop
        ? juce::jmax(minimumClipLengthInBeats, sourceLengthBeats * sampleTrimSpan)
        : juce::jmax(minimumClipLengthInBeats, analysis.clipLengthInBeats * sampleTrimSpan);
    const auto startBeat = juce::jlimit(
        0.0,
        juce::jmax(0.0, getTimelineEndBeats() - lengthBeats),
        snapBeatValue(xToBeatPosition(dragSourceDetails.localPosition.x)));

    pushUndoSnapshot();
    const auto clipName = isClipEditorDrop && payload->hasProperty("name")
        ? payload->getProperty("name").toString()
        : clipNameForImportedFile(sourceFile, *payload);

    if (createNewTrack)
    {
        // Name the track after the dropped sample (not the pack/folder). Strip a leading
        // "Pack - " prefix so the distinguishing part (e.g. "Armageddon - 140 BPM G# Min")
        // shows first in the header instead of an identical truncated pack name.
        juce::String trackName = clipName;
        if (const auto dash = trackName.indexOf(" - "); dash > 0)
            trackName = trackName.substring(dash + 3);
        if (trackName.isEmpty())
            trackName = clipName.isNotEmpty() ? clipName : juce::String("Audio Track");
        const auto trackColour = theme::tracks::colourForIndex(trackCountBeforeDrop);
        {
            TrackState t;
            t.name = trackName;
            t.isMidiTrack = false;
            t.colour = trackColour;
            tracks.push_back(std::move(t));
        }
        targetTrackIndex = static_cast<int>(tracks.size()) - 1;
    }

    auto& targetTrack = tracks[static_cast<std::size_t>(targetTrackIndex)];
    const auto clipColour = targetTrack.colour;
    // Auto-warp / auto-pitch only SHORT clips with a known tempo (loops). Full tracks /
    // long clips are NOT auto-warped or auto-pitched — otherwise pressing Play would
    // RubberBand-stretch the whole multi-minute file on the message thread and freeze the
    // UI. The key is still detected and shown; the user can enable warp manually.
    const bool autoWarp = analysis.durationSeconds > 0.0 && analysis.durationSeconds <= 90.0
                          && (analysis.detectedBars > 0 || analysis.sourceBpm > 0.0);
    targetTrack.clips.push_back(TimelineClip {
        clipName,
        ClipType::audio,
        startBeat,
        lengthBeats,
        clipColour,
        {},
        {},
        sourceFile.getFullPathName(),
        0.0,
        false,
        false,
        analysis.durationSeconds,
        analysis.sourceBpm,
        analysis.detectedBars,
        autoWarp,
        analysis.bpmGuessed,
        sourceLengthBeats,
        analysis.sourceKeyRoot,
        analysis.sourceKeyIsMinor,
        autoWarp   // keyShiftEnabled — auto pitch to project key (loops only)
    });
    auto& droppedClip = targetTrack.clips.back();
    droppedClip.signalAnalysisPending = analysis.needsSignalAnalysis;
    droppedClip.gainNormalizationPending = false;
    if (isClipEditorDrop)
    {
        droppedClip.sampleStartRatio = sampleStartRatio;
        droppedClip.sampleEndRatio = sampleEndRatio;
        droppedClip.gainDb = getPayloadDouble("gainDb", droppedClip.gainDb);
        droppedClip.transposeSemitones = getPayloadInt("transposeSemitones", droppedClip.transposeSemitones);
        droppedClip.warpEnabled = getPayloadBool("warpEnabled", droppedClip.warpEnabled);
        droppedClip.keyShiftEnabled = getPayloadBool("keyShiftEnabled", droppedClip.keyShiftEnabled);
    }

    setSingleSelection(SelectedClip { targetTrackIndex, static_cast<int>(targetTrack.clips.size()) - 1 });

    clearBrowserDropPreview();
    // Preserve the user's current timeline zoom on import/drop.
    clampScrollOffsets();
    repaint();
}

// ---- External files dragged in from Finder / the OS -------------------------
namespace
{
const char* const kAudioFileExtensions = "wav;aif;aiff;mp3;flac;ogg;m4a;wave";
}

bool ArrangementTimelineComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (juce::File(f).hasFileExtension(kAudioFileExtensions))
            return true;
    return false;
}

void ArrangementTimelineComponent::fileDragEnter(const juce::StringArray&, int, int) { repaint(); }
void ArrangementTimelineComponent::fileDragMove(const juce::StringArray&, int, int) {}
void ArrangementTimelineComponent::fileDragExit(const juce::StringArray&) { repaint(); }

void ArrangementTimelineComponent::filesDropped(const juce::StringArray& files, int x, int y)
{
    juce::Point<int> pos(x, y);
    const bool multiple = files.size() > 1;
    bool any = false;
    for (const auto& f : files)
    {
        juce::File file(f);
        // For a multi-file drop each sample becomes its own new track at the bottom.
        if (importAudioFileAt(file, multiple ? juce::Point<int>(x, std::numeric_limits<int>::max()) : pos))
            any = true;
    }
    if (any)
        notifyClipSelectionChanged();
    repaint();
}

bool ArrangementTimelineComponent::importAudioFileAt(const juce::File& file, juce::Point<int> position)
{
    if (! file.existsAsFile() || ! file.hasFileExtension(kAudioFileExtensions))
        return false;

    auto& tracks = project.getTracks();
    int targetTrackIndex = trackIndexFromY(position.y);
    bool createNewTrack = false;
    if (targetTrackIndex < 0 || targetTrackIndex >= static_cast<int>(tracks.size())
        || tracks[static_cast<std::size_t>(targetTrackIndex)].isMidiTrack)
    {
        targetTrackIndex = static_cast<int>(tracks.size());
        createNewTrack = true;
    }

    const auto analysis = analyzeImportedAudioClip(file, project.getTempoBpm(), project.getNumerator(), 4.0);
    const auto sourceLengthBeats = analysis.clipLengthInBeats;
    const auto lengthBeats = juce::jmax(minimumClipLengthInBeats, sourceLengthBeats);
    const auto startBeat = juce::jlimit(
        0.0,
        juce::jmax(0.0, getTimelineEndBeats() - lengthBeats),
        snapBeatValue(xToBeatPosition(position.x)));

    pushUndoSnapshot();

    const auto clipName = file.getFileNameWithoutExtension();
    if (createNewTrack)
    {
        juce::String trackName = clipName;
        if (const auto dash = trackName.indexOf(" - "); dash > 0)
            trackName = trackName.substring(dash + 3);
        if (trackName.isEmpty())
            trackName = "Audio Track";
        TrackState t;
        t.name = trackName;
        t.isMidiTrack = false;
        t.colour = theme::tracks::colourForIndex(static_cast<int>(tracks.size()));
        tracks.push_back(std::move(t));
        targetTrackIndex = static_cast<int>(tracks.size()) - 1;
    }

    auto& targetTrack = tracks[static_cast<std::size_t>(targetTrackIndex)];
    const bool autoWarp = analysis.durationSeconds > 0.0 && analysis.durationSeconds <= 90.0
                          && (analysis.detectedBars > 0 || analysis.sourceBpm > 0.0);
    targetTrack.clips.push_back(TimelineClip {
        clipName,
        ClipType::audio,
        startBeat,
        lengthBeats,
        targetTrack.colour,
        {},
        {},
        file.getFullPathName(),
        0.0,
        false,
        false,
        analysis.durationSeconds,
        analysis.sourceBpm,
        analysis.detectedBars,
        autoWarp,
        analysis.bpmGuessed,
        sourceLengthBeats,
        analysis.sourceKeyRoot,
        analysis.sourceKeyIsMinor,
        autoWarp
    });
    auto& droppedClip = targetTrack.clips.back();
    droppedClip.signalAnalysisPending = analysis.needsSignalAnalysis;
    droppedClip.gainNormalizationPending = false;

    setSingleSelection(SelectedClip { targetTrackIndex, static_cast<int>(targetTrack.clips.size()) - 1 });
    clampScrollOffsets();
    repaint();
    return true;
}

void ArrangementTimelineComponent::ensureBeatVisible(double endBeat)
{
    juce::ignoreUnused(endBeat);

    // Keep imports and drops from changing the user's timeline zoom.
    clampScrollOffsets();
}

void ArrangementTimelineComponent::adjustZoom(double horizontalDelta, double verticalDelta, std::optional<juce::Point<int>> focusPoint)
{
    auto bounds = getTimelineContentBounds(*this);
    auto tracksArea = bounds.withTrimmedTop(42);
    auto gridArea = tracksArea;
    gridArea.removeFromLeft(trackHeaderWidth);

    const auto fullWidth = static_cast<double>(gridArea.getWidth());
    const auto fullHeight = static_cast<double>(tracksArea.getHeight());
    double focusXInView = fullWidth * 0.5;
    double focusYInView = fullHeight * 0.5;
    if (focusPoint.has_value())
    {
        focusXInView = juce::jlimit(0.0, fullWidth, static_cast<double>(focusPoint->x - gridArea.getX()));
        focusYInView = juce::jlimit(0.0, fullHeight, static_cast<double>(focusPoint->y - tracksArea.getY()));
    }

    const auto oldPixelsPerBeat = pixelsPerBeat;
    const auto focusBeat = oldPixelsPerBeat > 0.0 ? (scrollX + focusXInView) / oldPixelsPerBeat : 0.0;
    const auto oldTrackHeight = static_cast<double>(getTotalTrackHeight());
    const auto focusTrackRatio = oldTrackHeight > 0.0 ? (scrollY + focusYInView) / oldTrackHeight : 0.5;
    if (std::abs(horizontalDelta) > 0.0001)
    {
        const auto zoomFactor = std::pow(1.2, horizontalDelta);
        pixelsPerBeat = juce::jlimit(minPixelsPerBeat, maxPixelsPerBeat, pixelsPerBeat * zoomFactor);
        scrollX = (focusBeat * pixelsPerBeat) - focusXInView;
    }

    if (std::abs(verticalDelta) > 0.0001)
    {
        const auto zoomFactor = std::pow(1.18, verticalDelta);
        verticalZoom = juce::jlimit(minimumVerticalZoom, maximumVerticalZoom, verticalZoom * zoomFactor);
        const auto newTrackHeight = static_cast<double>(getTotalTrackHeight());
        scrollY = (focusTrackRatio * newTrackHeight) - focusYInView;
    }

    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::timerCallback()
{
    // Smoothly open / close the freed append lane below the last track.
    const float appendTarget = browserAppendActive ? 1.0f : 0.0f;
    if (std::abs(browserAppendAnim - appendTarget) > 0.001f)
    {
        browserAppendAnim += (appendTarget - browserAppendAnim) * 0.25f;
        if (std::abs(browserAppendAnim - appendTarget) < 0.01f)
            browserAppendAnim = appendTarget;

        // Ease the view down while opening so the freed space stays visible.
        if (browserAppendActive)
        {
            const auto visibleH = static_cast<double>(getVisibleTrackAreaBounds(*this).getHeight());
            const auto maxScroll = juce::jmax(0.0, static_cast<double>(getTotalTrackHeight()) - visibleH);
            if (std::abs(maxScroll - scrollY) > 0.5)
            {
                scrollY += (maxScroll - scrollY) * 0.3;
                clampScrollOffsets();
            }
        }
    }
    repaint();
}

float ArrangementTimelineComponent::beatToX(double beat, juce::Rectangle<int> laneArea) const noexcept
{
    return static_cast<float>(laneArea.getX() + (beat * pixelsPerBeat) - scrollX);
}

void ArrangementTimelineComponent::clampScrollOffsets()
{
    auto bounds = getTimelineContentBounds(*this);
    auto rulerGridArea = bounds.withTrimmedTop(42);
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    
    const auto fullWidth = static_cast<double>(rulerGridArea.getWidth());
    const auto timelineWidth = getTimelineEndBeats() * pixelsPerBeat;
    const auto maxScroll = juce::jmax(0.0, timelineWidth - fullWidth);
    scrollX = juce::jlimit(0.0, maxScroll, scrollX);
    
    const auto visibleTrackHeight = static_cast<double>(getVisibleTrackAreaBounds(*this).getHeight());
    const auto trackContentHeight = static_cast<double>(getTotalTrackHeight());
    const auto maxVerticalScroll = juce::jmax(0.0, trackContentHeight - visibleTrackHeight);
    scrollY = juce::jlimit(0.0, maxVerticalScroll, scrollY);
}

double ArrangementTimelineComponent::getTimelineEndBeats() const noexcept
{
    return juce::jmax(minTimelineLengthInBeats, project.getContentEndInBeats() + timelinePaddingInBeats);
}

int ArrangementTimelineComponent::getLaneHeightForTrack(int trackIndex) const noexcept
{
    // Children of a collapsed folder are hidden: zero height removes them from every layout
    // calculation (lane bounds, total height, hit-testing) without special-casing each call.
    if (isTrackHidden(trackIndex))
        return 0;

    if (fitTrackLanesToVisibleArea)
    {
        int visibleTrackCount = 0;
        const auto trackCount = static_cast<int>(project.getTracks().size());
        for (int index = 0; index < trackCount; ++index)
            if (! isTrackHidden(index))
                ++visibleTrackCount;

        if (visibleTrackCount > 0)
        {
            const auto availableHeight = getVisibleTrackAreaBounds(*this).getHeight();
            const auto fittedHeight = availableHeight / visibleTrackCount;
            return juce::jlimit(minimumLaneHeight, defaultLaneHeight, fittedHeight);
        }
    }

    const auto defaultHeight = juce::jlimit(
        minimumLaneHeight,
        maximumLaneHeight,
        static_cast<int>(std::round(static_cast<double>(defaultLaneHeight) * verticalZoom)));

    if (const auto it = customTrackHeights.find(trackIndex); it != customTrackHeights.end())
        return juce::jlimit(minimumLaneHeight, maxExpandedLaneHeight, it->second);

    return defaultHeight;
}

int ArrangementTimelineComponent::getTrackTopForIndex(int trackIndex) const noexcept
{
    int top = 0;
    for (int index = 0; index < juce::jmax(0, trackIndex); ++index)
        top += getLaneHeightForTrack(index);

    return top;
}

int ArrangementTimelineComponent::getTotalTrackHeight() const noexcept
{
    const auto trackCount = static_cast<int>(project.getTracks().size());
    int totalHeight = 0;
    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
        totalHeight += getLaneHeightForTrack(trackIndex);

    if (browserAppendAnim > 0.0f)
        totalHeight += juce::roundToInt(defaultLaneHeight * browserAppendAnim);   // freed space (animated)
    return totalHeight;
}

juce::Rectangle<int> ArrangementTimelineComponent::getTrackLaneBounds(int trackIndex) const noexcept
{
    const auto bounds = getVisibleTrackAreaBounds(*this);
    const auto laneHeight = getLaneHeightForTrack(trackIndex);
    const int laneTop = bounds.getY() + getTrackTopForIndex(trackIndex) - static_cast<int>(std::round(scrollY));
    return juce::Rectangle<int>(bounds.getX(), laneTop, bounds.getWidth(), laneHeight);
}

juce::Rectangle<int> ArrangementTimelineComponent::getClipBounds(const TimelineClip& clip, int trackIndex) const noexcept
{
    auto lane = getTrackLaneBounds(trackIndex);
    auto clipLane = lane;
    clipLane.removeFromLeft(trackHeaderWidth);
    const auto clipX    = beatToX(clip.startBeat, clipLane);
    const auto clipEndX = beatToX(clip.startBeat + clip.lengthInBeats, clipLane);

    // Snap each edge to integer pixels with the SAME rounding rule so that two clips
    // sharing a beat boundary (clipA.end == clipB.start) land on the same pixel column
    // — no visible gap between them at any zoom level. Previously each clip was inset by
    // 1px on each side, which guaranteed a 2px gap between adjacent clips.
    const auto left  = juce::roundToInt(clipX);
    const auto right = juce::roundToInt(clipEndX);
    return juce::Rectangle<int>(
        left,
        lane.getY() + 1,
        juce::jmax(1, right - left),
        juce::jmax(1, lane.getHeight() - 2));
}

double ArrangementTimelineComponent::xToBeatDelta(int xDelta) const noexcept
{
    return pixelsPerBeat > 0.0 ? static_cast<double>(xDelta) / pixelsPerBeat : 0.0;
}

double ArrangementTimelineComponent::xToBeatPosition(int x) const noexcept
{
    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(timelineRulerHeight);
    bounds.removeFromLeft(trackHeaderWidth);

    const auto localX = static_cast<double>(x - bounds.getX());
    return pixelsPerBeat > 0.0 ? juce::jmax(0.0, (scrollX + localX) / pixelsPerBeat) : 0.0;
}

int ArrangementTimelineComponent::trackIndexFromY(int y) const noexcept
{
    const auto trackCount = static_cast<int>(project.getTracks().size());
    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        const auto lane = getTrackLaneBounds(trackIndex);
        if (y >= lane.getY() && y < lane.getBottom())
            return trackIndex;
    }

    return -1;
}

double ArrangementTimelineComponent::snapBeatValue(double beat) const noexcept
{
    return std::round(beat / snapSizeInBeats) * snapSizeInBeats;
}

bool ArrangementTimelineComponent::canClipLiveOnTrack(const TimelineClip& clip, int trackIndex) const noexcept
{
    const auto& track = project.getTracks()[static_cast<std::size_t>(trackIndex)];
    return (clip.type == ClipType::midi && track.isMidiTrack) || (clip.type == ClipType::audio && ! track.isMidiTrack);
}

void ArrangementTimelineComponent::moveSelectedClipToTrack(int targetTrackIndex)
{
    if (! dragState.has_value() || ! selectedClip.has_value())
        return;

    auto& tracks = project.getTracks();
    auto& sourceTrack = tracks[static_cast<std::size_t>(dragState->clip.trackIndex)];
    auto movingClip = sourceTrack.clips[static_cast<std::size_t>(dragState->clip.clipIndex)];

    if (! canClipLiveOnTrack(movingClip, targetTrackIndex))
        return;

    sourceTrack.clips.erase(sourceTrack.clips.begin() + dragState->clip.clipIndex);

    auto& targetTrack = tracks[static_cast<std::size_t>(targetTrackIndex)];
    targetTrack.clips.push_back(movingClip);
    const auto newClipIndex = static_cast<int>(targetTrack.clips.size()) - 1;

    dragState->clip = SelectedClip { targetTrackIndex, newClipIndex };
    setSingleSelection(dragState->clip);
}

bool ArrangementTimelineComponent::splitClipAtBeat(int trackIndex, int clipIndex, double splitBeat)
{
    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return false;
    auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips.size()))
        return false;

    const auto original = clips[static_cast<std::size_t>(clipIndex)];
    const auto clipEnd = original.startBeat + original.lengthInBeats;
    // Must split strictly inside the clip (leave at least a tiny sliver each side).
    if (splitBeat <= original.startBeat + beatEpsilon || splitBeat >= clipEnd - beatEpsilon)
        return false;

    const auto leftLen  = splitBeat - original.startBeat;
    const auto rightLen = clipEnd - splitBeat;

    // Where the split lands inside the source (respecting the existing trim).
    const auto trimStart = juce::jlimit(0.0, 1.0, original.sampleStartRatio);
    const auto trimEnd   = juce::jlimit(trimStart, 1.0, original.sampleEndRatio);
    const auto progress  = original.lengthInBeats > 0.0 ? leftLen / original.lengthInBeats : 0.5;
    const auto splitRatio = trimStart + progress * (trimEnd - trimStart);

    auto left = original;
    left.lengthInBeats  = leftLen;
    left.sampleEndRatio = splitRatio;
    left.fadeOutBeats   = 0.0;                                  // cut edge — no fade-out
    left.fadeInBeats    = juce::jmin(original.fadeInBeats, leftLen);

    auto right = original;
    right.startBeat       = splitBeat;
    right.lengthInBeats   = rightLen;
    right.sampleStartRatio = splitRatio;
    right.fadeInBeats     = 0.0;                                // cut edge — no fade-in
    right.fadeOutBeats    = juce::jmin(original.fadeOutBeats, rightLen);
    if (original.midiNotes.empty() == false)
    {
        // For MIDI clips, partition notes by the split point (relative to clip start).
        left.midiNotes.clear();
        right.midiNotes.clear();
        for (const auto& note : original.midiNotes)
        {
            if (note.startBeat < leftLen)
                left.midiNotes.push_back(note);
            else
            {
                auto shifted = note;
                shifted.startBeat -= leftLen;
                right.midiNotes.push_back(shifted);
            }
        }
    }

    clips[static_cast<std::size_t>(clipIndex)] = left;
    clips.insert(clips.begin() + clipIndex + 1, right);
    return true;
}

void ArrangementTimelineComponent::splitSelectionAtPlayhead()
{
    const auto playheadBeat = transport.getPlayheadBeat();
    auto& tracks = project.getTracks();

    // Gather targets: every selected clip the playhead passes through; if nothing is
    // selected, split whichever clip the playhead is over on any track.
    std::vector<SelectedClip> targets = selectedClips;
    if (targets.empty())
    {
        for (int t = 0; t < static_cast<int>(tracks.size()); ++t)
        {
            const auto& clips = tracks[static_cast<std::size_t>(t)].clips;
            for (int c = 0; c < static_cast<int>(clips.size()); ++c)
            {
                const auto& clip = clips[static_cast<std::size_t>(c)];
                if (playheadBeat > clip.startBeat && playheadBeat < clip.startBeat + clip.lengthInBeats)
                    targets.push_back({ t, c });
            }
        }
    }

    if (targets.empty())
        return;

    // Split from the highest clip index down so earlier indices stay valid.
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b)
    {
        if (a.trackIndex != b.trackIndex) return a.trackIndex > b.trackIndex;
        return a.clipIndex > b.clipIndex;
    });

    bool didSplit = false;
    bool captured = false;
    for (const auto& target : targets)
    {
        if (target.trackIndex < 0 || target.trackIndex >= static_cast<int>(tracks.size()))
            continue;
        const auto& clips = tracks[static_cast<std::size_t>(target.trackIndex)].clips;
        if (target.clipIndex < 0 || target.clipIndex >= static_cast<int>(clips.size()))
            continue;
        const auto& clip = clips[static_cast<std::size_t>(target.clipIndex)];
        if (playheadBeat <= clip.startBeat + beatEpsilon || playheadBeat >= clip.startBeat + clip.lengthInBeats - beatEpsilon)
            continue;

        if (! captured)
        {
            pushUndoSnapshot();
            captured = true;
        }
        if (splitClipAtBeat(target.trackIndex, target.clipIndex, playheadBeat))
            didSplit = true;
    }

    if (didSplit)
    {
        setSingleSelection(std::nullopt);
        notifyClipSelectionChanged();
        repaint();
    }
}

void ArrangementTimelineComponent::pushUndoSnapshot()
{
    undoStack.push_back(TimelineSnapshot { project.getTracks(), selectedClip });
    redoStack.clear();
}

void ArrangementTimelineComponent::restoreSnapshot(const TimelineSnapshot& snapshot)
{
    project.getTracks() = snapshot.tracks;
    selectedClip = snapshot.selectedClip;
    selectedClips.clear();
    if (selectedClip.has_value())
        selectedClips.push_back(*selectedClip);
    lastClickedClip = selectedClip;
    notifyClipSelectionChanged();
    hoverClip.reset();
    dragState.reset();
}

bool ArrangementTimelineComponent::hasTimelineChangedSince(const TimelineSnapshot& snapshot) const noexcept
{
    const auto& currentTracks = project.getTracks();

    if (currentTracks.size() != snapshot.tracks.size())
        return true;

    for (std::size_t trackIndex = 0; trackIndex < currentTracks.size(); ++trackIndex)
    {
        const auto& currentTrack = currentTracks[trackIndex];
        const auto& snapshotTrack = snapshot.tracks[trackIndex];

        if (currentTrack.clips.size() != snapshotTrack.clips.size())
            return true;

        for (std::size_t clipIndex = 0; clipIndex < currentTrack.clips.size(); ++clipIndex)
        {
            const auto& currentClip = currentTrack.clips[clipIndex];
            const auto& snapshotClip = snapshotTrack.clips[clipIndex];

            if (currentClip.name != snapshotClip.name
                || currentClip.type != snapshotClip.type
                || std::abs(currentClip.startBeat - snapshotClip.startBeat) > beatEpsilon
                || std::abs(currentClip.lengthInBeats - snapshotClip.lengthInBeats) > beatEpsilon
                || std::abs(currentClip.warpTargetLengthInBeats - snapshotClip.warpTargetLengthInBeats) > beatEpsilon
                || currentClip.colour != snapshotClip.colour)
            {
                return true;
            }
        }
    }

    return false;
}

void ArrangementTimelineComponent::clearBrowserDropPreview()
{
    browserDropPreviewBounds.reset();
    browserDropCreatesNewTrack = false;
    browserAppendActive = false;   // the open lane animates closed via the timer
    repaint();
}

void ArrangementTimelineComponent::updateBrowserDropPreview(const juce::Point<int>& position, const juce::var& description)
{
    const auto* payload = description.getDynamicObject();
    if (payload == nullptr)
    {
        clearBrowserDropPreview();
        return;
    }

    browserAppendActive = false;   // recompute below (base height excludes the freed lane)

    auto targetTrackIndex = trackIndexFromY(position.y);
    bool createNewTrack = false;
    const auto trackCount = static_cast<int>(project.getTracks().size());
    bool useBottomDropLane = false;

    if (trackCount > 0)
    {
        const auto visibleTracksArea = getVisibleTrackAreaBounds(*this);
        const auto needsBottomDropZone = getTotalTrackHeight() + defaultLaneHeight > visibleTracksArea.getHeight();
        if (needsBottomDropZone)
        {
            const auto bottomDropZoneTop = visibleTracksArea.getBottom() - juce::jmin(newTrackDropZoneHeight, visibleTracksArea.getHeight());
            const auto lastLane = getTrackLaneBounds(trackCount - 1);
            if (position.y >= juce::jmax(lastLane.getCentreY(), bottomDropZoneTop)
                && visibleTracksArea.contains(visibleTracksArea.getX() + 1, position.y))
            {
                targetTrackIndex = trackCount;
                createNewTrack = true;
                useBottomDropLane = true;
            }
        }
    }

    if (targetTrackIndex < 0)
    {
        auto tracksBounds = getTimelineContentBounds(*this);
        tracksBounds.removeFromTop(timelineRulerHeight);
        if (tracksBounds.contains(tracksBounds.getX() + 1, position.y))
        {
            targetTrackIndex = static_cast<int>(project.getTracks().size());
            createNewTrack = true;
        }
    }

    if (targetTrackIndex < 0)
    {
        clearBrowserDropPreview();
        return;
    }

    if (! createNewTrack)
    {
        const auto& track = project.getTracks()[static_cast<std::size_t>(targetTrackIndex)];
        if (track.isMidiTrack)
        {
            clearBrowserDropPreview();
            return;
        }
    }

    // The freed empty-lane animation only runs in the bottom/append case (playlist full).
    browserAppendActive = useBottomDropLane;

    const auto sourceFile = juce::File(payload->getProperty("path").toString());
    const auto fallbackLengthBeats = fallbackClipLengthInBeats(*payload);
    const auto analysis = analyzeImportedAudioClip(sourceFile, project.getTempoBpm(), project.getNumerator(), fallbackLengthBeats);

    const auto previewDouble = [&](const char* prop, double fb)
    {
        return payload->hasProperty(prop) ? static_cast<double>(payload->getProperty(prop)) : fb;
    };
    const auto sStart = juce::jlimit(0.0, 0.999, previewDouble("sampleStartRatio", 0.0));
    const auto sEnd = juce::jlimit(sStart + 0.001, 1.0, previewDouble("sampleEndRatio", 1.0));
    const auto trimSpan = juce::jmax(0.001, sEnd - sStart);
    const auto sourceLen = payload->hasProperty("sourceLengthBeats")
        ? juce::jmax(minimumClipLengthInBeats, previewDouble("sourceLengthBeats", analysis.clipLengthInBeats))
        : analysis.clipLengthInBeats;
    const auto lengthBeats = juce::jmax(minimumClipLengthInBeats, sourceLen * trimSpan);
    const auto startBeat = juce::jlimit(
        0.0,
        juce::jmax(0.0, getTimelineEndBeats() - lengthBeats),
        snapBeatValue(xToBeatPosition(position.x)));

    const auto previewColour = createNewTrack
        ? theme::tracks::colourForIndex(static_cast<int>(project.getTracks().size()))
        : project.getTracks()[static_cast<std::size_t>(targetTrackIndex)].colour;

    const TimelineClip previewClip {
        payload->getProperty("name").toString(),
        ClipType::audio,
        startBeat,
        lengthBeats,
        previewColour,
        {},
        {},
        sourceFile.getFullPathName(),
        0.0,
        false,
        false,
        analysis.durationSeconds,
        analysis.sourceBpm,
        analysis.detectedBars,
        analysis.detectedBars > 0,
        analysis.bpmGuessed,
        lengthBeats,
        analysis.sourceKeyRoot,
        analysis.sourceKeyIsMinor,
        true
    };

    browserDropPreviewColour = previewClip.colour;
    browserDropCreatesNewTrack = createNewTrack;
    if (useBottomDropLane)
    {
        // Append case: the animated freed lane is the indicator — no clip phantom.
        browserDropPreviewBounds.reset();
    }
    else
    {
        // Phantom clip showing exactly where the sample will land (on the hovered
        // track, or the new lane in the empty area below).
        browserDropPreviewBounds = getClipBounds(previewClip, targetTrackIndex);
    }
    repaint();
}

void ArrangementTimelineComponent::notifyClipSelectionChanged()
{
    if (onClipSelectionChanged == nullptr)
        return;

    if (selectedClip.has_value())
        onClipSelectionChanged(selectedClip->trackIndex, selectedClip->clipIndex);
    else if (selectedTrackIndex.has_value())
        // Track header selected but no clip — still report the track so consumers
        // (e.g. sampler arming) can react to a "track-only" selection.
        onClipSelectionChanged(*selectedTrackIndex, -1);
    else
        onClipSelectionChanged(-1, -1);
}

std::optional<juce::Rectangle<int>> ArrangementTimelineComponent::getSelectedTrackInspectorBounds() const noexcept
{
    int trackIndex = -1;
    if (selectedClip.has_value())
        trackIndex = selectedClip->trackIndex;
    else if (selectedTrackIndex.has_value())
        trackIndex = *selectedTrackIndex;

    if (trackIndex < 0 || trackIndex >= static_cast<int>(project.getTracks().size()))
        return std::nullopt;

    auto lane = getTrackLaneBounds(trackIndex);
    return lane.removeFromLeft(trackHeaderWidth).reduced(8, 6);
}

const ArrangementTimelineComponent::AudioPeaks* ArrangementTimelineComponent::getOrComputePeaks(const juce::String& path)
{
    if (path.isEmpty())
        return nullptr;

    const auto key = path.toStdString();
    if (const auto it = waveformCache.find(key); it != waveformCache.end())
        return &it->second;

    // Not cached: build the peaks on a background thread (reading a long file to compute
    // the waveform must not block the UI — that was the small freeze on drop). Draw nothing
    // for this clip until the peaks are ready, then repaint. Enqueue each file only once.
    if (waveformPending.find(key) == waveformPending.end())
    {
        waveformPending.insert(key);
        juce::Component::SafePointer<ArrangementTimelineComponent> safe(this);
        waveformPool.addJob([safe, path, key]
        {
            auto peaks = std::make_shared<AudioPeaks>();
            bool ok = false;

            juce::File file(path);
            if (file.existsAsFile())
            {
                auto& fm = getSharedWaveformFormatManager();
                std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
                if (reader != nullptr && reader->lengthInSamples > 0 && reader->numChannels > 0)
                {
                    constexpr int samplesPerBucket = 256;
                    const auto totalSamples = static_cast<int>(juce::jmin<juce::int64>(reader->lengthInSamples,
                                                                                       static_cast<juce::int64>(std::numeric_limits<int>::max())));
                    const auto numChannels = static_cast<int>(reader->numChannels);
                    const auto numBuckets = (totalSamples + samplesPerBucket - 1) / samplesPerBucket;

                    peaks->samplesPerBucket = samplesPerBucket;
                    peaks->minVals.assign(static_cast<size_t>(numBuckets), 0.0f);
                    peaks->maxVals.assign(static_cast<size_t>(numBuckets), 0.0f);

                    constexpr int chunkSize = 8192;
                    juce::AudioBuffer<float> chunk(numChannels, chunkSize);
                    int samplesProcessed = 0;
                    while (samplesProcessed < totalSamples)
                    {
                        const auto toRead = juce::jmin(chunkSize, totalSamples - samplesProcessed);
                        if (! reader->read(&chunk, 0, toRead, samplesProcessed, true, true))
                            break;
                        for (int i = 0; i < toRead; ++i)
                        {
                            const auto bucketIdx = (samplesProcessed + i) / samplesPerBucket;
                            if (bucketIdx >= numBuckets)
                                break;
                            float val = 0.0f;
                            for (int ch = 0; ch < numChannels; ++ch)
                                val += chunk.getSample(ch, i);
                            val /= static_cast<float>(numChannels);
                            peaks->minVals[static_cast<size_t>(bucketIdx)] = juce::jmin(peaks->minVals[static_cast<size_t>(bucketIdx)], val);
                            peaks->maxVals[static_cast<size_t>(bucketIdx)] = juce::jmax(peaks->maxVals[static_cast<size_t>(bucketIdx)], val);
                        }
                        samplesProcessed += toRead;
                    }
                    ok = true;
                }
            }

            juce::MessageManager::callAsync([safe, key, peaks, ok]
            {
                if (auto* self = safe.getComponent())
                {
                    self->waveformPending.erase(key);
                    if (ok)
                        self->waveformCache.emplace(key, std::move(*peaks));
                    self->repaint();
                }
            });
        });
    }
    return nullptr;
}
}  // namespace orion
