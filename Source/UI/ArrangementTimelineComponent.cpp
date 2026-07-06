#include "ArrangementTimelineComponent.h"

#include "OrionTheme.h"
#include "../Audio/WarpEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <tuple>

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

const auto timelineBackground = juce::Colour(0xff18212a); // lighter DAW canvas: enough midtone for a calm, readable grid
const auto barGridColour = juce::Colour(0xff647180);
const auto beatGridColour = juce::Colour(0xff4f5b68);
const auto subdivisionGridColour = juce::Colour(0xff3e4954);
const auto markerColour = juce::Colours::white.withAlpha(0.64f);
const auto textColour = juce::Colours::white.withAlpha(0.88f);
const auto playheadColour = orion::theme::states::playhead;
const auto loopRangeColour = juce::Colour(0xff7ecb6f);
constexpr auto resizeHandleWidth = 12;
constexpr auto fadeHandleHitRadius = 8;
constexpr auto fadeHandleBandHeight = 18;
constexpr auto minimumClipLengthInBeats = 1.0;
constexpr auto snapSizeInBeats = 0.25;
constexpr auto splitEdgeSnapPixels = 12.0;
constexpr auto splitEdgeSnapMaxBeats = 0.125;
constexpr auto minimumOneShotClipLengthInBeats = 0.0625;
constexpr auto minTrackHeaderWidth = 176;
constexpr auto maxTrackHeaderWidth = 360;
constexpr auto beatEpsilon = 0.0001;
constexpr auto defaultLaneHeight = 78;
constexpr auto minimumLaneHeight = 42;
constexpr auto maximumLaneHeight = 176;
constexpr auto minimumVerticalZoom = 0.54;
constexpr auto maximumVerticalZoom = 2.26;
constexpr auto editToolbarHeight = 34;
constexpr auto timelineRulerHeight = 30;
constexpr auto timelineTopChromeHeight = editToolbarHeight + timelineRulerHeight;
constexpr auto editToolButtonCount = 9;
constexpr auto loopLaneHeight = 11;
constexpr auto loopHandleHitWidth = 8;
constexpr auto playheadHitWidth = 8;
constexpr auto newTrackDropZoneHeight = 64;
constexpr auto maxExpandedLaneHeight = 240;
constexpr auto minPixelsPerBeat = 0.05;   // tiny absolute floor; the real max-zoom-out is content-adaptive (minZoomPixelsPerBeat)
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

double choosePlaylistSubdivisionStep(double pixelsPerBeat, int beatsPerBar)
{
    if (pixelsPerBeat <= 0.0)
        return static_cast<double>(juce::jmax(1, beatsPerBar));

    // Ableton-like playlist density: normal zoom still shows the musical pulse, but finer
    // divisions are reserved for closer zoom levels.
    static constexpr std::array<double, 7> steps { 1.0, 0.5, 0.25, 2.0, 4.0, 8.0, 16.0 };
    for (const auto step : steps)
    {
        if (pixelsPerBeat * step >= 18.0)
            return step;
    }

    return static_cast<double>(chooseGridStepBeats(pixelsPerBeat, beatsPerBar, 42.0));
}

juce::Rectangle<int> getTimelineContentBounds(const juce::Component& component)
{
    auto bounds = component.getLocalBounds();
    bounds.removeFromTop(4);
    return bounds;
}

juce::Rectangle<int> getVisibleTrackAreaBounds(const juce::Component& component)
{
    auto bounds = getTimelineContentBounds(component);
    bounds.removeFromTop(timelineTopChromeHeight);
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
    result.clipLengthInBeats = juce::jmax(minimumOneShotClipLengthInBeats, durationInBeats);

    // A one-shot is short with no clear bar structure — it plays at natural length/pitch.
    // Loops & textures (anything longer, or with detected bars) get a musical length so warp
    // maps them 1:1 to the grid.
    const bool isOneShot = warp.durationSeconds > 0.0 && warp.durationSeconds < 1.2 && warp.detectedBars == 0;

    if (warp.detectedBars > 0)
    {
        result.clipLengthInBeats = juce::jmax(minimumClipLengthInBeats,
                                              static_cast<double>(warp.detectedBars) * beatsPerBar);
    }
    else if (isOneShot)
    {
        result.clipLengthInBeats = juce::jmax(minimumOneShotClipLengthInBeats, durationInBeats);
    }
    else if (warp.sourceBpm > 0.0 && ! isOneShot && warp.durationSeconds > 0.0)
    {
        // Musical length (in beats) of the source — snapped to the grid. This is what the
        // clip occupies once warped to the project tempo (source-beats map 1:1 to project-beats).
        const auto sourceBeats = warp.durationSeconds * (warp.sourceBpm / 60.0);
        const auto snappedBeats = std::round(sourceBeats / snapSizeInBeats) * snapSizeInBeats;
        result.clipLengthInBeats = juce::jmax(minimumClipLengthInBeats, snappedBeats);
    }

    return result;
}

AudioImportAnalysis cachedAnalyzeImportedAudioClip(const juce::File& file,
                                                   double tempoBpm,
                                                   int numerator,
                                                   double fallbackLengthBeats)
{
    static std::map<std::string, AudioImportAnalysis> cache;

    const auto key = (file.getFullPathName()
                      + "|" + juce::String(file.getSize())
                      + "|" + juce::String(file.getLastModificationTime().toMilliseconds())
                      + "|" + juce::String(tempoBpm, 6)
                      + "|" + juce::String(numerator)
                      + "|" + juce::String(fallbackLengthBeats, 6))
                         .toStdString();

    if (const auto it = cache.find(key); it != cache.end())
        return it->second;

    auto result = analyzeImportedAudioClip(file, tempoBpm, numerator, fallbackLengthBeats);
    if (cache.size() > 256)
        cache.erase(cache.begin());

    cache.emplace(key, result);
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

struct EditToolInfo
{
    const char* name;
    const char* shortcut;
};

EditToolInfo getEditToolInfo(int index) noexcept
{
    static constexpr std::array<EditToolInfo, editToolButtonCount> tools {{
        { "Select", "Cmd+V" },
        { "Range", "Cmd+R" },
        { "Cut", "Cmd+B / Cmd+E" },
        { "Trim", "Cmd+T" },
        { "Stretch", "Cmd+S" },
        { "Draw", "Cmd+Shift+D" },
        { "Mute", "Cmd+M" },
        { "Erase", "Cmd+Shift+E" },
        { "Audition", "Cmd+A" }
    }};

    if (index < 0 || index >= static_cast<int>(tools.size()))
        return { "", "" };

    return tools[static_cast<std::size_t>(index)];
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
    selectedTrackIndices = { trackIndex };
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

// A compact fingerprint of the per-track instrument layout, so undo/redo can tell whether
// the restored state needs the engine's hosted-instrument slots re-synced. Index position
// matters (a reorder changes the signature even with the same plugins).
static juce::String instrumentLayoutSignature(const std::vector<TrackState>& tracks)
{
    juce::String sig;
    for (const auto& t : tracks)
        sig << t.instrumentPluginId << "|";
    return sig;
}

bool ArrangementTimelineComponent::undo()
{
    if (undoStack.empty())
        return false;

    const auto beforeSig = instrumentLayoutSignature(project.getTracks());
    redoStack.push_back(TimelineSnapshot { project.getTracks(), selectedClip });
    restoreSnapshot(undoStack.back());
    undoStack.pop_back();
    if (onInstrumentLayoutChangedByHistory && beforeSig != instrumentLayoutSignature(project.getTracks()))
        onInstrumentLayoutChangedByHistory();
    repaint();
    return true;
}

bool ArrangementTimelineComponent::redo()
{
    if (redoStack.empty())
        return false;

    const auto beforeSig = instrumentLayoutSignature(project.getTracks());
    undoStack.push_back(TimelineSnapshot { project.getTracks(), selectedClip });
    restoreSnapshot(redoStack.back());
    redoStack.pop_back();
    if (onInstrumentLayoutChangedByHistory && beforeSig != instrumentLayoutSignature(project.getTracks()))
        onInstrumentLayoutChangedByHistory();
    repaint();
    return true;
}

bool ArrangementTimelineComponent::selectAllClips()
{
    selectedClips.clear();
    const auto& tracks = project.getTracks();

    for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex)
    {
        if (isTrackHidden(trackIndex))
            continue;

        const auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
        for (int clipIndex = 0; clipIndex < static_cast<int>(clips.size()); ++clipIndex)
            selectedClips.push_back(SelectedClip { trackIndex, clipIndex });
    }

    selectedClip = selectedClips.empty()
        ? std::optional<SelectedClip>()
        : std::optional<SelectedClip>(selectedClips.back());
    lastClickedClip = selectedClip;
    selectedTrackIndex.reset();
    selectedTrackIndices.clear();
    focusedSplitBeat.reset();
    notifyClipSelectionChanged();
    repaint();
    return true;
}

void ArrangementTimelineComponent::resetForNewProject()
{
    selectedClip.reset();
    lastClickedClip.reset();
    selectedTrackIndex.reset();
    selectedTrackIndices.clear();
    selectedClips.clear();
    focusedSplitBeat.reset();
    dragState.reset();
    loopSelectionState.reset();
    hoverClip.reset();
    knifePreviewBeat.reset();
    undoStack.clear();
    redoStack.clear();
    scrollX = 0.0;
    timelineAutoFitActive = true;
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
    auto editToolbarArea = bounds.removeFromTop(editToolbarHeight);
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
    g.setColour(juce::Colours::black.withAlpha(0.04f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 18.0f);

    const auto beatsPerBar = static_cast<double>(project.getNumerator());
    const auto totalBeats = getTimelineEndBeats();
    auto loopLane = rulerGridArea.removeFromTop(loopLaneHeight);
    auto markerLane = rulerGridArea;
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.fillRoundedRectangle(loopLane.toFloat(), 4.0f);

    g.setColour(juce::Colours::black.withAlpha(0.18f));
    g.fillRect(editToolbarArea);
    g.setColour(juce::Colours::white.withAlpha(0.055f));
    g.drawHorizontalLine(editToolbarArea.getBottom(), static_cast<float>(editToolbarArea.getX()), static_cast<float>(editToolbarArea.getRight()));

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
    const auto subdivisionStepBeats = choosePlaylistSubdivisionStep(pixelsPerBeat, beatsPerBarInt);
    const auto labelStepBeats = chooseGridStepBeats(pixelsPerBeat, beatsPerBarInt, 58.0);

    // Adaptive hierarchical grid: keep the density readable and use opacity, not thick strokes,
    // for hierarchy. Live's manual documents the beat-time ruler/editing grid behavior, but not
    // pixel weights; visually its arrangement grid stays close to crisp 1px guides.
    auto drawHierarchicalGrid = [&](juce::Rectangle<int> verticalArea, float intensity)
    {
        const auto barWidth = pixelsPerBeat * static_cast<double>(beatsPerBarInt);
        const auto stepBeats = barWidth >= 10.0
            ? beatsPerBarInt
            : chooseGridStepBeats(pixelsPerBeat, beatsPerBarInt, 42.0);
        if (stepBeats <= 0)
            return;

        const auto firstBeat = (firstVisibleBeat / stepBeats) * stepBeats;
        for (int beat = firstBeat; beat <= maxBeat; beat += stepBeats)
        {
            const auto x = beatToX(static_cast<double>(beat), gridArea);
            if (x > static_cast<float>(gridArea.getRight() + 50))
                break;
            if (x < static_cast<float>(gridArea.getX() - 50))
                continue;

            // Opacity by bar-group. Keep stroke width near 1px so the grid reads as guides,
            // not vertical dividers.
            const int bars = beat / beatsPerBarInt;
            float alpha;
            if      (bars % 64 == 0) alpha = 0.68f;
            else if (bars % 32 == 0) alpha = 0.62f;
            else if (bars % 16 == 0) alpha = 0.56f;
            else if (bars % 8  == 0) alpha = 0.48f;
            else if (bars % 4  == 0) alpha = 0.39f;
            else if (bars % 2  == 0) alpha = 0.30f;
            else                     alpha = 0.22f;

            g.setColour(barGridColour.withAlpha(alpha * intensity));
            g.drawLine(x, static_cast<float>(verticalArea.getY()), x, static_cast<float>(verticalArea.getBottom()), 1.0f);
        }
    };

    auto drawSubdivisionGrid = [&](juce::Rectangle<int> verticalArea, float intensity)
    {
        // Beat/sub-beat lines. At arrangement-fit zoom this becomes half-bar/beat guidance;
        // closer zooms reveal half/quarter beats.
        if (subdivisionStepBeats <= 0.0 || pixelsPerBeat * subdivisionStepBeats < 18.0)
            return;

        const auto firstBeat = std::floor(static_cast<double>(firstVisibleBeat) / subdivisionStepBeats) * subdivisionStepBeats;
        const auto maxBeatDouble = static_cast<double>(maxBeat) + beatsPerBar;
        for (double beat = firstBeat; beat <= maxBeatDouble; beat += subdivisionStepBeats)
        {
            const auto x = beatToX(beat, gridArea);
            if (x > static_cast<float>(gridArea.getRight() + 50))
                break;
            if (x < static_cast<float>(gridArea.getX() - 50))
                continue;

            const auto beatInBar = std::fmod(std::fmod(beat, beatsPerBar) + beatsPerBar, beatsPerBar);
            const auto isBar = beatInBar <= beatEpsilon || std::abs(beatInBar - beatsPerBar) <= beatEpsilon;
            if (isBar)
                continue; // bar lines belong to the hierarchy pass

            const auto isWholeBeat = std::abs(beat - std::round(beat)) <= beatEpsilon;
            const auto isHalfBeat = std::abs((beat * 2.0) - std::round(beat * 2.0)) <= beatEpsilon;
            const auto isQuarterBeat = std::abs((beat * 4.0) - std::round(beat * 4.0)) <= beatEpsilon;
            const auto alpha = (isWholeBeat ? 0.16f
                                : isHalfBeat ? 0.10f
                                : isQuarterBeat ? 0.07f
                                : 0.05f) * intensity;

            g.setColour((isWholeBeat ? beatGridColour : subdivisionGridColour).withAlpha(alpha));
            g.drawLine(x, static_cast<float>(verticalArea.getY()), x, static_cast<float>(verticalArea.getBottom()), 1.0f);
        }
    };

    // Ableton-style alternating section bands: every other bar-group gets a faint fill so the grid
    // reads as columns, not just hairlines. This is what makes Live's timeline legible at a glance.
    auto drawAlternatingBands = [&](juce::Rectangle<int> verticalArea, float intensity)
    {
        // Band width scales with zoom; keep the fill barely perceptible so it doesn't compete
        // with the grid itself.
        int bandBars = 1;
        while (static_cast<double>(bandBars) * beatsPerBarInt * pixelsPerBeat < 96.0)
            bandBars *= 2;
        const int bandStepBeats = bandBars * beatsPerBarInt;
        if (bandStepBeats <= 0)
            return;

        const auto firstBeat = (firstVisibleBeat / bandStepBeats) * bandStepBeats;
        for (int beat = firstBeat; beat <= maxBeat; beat += bandStepBeats)
        {
            const auto x0 = beatToX(static_cast<double>(beat), gridArea);
            const auto x1 = beatToX(static_cast<double>(beat + bandStepBeats), gridArea);
            if (x1 < static_cast<float>(gridArea.getX()) || x0 > static_cast<float>(gridArea.getRight()))
                continue;
            const bool lightBand = ((beat / bandStepBeats) & 1) != 0;
            g.setColour(lightBand ? juce::Colours::white.withAlpha(0.012f * intensity)
                                  : juce::Colours::black.withAlpha(0.010f * intensity));
            g.fillRect(juce::Rectangle<float>(x0, static_cast<float>(verticalArea.getY()),
                                              x1 - x0, static_cast<float>(verticalArea.getHeight())));
        }
    };

    g.saveState();
    g.reduceClipRegion(markerLane);
    drawAlternatingBands(rulerGridArea, 1.0f);
    drawSubdivisionGrid(rulerGridArea, 1.0f);
    drawHierarchicalGrid(rulerGridArea, 1.0f);

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

    // Vertical divider between the track-header column and the playlist, spanning the full
    // track area (continues below the last track).
    {
        const auto sepX = visibleTracksArea.getX() + trackHeaderWidth;
        g.setColour(theme::line::normal);
        g.drawVerticalLine(sepX, static_cast<float>(visibleTracksArea.getY()), static_cast<float>(visibleTracksArea.getBottom()));
    }

    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        const auto trackArrayIndex = static_cast<std::size_t>(trackIndex);
        auto lane = getTrackLaneBounds(trackIndex);
        if (! lane.intersects(visibleTracksArea))
            continue;

        const auto rowBounds = lane;

        auto trackNameArea = lane.removeFromLeft(trackHeaderWidth);
        g.setColour(juce::Colours::white.withAlpha(0.11f));
        g.drawHorizontalLine(rowBounds.getBottom(), static_cast<float>(rowBounds.getX()), static_cast<float>(rowBounds.getRight()));

        // Live output level for this track (0..1) — drives the header meter and the
        // "this track is playing" border glow.
        const auto trackLevel = onRequestTrackLevel
            ? juce::jlimit(0.0f, 1.0f, onRequestTrackLevel(trackIndex))
            : 0.0f;
        const auto isAudible = trackLevel > 0.015f;
        const auto isSelectedTrack = (selectedTrackIndex.has_value() && *selectedTrackIndex == trackIndex)
            || selectedTrackIndices.count(trackIndex) > 0
            || std::any_of(selectedClips.begin(), selectedClips.end(), [trackIndex](const auto& selected)
            {
                return selected.trackIndex == trackIndex;
            });

        juce::ignoreUnused(trackNameArea);
        const auto layout = computeHeaderLayout(trackIndex);
        const auto trackColour = tracks[trackArrayIndex].colour.withSaturation(0.70f).darker(0.03f);

        // Active-track lane wash (Studio One style): a soft tint over the WHOLE lane length,
        // so the selected track reads clearly — most visible in the empty area after its
        // clips. Drawn behind the grid lines and clips.
        if (isSelectedTrack)
        {
            auto laneGrid = rowBounds.withTrimmedLeft(trackHeaderWidth);
            if (! laneGrid.isEmpty())
            {
                g.setColour(juce::Colours::white.withAlpha(0.08f));
                g.fillRect(laneGrid);
            }
        }
        const auto cardBounds = layout.card.toFloat();
        const auto cardR = 9.0f;

        // Header card is rounded only on the RIGHT — the left edge is square so it reads as
        // anchored to the timeline edge.
        const auto rightRoundedCard = [cardR](juce::Rectangle<float> r)
        {
            juce::Path p;
            p.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight(),
                                  cardR, cardR, false, true, false, true);
            return p;
        };

        // Glass track header (matches the reference): translucent dark body + a soft top
        // gloss + a crisp bright rim. Selection tints it with the track colour.
        {
            juce::ColourGradient cardFill(juce::Colour(0xff2c333d).withAlpha(0.58f),
                                          cardBounds.getX(), cardBounds.getY(),
                                          juce::Colour(0xff121821).withAlpha(0.64f),
                                          cardBounds.getX(), cardBounds.getBottom(), false);
            g.setGradientFill(cardFill);
            g.fillPath(rightRoundedCard(cardBounds.reduced(1.0f)));
        }
        if (isSelectedTrack)
        {
            g.setColour(trackColour.withAlpha(0.18f));
            g.fillPath(rightRoundedCard(cardBounds.reduced(1.0f)));
        }

        // Colour spine on the left edge (square).
        g.setColour(trackColour.withAlpha(0.92f));
        g.fillRect(layout.card.withWidth(5).toFloat().reduced(0.0f, 1.0f));

        // Top gloss reflection.
        {
            g.saveState();
            g.reduceClipRegion(rightRoundedCard(cardBounds.reduced(1.0f)));
            const auto gh = cardBounds.getHeight() * 0.5f;
            juce::ColourGradient gloss(juce::Colours::white.withAlpha(0.10f), cardBounds.getX(), cardBounds.getY(),
                                       juce::Colours::white.withAlpha(0.0f), cardBounds.getX(), cardBounds.getY() + gh, false);
            g.setGradientFill(gloss);
            g.fillRect(cardBounds.withHeight(gh));
            g.restoreState();
        }

        // Crisp bright rim — brightest when selected, brightens with signal otherwise.
        const auto rimTop = isSelectedTrack ? 0.9f : (isAudible ? juce::jlimit(0.45f, 0.8f, 0.45f + trackLevel * 0.35f) : 0.42f);
        juce::ColourGradient rim(juce::Colours::white.withAlpha(rimTop),
                                 cardBounds.getX(), cardBounds.getY(),
                                 juce::Colours::white.withAlpha(rimTop * 0.35f),
                                 cardBounds.getX(), cardBounds.getBottom(), false);
        g.setGradientFill(rim);
        g.strokePath(rightRoundedCard(cardBounds.reduced(1.0f)), juce::PathStrokeType(isSelectedTrack ? 1.6f : 1.0f));

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
            g.fillRect(juce::Rectangle<float>(sp.getX() + 5.0f, sp.getY(), 3.0f, sp.getHeight()));
        }

        g.setColour(juce::Colours::white.withAlpha(0.70f));
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(juce::String(trackIndex + 1), layout.number, juce::Justification::centred);

        g.setColour(textColour.withAlpha(0.94f));
        g.setFont(juce::FontOptions(13.5f, juce::Font::bold));
        g.drawText(tracks[trackArrayIndex].name, layout.title, juce::Justification::centredLeft, true);

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

    drawAlternatingBands(visibleTracksArea, 1.0f);    // Ableton-style column stripes, under the grid + clips
    drawSubdivisionGrid(visibleTracksArea, 0.92f);
    drawHierarchicalGrid(visibleTracksArea, 0.92f);   // grid reads across the full playlist height, just a hair under the ruler

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

            // Body fill: vertical gradient from the colour-system variants (top highlight
            // → bottom shade). Selected clips get the Studio One-style lightened body and
            // white focus border below, without changing the track lane/header.
            const auto variant    = theme::tracks::variantsFor(clip.colour);
            const auto clipBase   = variant.base;
            const auto gradTop    = isSelected ? variant.gradientTop.interpolatedWith(juce::Colours::white, 0.34f) : variant.gradientTop;
            const auto gradBottom = isSelected ? variant.gradientBottom.interpolatedWith(juce::Colours::white, 0.24f) : variant.gradientBottom;
            // Studio One-style waveform: a darker shade of the clip colour sitting on the
            // saturated body (not a glowing light tint).
            const auto waveformColour = variant.waveform.withAlpha(0.92f);

            {
                juce::ignoreUnused(clipBase);
                // Clean Studio One body: an opaque top→bottom vertical gradient of the
                // track colour. No glass, no translucency — fast and solid.
                juce::ColourGradient body(gradTop, clipBounds.getX(), clipBounds.getY(),
                                          gradBottom, clipBounds.getX(), clipBounds.getBottom(), false);
                g.setGradientFill(body);
                g.fillRoundedRectangle(clipBounds, 10.0f);
            }

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
                g.setColour((isSelected
                    ? variant.gradientBottom.interpolatedWith(juce::Colours::white, 0.22f).darker(0.08f)
                    : variant.gradientBottom.darker(0.30f)).withAlpha(0.92f));
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
                    // Scale the drawn waveform by the clip's gain so lowering it visibly
                    // shrinks the wave (Studio One style); clamped so big boosts stay in view.
                    const auto waveGain  = juce::jlimit(0.05f, 4.0f, juce::Decibels::decibelsToGain(static_cast<float>(clip.gainDb)));
                    const auto halfH     = juce::jmax(0.5f, static_cast<float>(clipBodyBounds.getHeight()) * 0.45f * waveGain);
                    const auto numBuckets = static_cast<int>(minVals->size());

                    // Live capture is full (no trim); otherwise draw only the trimmed
                    // region [sampleStartRatio..sampleEndRatio] of the source.
                    const auto trimStartR = isLive ? 0.0 : juce::jlimit(0.0, 0.999, clip.sampleStartRatio);
                    const auto trimEndR   = isLive ? 1.0 : juce::jlimit(trimStartR + 0.001, 1.0, clip.sampleEndRatio);
                    const auto bucketStart = trimStartR * numBuckets;
                    const auto bucketSpan  = juce::jmax(1.0, (trimEndR - trimStartR) * numBuckets);
                    const auto visibleSourceBeats = clip.warpTargetLengthInBeats > beatEpsilon
                        ? (trimEndR - trimStartR) * clip.warpTargetLengthInBeats
                        : clip.lengthInBeats;
                    const auto waveformWidth = isLive
                        ? bodyWidth
                        : juce::jlimit(1,
                                       bodyWidth,
                                       juce::roundToInt(visibleSourceBeats * pixelsPerBeat));

                    for (int px = 0; px < waveformWidth; ++px)
                    {
                        const auto bStart = static_cast<int>(bucketStart + static_cast<double>(px) * bucketSpan / waveformWidth);
                        const auto bEnd   = static_cast<int>(bucketStart + static_cast<double>(px + 1) * bucketSpan / waveformWidth);
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
            else if (! clip.midiNotes.empty() && clipBodyBounds.getWidth() > 8 && clipBodyBounds.getHeight() > 6)
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
            g.setColour(variant.waveform.withAlpha(isSelected ? 0.95f : 0.85f));
            g.drawRoundedRectangle(clipBounds.reduced(0.5f, 0.5f), 9.5f, 1.0f);

            if (isSelected)
            {
                g.setColour(juce::Colours::white.withAlpha(0.92f));
                g.drawRoundedRectangle(clipBounds.reduced(1.2f, 1.2f), 8.8f, 2.2f);
            }

            // Subtle 1px top highlight for a clean Studio One sheen (cheap — no blur, no
            // diagonal glass sweep).
            if (clipBounds.getHeight() > 14.0f)
            {
                g.setColour(juce::Colours::white.withAlpha(0.16f));
                g.drawLine(clipBounds.getX() + 4.0f, clipBounds.getY() + 1.0f,
                           clipBounds.getRight() - 4.0f, clipBounds.getY() + 1.0f, 1.0f);
            }

            // Subtle internal beat/bar grid for MIDI clips — clean low-contrast vertical
            // lines like Ableton (single thin line, not the old harsh black+white stripes).
            if (clip.type == ClipType::midi && clip.lengthInBeats > beatEpsilon && clipBounds.getWidth() > 20.0f)
            {
                juce::Path clipShape;
                clipShape.addRoundedRectangle(clipBounds, 10.0f);
                g.saveState();
                g.reduceClipRegion(clipShape);

                const auto clipStartBeat = clip.startBeat;
                const auto clipEndBeat = clip.startBeat + clip.lengthInBeats;
                const auto beatsPerBar = static_cast<double>(juce::jmax(1, project.getNumerator()));
                const auto firstLineBeat = std::ceil(clipStartBeat + beatEpsilon);
                const auto lastLineBeat = std::floor(clipEndBeat - beatEpsilon);

                for (double beat = firstLineBeat; beat <= lastLineBeat; beat += 1.0)
                {
                    const auto localBeat = beat - clipStartBeat;
                    const auto x = clipBounds.getX() + static_cast<float>((localBeat / clip.lengthInBeats) * clipBounds.getWidth());
                    const auto isBarLine = std::abs(std::fmod(beat, beatsPerBar)) <= beatEpsilon
                        || std::abs(std::fmod(beat, beatsPerBar) - beatsPerBar) <= beatEpsilon;
                    g.setColour(juce::Colours::white.withAlpha(isBarLine ? 0.15f : 0.06f));
                    g.drawLine(x, clipBounds.getY() + 3.0f, x, clipBounds.getBottom() - 3.0f, 1.0f);
                }
                g.restoreState();
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

                const auto supportsFades = clip.type != ClipType::midi;
                const auto fadeInPx = supportsFades
                    ? juce::jlimit(0.0f, maxFadePx, static_cast<float>(clip.fadeInBeats * pixelsPerBeat))
                    : 0.0f;
                const auto fadeOutPx = supportsFades
                    ? juce::jlimit(0.0f, maxFadePx, static_cast<float>(clip.fadeOutBeats * pixelsPerBeat))
                    : 0.0f;

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
                if (supportsFades && handleVisible && clipBounds.getWidth() > 18.0f)
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

            const auto isStretchDragging = dragState.has_value()
                && dragState->clip.trackIndex == trackIndex
                && dragState->clip.clipIndex == clipIndex
                && (dragState->mode == DragMode::stretchLeft || dragState->mode == DragMode::stretchRight);
            auto naturalSourceLengthBeats = 0.0;
            if (clip.detectedBars > 0)
                naturalSourceLengthBeats = static_cast<double>(clip.detectedBars * juce::jmax(1, project.getNumerator()));
            else if (clip.sourceDurationSeconds > 0.0 && clip.sourceBpm > 0.0)
                naturalSourceLengthBeats = clip.sourceDurationSeconds * (clip.sourceBpm / 60.0);

            const auto isTimeStretched = clip.type == ClipType::audio
                && clip.warpTargetLengthInBeats > beatEpsilon
                && naturalSourceLengthBeats > beatEpsilon
                && std::abs(clip.warpTargetLengthInBeats - naturalSourceLengthBeats) > 0.02;

            // Show the time-stretch "clock" badge the moment the user arms a stretch by
            // hovering an audio clip's edge with Alt held (or with the Stretch tool active),
            // BEFORE any drag — so it's clear the next drag will time-stretch, not trim.
            const auto isStretchHoverPreview = clip.type == ClipType::audio
                && hoverClip.has_value()
                && hoverClip->clip.trackIndex == trackIndex
                && hoverClip->clip.clipIndex == clipIndex
                && (hoverClip->overResizeHandle || hoverClip->overLeftResizeHandle)
                && (juce::ModifierKeys::getCurrentModifiers().isAltDown() || currentTool == ToolMode::stretch);

            if ((isStretchDragging || isTimeStretched || isStretchHoverPreview) && clipBoundsInt.getWidth() >= 42 && clipBoundsInt.getHeight() >= 26)
            {
                const auto badge = headerStrip.getSmallestIntegerContainer()
                    .withSizeKeepingCentre(24, 18)
                    .withX(clipBoundsInt.getRight() - 30)
                    .withY(clipBoundsInt.getY() + 3);
                const auto badgeArmed = isStretchDragging || isStretchHoverPreview;
                const auto badgeF = badge.toFloat();
                g.setColour(juce::Colours::black.withAlpha(badgeArmed ? 0.54f : 0.38f));
                g.fillRoundedRectangle(badgeF, 6.0f);
                g.setColour(theme::cool::cyan.withAlpha(badgeArmed ? 0.42f : 0.24f));
                g.fillRoundedRectangle(badgeF.reduced(1.0f), 5.0f);
                g.setColour(juce::Colours::white.withAlpha(badgeArmed ? 0.98f : 0.80f));
                g.drawRoundedRectangle(badgeF.reduced(0.5f), 5.5f, 1.0f);

                const auto cx = static_cast<float>(badge.getX() + 9);
                const auto cy = static_cast<float>(badge.getCentreY());
                g.drawEllipse(cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 1.3f);
                g.drawLine(cx, cy, cx, cy - 2.8f, 1.2f);
                g.drawLine(cx, cy, cx + 2.5f, cy + 1.6f, 1.2f);

                juce::Path arrow;
                const auto ax = static_cast<float>(badge.getRight() - 8);
                arrow.startNewSubPath(ax - 4.0f, cy);
                arrow.lineTo(ax + 3.0f, cy);
                arrow.startNewSubPath(ax + 0.5f, cy - 2.5f);
                arrow.lineTo(ax + 3.2f, cy);
                arrow.lineTo(ax + 0.5f, cy + 2.5f);
                g.strokePath(arrow, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            // Muted clips dim under a 40% black overlay (colour-system UI state).
            if (clip.muted)
            {
                g.setColour(theme::states::mutedClipOverlay);
                g.fillRoundedRectangle(clipBounds, 10.0f);
            }

            // Clip gain handle (Studio One-style): a dot at the top-centre of the clip,
            // dragged up/down to set the clip's gain. Brighter when set or being dragged.
            {
                const auto handle = getClipGainHandleBounds(clip, trackIndex);
                if (! handle.isEmpty())
                {
                    const bool draggingThis = clipGainDragState.active
                        && clipGainDragState.trackIndex == trackIndex
                        && clipGainDragState.clipIndex == clipIndex;
                    const bool active = draggingThis || std::abs(clip.gainDb) > 0.01;
                    const auto hf = handle.toFloat();
                    g.setColour(juce::Colours::black.withAlpha(0.55f));
                    g.fillEllipse(hf.expanded(1.2f));
                    g.setColour(juce::Colours::white.withAlpha(active ? 0.98f : 0.72f));
                    g.fillEllipse(hf);
                    g.setColour(juce::Colours::black.withAlpha(0.55f));
                    g.fillEllipse(hf.reduced(hf.getWidth() * 0.30f));

                    if (draggingThis)
                    {
                        g.setColour(juce::Colours::white);
                        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
                        const auto txt = (clip.gainDb > 0.0 ? "+" : "") + juce::String(clip.gainDb, 1) + " dB";
                        g.drawText(txt, handle.translated(0, handle.getHeight() + 1).withHeight(13).expanded(24, 0),
                                   juce::Justification::centred);
                    }
                }
            }
            g.restoreState();
        }
    }

    g.restoreState();

    auto drawSplitPreview = [&](const SelectedClip& target, double rawBeat, bool liveKnife)
    {
        const auto& tracksRef = project.getTracks();
        if (target.trackIndex < 0 || target.trackIndex >= static_cast<int>(tracksRef.size()))
            return;

        const auto& clips = tracksRef[static_cast<std::size_t>(target.trackIndex)].clips;
        if (target.clipIndex < 0 || target.clipIndex >= static_cast<int>(clips.size()))
            return;

        const auto& clip = clips[static_cast<std::size_t>(target.clipIndex)];
        const auto previewBeat = snapSplitBeatForClip(rawBeat, clip);
        const auto clipEnd = clip.startBeat + clip.lengthInBeats;
        if (previewBeat < clip.startBeat - beatEpsilon || previewBeat > clipEnd + beatEpsilon)
            return;

        const auto clipBounds = getClipBounds(clip, target.trackIndex);
        if (clipBounds.isEmpty())
            return;

        const auto x = beatToX(previewBeat, visibleGridArea);
        if (x < static_cast<float>(clipBounds.getX()) - 1.0f || x > static_cast<float>(clipBounds.getRight()) + 1.0f)
            return;

        const auto top = static_cast<float>(clipBounds.getY() + 2);
        const auto bottom = static_cast<float>(clipBounds.getBottom() - 2);
        g.setColour(juce::Colours::black.withAlpha(liveKnife ? 0.50f : 0.36f));
        g.drawLine(x + 1.0f, top, x + 1.0f, bottom, liveKnife ? 3.0f : 2.2f);
        g.setColour(theme::text::primary.withAlpha(liveKnife ? 0.96f : 0.78f));
        g.drawLine(x, top, x, bottom, liveKnife ? 1.7f : 1.3f);
        g.setColour(theme::warm::red.withAlpha(liveKnife ? 0.98f : 0.78f));
        g.fillRoundedRectangle(x - 3.5f, top - 1.0f, 7.0f, 3.0f, 1.5f);
        g.fillRoundedRectangle(x - 3.5f, bottom - 2.0f, 7.0f, 3.0f, 1.5f);
    };

    g.saveState();
    g.reduceClipRegion(visibleGridArea);
    if (! dragState.has_value())
    {
        if (knifePreviewBeat.has_value() && hoverClip.has_value())
            drawSplitPreview(hoverClip->clip, *knifePreviewBeat, true);
        else if (focusedSplitBeat.has_value())
        {
            if (selectedClip.has_value())
                drawSplitPreview(*selectedClip, *focusedSplitBeat, false);
            else
                for (const auto& selected : selectedClips)
                    drawSplitPreview(selected, *focusedSplitBeat, false);
        }
    }
    g.restoreState();

    // Snap guide: while dragging a clip edge (trim OR time-stretch) draw a full-height
    // vertical line at the snapped edge, plus a length readout. When zoomed far out a
    // 0.25-beat snap step is sub-pixel and the edge seems to move "freely" — this line
    // lands on the grid so the snap target is obvious without having to zoom in first.
    if (dragState.has_value()
        && (dragState->mode == DragMode::resizeLeft  || dragState->mode == DragMode::resizeRight
         || dragState->mode == DragMode::stretchLeft || dragState->mode == DragMode::stretchRight))
    {
        const auto& tracks = project.getTracks();
        const auto ti = dragState->clip.trackIndex;
        const auto ci = dragState->clip.clipIndex;
        if (ti >= 0 && ti < static_cast<int>(tracks.size())
            && ci >= 0 && ci < static_cast<int>(tracks[static_cast<std::size_t>(ti)].clips.size()))
        {
            const auto& dc = tracks[static_cast<std::size_t>(ti)].clips[static_cast<std::size_t>(ci)];
            const bool leftEdge = dragState->mode == DragMode::resizeLeft
                               || dragState->mode == DragMode::stretchLeft;
            const auto edgeBeat = leftEdge ? dc.startBeat : dc.startBeat + dc.lengthInBeats;
            const auto gx = beatToX(edgeBeat, gridArea);
            const auto isStretch = dragState->mode == DragMode::stretchLeft
                                || dragState->mode == DragMode::stretchRight;

            g.saveState();
            g.reduceClipRegion(visibleGridArea);
            const auto top = static_cast<float>(visibleGridArea.getY());
            const auto bottom = static_cast<float>(visibleGridArea.getBottom());
            const auto guideColour = isStretch ? theme::cool::cyan : theme::text::primary;
            g.setColour(juce::Colours::black.withAlpha(0.45f));
            g.drawLine(gx + 1.0f, top, gx + 1.0f, bottom, 2.6f);
            g.setColour(guideColour.withAlpha(0.96f));
            g.drawLine(gx, top, gx, bottom, 1.5f);

            // Length readout (bars.beats) so a precise stretch target is legible.
            const auto beatsPerBar = juce::jmax(1, project.getNumerator());
            const auto lenBeats = juce::jmax(0.0, dc.lengthInBeats);
            const auto bars = static_cast<int>(std::floor(lenBeats / beatsPerBar)) + 1;
            const auto beatInBar = lenBeats - static_cast<double>(bars - 1) * beatsPerBar;
            const juce::String label = juce::String(bars) + "." + juce::String(beatInBar + 1.0, 2);
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            const juce::Rectangle<float> tag(gx - 30.0f, top + 3.0f, 60.0f, 16.0f);
            g.setColour(juce::Colours::black.withAlpha(0.55f));
            g.fillRoundedRectangle(tag, 4.0f);
            g.setColour(guideColour.withAlpha(0.98f));
            g.drawText(label, tag, juce::Justification::centred);
            g.restoreState();
        }
    }

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

    auto drawDropPreviewWaveform = [&](const juce::String& sourcePath, juce::Rectangle<int> previewBounds, juce::Colour colour)
    {
        if (sourcePath.isEmpty() || previewBounds.getWidth() <= 14 || previewBounds.getHeight() <= 14)
            return;

        const auto* peaks = getOrComputePeaks(sourcePath);
        if (peaks == nullptr || peaks->minVals.empty())
            return;

        auto wave = previewBounds.reduced(10, 8);
        if (wave.isEmpty())
            return;

        g.saveState();
        g.reduceClipRegion(previewBounds);
        g.setColour(colour.darker(0.34f).withAlpha(0.72f));

        const auto centerY = static_cast<float>(wave.getCentreY());
        const auto halfH = juce::jmax(1.0f, static_cast<float>(wave.getHeight()) * 0.43f);
        const auto numBuckets = static_cast<int>(peaks->minVals.size());
        const auto waveformWidth = juce::jmax(1, wave.getWidth());

        for (int px = 0; px < waveformWidth; ++px)
        {
            const auto bStart = static_cast<int>(static_cast<double>(px) * numBuckets / waveformWidth);
            const auto bEnd = static_cast<int>(static_cast<double>(px + 1) * numBuckets / waveformWidth);
            const auto safeEnd = juce::jmax(bStart + 1, bEnd);

            float minVal = 0.0f;
            float maxVal = 0.0f;
            for (int b = bStart; b < safeEnd && b < numBuckets; ++b)
            {
                minVal = juce::jmin(minVal, peaks->minVals[static_cast<size_t>(b)]);
                maxVal = juce::jmax(maxVal, peaks->maxVals[static_cast<size_t>(b)]);
            }

            const auto x = static_cast<float>(wave.getX() + px);
            const auto top = centerY + minVal * halfH;
            const auto bottom = centerY + maxVal * halfH;
            if (bottom - top >= 0.5f)
                g.drawLine(x, top, x, bottom, 1.0f);
            else
                g.fillRect(x, centerY - 0.5f, 1.0f, 1.0f);
        }

        g.restoreState();
    };

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
        drawDropPreviewWaveform(browserDropPreviewSourcePath, *browserDropPreviewBounds, browserDropPreviewColour);
        g.setColour(browserDropPreviewColour.withAlpha(0.95f));
        g.drawRoundedRectangle(browserDropPreviewBounds->toFloat(), 10.0f, 1.5f);
        if (browserDropSnapBeat.has_value())
        {
            const auto snapX = beatToX(*browserDropSnapBeat, visibleGridArea);
            const auto lane = browserDropPreviewBounds->expanded(0, 6).toFloat();
            g.setColour(juce::Colours::black.withAlpha(0.36f));
            g.drawLine(snapX + 1.0f, lane.getY(), snapX + 1.0f, lane.getBottom(), 3.0f);
            g.setColour(browserDropPreviewColour.brighter(0.45f).withAlpha(0.98f));
            g.drawLine(snapX, lane.getY(), snapX, lane.getBottom(), 2.0f);

            auto marker = juce::Rectangle<float>(snapX - 5.0f, lane.getY() - 6.0f, 10.0f, 6.0f);
            juce::Path triangle;
            triangle.startNewSubPath(marker.getCentreX(), marker.getBottom());
            triangle.lineTo(marker.getX(), marker.getY());
            triangle.lineTo(marker.getRight(), marker.getY());
            triangle.closeSubPath();
            g.fillPath(triangle);
        }
        g.restoreState();
    }

    if (! externalFileDropPreviews.empty())
    {
        g.saveState();
        g.reduceClipRegion(visibleGridArea);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));

        for (const auto& preview : externalFileDropPreviews)
        {
            auto boundsF = preview.bounds.toFloat();
            if (preview.createsNewTrack)
            {
                auto lane = preview.bounds;
                lane.setX(visibleGridArea.getX());
                lane.setWidth(visibleGridArea.getWidth());
                g.setColour(preview.colour.withAlpha(0.09f));
                g.fillRect(lane);
            }

            g.setColour(preview.colour.withAlpha(0.30f));
            g.fillRoundedRectangle(boundsF, 9.0f);
            drawDropPreviewWaveform(preview.sourcePath, preview.bounds, preview.colour);
            g.setColour(preview.colour.brighter(0.28f).withAlpha(0.94f));
            g.drawRoundedRectangle(boundsF.reduced(0.5f), 9.0f, 1.6f);

            if (boundsF.getWidth() > 48.0f)
            {
                g.setColour(juce::Colours::black.withAlpha(0.34f));
                g.drawText(preview.label, preview.bounds.reduced(9, 0).translated(1, 1), juce::Justification::centredLeft, true);
                g.setColour(theme::text::primary.withAlpha(0.95f));
                g.drawText(preview.label, preview.bounds.reduced(9, 0), juce::Justification::centredLeft, true);
            }
        }

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

juce::Rectangle<int> ArrangementTimelineComponent::getEditToolbarBounds() const noexcept
{
    auto bounds = getTimelineContentBounds(*this);
    return bounds.removeFromTop(editToolbarHeight);
}

juce::Rectangle<int> ArrangementTimelineComponent::getToolButtonBounds(int index) const noexcept
{
    auto toolbar = getEditToolbarBounds();
    constexpr int w = 30, h = 26, gap = 12, padX = 0;
    return juce::Rectangle<int>(toolbar.getX() + padX + index * (w + gap),
                                toolbar.getCentreY() - h / 2,
                                w,
                                h);
}

juce::Rectangle<int> ArrangementTimelineComponent::getSplitSnapButtonBounds() const noexcept
{
    auto toolbar = getEditToolbarBounds();
    toolbar.removeFromLeft(trackHeaderWidth);
    return toolbar.removeFromRight(126).reduced(8, 6);
}

void ArrangementTimelineComponent::paintToolPalette(juce::Graphics& g)
{
    auto toolbar = getEditToolbarBounds();

    g.saveState();
    auto paintRegion = toolbar;
    paintRegion.setBottom(juce::jmin(getLocalBounds().getBottom(), toolbar.getBottom() + 34));
    g.reduceClipRegion(paintRegion);

    g.setColour(juce::Colours::black.withAlpha(0.10f));
    g.fillRect(toolbar);
    g.setColour(theme::line::subtle.withAlpha(0.60f));
    g.drawVerticalLine(toolbar.getX() + trackHeaderWidth, static_cast<float>(toolbar.getY()), static_cast<float>(toolbar.getBottom()));
    g.drawHorizontalLine(toolbar.getBottom() - 1, static_cast<float>(toolbar.getX()), static_cast<float>(toolbar.getRight()));

    const auto toolModeForIndex = [](int index)
    {
        switch (index)
        {
            case 0:  return ToolMode::select;
            case 1:  return ToolMode::range;
            case 2:  return ToolMode::split;
            case 3:  return ToolMode::trim;
            case 4:  return ToolMode::stretch;
            case 5:  return ToolMode::draw;
            case 6:  return ToolMode::mute;
            case 7:  return ToolMode::erase;
            case 8:  return ToolMode::audition;
            default: return ToolMode::select;
        }
    };

    const auto mouse = getMouseXYRelative();
    auto hoveredIndex = -1;
    for (int i = 0; i < editToolButtonCount; ++i)
        if (getToolButtonBounds(i).contains(mouse))
            hoveredIndex = i;
    const auto snapHovered = getSplitSnapButtonBounds().contains(mouse);

    auto drawButtonShell = [&](int index, bool active, bool enabled)
    {
        const auto b = getToolButtonBounds(index).toFloat();
        const auto hover = hoveredIndex == index;
        const auto base = hover && enabled ? theme::surface::primary.withAlpha(0.72f)
                                           : theme::surface::primary.withAlpha(active ? 0.56f : 0.34f);
        g.setColour(base);
        g.fillRoundedRectangle(b, 7.0f);
        g.setColour(active ? theme::warm::red.withAlpha(0.82f)
                           : juce::Colours::white.withAlpha(enabled ? 0.18f : 0.07f));
        g.drawRoundedRectangle(b, 7.0f, active ? 1.4f : 1.0f);
        return b;
    };

    for (int i = 0; i < editToolButtonCount; ++i)
    {
        const auto active = currentTool == toolModeForIndex(i);
        const auto enabled = true;
        const auto b = drawButtonShell(i, active, enabled);
        const auto iconColour = active ? theme::warm::red.withAlpha(0.98f)
                                       : juce::Colours::white.withAlpha(enabled ? 0.78f : 0.28f);
        const auto icon = juce::Rectangle<float>(24.0f, 24.0f).withCentre(b.getCentre());
        const auto sx = icon.getX();
        const auto sy = icon.getY();
        const auto stroke = juce::PathStrokeType(1.75f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const auto thinStroke = juce::PathStrokeType(1.55f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        auto px = [&](float x) { return sx + x; };
        auto py = [&](float y) { return sy + y; };
        g.setColour(iconColour);

        if (i == 0)
        {
            juce::Path p;
            p.startNewSubPath(px(6.4f), py(3.6f));
            p.lineTo(px(7.7f), py(19.4f));
            p.lineTo(px(12.0f), py(14.4f));
            p.lineTo(px(15.1f), py(20.1f));
            p.lineTo(px(18.0f), py(18.5f));
            p.lineTo(px(14.8f), py(13.0f));
            p.lineTo(px(20.3f), py(13.0f));
            p.closeSubPath();
            g.strokePath(p, stroke);
        }
        else if (i == 1)
        {
            const auto r = juce::Rectangle<float>(px(5.2f), py(5.2f), 13.6f, 13.6f);
            juce::Path box;
            box.addRoundedRectangle(r, 3.5f);
            juce::Path dashed;
            const float dashes[] = { 2.8f, 2.6f };
            juce::PathStrokeType(1.75f).createDashedStroke(dashed, box, dashes, 2);
            g.fillPath(dashed);
        }
        else if (i == 2)
        {
            g.drawEllipse(px(3.8f), py(14.2f), 6.7f, 6.7f, 1.85f);
            g.drawEllipse(px(13.5f), py(14.2f), 6.7f, 6.7f, 1.85f);
            g.drawLine(px(8.6f), py(14.8f), px(12.0f), py(11.7f), 1.8f);
            g.drawLine(px(15.2f), py(14.8f), px(12.0f), py(11.7f), 1.8f);

            juce::Path bladeA;
            bladeA.startNewSubPath(px(12.0f), py(11.7f));
            bladeA.lineTo(px(5.9f), py(4.2f));
            bladeA.lineTo(px(8.3f), py(3.0f));
            bladeA.lineTo(px(13.2f), py(10.8f));
            g.strokePath(bladeA, stroke);

            juce::Path bladeB;
            bladeB.startNewSubPath(px(12.0f), py(11.7f));
            bladeB.lineTo(px(18.0f), py(4.2f));
            bladeB.lineTo(px(15.7f), py(3.0f));
            bladeB.lineTo(px(10.8f), py(10.8f));
            g.strokePath(bladeB, stroke);
        }
        else if (i == 3)
        {
            g.drawLine(px(5.4f), py(5.0f), px(5.4f), py(19.0f), 1.8f);
            g.drawLine(px(18.6f), py(5.0f), px(18.6f), py(19.0f), 1.8f);
            g.drawLine(px(8.0f), py(12.0f), px(16.0f), py(12.0f), 1.75f);
            juce::Path leftArrow;
            leftArrow.addTriangle(px(8.0f), py(12.0f), px(11.5f), py(8.8f), px(11.5f), py(15.2f));
            g.fillPath(leftArrow);
            juce::Path rightArrow;
            rightArrow.addTriangle(px(16.0f), py(12.0f), px(12.5f), py(8.8f), px(12.5f), py(15.2f));
            g.fillPath(rightArrow);
        }
        else if (i == 4)
        {
            g.drawLine(px(5.8f), py(5.0f), px(5.8f), py(19.0f), 1.8f);
            g.drawLine(px(18.2f), py(5.0f), px(18.2f), py(19.0f), 1.8f);
            g.drawLine(px(8.2f), py(12.0f), px(10.2f), py(12.0f), 1.7f);
            g.drawLine(px(13.8f), py(12.0f), px(15.8f), py(12.0f), 1.7f);
            g.drawLine(px(12.0f), py(6.6f), px(12.0f), py(17.4f), 1.8f);
            g.drawLine(px(7.4f), py(12.0f), px(16.6f), py(12.0f), 1.8f);
            g.drawLine(px(9.0f), py(9.0f), px(15.0f), py(15.0f), 1.6f);
            g.drawLine(px(15.0f), py(9.0f), px(9.0f), py(15.0f), 1.6f);
        }
        else if (i == 5)
        {
            juce::Path pen;
            pen.startNewSubPath(px(6.0f), py(17.7f));
            pen.lineTo(px(15.9f), py(7.8f));
            pen.lineTo(px(19.0f), py(10.9f));
            pen.lineTo(px(9.1f), py(20.8f));
            pen.closeSubPath();
            g.strokePath(pen, stroke);
            juce::Path nib;
            nib.startNewSubPath(px(4.7f), py(21.3f));
            nib.lineTo(px(6.0f), py(17.7f));
            nib.lineTo(px(9.1f), py(20.8f));
            nib.closeSubPath();
            g.strokePath(nib, thinStroke);
            g.drawLine(px(14.4f), py(6.3f), px(20.5f), py(12.4f), 1.7f);
        }
        else if (i == 6)
        {
            juce::Path sp;
            sp.startNewSubPath(px(4.7f), py(9.2f));
            sp.lineTo(px(8.6f), py(9.2f));
            sp.lineTo(px(13.0f), py(5.8f));
            sp.lineTo(px(13.0f), py(18.2f));
            sp.lineTo(px(8.6f), py(14.8f));
            sp.lineTo(px(4.7f), py(14.8f));
            sp.closeSubPath();
            g.strokePath(sp, stroke);
            g.drawLine(px(16.1f), py(8.6f), px(20.0f), py(15.8f), 1.9f);
            g.drawLine(px(20.0f), py(8.6f), px(16.1f), py(15.8f), 1.9f);
        }
        else if (i == 7)
        {
            juce::Path e;
            e.startNewSubPath(px(5.0f), py(15.3f));
            e.lineTo(px(12.0f), py(8.3f));
            e.lineTo(px(18.8f), py(15.1f));
            e.lineTo(px(11.9f), py(21.0f));
            e.lineTo(px(5.0f), py(21.0f));
            e.closeSubPath();
            g.strokePath(e, stroke);
            g.drawLine(px(9.8f), py(17.4f), px(15.4f), py(11.8f), 1.45f);
            g.drawLine(px(5.0f), py(21.0f), px(19.2f), py(21.0f), 1.7f);
        }
        else if (i == 8)
        {
            juce::Path headband;
            headband.startNewSubPath(px(5.2f), py(15.0f));
            headband.cubicTo(px(5.2f), py(5.3f), px(18.8f), py(5.3f), px(18.8f), py(15.0f));
            g.strokePath(headband, stroke);
            g.drawLine(px(5.2f), py(14.3f), px(5.2f), py(20.0f), 2.1f);
            g.drawLine(px(18.8f), py(14.3f), px(18.8f), py(20.0f), 2.1f);
        }
    }

    auto snap = getSplitSnapButtonBounds().toFloat();
    g.setColour(theme::surface::primary.withAlpha(snapHovered ? 0.56f : 0.30f));
    g.fillRoundedRectangle(snap, 6.0f);
    g.setColour(theme::line::subtle.withAlpha(0.72f));
    g.drawRoundedRectangle(snap, 6.0f, 1.0f);
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.setColour(theme::text::muted);
    g.drawText("Snap", snap.removeFromLeft(38.0f), juce::Justification::centred);
    g.setColour(theme::text::primary.withAlpha(0.88f));
    g.drawText(getSplitSnapName(), snap, juce::Justification::centred);

    if (hoveredIndex >= 0 || snapHovered)
    {
        const auto enabled = true;
        const auto text = snapHovered
            ? juce::String("Split Snap  ") + getSplitSnapName() + "  - click to change"
            : (juce::String(getEditToolInfo(hoveredIndex).name) + "  " + juce::String(getEditToolInfo(hoveredIndex).shortcut)
                + (enabled ? "" : ""));
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        const auto textW = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text);
        auto tip = juce::Rectangle<float>(0.0f, 0.0f, textW + 22.0f, 24.0f);
        const auto target = snapHovered ? getSplitSnapButtonBounds() : getToolButtonBounds(hoveredIndex);
        tip.setCentre(static_cast<float>(target.getCentreX()), static_cast<float>(toolbar.getBottom() + 15));
        tip.setX(juce::jlimit(static_cast<float>(toolbar.getX() + 6),
                              static_cast<float>(toolbar.getRight() - 6) - tip.getWidth(),
                              tip.getX()));

        g.setColour(juce::Colours::black.withAlpha(0.50f));
        g.fillRoundedRectangle(tip.translated(0.0f, 2.0f), 6.0f);
        g.setColour(theme::surface::primary.withAlpha(0.96f));
        g.fillRoundedRectangle(tip, 6.0f);
        g.setColour(theme::line::normal.withAlpha(0.75f));
        g.drawRoundedRectangle(tip, 6.0f, 1.0f);
        g.setColour(enabled ? theme::text::primary : theme::text::muted);
        g.drawText(text, tip.reduced(10.0f, 0.0f), juce::Justification::centred);
    }

    g.restoreState();
}

bool ArrangementTimelineComponent::handleEditToolbarClick(juce::Point<int> position)
{
    if (! getEditToolbarBounds().contains(position))
        return false;

    if (getSplitSnapButtonBounds().contains(position))
    {
        showSplitSnapMenu();
        return true;
    }

    for (int i = 0; i < editToolButtonCount; ++i)
    {
        if (! getToolButtonBounds(i).contains(position))
            continue;

        switch (i)
        {
            case 0: currentTool = ToolMode::select; break;
            case 1: currentTool = ToolMode::range; break;
            case 2: currentTool = ToolMode::split; break;
            case 3: currentTool = ToolMode::trim; break;
            case 4: currentTool = ToolMode::stretch; break;
            case 5: currentTool = ToolMode::draw; break;
            case 6: currentTool = ToolMode::mute; break;
            case 7: currentTool = ToolMode::erase; break;
            case 8: currentTool = ToolMode::audition; break;
            default: currentTool = ToolMode::select; break;
        }

        setMouseCursor(juce::MouseCursor::NormalCursor);
        grabKeyboardFocus();
        repaint();
        return true;
    }

    return true;
}

void ArrangementTimelineComponent::showSplitSnapMenu()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Smart", true, splitSnapMode == SplitSnapMode::smart);
    menu.addItem(2, "Bar", true, splitSnapMode == SplitSnapMode::bar);
    menu.addItem(3, "Beat", true, splitSnapMode == SplitSnapMode::beat);
    menu.addItem(4, "1/2 Beat", true, splitSnapMode == SplitSnapMode::halfBeat);
    menu.addItem(5, "1/4 Beat", true, splitSnapMode == SplitSnapMode::quarterBeat);
    menu.addSeparator();
    menu.addItem(6, "Free", true, splitSnapMode == SplitSnapMode::free);

    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(this)
                           .withTargetScreenArea(localAreaToGlobal(getSplitSnapButtonBounds())),
                       [this](int result)
                       {
                           if (result == 1) splitSnapMode = SplitSnapMode::smart;
                           else if (result == 2) splitSnapMode = SplitSnapMode::bar;
                           else if (result == 3) splitSnapMode = SplitSnapMode::beat;
                           else if (result == 4) splitSnapMode = SplitSnapMode::halfBeat;
                           else if (result == 5) splitSnapMode = SplitSnapMode::quarterBeat;
                           else if (result == 6) splitSnapMode = SplitSnapMode::free;
                           else return;

                           repaint();
                       });
}

bool ArrangementTimelineComponent::duplicateSelectedClip()
{
    if (selectedClips.empty())
        return false;

    auto& tracks = project.getTracks();

    std::vector<DragState::ClipItem> sourceItems;
    sourceItems.reserve(selectedClips.size());
    auto selectionStart = std::numeric_limits<double>::max();
    auto selectionEnd = 0.0;

    for (const auto& selected : selectedClips)
    {
        if (selected.trackIndex < 0 || selected.trackIndex >= static_cast<int>(tracks.size()))
            continue;

        const auto& clips = tracks[static_cast<std::size_t>(selected.trackIndex)].clips;
        if (selected.clipIndex < 0 || selected.clipIndex >= static_cast<int>(clips.size()))
            continue;

        const auto& clip = clips[static_cast<std::size_t>(selected.clipIndex)];
        sourceItems.push_back(DragState::ClipItem { selected, clip.startBeat, clip.lengthInBeats });
        selectionStart = juce::jmin(selectionStart, clip.startBeat);
        selectionEnd = juce::jmax(selectionEnd, clip.startBeat + clip.lengthInBeats);
    }

    if (sourceItems.empty() || selectionEnd <= selectionStart)
        return false;

    const auto selectionSpan = snapBeatValue(selectionEnd - selectionStart);
    auto maxOffset = std::numeric_limits<double>::max();
    for (const auto& item : sourceItems)
        maxOffset = juce::jmin(maxOffset, getTimelineEndBeats() - (item.originalStartBeat + item.originalLengthInBeats));

    const auto offset = juce::jlimit(0.0, maxOffset, selectionSpan);
    if (offset <= 0.0)
        return false;

    pushUndoSnapshot();

    std::vector<SelectedClip> newSelection;
    newSelection.reserve(sourceItems.size());
    for (const auto& item : sourceItems)
    {
        auto& clips = tracks[static_cast<std::size_t>(item.clip.trackIndex)].clips;
        auto duplicateClip = clips[static_cast<std::size_t>(item.clip.clipIndex)];
        duplicateClip.startBeat = snapBeatValue(item.originalStartBeat + offset);
        duplicateClip.name += " Copy";
        clips.push_back(duplicateClip);
        newSelection.push_back(SelectedClip { item.clip.trackIndex, static_cast<int>(clips.size()) - 1 });
    }

    selectedClips = std::move(newSelection);
    selectedClip = selectedClips.empty() ? std::optional<SelectedClip>() : std::optional<SelectedClip>(selectedClips.back());
    lastClickedClip = selectedClip;
    focusedSplitBeat.reset();
    notifyClipSelectionChanged();
    repaint();
    return true;
}

bool ArrangementTimelineComponent::deleteSelectedClips()
{
    if (! selectedClip.has_value())
        return false;

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
    focusedSplitBeat.reset();
    notifyClipSelectionChanged();
    hoverClip.reset();
    dragState.reset();
    repaint();
    return true;
}

bool ArrangementTimelineComponent::loopToSelectedClip()
{
    if (selectedClips.empty())
        return false;

    const auto& tracks = project.getTracks();
    auto loopStart = std::numeric_limits<double>::max();
    auto loopEnd = 0.0;

    for (const auto& selected : selectedClips)
    {
        if (selected.trackIndex < 0 || selected.trackIndex >= static_cast<int>(tracks.size()))
            continue;

        const auto& clips = tracks[static_cast<std::size_t>(selected.trackIndex)].clips;
        if (selected.clipIndex < 0 || selected.clipIndex >= static_cast<int>(clips.size()))
            continue;

        const auto& clip = clips[static_cast<std::size_t>(selected.clipIndex)];
        loopStart = juce::jmin(loopStart, clip.startBeat);
        loopEnd = juce::jmax(loopEnd, clip.startBeat + clip.lengthInBeats);
    }

    if (loopEnd <= loopStart)
        return false;

    project.setLoopRange(loopStart, loopEnd);
    transport.setLoopEnabled(true);
    repaint();
    return true;
}

void ArrangementTimelineComponent::resized()
{
    if (timelineAutoFitActive)
        applyTimelineAutoFit();

    clampScrollOffsets();
    if (volumeEditorTrackIndex.has_value())
        trackVolumeInlineEditor.setBounds(getTrackVolumeValueBounds(*volumeEditorTrackIndex));
    repaint();
}

void ArrangementTimelineComponent::mouseDown(const juce::MouseEvent& event)
{
    if (volumeEditorTrackIndex.has_value() && ! trackVolumeInlineEditor.getBounds().contains(event.getPosition()))
        commitTrackVolumeEditor(true);

    if (handleEditToolbarClick(event.getPosition()))
        return;

    // Clip gain handle (a dot at the top-centre of each clip, Studio One style). Takes
    // priority over clip selection/move so the handle stays grabbable.
    {
        auto& tracks = project.getTracks();
        for (int ti = 0; ti < static_cast<int>(tracks.size()); ++ti)
        {
            const auto& clips = tracks[static_cast<std::size_t>(ti)].clips;
            for (int ci = 0; ci < static_cast<int>(clips.size()); ++ci)
            {
                const auto handle = getClipGainHandleBounds(clips[static_cast<std::size_t>(ci)], ti);
                if (handle.isEmpty() || ! handle.expanded(12, 8).contains(event.getPosition()))
                    continue;

                if (event.getNumberOfClicks() >= 2)
                {
                    auto& clip = tracks[static_cast<std::size_t>(ti)].clips[static_cast<std::size_t>(ci)];
                    if (std::abs(clip.gainDb) > 0.001)
                    {
                        pushUndoSnapshot();
                        clip.gainDb = 0.0;  // reset
                        repaint();
                    }
                    return;
                }

                clipGainDragState.active      = true;
                clipGainDragState.trackIndex  = ti;
                clipGainDragState.clipIndex   = ci;
                clipGainDragState.startY      = event.getPosition().y;
                clipGainDragState.startGainDb = clips[static_cast<std::size_t>(ci)].gainDb;
                return;
            }
        }
    }

    if (getAddTrackButtonBounds().contains(event.getPosition()))
    {
        if (onAddTrackRequested)
            onAddTrackRequested();        // open the full Add Track dialog
        else
            showAddTrackMenu();           // fallback: inline popup
        return;
    }

    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(editToolbarHeight);
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
        const auto clickedTrack = trackHeaderHit->trackIndex;
        setSingleSelection(std::nullopt);

        // Multi-select track headers: Cmd toggles, Shift extends a range from the anchor,
        // plain click selects just this one. Only for a plain body click (not on a control).
        if (trackHeaderHit->control == TrackHeaderControl::none && event.mods.isCommandDown())
        {
            if (selectedTrackIndices.count(clickedTrack) > 0)
                selectedTrackIndices.erase(clickedTrack);
            else
                selectedTrackIndices.insert(clickedTrack);
        }
        else if (trackHeaderHit->control == TrackHeaderControl::none && event.mods.isShiftDown()
                 && selectedTrackIndex.has_value())
        {
            selectedTrackIndices.clear();
            for (int i = juce::jmin(*selectedTrackIndex, clickedTrack); i <= juce::jmax(*selectedTrackIndex, clickedTrack); ++i)
                selectedTrackIndices.insert(i);
        }
        else
        {
            selectedTrackIndices = { clickedTrack };
        }
        selectedTrackIndex = clickedTrack;

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

    // Split tool: clicking a clip splits it at the clicked position, with edge magnet.
    if (currentTool == ToolMode::split && hit.has_value())
    {
        const auto& clip = project.getTracks()[static_cast<std::size_t>(hit->clip.trackIndex)]
                               .clips[static_cast<std::size_t>(hit->clip.clipIndex)];
        const auto rawBeat = xToBeatPosition(event.getPosition().x);
        const auto splitBeat = snapSplitBeatForClip(rawBeat, clip);
        pushUndoSnapshot();
        if (splitClipAtBeat(hit->clip.trackIndex, hit->clip.clipIndex, splitBeat))
        {
            setSingleSelection(std::nullopt);
            knifePreviewBeat.reset();
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

    if (currentTool == ToolMode::mute && hit.has_value())
    {
        auto& clip = project.getTracks()[static_cast<std::size_t>(hit->clip.trackIndex)]
                         .clips[static_cast<std::size_t>(hit->clip.clipIndex)];
        pushUndoSnapshot();
        clip.muted = ! clip.muted;
        setSingleSelection(hit->clip);
        focusedSplitBeat.reset();
        grabKeyboardFocus();
        repaint();
        return;
    }

    if (currentTool == ToolMode::erase && hit.has_value())
    {
        setSingleSelection(hit->clip);
        deleteSelectedClips();
        grabKeyboardFocus();
        repaint();
        return;
    }

    if (currentTool == ToolMode::draw && ! hit.has_value() && gridArea.contains(event.getPosition()))
    {
        const auto trackIndex = trackIndexFromY(event.getPosition().y);
        if (trackIndex >= 0 && trackIndex < static_cast<int>(project.getTracks().size())
            && project.getTracks()[static_cast<std::size_t>(trackIndex)].isMidiTrack)
        {
            createMidiClipAt(trackIndex, snapClipCreationBeat(xToBeatPosition(event.getPosition().x)));
            grabKeyboardFocus();
            repaint();
            return;
        }
    }

    if (hit.has_value())
    {
        const auto overClipEditHandle = hit->overResizeHandle
            || hit->overLeftResizeHandle
            || hit->overFadeInHandle
            || hit->overFadeOutHandle
            || hit->overFadeInCurveHandle
            || hit->overFadeOutCurveHandle;

        if (overClipEditHandle)
            focusedSplitBeat.reset();
        else
            focusedSplitBeat = xToBeatPosition(event.getPosition().x);

        if (event.mods.isShiftDown() && ! overClipEditHandle)
            selectRangeTo(hit->clip);
        else if (! isClipSelected(hit->clip) || selectedClips.size() <= 1 || overClipEditHandle)
            setSingleSelection(hit->clip);
    }
    else if (gridArea.contains(event.getPosition()))
    {
        if (! event.mods.isShiftDown())
        {
            selectedTrackIndex.reset();
    selectedTrackIndices.clear();
            setSingleSelection(std::nullopt);
            focusedSplitBeat.reset();
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
    selectedTrackIndices.clear();
        setSingleSelection(std::nullopt);
        focusedSplitBeat.reset();
    }
    dragState.reset();

    if (hit.has_value())
    {
        const auto& clip = project.getTracks()[static_cast<std::size_t>(hit->clip.trackIndex)].clips[static_cast<std::size_t>(hit->clip.clipIndex)];
        // Edge drags: plain = trim (constant speed), Alt = time-stretch.
        const auto stretch = event.mods.isAltDown() || currentTool == ToolMode::stretch;
        const auto trimMode = currentTool == ToolMode::trim || currentTool == ToolMode::select || currentTool == ToolMode::range;
        const auto fadeMode = currentTool == ToolMode::fade || currentTool == ToolMode::select;
        const auto dragMode = (fadeMode && hit->overFadeInCurveHandle)
            ? DragMode::fadeInCurve
            : (fadeMode && hit->overFadeOutCurveHandle)
                ? DragMode::fadeOutCurve
                : (fadeMode && hit->overFadeInHandle)
                    ? DragMode::fadeIn
                    : (fadeMode && hit->overFadeOutHandle)
                        ? DragMode::fadeOut
                        : ((trimMode || currentTool == ToolMode::stretch) && hit->overResizeHandle)
                            ? (stretch ? DragMode::stretchRight : DragMode::resizeRight)
                            : ((trimMode || currentTool == ToolMode::stretch) && hit->overLeftResizeHandle)
                                ? (stretch ? DragMode::stretchLeft : DragMode::resizeLeft)
                                : DragMode::move;

        std::vector<DragState::ClipItem> dragItems;
        const auto useSelectedGroup = dragMode == DragMode::move && isClipSelected(hit->clip) && selectedClips.size() > 1;
        const auto clipsToDrag = useSelectedGroup ? selectedClips : std::vector<SelectedClip> { hit->clip };
        const auto& tracks = project.getTracks();
        for (const auto& selected : clipsToDrag)
        {
            if (selected.trackIndex < 0 || selected.trackIndex >= static_cast<int>(tracks.size()))
                continue;

            const auto& selectedTrackClips = tracks[static_cast<std::size_t>(selected.trackIndex)].clips;
            if (selected.clipIndex < 0 || selected.clipIndex >= static_cast<int>(selectedTrackClips.size()))
                continue;

            const auto& selectedTimelineClip = selectedTrackClips[static_cast<std::size_t>(selected.clipIndex)];
            dragItems.push_back(DragState::ClipItem {
                selected,
                selectedTimelineClip.startBeat,
                selectedTimelineClip.lengthInBeats
            });
        }

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
            false,
            event.mods.isAltDown() && dragMode == DragMode::move,
            false,
            dragItems
        };
    }

    grabKeyboardFocus();
    repaint();
}

void ArrangementTimelineComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (clipGainDragState.active)
    {
        auto& tracks = project.getTracks();
        const auto ti = clipGainDragState.trackIndex;
        const auto ci = clipGainDragState.clipIndex;
        if (ti >= 0 && ti < static_cast<int>(tracks.size())
            && ci >= 0 && ci < static_cast<int>(tracks[static_cast<std::size_t>(ti)].clips.size()))
        {
            // Drag up = louder. ~3 px per dB; -24..+12 dB range.
            const auto deltaPx = static_cast<double>(clipGainDragState.startY - event.getPosition().y);
            const auto newGainDb = juce::jlimit(-24.0, 12.0, clipGainDragState.startGainDb + deltaPx / 3.0);
            auto& clip = tracks[static_cast<std::size_t>(ti)].clips[static_cast<std::size_t>(ci)];
            if (std::abs(clip.gainDb - newGainDb) > 0.001)
            {
                if (! clipGainDragState.historyCaptured)
                {
                    pushUndoSnapshot();
                    clipGainDragState.historyCaptured = true;
                }

                clip.gainDb = newGainDb;
                repaint();
            }
        }
        return;
    }

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

    createDragCopiesIfNeeded();

    if (dragState->mode == DragMode::move)
    {
        // Only move to another track after a deliberate drag — a plain click (with a few
        // pixels of jitter) must never teleport the clip to a neighbouring lane.
        const auto draggedFarEnough = event.getDistanceFromDragStart() > 5;
        const auto hoveredTrackIndex = trackIndexFromY(event.getPosition().y);
        if (draggedFarEnough && dragState->clipItems.size() <= 1
            && hoveredTrackIndex >= 0 && hoveredTrackIndex != dragState->clip.trackIndex)
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
        auto minDelta = -std::numeric_limits<double>::max();
        auto maxDelta = std::numeric_limits<double>::max();
        for (const auto& item : dragState->clipItems)
        {
            minDelta = juce::jmax(minDelta, -item.originalStartBeat);
            maxDelta = juce::jmin(maxDelta, getTimelineEndBeats() - (item.originalStartBeat + item.originalLengthInBeats));
        }

        const auto clampedDelta = juce::jlimit(minDelta, maxDelta, beatDelta);
        for (const auto& item : dragState->clipItems)
        {
            if (item.clip.trackIndex < 0 || item.clip.trackIndex >= static_cast<int>(tracks.size()))
                continue;

            auto& clips = tracks[static_cast<std::size_t>(item.clip.trackIndex)].clips;
            if (item.clip.clipIndex < 0 || item.clip.clipIndex >= static_cast<int>(clips.size()))
                continue;

            auto& movingClip = clips[static_cast<std::size_t>(item.clip.clipIndex)];
            movingClip.startBeat = snapBeatValue(item.originalStartBeat + clampedDelta);
        }

        repaint();
        return;
    }

    // ---- Trim / time-stretch (left or right edge) ----
    const auto origStart = dragState->originalStartBeat;
    const auto origLen   = dragState->originalLengthInBeats;
    const auto origEnd   = origStart + origLen;
    const auto minLen = snapSizeInBeats;

    if (clip.type == ClipType::midi)
    {
        switch (dragState->mode)
        {
            case DragMode::resizeRight:
            case DragMode::stretchRight:
            {
                const auto maxLenTimeline = getTimelineEndBeats() - origStart;
                clip.lengthInBeats = juce::jlimit(minLen,
                                                  juce::jmax(minLen, maxLenTimeline),
                                                  snapBeatValue(origLen + beatDelta));
                break;
            }
            case DragMode::resizeLeft:
            case DragMode::stretchLeft:
            {
                const auto newStart = juce::jlimit(0.0, origEnd - minLen, snapBeatValue(origStart + beatDelta));
                clip.startBeat = newStart;
                clip.lengthInBeats = origEnd - newStart;
                break;
            }
            default:
                break;
        }

        repaint();
        return;
    }

    const auto trimStart0 = juce::jlimit(0.0, 0.999, dragState->originalSampleStartRatio);
    const auto trimEnd0   = juce::jlimit(trimStart0 + 0.001, 1.0, dragState->originalSampleEndRatio);
    const auto trimSpan0  = juce::jmax(0.001, trimEnd0 - trimStart0);
    // Full source length in beats at 1:1 speed (constant-speed reference).
    const auto fullLen = dragState->originalWarpTargetLengthInBeats > 0.0
        ? dragState->originalWarpTargetLengthInBeats
        : origLen / trimSpan0;

    switch (dragState->mode)
    {
        case DragMode::resizeRight: // trim right edge — constant speed, reveal/hide source
        {
            const auto maxLenTimeline = getTimelineEndBeats() - origStart;
            const auto newLen = juce::jlimit(minLen, juce::jmax(minLen, maxLenTimeline),
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
    if (getEditToolbarBounds().contains(event.getPosition()))
    {
        bool overButton = getSplitSnapButtonBounds().contains(event.getPosition());
        for (int i = 0; i < editToolButtonCount; ++i)
            overButton = overButton || getToolButtonBounds(i).contains(event.getPosition());

        setMouseCursor(overButton ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
        return;
    }

    if (getAddTrackButtonBounds().contains(event.getPosition()))
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        return;
    }

    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(timelineTopChromeHeight);
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
    knifePreviewBeat = (currentTool == ToolMode::split && hoverClip.has_value())
        ? std::optional<double>(xToBeatPosition(event.getPosition().x))
        : std::nullopt;

    if (currentTool == ToolMode::split)
        setMouseCursor(hoverClip.has_value() ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
    else if (currentTool == ToolMode::erase || currentTool == ToolMode::mute || currentTool == ToolMode::draw)
        setMouseCursor(hoverClip.has_value() || currentTool == ToolMode::draw ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
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
    knifePreviewBeat.reset();
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

void ArrangementTimelineComponent::modifierKeysChanged(const juce::ModifierKeys&)
{
    // Alt arms time-stretch on the hovered clip edge. Repaint so the stretch "clock"
    // badge appears/disappears the instant Alt is pressed/released, without waiting for
    // the mouse to move (or for a drag to start).
    if (dragState.has_value())
        return;
    const auto overEdge = hoverClip.has_value()
        && hoverClip->clip.trackIndex >= 0
        && (hoverClip->overResizeHandle || hoverClip->overLeftResizeHandle);
    if (overEdge)
        repaint();
}

void ArrangementTimelineComponent::mouseUp(const juce::MouseEvent&)
{
    const auto clipGainHistoryCaptured = clipGainDragState.historyCaptured;
    const auto wasBoxSelecting = selectionBoxState.active;

    inspectorResizeState.active = false;
    inspectorResizeState.trackIndex = -1;
    trackHeaderWidthResizeState.active = false;
    playheadDragState.active = false;
    trackVolumeDragState.active = false;
    clipGainDragState = {};
    loopSelectionState.reset();
    selectionBoxState.active = false;

    // Box (marquee) selection only notifies ONCE, on release — notifying on every drag move
    // made the host re-open/relayout the lower panels each frame (sampler/steps flickered).
    if (wasBoxSelecting)
        notifyClipSelectionChanged();

    if (clipGainHistoryCaptured && ! undoStack.empty() && ! hasTimelineChangedSince(undoStack.back()))
    {
        undoStack.pop_back();
    }

    if (dragState.has_value() && dragState->historyCaptured && ! undoStack.empty() && ! hasTimelineChangedSince(undoStack.back()))
    {
        undoStack.pop_back();
    }

    // A time-stretch / length edit just finished — tell the host so it re-preps warp live (the
    // new speed is heard immediately, without stopping and replaying).
    if (onClipWarpEdited && dragState.has_value()
        && (dragState->mode == DragMode::stretchLeft || dragState->mode == DragMode::stretchRight
            || dragState->mode == DragMode::resizeLeft || dragState->mode == DragMode::resizeRight))
    {
        onClipWarpEdited();
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
    bounds.removeFromTop(editToolbarHeight);
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

    // Double-click the beat ruler = zoom to fit all content to the width (Ableton "W").
    if (rulerGridArea.contains(event.getPosition()))
    {
        zoomToFitContent();
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
            createMidiClipAt(trackIndex, snapClipCreationBeat(xToBeatPosition(event.getPosition().x)));
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

    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0))
        return selectAllClips();

    // W — zoom to fit all content to the width (like Ableton's Optimize Arrangement Width).
    if (key == juce::KeyPress('w', 0, 0))
    {
        zoomToFitContent();
        return true;
    }

    if (selectedClip.has_value() && (key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey))
    {
        return deleteSelectedClips();
    }

    if (! selectedClip.has_value()
        && (! selectedTrackIndices.empty() || selectedTrackIndex.has_value())
        && (key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey))
    {
        deleteSelectedTracks();
        return true;
    }

    if (selectedClip.has_value() && key == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0))
    {
        return duplicateSelectedClip();
    }

    // Split selected clips at the focused split marker, or at the playhead as a fallback.
    if (key == juce::KeyPress('e', juce::ModifierKeys::commandModifier, 0))
    {
        splitSelectionAtFocusedBeat();
        return true;
    }

    if (selectedClip.has_value() && key == juce::KeyPress('l', juce::ModifierKeys::commandModifier, 0))
    {
        return loopToSelectedClip();
    }

    const auto setTool = [this](ToolMode tool)
    {
        currentTool = tool;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return true;
    };

    if (key == juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0))
        return setTool(ToolMode::select);

    if (key == juce::KeyPress('b', juce::ModifierKeys::commandModifier, 0))
        return setTool(ToolMode::split);

    if (key == juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0))
        return setTool(ToolMode::range);

    if (key == juce::KeyPress('t', juce::ModifierKeys::commandModifier, 0))
        return setTool(ToolMode::trim);

    if (key == juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0))
        return setTool(ToolMode::stretch);

    if (key == juce::KeyPress('f', juce::ModifierKeys::commandModifier, 0))
        return setTool(ToolMode::fade);

    if (key == juce::KeyPress('d', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
        return setTool(ToolMode::draw);

    if (key == juce::KeyPress('m', juce::ModifierKeys::commandModifier, 0))
        return setTool(ToolMode::mute);

    if (key == juce::KeyPress('e', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
        return setTool(ToolMode::erase);

    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
        return setTool(ToolMode::audition);

    // ---- Keyboard clip relocation (Logic-style, no dragging) ----
    // Alt+Up / Alt+Down: move the selected clip(s) one track up/down, keeping the time position.
    if (key == juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::altModifier, 0)
        && ! clipsToRelocate().empty())
    {
        nudgeSelectedClipsByTracks(-1);
        return true;
    }
    if (key == juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::altModifier, 0)
        && ! clipsToRelocate().empty())
    {
        nudgeSelectedClipsByTracks(+1);
        return true;
    }
    // Alt+M: move the selected clip(s) onto the selected track header, keeping the time position.
    if (key == juce::KeyPress('m', juce::ModifierKeys::altModifier, 0)
        && selectedTrackIndex.has_value() && ! clipsToRelocate().empty())
    {
        moveSelectedClipsToSelectedTrack();
        return true;
    }
    // Alt+P: move the selected clip(s) onto the playhead, on the same track(s).
    if (key == juce::KeyPress('p', juce::ModifierKeys::altModifier, 0)
        && ! clipsToRelocate().empty())
    {
        moveSelectedClipsToPlayhead();
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

            const auto edgeHitBounds = clipBounds.expanded(resizeHandleWidth, 0);
            if (! edgeHitBounds.contains(position))
                continue;

            if (midiOnly && clip.type != ClipType::midi)
                return std::nullopt;

            // Fade handles live in a thin band at the very top of the clip, at the
            // X where each fade currently ends (Studio One style). They take priority
            // over the right-edge resize handle while inside that top band.
            const auto supportsFades = clip.type != ClipType::midi;
            const auto inFadeBand = supportsFades && position.y <= clipBounds.getY() + fadeHandleBandHeight;
            const auto fadeInHandleX = clipBounds.getX()
                + juce::roundToInt(clip.fadeInBeats * pixelsPerBeat);
            const auto fadeOutHandleX = clipBounds.getRight()
                - juce::roundToInt(clip.fadeOutBeats * pixelsPerBeat);
            const auto overFadeIn = inFadeBand
                && std::abs(position.x - fadeInHandleX) <= fadeHandleHitRadius;
            const auto overFadeOut = inFadeBand
                && std::abs(position.x - fadeOutHandleX) <= fadeHandleHitRadius;

            const auto distanceFromLeft = std::abs(position.x - clipBounds.getX());
            const auto distanceFromRight = std::abs(position.x - clipBounds.getRight());
            const auto overResize = distanceFromRight <= resizeHandleWidth
                && distanceFromRight <= distanceFromLeft;
            const auto overResizeLeft = distanceFromLeft <= resizeHandleWidth
                && distanceFromLeft < distanceFromRight;

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
            if (supportsFades && fadeInPx > 18.0)
            {
                const auto p = curveHandlePoint(clipBounds.getX() + fadeInPx * 0.5, clip.fadeInCurve);
                overFadeInCurve = position.getDistanceFrom(p) <= fadeHandleHitRadius;
            }
            if (supportsFades && fadeOutPx > 18.0)
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
    selectedTrackIndices.clear();
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

    // NB: no notifyClipSelectionChanged() here — the box drag updates the highlight via repaint
    // only; the host is notified once on mouse-up so panels don't thrash during the drag.
}

void ArrangementTimelineComponent::createDragCopiesIfNeeded()
{
    if (! dragState.has_value() || dragState->mode != DragMode::move || ! dragState->copyOnDrag || dragState->copyCreated)
        return;

    auto& tracks = project.getTracks();
    std::vector<DragState::ClipItem> copiedItems;
    std::vector<SelectedClip> newSelection;
    copiedItems.reserve(dragState->clipItems.size());
    newSelection.reserve(dragState->clipItems.size());

    for (const auto& item : dragState->clipItems)
    {
        if (item.clip.trackIndex < 0 || item.clip.trackIndex >= static_cast<int>(tracks.size()))
            continue;

        auto& clips = tracks[static_cast<std::size_t>(item.clip.trackIndex)].clips;
        if (item.clip.clipIndex < 0 || item.clip.clipIndex >= static_cast<int>(clips.size()))
            continue;

        auto copiedClip = clips[static_cast<std::size_t>(item.clip.clipIndex)];
        copiedClip.name += " Copy";
        clips.push_back(copiedClip);

        const auto copiedSelection = SelectedClip { item.clip.trackIndex, static_cast<int>(clips.size()) - 1 };
        copiedItems.push_back(DragState::ClipItem {
            copiedSelection,
            item.originalStartBeat,
            item.originalLengthInBeats
        });
        newSelection.push_back(copiedSelection);
    }

    if (copiedItems.empty())
        return;

    dragState->clipItems = std::move(copiedItems);
    dragState->clip = dragState->clipItems.front().clip;
    dragState->copyCreated = true;
    selectedClips = std::move(newSelection);
    selectedClip = selectedClips.empty() ? std::optional<SelectedClip>() : std::optional<SelectedClip>(selectedClips.back());
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
    bounds.removeFromTop(editToolbarHeight);
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
    const auto& tracks = project.getTracks();
    layout.card = headerArea;

    // Studio One-style full-row track headers: a fixed number gutter on the left,
    // content fills the remaining row, and the meter stays on the right edge.
    auto inner = layout.card.reduced(8, 6);
    const bool isFolderTrack = trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())
                            && tracks[static_cast<std::size_t>(trackIndex)].isFolder;

    layout.number = inner.removeFromLeft(28);
    inner.removeFromLeft(6);

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

    // Instrument (load-VST) button on the right side of the controls row — MIDI tracks only,
    // since audio / folder tracks have no hosted VST instrument. Hidden on sampler tracks:
    // they already play the built-in sampler, so the VST-instrument button is irrelevant there.
    const bool isMidiTrack = trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())
                          && tracks[static_cast<std::size_t>(trackIndex)].isMidiTrack;
    const bool hasSampler = isMidiTrack
                          && tracks[static_cast<std::size_t>(trackIndex)].samplerSourcePath.isNotEmpty();
    if (isMidiTrack && ! hasSampler && controlsRow.getWidth() >= buttonSize)
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
    // Keep the audio engine's per-track instrument/insert slots aligned with the new track
    // indices — otherwise a lower track's instrument keeps the wrong key and a held note,
    // never receiving its note-off, rings forever.
    if (onTrackDeleted)
        onTrackDeleted(trackIndex);
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
    selectedTrackIndices.clear();
    selectedClip.reset();
    selectedClips.clear();
    lastClickedClip.reset();
    notifyClipSelectionChanged();
    hoverClip.reset();
    dragState.reset();
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::deleteSelectedTracks()
{
    // Collect every selected track (the multi-select set, or the single anchor as a fallback).
    std::vector<int> indices(selectedTrackIndices.begin(), selectedTrackIndices.end());
    if (indices.empty() && selectedTrackIndex.has_value())
        indices.push_back(*selectedTrackIndex);
    if (indices.empty())
        return;

    // Delete highest index first so the lower indices (and each onTrackDeleted call) stay valid.
    std::sort(indices.begin(), indices.end(), [](int a, int b) { return a > b; });

    auto& tracks = project.getTracks();
    pushUndoSnapshot();

    for (const int trackIndex : indices)
    {
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
            continue;

        tracks.erase(tracks.begin() + trackIndex);
        if (onTrackDeleted)   // realign the audio engine's per-track instrument/insert slots
            onTrackDeleted(trackIndex);

        std::map<int, int> reindexedHeights;
        for (const auto& [index, height] : customTrackHeights)
        {
            if (index < trackIndex)       reindexedHeights[index] = height;
            else if (index > trackIndex)  reindexedHeights[index - 1] = height;
        }
        customTrackHeights = std::move(reindexedHeights);
    }

    selectedTrackIndices.clear();
    selectedTrackIndex.reset();
    selectedTrackIndices.clear();
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
    bounds.removeFromTop(timelineTopChromeHeight);

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
        if (std::abs(scrollX - previousScrollX) > 0.01)
            timelineAutoFitActive = false;

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
    auto tracksArea = bounds.withTrimmedTop(timelineTopChromeHeight);
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
        tracksBounds.removeFromTop(timelineTopChromeHeight);
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
    const auto analysis = cachedAnalyzeImportedAudioClip(sourceFile, project.getTempoBpm(), project.getNumerator(), fallbackLengthBeats);
    const auto minImportedLengthBeats = analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2 && analysis.detectedBars == 0
        ? minimumOneShotClipLengthInBeats
        : minimumClipLengthInBeats;
    const auto sourceLengthBeats = isClipEditorDrop
        ? juce::jmax(minImportedLengthBeats, getPayloadDouble("sourceLengthBeats", analysis.clipLengthInBeats))
        : analysis.clipLengthInBeats;
    const auto lengthBeats = isClipEditorDrop
        ? juce::jmax(minImportedLengthBeats, sourceLengthBeats * sampleTrimSpan)
        : juce::jmax(minImportedLengthBeats, analysis.clipLengthInBeats * sampleTrimSpan);
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
    // Warp loops & textures (anything with a tempo/bars that isn't a tiny one-shot) so they
    // sync to the grid. One-shots (short, no bars) stay unstretched at natural pitch.
    const bool isOneShot = analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2
                           && analysis.detectedBars == 0;
    const bool autoWarp = analysis.durationSeconds > 0.0 && analysis.durationSeconds <= 90.0
                          && ! isOneShot
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
        analysis.sourceKeyRoot >= 0   // keyShiftEnabled — pitch to project key whenever a key is
                                      // known (textures/melodies too, not only tempo'd loops)
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
    ensureBeatVisible(droppedClip.startBeat + droppedClip.lengthInBeats);
    repaint();
}

int ArrangementTimelineComponent::addAudioClipToTrack(const juce::File& file, int trackIndex, double startBeat)
{
    auto& tracks = project.getTracks();
    if (! file.existsAsFile() || trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return -1;

    const auto numerator = juce::jmax(1, project.getNumerator());
    const auto fallbackLengthBeats = static_cast<double>(numerator);   // one bar if analysis fails
    const auto analysis = cachedAnalyzeImportedAudioClip(file, project.getTempoBpm(), numerator, fallbackLengthBeats);
    const auto lengthBeats = analysis.clipLengthInBeats;
    const auto clampedStart = juce::jlimit(0.0, juce::jmax(0.0, getTimelineEndBeats() - lengthBeats),
                                           juce::jmax(0.0, startBeat));

    auto& targetTrack = tracks[static_cast<std::size_t>(trackIndex)];
    const auto clipColour = targetTrack.colour;
    // Mirror the drop builder: auto-warp short tempo'd loops, leave one-shots/long files alone.
    const bool isOneShot = analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2
                           && analysis.detectedBars == 0;
    const bool autoWarp = analysis.durationSeconds > 0.0 && analysis.durationSeconds <= 90.0
                          && ! isOneShot
                          && (analysis.detectedBars > 0 || analysis.sourceBpm > 0.0);
    targetTrack.clips.push_back(TimelineClip {
        file.getFileNameWithoutExtension(),
        ClipType::audio,
        clampedStart,
        lengthBeats,
        clipColour,
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
        lengthBeats,
        analysis.sourceKeyRoot,
        analysis.sourceKeyIsMinor,
        analysis.sourceKeyRoot >= 0
    });
    auto& clip = targetTrack.clips.back();
    clip.signalAnalysisPending = analysis.needsSignalAnalysis;
    clip.gainNormalizationPending = false;

    ensureBeatVisible(clip.startBeat + clip.lengthInBeats);
    repaint();
    return static_cast<int>(targetTrack.clips.size()) - 1;
}

int ArrangementTimelineComponent::replaceSelectedAudioClipsSource(const juce::File& file)
{
    if (! file.existsAsFile() || selectedClips.empty())
        return 0;

    const auto numerator = juce::jmax(1, project.getNumerator());
    const auto analysis = cachedAnalyzeImportedAudioClip(file, project.getTempoBpm(), numerator,
                                                         static_cast<double>(numerator));
    auto& tracks = project.getTracks();

    bool capturedUndo = false;
    int replaced = 0;
    for (const auto& sel : selectedClips)
    {
        if (sel.trackIndex < 0 || sel.trackIndex >= static_cast<int>(tracks.size()))
            continue;
        auto& clips = tracks[static_cast<std::size_t>(sel.trackIndex)].clips;
        if (sel.clipIndex < 0 || sel.clipIndex >= static_cast<int>(clips.size()))
            continue;
        auto& clip = clips[static_cast<std::size_t>(sel.clipIndex)];
        if (clip.type != ClipType::audio)
            continue;

        if (! capturedUndo) { pushUndoSnapshot(); capturedUndo = true; }

        // Swap the audio file but keep the clip's position, length and trim.
        clip.sourcePath = file.getFullPathName();
        clip.sourceDurationSeconds = analysis.durationSeconds;
        clip.sourceBpm = analysis.sourceBpm;
        clip.detectedBars = analysis.detectedBars;
        clip.bpmGuessed = analysis.bpmGuessed;
        clip.sourceKeyRoot = analysis.sourceKeyRoot;
        clip.sourceKeyIsMinor = analysis.sourceKeyIsMinor;
        clip.signalAnalysisPending = analysis.needsSignalAnalysis;
        ++replaced;
    }

    if (replaced > 0)
        repaint();
    return replaced;
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

void ArrangementTimelineComponent::fileDragEnter(const juce::StringArray& files, int x, int y)
{
    updateExternalFileDropPreview(files, juce::Point<int>(x, y));
}

void ArrangementTimelineComponent::fileDragMove(const juce::StringArray& files, int x, int y)
{
    updateExternalFileDropPreview(files, juce::Point<int>(x, y));
}

void ArrangementTimelineComponent::fileDragExit(const juce::StringArray&)
{
    clearExternalFileDropPreview();
}

void ArrangementTimelineComponent::filesDropped(const juce::StringArray& files, int x, int y)
{
    clearExternalFileDropPreview();

    juce::StringArray audioFiles;
    for (const auto& f : files)
    {
        juce::File file(f);
        if (file.existsAsFile() && file.hasFileExtension(kAudioFileExtensions))
            audioFiles.add(file.getFullPathName());
    }

    if (audioFiles.isEmpty())
        return;

    const juce::Point<int> pos(x, y);
    if (audioFiles.size() <= 1)
    {
        importDroppedAudioFiles(audioFiles, pos, MultiFileDropMode::separateTracks);
        return;
    }

    auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::QuestionIcon)
        .withTitle("Import audio files")
        .withMessage("How do you want to place these files in the playlist?")
        .withButton("Separate Tracks")
        .withButton("One Track")
        .withButton("Cancel");

    juce::Component::SafePointer<ArrangementTimelineComponent> safeThis(this);
    juce::AlertWindow::showAsync(options, [safeThis, audioFiles, pos](int result)
    {
        if (safeThis == nullptr || result == 0 || result == 3)
            return;

        safeThis->importDroppedAudioFiles(audioFiles,
                                          pos,
                                          result == 2 ? MultiFileDropMode::oneTrack
                                                      : MultiFileDropMode::separateTracks);
    });
}

void ArrangementTimelineComponent::importDroppedAudioFiles(const juce::StringArray& files,
                                                           juce::Point<int> position,
                                                           MultiFileDropMode mode)
{
    if (files.isEmpty())
        return;

    pushUndoSnapshot();

    bool any = false;
    if (mode == MultiFileDropMode::separateTracks)
    {
        for (const auto& f : files)
            if (importAudioFileAt(juce::File(f), juce::Point<int>(position.x, std::numeric_limits<int>::max()), false))
                any = true;
    }
    else
    {
        std::optional<int> targetTrack;
        auto startBeat = snapBeatValue(xToBeatPosition(position.x));

        for (const auto& f : files)
        {
            if (importAudioFileAt(juce::File(f), position, false, targetTrack, startBeat, ! targetTrack.has_value()))
            {
                any = true;
                if (! targetTrack.has_value())
                    targetTrack = selectedClip.has_value() ? selectedClip->trackIndex : std::optional<int> {};
                if (selectedClip.has_value())
                {
                    const auto& tracks = project.getTracks();
                    const auto& clip = tracks[static_cast<std::size_t>(selectedClip->trackIndex)]
                        .clips[static_cast<std::size_t>(selectedClip->clipIndex)];
                    startBeat = clip.startBeat + clip.lengthInBeats;
                }
            }
        }
    }

    if (! any)
        dropLastUndoSnapshot();
    else
        notifyClipSelectionChanged();

    repaint();
}

bool ArrangementTimelineComponent::importAudioFileAt(const juce::File& file,
                                                     juce::Point<int> position,
                                                     bool captureHistory,
                                                     std::optional<int> forcedTrackIndex,
                                                     std::optional<double> forcedStartBeat,
                                                     bool forceCreateNewTrack)
{
    if (! file.existsAsFile() || ! file.hasFileExtension(kAudioFileExtensions))
        return false;

    auto& tracks = project.getTracks();
    int targetTrackIndex = forcedTrackIndex.value_or(trackIndexFromY(position.y));
    bool createNewTrack = forceCreateNewTrack;
    if (! createNewTrack
        && (targetTrackIndex < 0 || targetTrackIndex >= static_cast<int>(tracks.size())
        || tracks[static_cast<std::size_t>(targetTrackIndex)].isMidiTrack)
        && ! forcedTrackIndex.has_value())
    {
        targetTrackIndex = static_cast<int>(tracks.size());
        createNewTrack = true;
    }
    else if (! createNewTrack
             && (targetTrackIndex < 0 || targetTrackIndex >= static_cast<int>(tracks.size())
                 || tracks[static_cast<std::size_t>(targetTrackIndex)].isMidiTrack))
    {
        return false;
    }

    const auto analysis = cachedAnalyzeImportedAudioClip(file, project.getTempoBpm(), project.getNumerator(), 4.0);
    const auto sourceLengthBeats = analysis.clipLengthInBeats;
    const auto minImportedLengthBeats = analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2 && analysis.detectedBars == 0
        ? minimumOneShotClipLengthInBeats
        : minimumClipLengthInBeats;
    const auto lengthBeats = juce::jmax(minImportedLengthBeats, sourceLengthBeats);
    const auto rawStartBeat = forcedStartBeat.value_or(snapBeatValue(xToBeatPosition(position.x)));
    const auto startBeat = forcedStartBeat.has_value()
        ? juce::jmax(0.0, *forcedStartBeat)
        : juce::jlimit(0.0, juce::jmax(0.0, getTimelineEndBeats() - lengthBeats), rawStartBeat);

    if (captureHistory)
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
    // Warp loops & textures (anything with a tempo/bars that isn't a tiny one-shot) so they
    // sync to the grid. One-shots (short, no bars) stay unstretched at natural pitch.
    const bool isOneShot = analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2
                           && analysis.detectedBars == 0;
    const bool autoWarp = analysis.durationSeconds > 0.0 && analysis.durationSeconds <= 90.0
                          && ! isOneShot
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
        analysis.sourceKeyRoot >= 0   // keyShiftEnabled — pitch to project key whenever a key is known
    });
    auto& droppedClip = targetTrack.clips.back();
    droppedClip.signalAnalysisPending = analysis.needsSignalAnalysis;
    droppedClip.gainNormalizationPending = false;

    setSingleSelection(SelectedClip { targetTrackIndex, static_cast<int>(targetTrack.clips.size()) - 1 });
    ensureBeatVisible(droppedClip.startBeat + droppedClip.lengthInBeats);
    repaint();
    return true;
}

void ArrangementTimelineComponent::ensureBeatVisible(double endBeat)
{
    juce::ignoreUnused(endBeat);
    timelineAutoFitActive = false;
    clampScrollOffsets();
}

void ArrangementTimelineComponent::adjustZoom(double horizontalDelta, double verticalDelta, std::optional<juce::Point<int>> focusPoint)
{
    auto bounds = getTimelineContentBounds(*this);
    auto tracksArea = bounds.withTrimmedTop(timelineTopChromeHeight);
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
        timelineAutoFitActive = false;
        const auto zoomFactor = std::pow(1.2, horizontalDelta);
        targetPixelsPerBeat = juce::jlimit(minZoomPixelsPerBeat(), maxPixelsPerBeat, targetPixelsPerBeat * zoomFactor);
        zoomFocusBeat = focusBeat;
        zoomFocusXInView = focusXInView;
        zoomAnimating = true;
    }

    if (std::abs(verticalDelta) > 0.0001)
    {
        const auto zoomFactor = std::pow(1.18, verticalDelta);
        targetVerticalZoom = juce::jlimit(minimumVerticalZoom, maximumVerticalZoom, targetVerticalZoom * zoomFactor);
        zoomFocusTrackRatio = focusTrackRatio;
        zoomFocusYInView = focusYInView;
        zoomAnimating = true;
    }
    // The eased application happens in timerCallback; nothing to apply instantly here.
}

void ArrangementTimelineComponent::timerCallback()
{
    // Ease the zoom toward its target while keeping the focus point pinned, so the playlist glides
    // instead of stepping. Scroll is recomputed each frame from the pinned focus beat / track ratio.
    if (zoomAnimating)
    {
        bool done = true;
        const double dh = targetPixelsPerBeat - pixelsPerBeat;
        if (std::abs(dh) > pixelsPerBeat * 0.0025)
        {
            pixelsPerBeat = juce::jlimit(minZoomPixelsPerBeat(), maxPixelsPerBeat, pixelsPerBeat + dh * 0.3);
            done = false;
        }
        else
            pixelsPerBeat = juce::jlimit(minZoomPixelsPerBeat(), maxPixelsPerBeat, targetPixelsPerBeat);
        scrollX = (zoomFocusBeat * pixelsPerBeat) - zoomFocusXInView;

        const double dv = targetVerticalZoom - verticalZoom;
        if (std::abs(dv) > verticalZoom * 0.0025)
        {
            verticalZoom = juce::jlimit(minimumVerticalZoom, maximumVerticalZoom, verticalZoom + dv * 0.3);
            done = false;
        }
        else
            verticalZoom = juce::jlimit(minimumVerticalZoom, maximumVerticalZoom, targetVerticalZoom);
        scrollY = (zoomFocusTrackRatio * static_cast<double>(getTotalTrackHeight())) - zoomFocusYInView;

        clampScrollOffsets();
        if (done)
            zoomAnimating = false;
    }

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
    auto rulerGridArea = bounds.withTrimmedTop(timelineTopChromeHeight);
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    
    // Enforce the content-adaptive max-zoom-out floor here too, so the view fits the content on
    // load / after edits (not only when the user actively zooms) — no fixed "sliver" view.
    pixelsPerBeat = juce::jlimit(minZoomPixelsPerBeat(), maxPixelsPerBeat, pixelsPerBeat);

    const auto fullWidth = static_cast<double>(rulerGridArea.getWidth());
    const auto timelineWidth = getTimelineEndBeats() * pixelsPerBeat;
    const auto maxScroll = juce::jmax(0.0, timelineWidth - fullWidth);
    scrollX = juce::jlimit(0.0, maxScroll, scrollX);
    
    const auto visibleTrackHeight = static_cast<double>(getVisibleTrackAreaBounds(*this).getHeight());
    const auto trackContentHeight = static_cast<double>(getTotalTrackHeight());
    const auto maxVerticalScroll = juce::jmax(0.0, trackContentHeight - visibleTrackHeight);
    scrollY = juce::jlimit(0.0, maxVerticalScroll, scrollY);

    // Keep the zoom targets in step with any external change (fit, load, clamp) so the easing timer
    // doesn't yank the view back toward a stale target.
    if (! zoomAnimating)
    {
        targetPixelsPerBeat = pixelsPerBeat;
        targetVerticalZoom = verticalZoom;
    }
}

void ArrangementTimelineComponent::zoomToFitContent()
{
    timelineAutoFitActive = true;
    applyTimelineAutoFit();
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::applyTimelineAutoFit()
{
    auto bounds = getTimelineContentBounds(*this);
    auto rulerGridArea = bounds.withTrimmedTop(timelineTopChromeHeight);
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto viewportW = static_cast<double>(juce::jmax(1, rulerGridArea.getWidth()));

    pixelsPerBeat = juce::jlimit(minZoomPixelsPerBeat(), maxPixelsPerBeat,
                                 viewportW / autoFitTimelineBeats());
    targetPixelsPerBeat = pixelsPerBeat;
    zoomAnimating = false;
    scrollX = 0.0;
}

double ArrangementTimelineComponent::minZoomPixelsPerBeat() const noexcept
{
    auto bounds = getTimelineContentBounds(*this);
    auto rulerGridArea = bounds.withTrimmedTop(timelineTopChromeHeight);
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto viewportW = static_cast<double>(juce::jmax(1, rulerGridArea.getWidth()));

    // Max zoom-out fits the content with the same tail as auto-fit. Without this, the lower zoom
    // bound clamps away the right-side breathing room on long clips.
    const auto beatsPerBar   = static_cast<double>(juce::jmax(1, project.getNumerator()));
    const auto minVisible    = 64.0 * beatsPerBar;
    const auto targetBeats   = juce::jmax(minVisible, autoFitTimelineBeats());
    return juce::jmax(minPixelsPerBeat, viewportW / targetBeats);
}

double ArrangementTimelineComponent::autoFitTimelineBeats() const noexcept
{
    const auto beatsPerBar = static_cast<double>(juce::jmax(1, project.getNumerator()));
    const auto contentBeats = project.getContentEndInBeats();
    const auto marginBeats = contentBeats > 0.0 ? juce::jmax(8.0 * beatsPerBar, contentBeats * 0.08) : 0.0;
    // Live's max zoom-out does not resize the whole arrangement to the dropped sample; it
    // keeps the current density and only leaves a modest tail to the right of long content.
    const auto defaultVisibleBeats = 64.0 * beatsPerBar;
    return juce::jmax(defaultVisibleBeats, contentBeats + marginBeats);
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
        int visiblePosition   = 0;   // this track's order among the visible ones
        const auto trackCount = static_cast<int>(project.getTracks().size());
        for (int index = 0; index < trackCount; ++index)
            if (! isTrackHidden(index))
            {
                if (index < trackIndex)
                    ++visiblePosition;
                ++visibleTrackCount;
            }

        if (visibleTrackCount > 0)
        {
            const auto availableHeight = juce::jmax(0, getVisibleTrackAreaBounds(*this).getHeight());
            // Distribute the height EXACTLY: base share for everyone, and the integer
            // remainder spread one px at a time across the first `remainder` lanes so the
            // lanes sum to the available height with no leftover and, crucially, no overflow
            // (which previously pushed the bottom track under the open panel). We deliberately
            // do NOT clamp up to minimumLaneHeight here — fitting all tracks takes priority —
            // but we cap at defaultLaneHeight so a handful of tracks don't balloon.
            const auto base      = availableHeight / visibleTrackCount;
            const auto remainder = availableHeight % visibleTrackCount;
            const auto share     = base + (visiblePosition < remainder ? 1 : 0);
            return juce::jmax(1, juce::jmin(defaultLaneHeight, share));
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

juce::Rectangle<int> ArrangementTimelineComponent::getClipGainHandleBounds(const TimelineClip& clip, int trackIndex) const noexcept
{
    const auto clipBounds = getClipBounds(clip, trackIndex);
    if (clipBounds.getWidth() < 14 || clipBounds.getHeight() < 14)
        return {};
    constexpr int diameter = 6;
    const int cx = clipBounds.getCentreX();
    const int cy = clipBounds.getY() + diameter / 2 + 3;
    return juce::Rectangle<int>(cx - diameter / 2, cy - diameter / 2, diameter, diameter);
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
    bounds.removeFromTop(timelineTopChromeHeight);
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

double ArrangementTimelineComponent::snapClipCreationBeat(double beat) const noexcept
{
    const auto pixelTolerance = pixelsPerBeat > 0.0 ? 12.0 / pixelsPerBeat : snapSizeInBeats;
    const auto tolerance = juce::jlimit(beatEpsilon, 0.20, pixelTolerance);
    const auto beatsPerBar = static_cast<double>(juce::jmax(1, project.getNumerator()));

    const auto nearestBar = std::round(beat / beatsPerBar) * beatsPerBar;
    if (std::abs(beat - nearestBar) <= tolerance)
        return juce::jmax(0.0, nearestBar);

    const auto nearestBeat = std::round(beat);
    if (std::abs(beat - nearestBeat) <= tolerance)
        return juce::jmax(0.0, nearestBeat);

    return juce::jmax(0.0, snapBeatValue(beat));
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
    if (! dragState->clipItems.empty())
        dragState->clipItems.front().clip = dragState->clip;
    setSingleSelection(dragState->clip);
}

std::vector<ArrangementTimelineComponent::SelectedClip>
ArrangementTimelineComponent::clipsToRelocate() const
{
    if (! selectedClips.empty())
        return selectedClips;
    if (lastClickedClip.has_value())
        return { *lastClickedClip };
    return {};
}

std::vector<ArrangementTimelineComponent::SelectedClip>
ArrangementTimelineComponent::relocateClips(std::vector<std::tuple<int, int, int, double>> moves)
{
    auto& tracks = project.getTracks();

    // Snapshot the moving clips first (copies), because erasing shifts indices.
    struct Pending { TimelineClip clip; int targetTrack; };
    std::vector<Pending> pending;
    for (const auto& [srcTrack, srcIdx, dstTrack, newStart] : moves)
    {
        if (srcTrack < 0 || srcTrack >= static_cast<int>(tracks.size())) continue;
        auto& clips = tracks[static_cast<std::size_t>(srcTrack)].clips;
        if (srcIdx < 0 || srcIdx >= static_cast<int>(clips.size())) continue;
        auto clip = clips[static_cast<std::size_t>(srcIdx)];
        clip.startBeat = juce::jmax(0.0, newStart);
        pending.push_back({ std::move(clip), dstTrack });
    }

    // Erase originals, highest index first within each track so earlier indices stay valid.
    std::sort(moves.begin(), moves.end(), [](const auto& a, const auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
        return std::get<1>(a) > std::get<1>(b);
    });
    for (const auto& [srcTrack, srcIdx, dstTrack, newStart] : moves)
    {
        juce::ignoreUnused(dstTrack, newStart);
        if (srcTrack < 0 || srcTrack >= static_cast<int>(tracks.size())) continue;
        auto& clips = tracks[static_cast<std::size_t>(srcTrack)].clips;
        if (srcIdx >= 0 && srcIdx < static_cast<int>(clips.size()))
            clips.erase(clips.begin() + srcIdx);
    }

    std::vector<SelectedClip> newSelection;
    for (auto& p : pending)
    {
        auto& dst = tracks[static_cast<std::size_t>(p.targetTrack)].clips;
        dst.push_back(std::move(p.clip));
        newSelection.push_back(SelectedClip { p.targetTrack, static_cast<int>(dst.size()) - 1 });
    }
    return newSelection;
}

void ArrangementTimelineComponent::nudgeSelectedClipsByTracks(int delta)
{
    const auto src = clipsToRelocate();
    if (src.empty() || delta == 0)
        return;

    auto& tracks = project.getTracks();
    const auto trackCount = static_cast<int>(tracks.size());

    std::vector<std::tuple<int, int, int, double>> moves;
    for (const auto& c : src)
    {
        const auto dst = c.trackIndex + delta;
        if (dst < 0 || dst >= trackCount) return;   // don't move any if one would fall off the ends
        if (c.trackIndex < 0 || c.trackIndex >= trackCount) return;
        const auto& clip = tracks[static_cast<std::size_t>(c.trackIndex)].clips[static_cast<std::size_t>(c.clipIndex)];
        if (! canClipLiveOnTrack(clip, dst)) return;   // type mismatch → cancel the whole move
        moves.emplace_back(c.trackIndex, c.clipIndex, dst, clip.startBeat);
    }

    pushUndoSnapshot();
    const auto newSel = relocateClips(std::move(moves));
    selectedClips = newSel;
    selectedClip = newSel.empty() ? std::optional<SelectedClip>{} : std::optional<SelectedClip>{ newSel.back() };
    lastClickedClip = selectedClip;
    selectedTrackIndex.reset();
    selectedTrackIndices.clear();
    clampScrollOffsets();
    notifyClipSelectionChanged();
    repaint();
}

void ArrangementTimelineComponent::moveSelectedClipsToSelectedTrack()
{
    if (! selectedTrackIndex.has_value())
        return;
    const auto target = *selectedTrackIndex;
    const auto src = clipsToRelocate();
    if (src.empty())
        return;

    auto& tracks = project.getTracks();
    if (target < 0 || target >= static_cast<int>(tracks.size()))
        return;

    std::vector<std::tuple<int, int, int, double>> moves;
    for (const auto& c : src)
    {
        if (c.trackIndex < 0 || c.trackIndex >= static_cast<int>(tracks.size())) continue;
        if (c.trackIndex == target) continue;   // already there
        const auto& clip = tracks[static_cast<std::size_t>(c.trackIndex)].clips[static_cast<std::size_t>(c.clipIndex)];
        if (! canClipLiveOnTrack(clip, target)) return;   // type mismatch → cancel
        moves.emplace_back(c.trackIndex, c.clipIndex, target, clip.startBeat);
    }
    if (moves.empty())
        return;

    pushUndoSnapshot();
    const auto newSel = relocateClips(std::move(moves));
    selectedClips = newSel;
    selectedClip = newSel.empty() ? std::optional<SelectedClip>{} : std::optional<SelectedClip>{ newSel.back() };
    lastClickedClip = selectedClip;
    selectedTrackIndex.reset();
    selectedTrackIndices.clear();
    clampScrollOffsets();
    notifyClipSelectionChanged();
    repaint();
}

void ArrangementTimelineComponent::moveSelectedClipsToPlayhead()
{
    const auto src = clipsToRelocate();
    if (src.empty())
        return;

    auto& tracks = project.getTracks();
    const auto playhead = juce::jmax(0.0, transport.getPlayheadBeat());

    // Anchor on the earliest selected clip so multiple clips keep their relative spacing.
    double anchor = std::numeric_limits<double>::max();
    for (const auto& c : src)
        if (c.trackIndex >= 0 && c.trackIndex < static_cast<int>(tracks.size())
            && c.clipIndex >= 0 && c.clipIndex < static_cast<int>(tracks[static_cast<std::size_t>(c.trackIndex)].clips.size()))
            anchor = juce::jmin(anchor, tracks[static_cast<std::size_t>(c.trackIndex)].clips[static_cast<std::size_t>(c.clipIndex)].startBeat);
    if (anchor == std::numeric_limits<double>::max())
        return;

    std::vector<std::tuple<int, int, int, double>> moves;
    for (const auto& c : src)
    {
        if (c.trackIndex < 0 || c.trackIndex >= static_cast<int>(tracks.size())) continue;
        const auto& clip = tracks[static_cast<std::size_t>(c.trackIndex)].clips[static_cast<std::size_t>(c.clipIndex)];
        moves.emplace_back(c.trackIndex, c.clipIndex, c.trackIndex, playhead + (clip.startBeat - anchor));
    }
    if (moves.empty())
        return;

    pushUndoSnapshot();
    const auto newSel = relocateClips(std::move(moves));
    selectedClips = newSel;
    selectedClip = newSel.empty() ? std::optional<SelectedClip>{} : std::optional<SelectedClip>{ newSel.back() };
    lastClickedClip = selectedClip;
    clampScrollOffsets();
    notifyClipSelectionChanged();
    repaint();
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

double ArrangementTimelineComponent::snapSplitBeatToClipEdge(double splitBeat, const TimelineClip& clip) const noexcept
{
    const auto clipEnd = clip.startBeat + clip.lengthInBeats;
    const auto pixelTolerance = pixelsPerBeat > 0.0 ? splitEdgeSnapPixels / pixelsPerBeat : splitEdgeSnapMaxBeats;
    const auto clipRelativeLimit = juce::jmax(beatEpsilon, clip.lengthInBeats * 0.18);
    const auto tolerance = juce::jmax(beatEpsilon, juce::jmin(splitEdgeSnapMaxBeats, pixelTolerance, clipRelativeLimit));

    if (std::abs(splitBeat - clip.startBeat) <= tolerance)
        return clip.startBeat;

    if (std::abs(splitBeat - clipEnd) <= tolerance)
        return clipEnd;

    return splitBeat;
}

double ArrangementTimelineComponent::getSplitSnapStepInBeats() const noexcept
{
    switch (splitSnapMode)
    {
        case SplitSnapMode::bar:         return static_cast<double>(juce::jmax(1, project.getNumerator()));
        case SplitSnapMode::beat:        return 1.0;
        case SplitSnapMode::halfBeat:    return 0.5;
        case SplitSnapMode::quarterBeat: return 0.25;
        case SplitSnapMode::free:        return 0.0;
        case SplitSnapMode::smart:
        default:
            if (pixelsPerBeat >= 220.0) return 0.25;
            if (pixelsPerBeat >= 140.0) return 0.5;
            if (pixelsPerBeat >= 45.0)  return 1.0;
            return static_cast<double>(juce::jmax(1, project.getNumerator()));
    }
}

juce::String ArrangementTimelineComponent::getSplitSnapName() const
{
    switch (splitSnapMode)
    {
        case SplitSnapMode::smart:       return "Smart";
        case SplitSnapMode::bar:         return "Bar";
        case SplitSnapMode::beat:        return "Beat";
        case SplitSnapMode::halfBeat:    return "1/2";
        case SplitSnapMode::quarterBeat: return "1/4";
        case SplitSnapMode::free:        return "Free";
    }

    return "Smart";
}

double ArrangementTimelineComponent::snapSplitBeatForClip(double splitBeat, const TimelineClip& clip) const noexcept
{
    const auto edgeBeat = snapSplitBeatToClipEdge(splitBeat, clip);
    if (std::abs(edgeBeat - splitBeat) > beatEpsilon)
        return edgeBeat;

    const auto step = getSplitSnapStepInBeats();
    if (step <= beatEpsilon)
        return splitBeat;

    const auto snappedBeat = std::round(splitBeat / step) * step;
    const auto clipEnd = clip.startBeat + clip.lengthInBeats;
    if (snappedBeat <= clip.startBeat + beatEpsilon || snappedBeat >= clipEnd - beatEpsilon)
        return splitBeat;

    return snappedBeat;
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

void ArrangementTimelineComponent::splitSelectionAtFocusedBeat()
{
    if (! focusedSplitBeat.has_value())
    {
        splitSelectionAtPlayhead();
        return;
    }

    const auto rawSplitBeat = *focusedSplitBeat;
    auto& tracks = project.getTracks();
    std::vector<SelectedClip> targets = selectedClips;

    if (targets.empty())
    {
        splitSelectionAtPlayhead();
        return;
    }

    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b)
    {
        if (a.trackIndex != b.trackIndex) return a.trackIndex > b.trackIndex;
        return a.clipIndex > b.clipIndex;
    });

    bool didSplit = false;
    bool captured = false;
    bool snappedToExistingEdge = false;
    for (const auto& target : targets)
    {
        if (target.trackIndex < 0 || target.trackIndex >= static_cast<int>(tracks.size()))
            continue;

        const auto& clips = tracks[static_cast<std::size_t>(target.trackIndex)].clips;
        if (target.clipIndex < 0 || target.clipIndex >= static_cast<int>(clips.size()))
            continue;

        const auto& clip = clips[static_cast<std::size_t>(target.clipIndex)];
        const auto splitBeat = snapSplitBeatForClip(rawSplitBeat, clip);
        if (splitBeat <= clip.startBeat + beatEpsilon || splitBeat >= clip.startBeat + clip.lengthInBeats - beatEpsilon)
        {
            if (std::abs(splitBeat - clip.startBeat) <= beatEpsilon
                || std::abs(splitBeat - (clip.startBeat + clip.lengthInBeats)) <= beatEpsilon)
                snappedToExistingEdge = true;
            continue;
        }

        if (! captured)
        {
            pushUndoSnapshot();
            captured = true;
        }

        if (splitClipAtBeat(target.trackIndex, target.clipIndex, splitBeat))
            didSplit = true;
    }

    if (! didSplit && ! snappedToExistingEdge)
    {
        splitSelectionAtPlayhead();
        return;
    }

    if (! didSplit)
    {
        focusedSplitBeat.reset();
        knifePreviewBeat.reset();
        repaint();
        return;
    }

    focusedSplitBeat.reset();
    knifePreviewBeat.reset();
    setSingleSelection(std::nullopt);
    notifyClipSelectionChanged();
    repaint();
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
    focusedSplitBeat.reset();
    notifyClipSelectionChanged();
    hoverClip.reset();
    knifePreviewBeat.reset();
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
                || std::abs(currentClip.gainDb - snapshotClip.gainDb) > 0.001
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
    browserDropSnapBeat.reset();
    browserDropPreviewSourcePath = {};
    browserDropCreatesNewTrack = false;
    browserAppendActive = false;   // the open lane animates closed via the timer
    repaint();
}

void ArrangementTimelineComponent::clearExternalFileDropPreview()
{
    if (externalFileDropPreviews.empty())
        return;

    externalFileDropPreviews.clear();
    repaint();
}

void ArrangementTimelineComponent::updateExternalFileDropPreview(const juce::StringArray& files, juce::Point<int> position)
{
    clearBrowserDropPreview();
    externalFileDropPreviews.clear();

    juce::StringArray audioFiles;
    for (const auto& f : files)
    {
        const juce::File file(f);
        if (file.existsAsFile() && file.hasFileExtension(kAudioFileExtensions))
            audioFiles.add(file.getFullPathName());
    }

    if (audioFiles.isEmpty())
    {
        repaint();
        return;
    }

    const auto visibleTracksArea = getVisibleTrackAreaBounds(*this);
    auto visibleGridArea = visibleTracksArea;
    visibleGridArea.removeFromLeft(trackHeaderWidth);
    if (visibleGridArea.isEmpty())
    {
        repaint();
        return;
    }

    const auto trackCount = static_cast<int>(project.getTracks().size());
    const auto startBeat = snapBeatValue(xToBeatPosition(position.x));
    const auto startX = beatToX(startBeat, visibleGridArea);
    const auto x = juce::roundToInt(startX);

    const bool multiple = audioFiles.size() > 1;
    int targetTrackIndex = trackIndexFromY(position.y);
    bool createNewTrack = multiple
        || targetTrackIndex < 0
        || targetTrackIndex >= trackCount
        || (targetTrackIndex >= 0 && project.getTracks()[static_cast<std::size_t>(targetTrackIndex)].isMidiTrack);

    const auto maxPreviewCount = juce::jmin(audioFiles.size(), 8);
    for (int i = 0; i < maxPreviewCount; ++i)
    {
        const juce::File file(audioFiles[i]);
        getOrComputePeaks(file.getFullPathName());
        const auto analysis = cachedAnalyzeImportedAudioClip(file, project.getTempoBpm(), project.getNumerator(), 4.0);
        const auto minPreviewLengthBeats = analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2 && analysis.detectedBars == 0
            ? minimumOneShotClipLengthInBeats
            : minimumClipLengthInBeats;
        const auto previewLengthBeats = juce::jmax(minPreviewLengthBeats, analysis.clipLengthInBeats);
        const auto endX = beatToX(startBeat + previewLengthBeats, visibleGridArea);
        const auto clipW = juce::jmax(12, juce::roundToInt(endX - startX));
        const auto colour = theme::tracks::colourForIndex(createNewTrack ? trackCount + i : targetTrackIndex);
        juce::Rectangle<int> lane;

        if (createNewTrack)
        {
            const auto laneH = juce::jmax(32, defaultLaneHeight);
            const auto laneTop = trackCount > 0
                ? getTrackLaneBounds(trackCount - 1).getBottom() + i * laneH
                : visibleTracksArea.getY() + i * laneH;
            lane = juce::Rectangle<int>(visibleGridArea.getX(), laneTop, visibleGridArea.getWidth(), laneH);
        }
        else
        {
            lane = getTrackLaneBounds(targetTrackIndex);
            lane.removeFromLeft(trackHeaderWidth);
        }

        auto clipBounds = juce::Rectangle<int>(x,
                                               lane.getY() + 5,
                                               clipW,
                                               juce::jmax(18, lane.getHeight() - 10))
                              .getIntersection(visibleGridArea.expanded(0, defaultLaneHeight * maxPreviewCount));
        if (clipBounds.isEmpty())
            continue;

        externalFileDropPreviews.push_back(FileDropPreview {
            clipBounds,
            file.getFileNameWithoutExtension(),
            file.getFullPathName(),
            colour,
            createNewTrack
        });
    }

    if (audioFiles.size() > maxPreviewCount && ! externalFileDropPreviews.empty())
    {
        auto& last = externalFileDropPreviews.back();
        last.label = last.label + "  +" + juce::String(audioFiles.size() - maxPreviewCount);
    }

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
        tracksBounds.removeFromTop(timelineTopChromeHeight);
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
    getOrComputePeaks(sourceFile.getFullPathName());
    const auto analysis = cachedAnalyzeImportedAudioClip(sourceFile, project.getTempoBpm(), project.getNumerator(), fallbackLengthBeats);

    const auto previewDouble = [&](const char* prop, double fb)
    {
        return payload->hasProperty(prop) ? static_cast<double>(payload->getProperty(prop)) : fb;
    };
    const auto sStart = juce::jlimit(0.0, 0.999, previewDouble("sampleStartRatio", 0.0));
    const auto sEnd = juce::jlimit(sStart + 0.001, 1.0, previewDouble("sampleEndRatio", 1.0));
    const auto trimSpan = juce::jmax(0.001, sEnd - sStart);
    const auto sourceLen = payload->hasProperty("sourceLengthBeats")
        ? juce::jmax(analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2 && analysis.detectedBars == 0
                         ? minimumOneShotClipLengthInBeats
                         : minimumClipLengthInBeats,
                     previewDouble("sourceLengthBeats", analysis.clipLengthInBeats))
        : analysis.clipLengthInBeats;
    const auto minPreviewLengthBeats = analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2 && analysis.detectedBars == 0
        ? minimumOneShotClipLengthInBeats
        : minimumClipLengthInBeats;
    const auto lengthBeats = juce::jmax(minPreviewLengthBeats, sourceLen * trimSpan);
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
    browserDropPreviewSourcePath = sourceFile.getFullPathName();
    browserDropCreatesNewTrack = createNewTrack;
    browserDropSnapBeat = startBeat;
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
