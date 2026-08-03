#include "ArrangementTimelineComponent.h"

#include "OrionTheme.h"
#include "../Audio/ChordDetector.h"
#include "../Audio/StemSeparator.h"
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

// Popover to choose which stems to extract (like Logic's Stem Splitter). Fires `onSeparate` with the
// selected stem names (Demucs computes all 6 in one pass; we only lay out the chosen ones).
class StemPickerCallout final : public juce::Component
{
public:
    explicit StemPickerCallout(std::function<void(std::vector<juce::String>)> onSeparate)
        : go(std::move(onSeparate))
    {
        static const std::array<std::pair<const char*, const char*>, 6> items {{
            { "Vocals", "vocals" }, { "Drums", "drums" }, { "Bass", "bass" },
            { "Guitar", "guitar" }, { "Piano", "piano" }, { "Other", "other" } }};

        title.setText("Separate stems", juce::dontSendNotification);
        title.setFont(juce::FontOptions("Avenir Next", 14.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, orion::theme::text::primary);
        addAndMakeVisible(title);

        for (const auto& it : items)
        {
            auto* t = toggles.add(new juce::ToggleButton(it.first));
            t->setToggleState(true, juce::dontSendNotification);
            t->setColour(juce::ToggleButton::textColourId, orion::theme::text::primary);
            addAndMakeVisible(t);
            names.add(it.second);
        }

        separateBtn.setButtonText("Separate");
        separateBtn.onClick = [this]
        {
            std::vector<juce::String> sel;
            for (int i = 0; i < toggles.size(); ++i)
                if (toggles[i]->getToggleState()) sel.push_back(names[i]);
            auto cb = go;                                  // copy before dismiss deletes us
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
            if (! sel.empty() && cb) cb(sel);
        };
        cancelBtn.setButtonText("Cancel");
        cancelBtn.onClick = [this] { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
        addAndMakeVisible(separateBtn);
        addAndMakeVisible(cancelBtn);

        setSize(226, 22 + 6 * 28 + 16 + 34 + 24);
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(orion::theme::surface::panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), orion::theme::metrics::panelRadius);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14, 12);
        title.setBounds(r.removeFromTop(22));
        r.removeFromTop(6);
        for (auto* t : toggles) t->setBounds(r.removeFromTop(28));
        r.removeFromTop(10);
        auto row = r.removeFromTop(30);
        cancelBtn.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(3, 0));
        separateBtn.setBounds(row.reduced(3, 0));
    }

private:
    juce::Label title;
    juce::OwnedArray<juce::ToggleButton> toggles;
    juce::StringArray names;
    juce::TextButton separateBtn, cancelBtn;
    std::function<void(std::vector<juce::String>)> go;
};

const auto timelineBackground = juce::Colour(0xff18212a); // lighter DAW canvas: enough midtone for a calm, readable grid
const auto barGridColour = juce::Colour(0xff8794a3);        // brighter so grid lines are clearly noticeable
const auto beatGridColour = juce::Colour(0xff6f7c8b);
const auto subdivisionGridColour = juce::Colour(0xff586472);
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
constexpr auto editToolbarHeight = 40;
constexpr auto timelineRulerHeight = 30;
constexpr auto timelineTopChromeHeight = editToolbarHeight + timelineRulerHeight;
constexpr auto chordLaneHeight = 46;   // arrangement chord lane, shown below the ruler when enabled
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

double chooseStretchSnapStep(double pixelsPerBeat)
{
    // Live-style adaptive editing grid: retain fine resolution when it is readable,
    // but make edge drags jump between distinct on-screen cells at wider zoom levels.
    // A fixed 1/16-beat step looked like pixel-by-pixel crawling once the timeline
    // was zoomed out.
    static constexpr std::array<double, 8> steps {
        1.0 / 32.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0,
        1.0 / 2.0, 1.0, 2.0, 4.0
    };

    constexpr double minimumCellWidthPixels = 16.0;
    for (const auto step : steps)
        if (pixelsPerBeat * step >= minimumCellWidthPixels)
            return step;

    return steps.back();
}

juce::Rectangle<int> getTimelineContentBounds(const juce::Component& component)
{
    auto bounds = component.getLocalBounds();
    bounds.removeFromTop(4);
    return bounds;
}

// Total top chrome (edit toolbar + ruler + optional chord lane). The chord lane, when shown, sits
// between the ruler and the tracks, so every "tracks start" offset must account for it.
int timelineTopChromeFor(const juce::Component& component)
{
    const auto* atc = dynamic_cast<const orion::ArrangementTimelineComponent*>(&component);
    return timelineTopChromeHeight + (atc != nullptr && atc->isChordLaneShown() ? chordLaneHeight : 0);
}

juce::Rectangle<int> getVisibleTrackAreaBounds(const juce::Component& component)
{
    auto bounds = getTimelineContentBounds(component);
    bounds.removeFromTop(timelineTopChromeFor(component));
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
    // Mouse drags repaint on demand; 60 Hz is enough for transport/zoom animation and
    // avoids waking the software renderer twice per display frame.
    startTimerHz(60);
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
    redoStack.push_back(TimelineSnapshot { project.getTracks(), selectedClip, project.getChordTrack() });
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
    undoStack.push_back(TimelineSnapshot { project.getTracks(), selectedClip, project.getChordTrack() });
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
    // Chord lane sits between the ruler and the tracks when enabled.
    const bool chordLaneOn = project.isChordLaneVisible();
    auto chordLaneArea = chordLaneOn ? bounds.removeFromTop(chordLaneHeight) : juce::Rectangle<int>();
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
    g.fillRoundedRectangle(getLocalBounds().toFloat(), orion::theme::metrics::panelRadius);

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
    g.fillRoundedRectangle(addTrackButton, orion::theme::metrics::controlRadius);
    g.setColour(theme::accent::activeCoral.withAlpha(0.62f));
    g.drawRoundedRectangle(addTrackButton.reduced(0.5f), orion::theme::metrics::controlRadius, 1.2f);
    auto plus = addTrackButton.reduced(8.0f);
    g.setColour(theme::text::primary.withAlpha(0.90f));
    g.drawLine(plus.getCentreX(), plus.getY(), plus.getCentreX(), plus.getBottom(), 2.0f);
    g.drawLine(plus.getX(), plus.getCentreY(), plus.getRight(), plus.getCentreY(), 2.0f);

    // Chord-lane toggle, immediately left of the "+".
    const auto chordBtn = getChordLaneToggleBounds().toFloat();
    const bool chordOn = project.isChordLaneVisible();
    g.setColour(chordOn ? theme::accent::activeCoral.withAlpha(0.85f) : theme::surface::primary.withAlpha(0.34f));
    g.fillRoundedRectangle(chordBtn, orion::theme::metrics::controlRadius);
    g.setColour(chordOn ? theme::accent::activeCoral : juce::Colours::white.withAlpha(0.18f));
    g.drawRoundedRectangle(chordBtn.reduced(0.5f), orion::theme::metrics::controlRadius, 1.2f);
    g.setColour(chordOn ? juce::Colours::black.withAlpha(0.9f) : theme::text::primary.withAlpha(0.9f));
    g.setFont(juce::FontOptions("Avenir Next", 13.0f, juce::Font::bold));
    g.drawText("Ch", chordBtn, juce::Justification::centred);

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
    // Draw the SAME grid that snapping uses, so you always snap to the lines you see (Ableton).
    const auto subdivisionStepBeats = currentGridBeats();
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
        // Beat/sub-beat lines — the snap grid itself. Hidden only when the lines would be too dense
        // to read (a very fine fixed grid at low zoom); the adaptive grid keeps them comfortable.
        if (subdivisionStepBeats <= 0.0 || pixelsPerBeat * subdivisionStepBeats < 5.0)
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
            // Ableton-visible grid: clearly-readable subdivision lines (its Customization tab even
            // exposes grid-line opacity). These are the snap lines, so they must actually be seen.
            const auto alpha = (isWholeBeat ? 0.34f
                                : isHalfBeat ? 0.24f
                                : isQuarterBeat ? 0.18f
                                : 0.14f) * intensity;

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
        g.setFont(juce::FontOptions("Avenir Next", 13.0f, juce::Font::bold));
        g.drawText(juce::String(barNumber), static_cast<int>(x) + 6, markerLane.getY(), 48, markerLane.getHeight(), juce::Justification::centredLeft);
    }

    // Grid-step readout, pinned to the right of the ruler (Ableton shows the current grid spacing here).
    {
        const auto label = "Grid " + gridStepName() + (gridAdaptive ? "" : juce::String("  (fixed)"));
        g.setFont(juce::FontOptions("Avenir Next", 11.0f, juce::Font::bold));
        const int w = juce::jmax(48, g.getCurrentFont().getStringWidth(label) + 16);
        juce::Rectangle<int> pill(gridArea.getRight() - w - 8, markerLane.getY() + 3, w, markerLane.getHeight() - 6);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.fillRoundedRectangle(pill.toFloat(), 4.0f);
        g.setColour(gridSnapEnabled ? markerColour : juce::Colours::grey.withAlpha(0.8f));
        g.drawText(label, pill, juce::Justification::centred);
    }
    g.restoreState();

    const auto& tracks = project.getTracks();
    // Dragging a clip edge can generate mouse updates faster than a dense arrangement can redraw its
    // per-pixel waveforms. We used to HIDE every waveform while trimming — which looked like all the
    // audio content vanished the moment you touched a clip edge. Instead keep them visible (you need
    // to see the wave to trim!) and just halve the column resolution during the drag.
    const bool isClipEdgeDragging = dragState.has_value()
        && (dragState->mode == DragMode::resizeLeft || dragState->mode == DragMode::resizeRight
            || dragState->mode == DragMode::stretchLeft || dragState->mode == DragMode::stretchRight);

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
        const auto cardR = theme::metrics::controlRadius;

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
        g.setFont(juce::FontOptions("Avenir Next", 14.0f, juce::Font::bold));
        g.drawText(juce::String(trackIndex + 1), layout.number, juce::Justification::centred);

        g.setColour(textColour.withAlpha(0.94f));
        g.setFont(juce::FontOptions("Avenir Next", 13.5f, juce::Font::bold));
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
            g.fillRoundedRectangle(buttonBounds.toFloat(), theme::metrics::controlRadius);
            g.setColour(juce::Colours::white.withAlpha(active ? 0.98f : 0.84f));
            g.setFont(juce::FontOptions("Avenir Next", 13.0f, juce::Font::bold));
            g.drawText(buttonText, buttonBounds, juce::Justification::centred);
        }

        // Instrument button (MIDI tracks only): a tiny piano-keys glyph. The background is
        // tinted in the track colour when an instrument is loaded so it reads as "active".
        if (! layout.instrumentButton.isEmpty())
        {
            const auto hasInstrument = tracks[trackArrayIndex].instrumentPluginId.isNotEmpty();
            auto box = layout.instrumentButton.toFloat();
            g.setColour(hasInstrument ? trackColour.withAlpha(0.74f) : juce::Colours::black.withAlpha(0.42f));
            g.fillRoundedRectangle(box, theme::metrics::controlRadius);

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
            // The graphics clip rejects invisible pixels, but it does not stop the CPU work
            // below: waveform aggregation used to still walk every column of clips that were
            // entirely off-screen. Skip them before building gradients, paths or wave peaks.
            if (! clipBoundsInt.intersects(visibleGridArea))
                continue;
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
                g.fillRoundedRectangle(clipBounds, orion::theme::metrics::controlRadius);
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

                    // Long clips can span thousands of pixels outside the viewport. Render only
                    // the visible columns, while retaining the same source-to-screen mapping.
                    const auto visibleWaveBounds = clipBodyBounds.getIntersection(visibleGridArea);
                    const auto firstWavePixel = juce::jlimit(0, waveformWidth,
                        visibleWaveBounds.getX() - bodyX);
                    const auto lastWavePixel = juce::jlimit(firstWavePixel, waveformWidth,
                        visibleWaveBounds.getRight() - bodyX);

                    // Half-resolution columns while trimming: keeps the edge glued to the pointer on a
                    // dense arrangement without hiding the waveform (each column is drawn twice as wide).
                    const int step = isClipEdgeDragging ? 2 : 1;
                    for (int px = firstWavePixel; px < lastWavePixel; px += step)
                    {
                        const auto bStart = static_cast<int>(bucketStart + static_cast<double>(px) * bucketSpan / waveformWidth);
                        const auto bEnd   = static_cast<int>(bucketStart + static_cast<double>(px + step) * bucketSpan / waveformWidth);
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
                            g.drawLine(x, top, x, bottom, static_cast<float>(step));
                        else
                            g.fillRect(x, centerY - 0.5f, static_cast<float>(step), 1.0f);
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
            g.drawRoundedRectangle(clipBounds.reduced(0.5f, 0.5f), theme::metrics::controlRadius, 1.0f);

            if (isSelected)
            {
                g.setColour(juce::Colours::white.withAlpha(0.92f));
                g.drawRoundedRectangle(clipBounds.reduced(1.2f, 1.2f), theme::metrics::controlRadius, 2.2f);
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
                clipShape.addRoundedRectangle(clipBounds, theme::metrics::controlRadius);
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
                clipShape.addRoundedRectangle(clipBounds, theme::metrics::controlRadius);
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
                g.setFont(juce::FontOptions("Avenir Next", adaptiveSize, juce::Font::bold));

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
            g.fillRoundedRectangle(clipBounds, orion::theme::metrics::controlRadius);
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
            // Full-height snap guide across the whole arrangement (Live-style). It only clears
            // cleanly because the resize drag repaints a full-height strip (see mouseDrag); a
            // plain per-clip repaint left a ghost of the line below the clip at the drag start.
            if (! isStretch)
            {
                g.setColour(juce::Colours::black.withAlpha(0.45f));
                g.drawLine(gx + 1.0f, top, gx + 1.0f, bottom, 2.6f);
                g.setColour(guideColour.withAlpha(0.96f));
                g.drawLine(gx, top, gx, bottom, 1.5f);
            }

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

    // Automation envelopes over the track lanes (under the playhead).
    {
        g.saveState();
        g.reduceClipRegion(visibleGridArea);
        drawAutomationOverlay(g);
        g.restoreState();
    }

    // The parameter selector chips live in the track HEADER, so they draw in a separate pass that is
    // NOT clipped to the grid (the block above excludes the header).
    if (automationMode)
    {
        g.saveState();
        g.reduceClipRegion(getVisibleTrackAreaBounds(*this));
        drawAutomationHeaderChips(g);
        g.restoreState();
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

    if (project.isChordLaneVisible())
        drawChordLane(g);

    // Alt-drag chord marquee: show the actual drag box (spanning tracks) for clear feedback.
    if (chordMarqueeStart.has_value() && ! chordMarqueeRect.isEmpty())
    {
        g.setColour(theme::cool::cyan.withAlpha(0.10f));
        g.fillRect(chordMarqueeRect);
        g.setColour(theme::cool::cyan.withAlpha(0.55f));
        g.drawRect(chordMarqueeRect, 1);
    }

    // Dragging the chord progression onto a track → highlight the drop target.
    if (chordDragToTrack)
    {
        auto ghost = chordDropGhost;
        if (! ghost.isEmpty())
            ghost.removeFromLeft(trackHeaderWidth);
        if (! ghost.isEmpty())
        {
            g.setColour(theme::cool::cyan.withAlpha(0.16f));
            g.fillRoundedRectangle(ghost.toFloat(), 4.0f);
            g.setColour(theme::cool::cyan.withAlpha(0.75f));
            g.drawRoundedRectangle(ghost.toFloat().reduced(0.5f), 4.0f, 1.5f);
            g.setColour(theme::cool::cyan);
            g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
            g.drawText("Drop: chords \xE2\x86\x92 MIDI", ghost.reduced(8, 4),
                       juce::Justification::topLeft, false);
        }
    }

    // Stem-separation status banner (long offline op) — centred near the top of the timeline.
    if (stemRunning || stemStatus.isNotEmpty())
    {
        auto area = getTimelineContentBounds(*this);
        juce::Rectangle<int> banner(area.getCentreX() - 170, area.getY() + 54, 340, 46);
        g.setColour(theme::surface::panel.withAlpha(0.96f));
        g.fillRoundedRectangle(banner.toFloat(), 8.0f);
        g.setColour(theme::cool::cyan.withAlpha(0.55f));
        g.drawRoundedRectangle(banner.toFloat().reduced(0.5f), 8.0f, 1.0f);

        auto inner = banner.reduced(12, 8);
        g.setColour(theme::text::primary);
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        const auto label = stemRunning
            ? ("Separating stems\xE2\x80\xA6 " + juce::String(juce::roundToInt(stemProgress * 100.0f)) + "%")
            : stemStatus;
        g.drawText(label, inner.removeFromTop(16), juce::Justification::centredLeft, true);

        if (stemRunning)
        {
            inner.removeFromTop(4);
            auto bar = inner.removeFromTop(6);
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.fillRoundedRectangle(bar.toFloat(), 3.0f);
            g.setColour(theme::cool::cyan);
            g.fillRoundedRectangle(bar.withWidth(juce::jmax(2, juce::roundToInt(bar.getWidth() * stemProgress))).toFloat(), 3.0f);
        }
    }

    paintToolPalette(g);
    drawRemoteCursors(g);
}

void ArrangementTimelineComponent::setRemoteCursors(std::vector<RemoteCursor> cursors)
{
    // Cheap identity check: cursors move constantly, and repainting the whole arrangement on every
    // presence packet would be wasteful when nothing actually moved.
    const auto same = cursors.size() == remoteCursors.size()
                   && std::equal(cursors.begin(), cursors.end(), remoteCursors.begin(),
                                 [](const RemoteCursor& a, const RemoteCursor& b)
                                 {
                                     return a.name == b.name
                                         && std::abs(a.beat - b.beat) < 1.0e-6
                                         && std::abs(a.contentY - b.contentY) < 0.5;
                                 });
    if (same)
        return;

    remoteCursors = std::move(cursors);
    repaint();
}

bool ArrangementTimelineComponent::pointToProjectPosition(juce::Point<int> point,
                                                          double& beatOut,
                                                          double& contentYOut) const
{
    if (point.x < trackHeaderWidth || ! getLocalBounds().contains(point))
        return false;

    // Anywhere over the grid counts, not just on top of a track. Requiring a lane hit meant a
    // collaborator had no cursor at all on an empty project, and it blinked out whenever they moved
    // through the space below the last track — Figma shows you the pointer anywhere on the canvas.
    beatOut = xToBeatPosition(point.x);

    // Content space = screen position minus the track area's origin, with the local scroll added
    // back in. The peer reverses it with their own scroll, so the cursor tracks the same row.
    const auto area = getVisibleTrackAreaBounds(*this);
    contentYOut = static_cast<double>(point.y - area.getY()) + scrollY;
    return true;
}

void ArrangementTimelineComponent::drawRemoteCursors(juce::Graphics& g)
{
    if (remoteCursors.empty())
        return;

    auto gridArea = getVisibleTrackAreaBounds(*this);
    // beatToX measures from the lane area's left edge, which sits AFTER the track headers — the same
    // inset getClipBounds applies. Without removing it here the cursor lands ~trackHeaderWidth px too
    // far left (it must map beats identically to xToBeatPosition, which the sender uses).
    auto beatArea = gridArea;
    beatArea.removeFromLeft(trackHeaderWidth);

    for (const auto& cursor : remoteCursors)
    {
        if (gridArea.isEmpty())
            continue;

        const auto x = beatToX(cursor.beat, beatArea);
        if (x < static_cast<float>(trackHeaderWidth) || x > static_cast<float>(getWidth()))
            continue;

        // Undo our own scroll to place their content-space position on our screen.
        const auto y = static_cast<float>(gridArea.getY() + cursor.contentY - scrollY);
        if (y < static_cast<float>(gridArea.getY() - 4) || y > static_cast<float>(gridArea.getBottom()))
            continue;

        // Arrow head.
        juce::Path arrow;
        arrow.startNewSubPath(x, y);
        arrow.lineTo(x, y + 14.0f);
        arrow.lineTo(x + 3.8f, y + 10.4f);
        arrow.lineTo(x + 8.5f, y + 15.5f);
        arrow.lineTo(x + 11.0f, y + 13.2f);
        arrow.lineTo(x + 6.4f, y + 8.4f);
        arrow.lineTo(x + 11.0f, y + 7.0f);
        arrow.closeSubPath();

        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillPath(arrow, juce::AffineTransform::translation(1.0f, 1.5f));
        g.setColour(cursor.colour);
        g.fillPath(arrow);

        // Name tag beside it.
        if (cursor.name.isNotEmpty())
        {
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            const auto textWidth = juce::jmin(140.0f, static_cast<float>(g.getCurrentFont().getStringWidth(cursor.name)) + 12.0f);
            juce::Rectangle<float> tag(x + 12.0f, y + 12.0f, textWidth, 16.0f);
            g.setColour(cursor.colour.withAlpha(0.92f));
            g.fillRoundedRectangle(tag, 4.0f);
            g.setColour(cursor.colour.contrasting(0.85f));
            g.drawText(cursor.name, tag, juce::Justification::centred, false);
        }
    }
}

//============================================================================ chord lane
bool ArrangementTimelineComponent::isChordLaneShown() const noexcept
{
    return project.isChordLaneVisible();
}

void ArrangementTimelineComponent::setChordLaneShown(bool shown)
{
    if (project.isChordLaneVisible() == shown)
        return;
    project.setChordLaneVisible(shown);
    if (! shown && arrChordSelector != nullptr)
        arrChordSelector->setVisible(false);
    resized();
    repaint();
}

juce::Rectangle<int> ArrangementTimelineComponent::getChordLaneBounds() const noexcept
{
    if (! project.isChordLaneVisible())
        return {};
    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(editToolbarHeight);
    bounds.removeFromTop(timelineRulerHeight);
    return bounds.removeFromTop(chordLaneHeight);
}

juce::Rectangle<int> ArrangementTimelineComponent::getChordLaneGridArea() const noexcept
{
    auto lane = getChordLaneBounds();
    lane.removeFromLeft(trackHeaderWidth);
    return lane;
}

juce::Rectangle<int> ArrangementTimelineComponent::getChordOctaveUpBounds() const noexcept
{
    const auto lane = getChordLaneBounds();
    if (lane.isEmpty()) return {};
    const juce::Rectangle<int> box(lane.getX() + 108, lane.getY() + 5, 20, lane.getHeight() - 10);
    return box.withHeight(box.getHeight() / 2);
}

juce::Rectangle<int> ArrangementTimelineComponent::getChordOctaveDownBounds() const noexcept
{
    const auto up = getChordOctaveUpBounds();
    if (up.isEmpty()) return {};
    return up.withY(up.getBottom());
}

int ArrangementTimelineComponent::chordEventAtPoint(juce::Point<int> position) const
{
    const auto grid = getChordLaneGridArea();
    if (grid.isEmpty() || ! grid.contains(position))   // grid only — never the track-header gutter
        return -1;
    const auto& chords = project.getChordTrack();
    for (int i = static_cast<int>(chords.size()) - 1; i >= 0; --i)
    {
        const auto x0 = beatToX(chords[static_cast<std::size_t>(i)].startBeat, grid);
        const auto x1 = beatToX(chords[static_cast<std::size_t>(i)].startBeat
                                    + chords[static_cast<std::size_t>(i)].lengthInBeats, grid);
        if (position.x >= static_cast<int>(x0) && position.x <= static_cast<int>(x1))
            return i;
    }
    return -1;
}

void ArrangementTimelineComponent::addChordAtBeat(double beat)
{
    const auto beatsPerBar = static_cast<double>(juce::jmax(1, project.getNumerator()));
    const auto snapped = juce::jmax(0.0, std::floor(snapBeatValue(beat) / beatsPerBar) * beatsPerBar);

    const std::array<int, 7> pattern = project.isKeyMinor()
        ? std::array<int, 7>{ 0, 2, 3, 5, 7, 8, 10 }
        : std::array<int, 7>{ 0, 2, 4, 5, 7, 9, 11 };
    const auto tonic = orion::chords::diatonicTriads(project.getKeyRoot(), pattern)[0];

    pushUndoSnapshot();
    {
        const juce::ScopedLock sl(project.getAudioEditLock());
        project.getChordTrack().push_back(ChordEvent { snapped, beatsPerBar, tonic });
    }
    if (onChordLaneChanged) onChordLaneChanged();
    if (onChordAudition)
        onChordAudition(orion::chords::pitchesInKey(tonic, project.getKeyRoot(), pattern, 48 + project.getChordLaneOctave() * 12));
    repaint();
}

void ArrangementTimelineComponent::openChordEditorFor(int index)
{
    auto& chords = project.getChordTrack();
    if (index < 0 || index >= static_cast<int>(chords.size()))
        return;
    editingChordIndex = index;

    // Only place the picker when it's first opened; switching between blocks while it's open updates
    // its content in place (it shouldn't jump around under each block you click).
    const bool alreadyOpen = arrChordSelector != nullptr && arrChordSelector->isVisible();

    const std::array<int, 7> pattern = project.isKeyMinor()
        ? std::array<int, 7>{ 0, 2, 3, 5, 7, 8, 10 }
        : std::array<int, 7>{ 0, 2, 4, 5, 7, 9, 11 };
    const juce::String keyName = juce::String(orion::chords::rootName(project.getKeyRoot()))
                               + (project.isKeyMinor() ? " Minor" : " Major");

    if (arrChordSelector == nullptr)
    {
        arrChordSelector = std::make_unique<ChordSelectorComponent>();
        addChildComponent(*arrChordSelector);
        arrChordSelector->onAudition = [this](const std::vector<int>& p) { if (onChordAudition) onChordAudition(p); };
        arrChordSelector->onChordChanged = [this](const orion::chords::ChordSpec& s)
        {
            const juce::ScopedLock sl(project.getAudioEditLock());
            auto& ct = project.getChordTrack();
            if (editingChordIndex >= 0 && editingChordIndex < static_cast<int>(ct.size()))
                ct[static_cast<std::size_t>(editingChordIndex)].spec = s;
            if (onChordLaneChanged) onChordLaneChanged();
            repaint();
        };
        arrChordSelector->onClose = [this] { if (arrChordSelector) arrChordSelector->setVisible(false); };
    }

    arrChordSelector->setProjectKey(project.getKeyRoot(), pattern, keyName);
    arrChordSelector->setChord(chords[static_cast<std::size_t>(index)].spec, false);

    if (! alreadyOpen)
    {
        const auto grid = getChordLaneGridArea();
        const auto blockX = static_cast<int>(beatToX(chords[static_cast<std::size_t>(index)].startBeat, grid));
        constexpr int w = 620, h = 400;
        const int x = juce::jlimit(getLocalBounds().getX() + 8,
                                   juce::jmax(getLocalBounds().getX() + 8, getWidth() - w - 8), blockX);
        const int y = juce::jmin(getChordLaneBounds().getBottom() + 6, getHeight() - h - 8);
        arrChordSelector->setBounds(x, juce::jmax(8, y), w, h);
    }
    arrChordSelector->setVisible(true);
    arrChordSelector->toFront(true);
}

void ArrangementTimelineComponent::updateChordMarqueeSelection()
{
    const auto grid = getChordLaneGridArea();
    if (grid.isEmpty())
        return;
    const auto left  = chordMarqueeRect.getX();
    const auto right = chordMarqueeRect.getRight();

    selectedChords.clear();
    const auto& chords = project.getChordTrack();
    for (int i = 0; i < static_cast<int>(chords.size()); ++i)
    {
        // Horizontal overlap is enough (the lane is a single thin row) — forgiving to select a range.
        const auto x0 = beatToX(chords[static_cast<std::size_t>(i)].startBeat, grid);
        const auto x1 = beatToX(chords[static_cast<std::size_t>(i)].startBeat
                                    + chords[static_cast<std::size_t>(i)].lengthInBeats, grid);
        if (x1 >= static_cast<float>(left) && x0 <= static_cast<float>(right))
            selectedChords.insert(i);
    }
}

bool ArrangementTimelineComponent::duplicateSelectedChords()
{
    if (selectedChords.empty())
        return false;

    pushUndoSnapshot();
    std::set<int> newSelection;
    {
        const juce::ScopedLock sl(project.getAudioEditLock());
        auto& ct = project.getChordTrack();

        // Duplicate the selected group directly after itself (extends the progression).
        double minStart = std::numeric_limits<double>::max(), maxEnd = 0.0;
        for (int i : selectedChords)
            if (i >= 0 && i < static_cast<int>(ct.size()))
            {
                minStart = juce::jmin(minStart, ct[static_cast<std::size_t>(i)].startBeat);
                maxEnd   = juce::jmax(maxEnd, ct[static_cast<std::size_t>(i)].startBeat + ct[static_cast<std::size_t>(i)].lengthInBeats);
            }
        const double shift = juce::jmax(0.0, maxEnd - minStart);

        std::vector<ChordEvent> copies;
        for (int i : selectedChords)
            if (i >= 0 && i < static_cast<int>(ct.size()))
            {
                auto c = ct[static_cast<std::size_t>(i)];
                c.startBeat += shift;
                copies.push_back(c);
            }
        for (auto& c : copies)
        {
            ct.push_back(c);
            newSelection.insert(static_cast<int>(ct.size()) - 1);
        }
    }
    selectedChords = newSelection;
    if (onChordLaneChanged) onChordLaneChanged();
    repaint();
    return true;
}

bool ArrangementTimelineComponent::deleteSelectedChords()
{
    if (selectedChords.empty())
        return false;

    pushUndoSnapshot();
    {
        const juce::ScopedLock sl(project.getAudioEditLock());
        auto& ct = project.getChordTrack();
        // Erase from the highest index down so earlier indices stay valid.
        for (auto it = selectedChords.rbegin(); it != selectedChords.rend(); ++it)
            if (*it >= 0 && *it < static_cast<int>(ct.size()))
                ct.erase(ct.begin() + *it);
    }
    selectedChords.clear();
    if (onChordLaneChanged) onChordLaneChanged();
    repaint();
    return true;
}

void ArrangementTimelineComponent::detectChordsForClip(const juce::File& file, double startBeat,
                                                       int numBars, int keyRoot, bool keyMinor,
                                                       double clipLengthBeats, double sampleStartRatio,
                                                       double sourceLengthBeats)
{
    if (! file.existsAsFile())
        return;

    // Source→timeline transform, IDENTICAL to the waveform drawing: a full-file ratio r maps to
    // beat = startBeat + (r - sampleStartRatio) * sourceLengthBeats. This is what makes chords land
    // exactly under the sample. sourceLengthBeats defaults to the clip length when not supplied.
    const double srcLenBeats = sourceLengthBeats > 0.0 ? sourceLengthBeats : clipLengthBeats;

    const double beatsPerBar = static_cast<double>(juce::jmax(1, project.getNumerator()));
    // detectedBars can be 0 right after a drop (deep analysis not done yet) — estimate from the
    // clip length so chord detection still fires (this was the "chords don't always appear" bug).
    if (numBars <= 0)
        numBars = juce::jmax(1, static_cast<int>(std::round(clipLengthBeats / beatsPerBar)));

    chordAnalysisRunning = true;   // drive the in-lane progress indicator
    if (! project.isChordLaneVisible()) project.setChordLaneVisible(true);
    resized();
    repaint();

    juce::Component::SafePointer<ArrangementTimelineComponent> safe(this);

    juce::Thread::launch([safe, file, startBeat, numBars, keyRoot, keyMinor, beatsPerBar, clipLengthBeats, sampleStartRatio, srcLenBeats]
    {
        orion::chorddetect::Options opts;
        opts.numBars = numBars;
        opts.beatsPerBar = static_cast<int>(beatsPerBar);
        opts.keyRoot = keyRoot;
        opts.keyMinor = keyMinor;
        opts.clipLengthBeats = clipLengthBeats;

        // Primary: Chordino change points as source-file ratios → mapped with the WAVEFORM transform.
        auto changes = orion::chorddetect::detectChordChanges(file, opts);
        // Fallback (helper unavailable): native per-beat segments.
        auto timedFallback = changes.empty() ? orion::chorddetect::detectTimedChords(file, opts)
                                             : std::vector<orion::chorddetect::TimedChord>{};

        juce::MessageManager::callAsync([safe, changes, timedFallback, startBeat, clipLengthBeats, sampleStartRatio, srcLenBeats]
        {
            if (safe == nullptr)
                return;
            safe->chordAnalysisRunning = false;   // analysis done → hide the progress indicator

            std::vector<orion::chorddetect::TimedChord> timed = timedFallback;
            if (! changes.empty())
            {
                // Same transform as the waveform: r → startBeat + (r - sampleStartRatio) * srcLenBeats.
                const double clipEndRel = clipLengthBeats;   // relative to startBeat
                const auto toRel = [&](double r) { return (r - sampleStartRatio) * srcLenBeats; };
                for (std::size_t i = 0; i < changes.size(); ++i)
                {
                    double s = juce::jlimit(0.0, clipEndRel, toRel(changes[i].sourceRatio));
                    double e = (i + 1 < changes.size()) ? juce::jlimit(0.0, clipEndRel, toRel(changes[i + 1].sourceRatio))
                                                        : clipEndRel;
                    if (e - s <= 1.0e-4) continue;
                    timed.push_back({ s, e - s, changes[i].spec });
                }
            }
            if (timed.empty())
            {
                safe->repaint();
                return;
            }

            const juce::ScopedLock sl(safe->project.getAudioEditLock());
            auto& ct = safe->project.getChordTrack();
            for (const auto& t : timed)
            {
                ChordEvent ev;
                ev.startBeat = startBeat + t.startBeat;
                ev.lengthInBeats = t.lengthInBeats;
                ev.spec = t.spec;
                ct.push_back(ev);
            }
            safe->project.setChordLaneVisible(true);
            if (safe->onChordLaneChanged) safe->onChordLaneChanged();
            safe->resized();
            safe->repaint();
        });
    });
}

void ArrangementTimelineComponent::drawChordLane(juce::Graphics& g)
{
    const auto lane = getChordLaneBounds();
    const auto grid = getChordLaneGridArea();
    if (lane.isEmpty())
        return;

    g.saveState();
    g.reduceClipRegion(lane);

    g.setColour(theme::surface::elevated.withAlpha(0.72f));
    g.fillRect(lane);
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawHorizontalLine(lane.getBottom() - 1, static_cast<float>(lane.getX()), static_cast<float>(lane.getRight()));

    // Header gutter label.
    auto label = lane.withWidth(trackHeaderWidth).reduced(10, 0);
    g.setColour(theme::text::primary.withAlpha(0.6f));
    g.setFont(juce::Font(11.0f, juce::Font::bold));
    g.drawText("CHORDS", label, juce::Justification::centredLeft);

    // Octave ▲▼ for chord playback/audition.
    const auto up = getChordOctaveUpBounds();
    const auto dn = getChordOctaveDownBounds();
    const int oct = project.getChordLaneOctave();
    auto drawArrow = [&](juce::Rectangle<int> r, bool pointUp)
    {
        g.setColour(theme::surface::elevated.withAlpha(0.9f));
        g.fillRoundedRectangle(r.toFloat().reduced(1.0f), 3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        juce::Path p;
        const auto c = r.toFloat().reduced(6.0f, 4.0f);
        if (pointUp) { p.startNewSubPath(c.getX(), c.getBottom()); p.lineTo(c.getCentreX(), c.getY()); p.lineTo(c.getRight(), c.getBottom()); }
        else         { p.startNewSubPath(c.getX(), c.getY()); p.lineTo(c.getCentreX(), c.getBottom()); p.lineTo(c.getRight(), c.getY()); }
        g.strokePath(p, juce::PathStrokeType(1.4f));
    };
    drawArrow(up, true);
    drawArrow(dn, false);
    g.setColour(theme::text::primary.withAlpha(oct == 0 ? 0.45f : 0.85f));
    g.setFont(juce::Font(10.5f, juce::Font::bold));
    g.drawText((oct > 0 ? "+" : "") + juce::String(oct), juce::Rectangle<int>(up.getRight() + 2, lane.getY(), 24, lane.getHeight()), juce::Justification::centredLeft);

    // Clip the blocks to the grid so they never bleed left into the track-header gutter when scrolled.
    g.reduceClipRegion(grid);
    const auto& chords = project.getChordTrack();
    for (int i = 0; i < static_cast<int>(chords.size()); ++i)
    {
        const auto& ev = chords[static_cast<std::size_t>(i)];
        const auto x0 = beatToX(ev.startBeat, grid);
        const auto x1 = beatToX(ev.startBeat + ev.lengthInBeats, grid);
        if (x1 < grid.getX() - 2 || x0 > grid.getRight() + 2)
            continue;
        juce::Rectangle<float> block(x0 + 1.0f, lane.getY() + 2.0f,
                                     juce::jmax(14.0f, x1 - x0 - 2.0f), lane.getHeight() - 4.0f);
        const bool editing = i == editingChordIndex && arrChordSelector != nullptr && arrChordSelector->isVisible();
        const bool selected = selectedChords.count(i) > 0;
        g.setColour((editing ? theme::cool::cyan : theme::cool::aqua).withAlpha(selected ? 0.95f : 0.82f));
        g.fillRoundedRectangle(block, 5.0f);
        g.setColour(selected ? juce::Colours::white.withAlpha(0.95f) : juce::Colours::black.withAlpha(0.35f));
        g.drawRoundedRectangle(block, 5.0f, selected ? 1.8f : 1.0f);
        g.setColour(juce::Colours::black.withAlpha(0.92f));
        g.setFont(juce::Font(12.5f, juce::Font::bold));
        g.drawText(orion::chords::chordName(ev.spec), block.toNearestInt().reduced(6, 0), juce::Justification::centredLeft);
    }

    // Marquee box (drawn across the lane's full height for a clear selection cue).
    if (chordMarqueeStart.has_value() && ! chordMarqueeRect.isEmpty())
    {
        const auto box = juce::Rectangle<int>(chordMarqueeRect.getX(), lane.getY(),
                                              chordMarqueeRect.getWidth(), lane.getHeight())
                             .getIntersection(grid);
        g.setColour(theme::cool::cyan.withAlpha(0.14f));
        g.fillRect(box);
        g.setColour(theme::cool::cyan.withAlpha(0.6f));
        g.drawRect(box, 1);
    }

    // Indeterminate progress bar while a chord analysis is running.
    if (chordAnalysisRunning && ! grid.isEmpty())
    {
        g.setColour(theme::surface::primary.withAlpha(0.82f));
        g.fillRect(grid);

        const int barH = 4;
        auto track = grid.reduced(24, 0).withHeight(barH).withY(grid.getCentreY() + 6);
        g.setColour(juce::Colours::white.withAlpha(0.10f));
        g.fillRoundedRectangle(track.toFloat(), barH * 0.5f);

        // Moving highlight segment (indeterminate — Chordino gives no percentage).
        const double phase = (juce::Time::getMillisecondCounter() % 1100) / 1100.0;
        const float segW = track.getWidth() * 0.28f;
        const float travel = track.getWidth() + segW;
        const float x = track.getX() - segW + static_cast<float>(phase) * travel;
        juce::Rectangle<float> seg(x, (float) track.getY(), segW, (float) track.getHeight());
        g.setColour(theme::cool::cyan);
        g.saveState();
        g.reduceClipRegion(track);
        g.fillRoundedRectangle(seg, barH * 0.5f);
        g.restoreState();

        g.setColour(theme::text::primary.withAlpha(0.85f));
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText("Analyzing chords\xE2\x80\xA6", grid.reduced(24, 0).withHeight(16).withY(grid.getCentreY() - 16),
                   juce::Justification::centredLeft, false);
    }

    g.restoreState();
}

juce::Rectangle<int> ArrangementTimelineComponent::getEditToolbarBounds() const noexcept
{
    auto bounds = getTimelineContentBounds(*this);
    return bounds.removeFromTop(editToolbarHeight);
}

juce::Rectangle<int> ArrangementTimelineComponent::getToolButtonBounds(int index) const noexcept
{
    auto toolbar = getEditToolbarBounds();
    constexpr int w = 30, h = 28, gap = 10, padX = 0;
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

    g.setColour(theme::surface::elevated.withAlpha(0.46f));
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
        g.fillRoundedRectangle(b, theme::metrics::controlRadius);
        g.setColour(active ? theme::accent::activeCoral.withAlpha(0.82f)
                           : juce::Colours::white.withAlpha(enabled ? 0.18f : 0.07f));
        g.drawRoundedRectangle(b, theme::metrics::controlRadius, active ? 1.4f : 1.0f);
        return b;
    };

    for (int i = 0; i < editToolButtonCount; ++i)
    {
        const auto active = currentTool == toolModeForIndex(i);
        const auto enabled = true;
        const auto b = drawButtonShell(i, active, enabled);
        const auto iconColour = active ? theme::accent::activeCoral.withAlpha(0.98f)
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

juce::var ArrangementTimelineComponent::createPlaylistBlocksDragPayload(const std::vector<SelectedClip>& clips) const
{
    auto* payload = new juce::DynamicObject();
    payload->setProperty("type", "playlist-blocks");

    const auto& tracks = project.getTracks();
    double startBeat = std::numeric_limits<double>::max();
    double endBeat = 0.0;
    std::set<int> trackSet;
    juce::String title;
    int validClips = 0;

    for (const auto& selected : clips)
    {
        if (selected.trackIndex < 0 || selected.trackIndex >= static_cast<int>(tracks.size()))
            continue;

        const auto& track = tracks[static_cast<std::size_t>(selected.trackIndex)];
        if (selected.clipIndex < 0 || selected.clipIndex >= static_cast<int>(track.clips.size()))
            continue;

        const auto& clip = track.clips[static_cast<std::size_t>(selected.clipIndex)];
        startBeat = juce::jmin(startBeat, clip.startBeat);
        endBeat = juce::jmax(endBeat, clip.startBeat + clip.lengthInBeats);
        trackSet.insert(selected.trackIndex);
        if (title.isEmpty())
            title = clip.name.isNotEmpty() ? clip.name : track.name;
        ++validClips;
    }

    if (validClips == 0)
    {
        payload->setProperty("clipCount", 0);
        payload->setProperty("trackCount", 0);
        payload->setProperty("startBeat", 0.0);
        payload->setProperty("endBeat", 0.0);
        payload->setProperty("title", "Empty selection");
        return juce::var(payload);
    }

    if (validClips > 1)
        title = juce::String(validClips) + " playlist blocks";

    payload->setProperty("clipCount", validClips);
    payload->setProperty("trackCount", static_cast<int>(trackSet.size()));
    payload->setProperty("startBeat", startBeat);
    payload->setProperty("endBeat", endBeat);
    payload->setProperty("title", title);
    return juce::var(payload);
}

void ArrangementTimelineComponent::mouseDown(const juce::MouseEvent& event)
{
    playlistBlocksDragStarted = false;

    // Automation editing takes over the lane grid; clip editing is suspended while it's on.
    if (handleAutomationMouseDown(event))
        return;

    if (volumeEditorTrackIndex.has_value() && ! trackVolumeInlineEditor.getBounds().contains(event.getPosition()))
        commitTrackVolumeEditor(true);

    // Clicking anything that isn't the chord lane deselects chords, so Backspace doesn't
    // delete a lingering chord instead of the clicked clip/track.
    if (! selectedChords.empty()
        && ! (project.isChordLaneVisible() && getChordLaneGridArea().contains(event.getPosition()))
        && ! event.mods.isAltDown())
    {
        selectedChords.clear();
        repaint();
    }

    // Alt-drag over EMPTY grid marquee-selects chords by time range — easier than aiming at the thin
    // lane, and works right over the audio you're rearranging. Starting the drag ON a chord block is
    // NOT a marquee (that stacked a second selection frame over the block); it moves the block instead.
    if (project.isChordLaneVisible() && event.mods.isAltDown()
        && event.getPosition().x >= getChordLaneGridArea().getX()
        && chordEventAtPoint(event.getPosition()) < 0)
    {
        if (! (event.mods.isCommandDown() || event.mods.isShiftDown()))
            selectedChords.clear();
        chordMarqueeStart = event.getPosition();
        chordMarqueeRect = {};
        repaint();
        return;
    }

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

    if (getChordLaneToggleBounds().contains(event.getPosition()))
    {
        setChordLaneShown(! project.isChordLaneVisible());
        return;
    }

    // Chord lane octave ▲▼.
    if (project.isChordLaneVisible() && getChordOctaveUpBounds().contains(event.getPosition()))
    {
        project.setChordLaneOctave(project.getChordLaneOctave() + 1);
        repaint();
        return;
    }
    if (project.isChordLaneVisible() && getChordOctaveDownBounds().contains(event.getPosition()))
    {
        project.setChordLaneOctave(project.getChordLaneOctave() - 1);
        repaint();
        return;
    }

    if (getAddTrackButtonBounds().contains(event.getPosition()))
    {
        if (onAddTrackRequested)
            onAddTrackRequested();        // open the full Add Track dialog
        else
            showAddTrackMenu();           // fallback: inline popup
        return;
    }

    // Right-click an audio clip → toggle "Follow chord lane" (live re-harmonisation).
    if (event.mods.isPopupMenu())
    {
        if (const auto hit = hitTestClip(event.getPosition(), false); hit.has_value())
        {
            auto& tracks = project.getTracks();
            if (hit->trackIndex < static_cast<int>(tracks.size()))
            {
                auto& clips = tracks[static_cast<std::size_t>(hit->trackIndex)].clips;
                if (hit->clipIndex < static_cast<int>(clips.size()) && clips[static_cast<std::size_t>(hit->clipIndex)].type == ClipType::audio)
                {
                    const auto& clip = clips[static_cast<std::size_t>(hit->clipIndex)];
                    const bool on = clip.followsChordLane;

                    // Gain/normalize act on the selection when the clicked clip is part of it —
                    // right-clicking an unselected clip shouldn't silently retarget the menu.
                    const bool clickedIsSelected = std::any_of(selectedClips.begin(), selectedClips.end(),
                        [&] (const SelectedClip& s) { return s.trackIndex == hit->trackIndex && s.clipIndex == hit->clipIndex; });
                    const std::vector<SelectedClip> gainTargets = clickedIsSelected && selectedClips.size() > 1
                        ? selectedClips
                        : std::vector<SelectedClip>{ { hit->trackIndex, hit->clipIndex } };
                    const bool multi = gainTargets.size() > 1;
                    const auto suffix = multi ? " (" + juce::String(gainTargets.size()) + " clips)" : juce::String();

                    juce::PopupMenu menu;
                    menu.addItem(2, "Analyze chords");   // Logic-style: (re)detect this loop's progression
                    menu.addItem(3, "Separate stems (6)", orion::stems::isAvailable() && ! stemRunning, false);
                    menu.addItem(1, "Follow chord lane (re-harmonise)", true, on);
                    menu.addSeparator();
                    menu.addItem(7, "Match loudness to loudest" + suffix, multi, false);
                    menu.addItem(4, "Normalize peaks" + suffix);
                    menu.addItem(5, "Normalize peaks, keep balance" + suffix, multi, false);
                    menu.addItem(6, "Reset gain" + suffix);
                    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this)
                                           .withTargetScreenArea({ event.getScreenX(), event.getScreenY(), 1, 1 }),
                                       [this, ti = hit->trackIndex, ci = hit->clipIndex, gainTargets](int r)
                                       {
                                           if (r == 4 || r == 5)
                                           {
                                               if (onNormalizeClips) onNormalizeClips(gainTargets, r == 5);
                                               return;
                                           }
                                           if (r == 6)
                                           {
                                               if (onSetClipsGainDb) onSetClipsGainDb(gainTargets, 0.0);
                                               return;
                                           }
                                           if (r == 7)
                                           {
                                               if (onMatchClipLoudness) onMatchClipLoudness(gainTargets);
                                               return;
                                           }
                                           if (r != 1 && r != 2 && r != 3) return;
                                           auto& t = project.getTracks();
                                           if (ti >= static_cast<int>(t.size()) || ci >= static_cast<int>(t[static_cast<std::size_t>(ti)].clips.size()))
                                               return;
                                           if (r == 3)
                                           {
                                               // Pick which stems to extract (Logic-style), then run.
                                               juce::Rectangle<int> anchor(getWidth() / 2 - 1, getHeight() / 2 - 1, 2, 2);
                                               if (ci < static_cast<int>(t[static_cast<std::size_t>(ti)].clips.size()))
                                                   anchor = getClipBounds(t[static_cast<std::size_t>(ti)].clips[static_cast<std::size_t>(ci)], ti);
                                               auto content = std::make_unique<StemPickerCallout>(
                                                   [this, ti, ci](std::vector<juce::String> sel) { separateStemsForClip(ti, ci, sel); });
                                               juce::CallOutBox::launchAsynchronously(std::move(content), localAreaToGlobal(anchor), nullptr);
                                               return;
                                           }
                                           auto& c = t[static_cast<std::size_t>(ti)].clips[static_cast<std::size_t>(ci)];
                                           if (r == 1)
                                           {
                                               c.followsChordLane = ! c.followsChordLane;
                                               if (! project.isChordLaneVisible()) project.setChordLaneVisible(true);
                                               if (onChordLaneChanged) onChordLaneChanged();
                                               repaint();
                                           }
                                           else   // r == 2: analyze this clip's chords → replace the lane
                                           {
                                               {
                                                   const juce::ScopedLock sl(project.getAudioEditLock());
                                                   project.getChordTrack().clear();
                                               }
                                               const double srcLen = c.warpTargetLengthInBeats > 0.0 ? c.warpTargetLengthInBeats
                                                                                                     : c.lengthInBeats;
                                               detectChordsForClip(juce::File(c.sourcePath), c.startBeat, c.detectedBars,
                                                                   c.sourceKeyRoot, c.sourceKeyIsMinor, c.lengthInBeats,
                                                                   c.sampleStartRatio, srcLen);
                                           }
                                       });
                    return;
                }
            }
        }
    }

    // Chord lane: click a block → select + audition (+ arm move); drag empty space → marquee-select.
    if (project.isChordLaneVisible() && getChordLaneGridArea().contains(event.getPosition()))
    {
        const int idx = chordEventAtPoint(event.getPosition());
        if (idx >= 0)
        {
            if (event.mods.isCommandDown() || event.mods.isShiftDown())
            {
                if (selectedChords.count(idx)) selectedChords.erase(idx);
                else                           selectedChords.insert(idx);
            }
            else if (! selectedChords.count(idx))   // keep an existing multi-selection so you can drag the group
            {
                selectedChords = { idx };
            }
            setSingleSelection(std::nullopt);        // clicking a chord clears clip selection

            // Arm a move of the current selection.
            chordMoving = true;
            chordMoveCaptured = false;
            chordDragAnchorBeat = xToBeatPosition(event.getPosition().x);
            chordDragOrig.clear();
            const auto& ct = project.getChordTrack();
            for (int i : selectedChords)
                if (i >= 0 && i < static_cast<int>(ct.size()))
                    chordDragOrig.emplace_back(i, ct[static_cast<std::size_t>(i)].startBeat);
            if (idx >= 0 && idx < static_cast<int>(ct.size()))
                chordDragHomeStart = ct[static_cast<std::size_t>(idx)].startBeat;

            if (onChordAudition)
            {
                const std::array<int, 7> pattern = project.isKeyMinor()
                    ? std::array<int, 7>{ 0, 2, 3, 5, 7, 8, 10 }
                    : std::array<int, 7>{ 0, 2, 4, 5, 7, 9, 11 };
                onChordAudition(orion::chords::pitchesInKey(
                    project.getChordTrack()[static_cast<std::size_t>(idx)].spec,
                    project.getKeyRoot(), pattern, 48 + project.getChordLaneOctave() * 12));
            }

            // If the chord picker is already open, a single click on another block switches the picker
            // to that block (it follows the selection) — no need to double-click each one.
            if (arrChordSelector != nullptr && arrChordSelector->isVisible())
                openChordEditorFor(idx);
        }
        else
        {
            if (! (event.mods.isCommandDown() || event.mods.isShiftDown()))
                selectedChords.clear();
            chordMarqueeStart = event.getPosition();
            chordMarqueeRect = {};
        }
        repaint();
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
            // Arm a drag but DON'T jump the value on the initial click — a click near the left edge used
            // to snap the volume to the bottom (-24). The value only changes once the user drags.
            trackVolumeDragState = TrackVolumeDragState { true, trackHeaderHit->trackIndex, trackHeaderHit->bounds };
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
    // Hold Cmd during a drag to temporarily bypass snapping (free, fine movement) — Ableton behaviour.
    snapBypass = event.mods.isCommandDown();

    if (handleAutomationMouseDrag(event))
        return;

    // Clip moves in the arrangement are ALWAYS in place (the clip follows the cursor, no floating
    // badge). The old JUCE drag-and-drop badge is disabled outright — the bounds guard was flaky
    // (a momentary cursor excursion latched the badge on for the whole drag), and drag-to-Jam via
    // this payload is unreachable anyway (the Jam view replaces the arrangement area).
    if (false)
    {
        std::vector<SelectedClip> clips;
        clips.reserve(dragState->clipItems.size());
        for (const auto& item : dragState->clipItems)
            clips.push_back(item.clip);

        if (! clips.empty())
        {
            if (auto* dragContainer = juce::DragAndDropContainer::findParentDragContainerFor(this))
            {
                auto payload = createPlaylistBlocksDragPayload(clips);
                juce::Image dragImage(juce::Image::ARGB, 220, 56, true);
                juce::Graphics dg(dragImage);
                dg.setColour(juce::Colour(0xff151d29).withAlpha(0.94f));
                dg.fillRoundedRectangle(dragImage.getBounds().toFloat().reduced(0.5f), 8.0f);
                dg.setColour(theme::cool::cyan.withAlpha(0.86f));
                dg.drawRoundedRectangle(dragImage.getBounds().toFloat().reduced(1.0f), 8.0f, 1.2f);
                dg.setColour(theme::text::primary);
                dg.setFont(juce::FontOptions(14.0f, juce::Font::bold));
                const auto* object = payload.getDynamicObject();
                const auto title = object != nullptr ? object->getProperty("title").toString() : juce::String("Playlist block");
                dg.drawFittedText(title, dragImage.getBounds().reduced(14, 8), juce::Justification::centredLeft, 1);
                dg.setColour(theme::text::secondary);
                dg.setFont(juce::FontOptions(11.0f));
                const auto clipCount = object != nullptr ? static_cast<int>(object->getProperty("clipCount")) : 1;
                dg.drawFittedText(juce::String(clipCount) + (clipCount == 1 ? " clip" : " clips"),
                                  dragImage.getBounds().reduced(14, 8).withTrimmedTop(24),
                                  juce::Justification::centredLeft, 1);

                dragContainer->startDragging(payload, this, juce::ScaledImage(dragImage), true, nullptr, &event.source);
                playlistBlocksDragStarted = true;
            }
        }
    }

    // Move selected chords in time.
    if (chordMoving)
    {
        // Dragging DOWN out of the lane → bake the whole progression into a MIDI clip on drop.
        const int laneBottom = getChordLaneBounds().getBottom();
        if (event.getPosition().y > laneBottom + 6)
        {
            if (! chordDragToTrack)
            {
                // Undo any incidental in-lane time move from the initial press.
                const juce::ScopedLock sl(project.getAudioEditLock());
                auto& ct = project.getChordTrack();
                for (const auto& [i, orig] : chordDragOrig)
                    if (i >= 0 && i < static_cast<int>(ct.size()))
                        ct[static_cast<std::size_t>(i)].startBeat = orig;
                chordDragToTrack = true;
            }
            chordDropTargetTrack = trackIndexFromY(event.getPosition().y);
            chordDropGhost = chordDropTargetTrack >= 0 ? getTrackLaneBounds(chordDropTargetTrack)
                                                       : juce::Rectangle<int>();
            repaint();
            return;
        }
        if (chordDragToTrack)   // came back up into the lane → resume normal in-time move
        {
            chordDragToTrack = false;
            chordDropTargetTrack = -1;
            chordDropGhost = {};
        }

        const auto delta = snapBeatValue(xToBeatPosition(event.getPosition().x) - chordDragAnchorBeat);
        if (std::abs(delta) > 1.0e-6 || chordMoveCaptured)
        {
            if (! chordMoveCaptured) { pushUndoSnapshot(); chordMoveCaptured = true; }
            const juce::ScopedLock sl(project.getAudioEditLock());
            auto& ct = project.getChordTrack();

            if (chordDragOrig.size() == 1)
            {
                // Live reorder: the dragged block floats under the cursor while a "hole" follows it;
                // any block the cursor passes over slides into the hole (ghost preview, both directions).
                const int dragged = chordDragOrig.front().first;
                if (dragged >= 0 && dragged < static_cast<int>(ct.size()))
                {
                    const double origStart = chordDragOrig.front().second;
                    const double len = ct[static_cast<std::size_t>(dragged)].lengthInBeats;
                    ct[static_cast<std::size_t>(dragged)].startBeat = juce::jmax(0.0, origStart + delta);
                    const double centre = ct[static_cast<std::size_t>(dragged)].startBeat + len * 0.5;
                    for (int j = 0; j < static_cast<int>(ct.size()); ++j)
                    {
                        if (j == dragged) continue;
                        const double js = ct[static_cast<std::size_t>(j)].startBeat;
                        if (centre >= js && centre < js + ct[static_cast<std::size_t>(j)].lengthInBeats
                            && std::abs(js - chordDragHomeStart) > 1.0e-6)
                        {
                            ct[static_cast<std::size_t>(j)].startBeat = chordDragHomeStart;
                            chordDragHomeStart = js;
                            break;
                        }
                    }
                }
            }
            else
            {
                for (const auto& [i, orig] : chordDragOrig)
                    if (i >= 0 && i < static_cast<int>(ct.size()))
                        ct[static_cast<std::size_t>(i)].startBeat = juce::jmax(0.0, orig + delta);
            }
            repaint();
        }
        return;
    }
    // Marquee-select chords on the lane.
    if (chordMarqueeStart.has_value())
    {
        chordMarqueeRect = juce::Rectangle<int>(*chordMarqueeStart, event.getPosition());
        updateChordMarqueeSelection();
        repaint();
        return;
    }

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
        // pixels of jitter) must never teleport the clip to a neighbouring lane. Group drags and
        // Alt-drag copies move too: every clip shifts by the same lane delta as the grabbed one.
        const auto draggedFarEnough = event.getDistanceFromDragStart() > 5;
        const auto hoveredTrackIndex = trackIndexFromY(event.getPosition().y);
        if (draggedFarEnough && hoveredTrackIndex >= 0 && hoveredTrackIndex != dragState->clip.trackIndex)
            moveDraggedClipsByTrackDelta(hoveredTrackIndex - dragState->clip.trackIndex);
    }

    auto& tracks = project.getTracks();
    auto& clip = tracks[static_cast<std::size_t>(dragState->clip.trackIndex)].clips[static_cast<std::size_t>(dragState->clip.clipIndex)];
    const auto clipBoundsBeforeEdit = getClipBounds(clip, dragState->clip.trackIndex);
    const auto repaintEditedClip = [&]
    {
        const auto clipBoundsAfterEdit = getClipBounds(clip, dragState->clip.trackIndex);
        auto region = clipBoundsBeforeEdit.getUnion(clipBoundsAfterEdit).expanded(14, 12);

        // Edge edits draw a full-height snap guide at the clip edge. That line extends far below
        // the clip, so a clip-bounds-only repaint left its old position on screen as a ghost.
        // Extend the repaint to the full track-area height (its horizontal span already covers
        // the edge's old and new positions), so the moving line clears cleanly.
        const bool edgeEdit = dragState->mode == DragMode::resizeLeft  || dragState->mode == DragMode::resizeRight
                           || dragState->mode == DragMode::stretchLeft || dragState->mode == DragMode::stretchRight;
        if (edgeEdit)
        {
            const auto trackArea = getVisibleTrackAreaBounds(*this);
            region.setY(juce::jmin(region.getY(), trackArea.getY()));
            region.setBottom(juce::jmax(region.getBottom(), trackArea.getBottom()));
        }

        repaint(region.getIntersection(getLocalBounds()));
    };
    const auto beatDelta = snapBeatValue(xToBeatDelta(event.getDistanceFromDragStartX()));
    // Edge edits should lock the edge directly to the cursor's snapped grid position.
    // Using a delta from the grab point made an edge picked up inside its hit area lag
    // behind the pointer by up to one grid step, which felt noticeably unlike Warp.
    const auto stretchSnapStep = chooseStretchSnapStep(pixelsPerBeat);
    const auto snappedStretchCursorBeat = std::round(xToBeatPosition(event.getPosition().x) / stretchSnapStep)
        * stretchSnapStep;

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
        repaintEditedClip();
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
        repaintEditedClip();
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

        repaintEditedClip();
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
            const auto newLen = juce::jlimit(minLen, juce::jmax(minLen, maxLenTimeline),
                                             snappedStretchCursorBeat - origStart);
            clip.lengthInBeats = newLen;
            clip.sampleStartRatio = trimStart0;
            clip.sampleEndRatio   = trimEnd0;
            clip.warpTargetLengthInBeats = newLen / trimSpan0;   // scale full-source warp length
            break;
        }
        case DragMode::stretchLeft: // time-stretch from left edge
        {
            const auto newStart = juce::jlimit(0.0, origEnd - minLen, snappedStretchCursorBeat);
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

    repaintEditedClip();
}

void ArrangementTimelineComponent::mouseMove(const juce::MouseEvent& event)
{
    // Track which automation point the cursor is over, so its value readout can show (Bitwig-style).
    if (automationMode)
    {
        int hoverTrack = -1, hoverPoint = -1;
        const auto& tracks = project.getTracks();
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        {
            const auto lane = getTrackLaneBounds(i);
            if (event.y < lane.getY() || event.y >= lane.getBottom())
                continue;
            if (const int p = automationPointAt(i, event.getPosition()); p >= 0)
            {
                hoverTrack = i;
                hoverPoint = p;
            }
            break;
        }
        if (hoverTrack != automationHoverTrack || hoverPoint != automationHoverPoint)
        {
            automationHoverTrack = hoverTrack;
            automationHoverPoint = hoverPoint;
            repaint();
        }
    }

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
    bounds.removeFromTop(timelineTopChromeFor(*this));
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
    playlistBlocksDragStarted = false;
    snapBypass = false;   // end of drag — restore snapping

    if (handleAutomationMouseUp())
        return;

    if (chordMoving)
    {
        // Dropped below the lane → bake the progression into a MIDI clip on the target track.
        if (chordDragToTrack)
        {
            const int target = chordDropTargetTrack;
            chordMoving = false;
            chordMoveCaptured = false;
            chordDragOrig.clear();
            chordDragToTrack = false;
            chordDropTargetTrack = -1;
            chordDropGhost = {};
            bakeChordsToMidiClip(target);
            return;
        }

        const bool moved = chordMoveCaptured;

        // Settle the dragged block into the hole it opened up during the live reorder.
        if (moved && chordDragOrig.size() == 1)
        {
            const juce::ScopedLock sl(project.getAudioEditLock());
            auto& ct = project.getChordTrack();
            const int dragged = chordDragOrig.front().first;
            if (dragged >= 0 && dragged < static_cast<int>(ct.size()))
                ct[static_cast<std::size_t>(dragged)].startBeat = juce::jmax(0.0, chordDragHomeStart);
        }

        chordMoving = false;
        chordMoveCaptured = false;
        chordDragOrig.clear();
        if (moved && onChordLaneChanged) onChordLaneChanged();
        repaint();
        return;
    }
    if (chordMarqueeStart.has_value())
    {
        chordMarqueeStart.reset();
        chordMarqueeRect = {};
        repaint();
        return;
    }

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
    // Automation: double-click a point to delete it, like Ableton — the curve returns to the shape it
    // had before that point ("возвращение точки назад").
    if (automationMode)
    {
        auto& tracks = project.getTracks();
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        {
            const auto lane = getTrackLaneBounds(i);
            if (event.y < lane.getY() || event.y >= lane.getBottom())
                continue;
            const int hit = automationPointAt(i, event.getPosition());
            if (hit >= 0)
            {
                captureUndoSnapshot();
                {
                    const juce::ScopedLock sl(project.getAudioEditLock());
                    auto& pts = tracks[static_cast<std::size_t>(i)]
                                    .laneFor(automationParam, automationTargetIndex, automationParamIndex).points;
                    if (hit < static_cast<int>(pts.size()))
                        pts.erase(pts.begin() + hit);
                }
                automationDragTrack = automationDragPoint = -1;
                automationHoverTrack = automationHoverPoint = -1;
                repaint();
                return;
            }
            break;   // in this lane but not on a point — let normal clip double-click handling run
        }
    }

    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(editToolbarHeight);
    auto rulerArea = bounds.removeFromTop(timelineRulerHeight);
    auto rulerGridArea = rulerArea;
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto loopLane = rulerGridArea.removeFromTop(loopLaneHeight);
    const auto beatsPerBar = static_cast<double>(project.getNumerator());

    // Chord lane: double-click a chord to edit it in the selector, or empty space to drop a tonic chord.
    if (project.isChordLaneVisible() && getChordLaneGridArea().contains(event.getPosition()))
    {
        const int idx = chordEventAtPoint(event.getPosition());
        if (idx >= 0)
            openChordEditorFor(idx);
        else
            addChordAtBeat(xToBeatPosition(event.getPosition().x));
        return;
    }

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

    // 'A' toggles automation editing; Shift+A switches the shown parameter (Volume ↔ Pan).
    if (key == juce::KeyPress('a', juce::ModifierKeys::shiftModifier, 0))
    {
        setAutomationParam(automationParam == AutomationParam::trackVolume ? AutomationParam::trackPan
                                                                           : AutomationParam::trackVolume);
        return true;
    }
    if (key == juce::KeyPress('a', juce::ModifierKeys::noModifiers, 0))
    {
        setAutomationMode(! automationMode);
        return true;
    }

    // Editing-grid shortcuts (Ableton §6.10): Cmd+1 finer, Cmd+2 coarser, Cmd+3 triplets,
    // Cmd+4 snap on/off, Cmd+5 adaptive/fixed.
    if (key == juce::KeyPress('1', juce::ModifierKeys::commandModifier, 0)) { adjustGridDensity(+1); return true; }
    if (key == juce::KeyPress('2', juce::ModifierKeys::commandModifier, 0)) { adjustGridDensity(-1); return true; }
    if (key == juce::KeyPress('3', juce::ModifierKeys::commandModifier, 0)) { toggleGridTriplet(); return true; }
    if (key == juce::KeyPress('4', juce::ModifierKeys::commandModifier, 0)) { toggleGridSnap(); return true; }
    if (key == juce::KeyPress('5', juce::ModifierKeys::commandModifier, 0)) { toggleGridAdaptive(); return true; }

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

    if (! selectedChords.empty() && (key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey))
    {
        return deleteSelectedChords();
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

    if (! selectedChords.empty() && key == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0))
    {
        return duplicateSelectedChords();
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

juce::Rectangle<int> ArrangementTimelineComponent::getChordLaneToggleBounds() const noexcept
{
    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(editToolbarHeight);
    auto rulerArea = bounds.removeFromTop(timelineRulerHeight);
    auto headerArea = rulerArea.removeFromLeft(trackHeaderWidth);
    headerArea.removeFromRight(44);   // skip the "+" add-track button
    return headerArea.removeFromRight(40).withSizeKeepingCentre(30, 30);
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

// Bake the whole chord-lane progression into one MIDI clip (Logic-style "drag chord track to a
// MIDI track"). Preserves each chord's beat position/length; voices with the same key-aware
// intervals used for audition, at the chord lane's octave. If the target isn't a MIDI track, a
// fresh MIDI track is created for it.
void ArrangementTimelineComponent::bakeChordsToMidiClip(int trackIndex)
{
    const auto& ct = project.getChordTrack();
    if (ct.empty())
        return;

    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())
        || ! tracks[static_cast<std::size_t>(trackIndex)].isMidiTrack)
    {
        addMidiTrack();                                    // pushes its own undo snapshot
        trackIndex = static_cast<int>(tracks.size()) - 1;
    }
    else
    {
        pushUndoSnapshot();
    }

    // Time span of the progression.
    double minStart = ct.front().startBeat, maxEnd = 0.0;
    for (const auto& ev : ct)
    {
        minStart = juce::jmin(minStart, ev.startBeat);
        maxEnd   = juce::jmax(maxEnd, ev.startBeat + ev.lengthInBeats);
    }
    const double clipStart = juce::jmax(0.0, minStart);
    const double clipLen   = juce::jmax(1.0, maxEnd - clipStart);

    const std::array<int, 7> pattern = project.isKeyMinor()
        ? std::array<int, 7>{ 0, 2, 3, 5, 7, 8, 10 }
        : std::array<int, 7>{ 0, 2, 4, 5, 7, 9, 11 };
    const int base = 48 + project.getChordLaneOctave() * 12;

    // Build raw per-chord note segments, then TIE consecutive segments of the same pitch that abut
    // into one sustained note. Two chords that share a tone (very common in a progression) would
    // otherwise place a note-off and a note-on on the same tick; some instruments drop the retrigger
    // and the shared voice goes silent (only the changed tone sounds). Tying = correct voice-leading.
    struct Seg { int pitch; double start; double end; };
    std::vector<Seg> segs;
    for (const auto& ev : ct)
        for (int p : orion::chords::pitchesInKey(ev.spec, project.getKeyRoot(), pattern, base))
            segs.push_back({ p, ev.startBeat, ev.startBeat + ev.lengthInBeats });

    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b)
              { return a.pitch != b.pitch ? a.pitch < b.pitch : a.start < b.start; });

    std::vector<MidiNote> notes;
    for (const auto& s : segs)
    {
        if (! notes.empty())
        {
            auto& prev = notes.back();
            const double prevEnd = clipStart + prev.startBeat + prev.lengthInBeats;
            if (prev.pitch == s.pitch && s.start <= prevEnd + 1.0e-6)   // abutting/overlapping same pitch → tie
            {
                prev.lengthInBeats = juce::jmax(prev.lengthInBeats, (s.end - clipStart) - prev.startBeat);
                continue;
            }
        }
        notes.push_back(MidiNote { s.pitch, s.start - clipStart, s.end - s.start, 100 });
    }

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    TimelineClip clip;
    clip.name          = "Chords";
    clip.type          = ClipType::midi;
    clip.startBeat     = clipStart;
    clip.lengthInBeats = clipLen;
    clip.colour        = track.colour;
    clip.midiNotes     = std::move(notes);

    {
        const juce::ScopedLock sl(project.getAudioEditLock());
        track.clips.push_back(std::move(clip));
    }

    setSingleSelection(SelectedClip { trackIndex, static_cast<int>(track.clips.size()) - 1 });
    repaint();
}

// Kick off a background 6-stem separation of the clip's audio (Demucs, like Logic's Stem Splitter).
// The heavy work runs off the message thread; a progress banner shows while it runs; on completion
// applyStemResult lays the stems out as new tracks.
void ArrangementTimelineComponent::separateStemsForClip(int trackIndex, int clipIndex, const std::vector<juce::String>& wantedStems)
{
    if (stemRunning || wantedStems.empty())
        return;
    auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(clips.size()))
        return;

    const TimelineClip original = clips[static_cast<std::size_t>(clipIndex)];   // capture by value
    const juce::File src(original.sourcePath);
    if (! src.existsAsFile())
        return;

    // Persistent, writable output location (source folders may be read-only).
    auto outDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                      .getChildFile("Orion").getChildFile("Stems")
                      .getChildFile(src.getFileNameWithoutExtension() + "_" + juce::String(juce::Time::currentTimeMillis()));

    stemRunning = true;
    stemProgress = 0.0f;
    stemStatus = "Separating stems\xE2\x80\xA6";
    repaint();

    const juce::StringArray wanted (wantedStems.data(), static_cast<int>(wantedStems.size()));
    juce::Component::SafePointer<ArrangementTimelineComponent> safe(this);
    juce::Thread::launch([safe, src, outDir, original, trackIndex, wanted]
    {
        auto res = orion::stems::separate(
            src, outDir,
            [safe](float p) { juce::MessageManager::callAsync([safe, p] { if (safe != nullptr) safe->stemProgress = p; }); },
            [safe] { return safe == nullptr; });

        juce::MessageManager::callAsync([safe, res, original, trackIndex, wanted]
        {
            if (safe == nullptr)
                return;
            safe->stemRunning = false;
            if (! res.ok)
            {
                safe->stemStatus = "Stem split failed: " + res.error;
                safe->repaint();
                return;
            }
            // Keep only the stems the user selected.
            auto filtered = res;
            filtered.stems.erase(std::remove_if(filtered.stems.begin(), filtered.stems.end(),
                                 [&](const orion::stems::Stem& s) { return ! wanted.contains(s.name); }),
                                 filtered.stems.end());
            if (filtered.stems.empty())
            {
                safe->stemStatus = {};
                safe->repaint();
                return;
            }
            safe->applyStemResult(filtered, original, trackIndex);
        });
    });
}

void ArrangementTimelineComponent::applyStemResult(const orion::stems::Result& res, const TimelineClip& original, int originalTrackIndex)
{
    if (res.stems.empty())
        return;

    pushUndoSnapshot();

    {
        auto& tracks = project.getTracks();
        originalTrackIndex = juce::jlimit(0, static_cast<int>(tracks.size()) - 1, originalTrackIndex);
        // Mute the original clip (still identify it by source + position; it may have moved index).
        for (auto& c : tracks[static_cast<std::size_t>(originalTrackIndex)].clips)
            if (c.sourcePath == original.sourcePath && std::abs(c.startBeat - original.startBeat) < 1.0e-6)
                c.muted = true;
    }

    int insertAt = originalTrackIndex + 1;
    for (const auto& stem : res.stems)
    {
        const auto title = stem.name.substring(0, 1).toUpperCase() + stem.name.substring(1);   // "Drums"
        const int idx = insertTrackAt(insertAt, false, title, juce::Colour(), true);
        if (idx < 0 || idx >= static_cast<int>(project.getTracks().size()))
            continue;

        auto& t = project.getTracks()[static_cast<std::size_t>(idx)];
        // Mirror the original clip's timing/warp so stems line up EXACTLY under it; just swap the audio.
        TimelineClip clip = original;
        clip.name = title;
        clip.sourcePath = stem.file.getFullPathName();
        clip.colour = t.colour;
        clip.muted = false;
        clip.solo = false;
        clip.followsChordLane = false;
        clip.midiNotes.clear();
        clip.pitchSlides.clear();
        clip.signalAnalysisPending = false;
        {
            const juce::ScopedLock sl(project.getAudioEditLock());
            t.clips.clear();
            t.clips.push_back(clip);
        }
        insertAt = idx + 1;
    }

    stemStatus = {};
    notifyClipSelectionChanged();
    clampScrollOffsets();
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
    bounds.removeFromTop(timelineTopChromeFor(*this));

    const auto visibleTracksArea = getVisibleTrackAreaBounds(*this);
    auto visibleGridArea = visibleTracksArea;
    visibleGridArea.removeFromLeft(trackHeaderWidth);

    const bool overChordLane = getChordLaneBounds().contains(event.getPosition());
    if (! visibleTracksArea.contains(event.getPosition()) && ! overChordLane)
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

    if (isExplicitHorizontal && (visibleGridArea.contains(event.getPosition()) || overChordLane))
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
    auto tracksArea = bounds.withTrimmedTop(timelineTopChromeFor(*this));
    auto headerArea = tracksArea.removeFromLeft(trackHeaderWidth);
    auto gridArea = tracksArea;
    // The chord lane maps horizontally like the grid, so zooming over it should zoom time too.
    const bool overGrid = gridArea.contains(event.getPosition())
                       || getChordLaneGridArea().contains(event.getPosition());

    if (! headerArea.contains(event.getPosition()) && ! overGrid)
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
    const auto horizontalDelta = overGrid ? stableDelta : 0.0;
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
        tracksBounds.removeFromTop(timelineTopChromeFor(*this));
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

    if (autoWarp && autoDetectChordsOnImport)
        detectChordsForClip(sourceFile, startBeat, analysis.detectedBars, analysis.sourceKeyRoot, analysis.sourceKeyIsMinor, lengthBeats);

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

    if (autoWarp && autoDetectChordsOnImport)
        detectChordsForClip(file, clampedStart, analysis.detectedBars, analysis.sourceKeyRoot, analysis.sourceKeyIsMinor, lengthBeats);

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

    if (autoWarp && autoDetectChordsOnImport)
        detectChordsForClip(file, startBeat, analysis.detectedBars, analysis.sourceKeyRoot, analysis.sourceKeyIsMinor, lengthBeats);

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
    auto tracksArea = bounds.withTrimmedTop(timelineTopChromeFor(*this));
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
    const bool zoomH = std::abs(horizontalDelta) > 0.0001;
    const bool zoomV = std::abs(verticalDelta) > 0.0001;
    if (! zoomH && ! zoomV)
        return;

    // Capture the focus for BOTH axes on any zoom. timerCallback recomputes scrollX AND scrollY every
    // frame while animating, so if we only refreshed the zoomed axis the other one was recomputed from
    // a STALE focus captured during some earlier zoom — which is what threw the view to the bottom of
    // the track list when zooming horizontally. With both fresh, the non-zoomed axis is an identity
    // (ratio * unchangedSize - offset == the current scroll) and therefore stays exactly put.
    zoomFocusBeat       = focusBeat;
    zoomFocusXInView    = focusXInView;
    zoomFocusTrackRatio = focusTrackRatio;
    zoomFocusYInView    = focusYInView;

    if (zoomH)
    {
        timelineAutoFitActive = false;
        const auto zoomFactor = std::pow(1.2, horizontalDelta);
        targetPixelsPerBeat = juce::jlimit(minZoomPixelsPerBeat(), maxPixelsPerBeat, targetPixelsPerBeat * zoomFactor);
    }
    if (zoomV)
    {
        const auto zoomFactor = std::pow(1.18, verticalDelta);
        targetVerticalZoom = juce::jlimit(minimumVerticalZoom, maximumVerticalZoom, targetVerticalZoom * zoomFactor);
    }
    zoomAnimating = true;
    // The eased application happens in timerCallback; nothing to apply instantly here.
}

void ArrangementTimelineComponent::timerCallback()
{
    if (chordAnalysisRunning)
        repaint(getChordLaneBounds());   // animate the indeterminate progress bar
    if (stemRunning)
        repaint();                        // refresh the stem-separation progress banner
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

    // Repaint only when the transport or an animation needs it. The playlist's render layers
    // overlap (clips, waveform, meter headers and playhead), so this stays as one coherent frame.
    const bool playing   = transport.isPlaying() || transport.isCountInActive();
    if (playing) meterSettleFrames = 120;                 // keep meters live while playing
    else if (meterSettleFrames > 0) --meterSettleFrames;  // ~1 s after stop → meters decay, then quiet
    const bool animating = zoomAnimating || std::abs(browserAppendAnim - appendTarget) > 0.001f;
    if (playing || animating || meterSettleFrames > 0)
        repaint();
}

float ArrangementTimelineComponent::beatToX(double beat, juce::Rectangle<int> laneArea) const noexcept
{
    return static_cast<float>(laneArea.getX() + (beat * pixelsPerBeat) - scrollX);
}

void ArrangementTimelineComponent::clampScrollOffsets()
{
    auto bounds = getTimelineContentBounds(*this);
    auto rulerGridArea = bounds.withTrimmedTop(timelineTopChromeFor(*this));
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
    auto rulerGridArea = bounds.withTrimmedTop(timelineTopChromeFor(*this));
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto viewportW = static_cast<double>(juce::jmax(1, rulerGridArea.getWidth()));

    // Default view uses a FIXED grid density rather than a fixed bar count, so the cells look the same
    // size regardless of window width. Tuned so ~45 bars show (matches Ableton's default bar count on
    // the user's wider window; cells end up a touch larger than Ableton's, which reads well). Longer
    // content still zooms out to fit (whichever needs the wider view wins).
    constexpr double ableton_px_per_beat = 5.25;   // Ableton's measured grid-cell width (~20.7 px/bar)
    const auto beatsPerBar   = static_cast<double>(juce::jmax(1, project.getNumerator()));
    const auto contentBeats  = project.getContentEndInBeats();
    const auto marginBeats   = contentBeats > 0.0 ? juce::jmax(8.0 * beatsPerBar, contentBeats * 0.08) : 0.0;
    const auto defaultBeats  = viewportW / ableton_px_per_beat;         // density-based default
    const auto fitBeats      = juce::jmax(defaultBeats, contentBeats + marginBeats);

    pixelsPerBeat = juce::jlimit(minZoomPixelsPerBeat(), maxPixelsPerBeat, viewportW / fitBeats);
    targetPixelsPerBeat = pixelsPerBeat;
    zoomAnimating = false;
    scrollX = 0.0;
}

double ArrangementTimelineComponent::minZoomPixelsPerBeat() const noexcept
{
    auto bounds = getTimelineContentBounds(*this);
    auto rulerGridArea = bounds.withTrimmedTop(timelineTopChromeFor(*this));
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto viewportW = static_cast<double>(juce::jmax(1, rulerGridArea.getWidth()));

    // Max zoom-out fits the content with the same tail as auto-fit. Without this, the lower zoom
    // bound clamps away the right-side breathing room on long clips. The floor here is the widest
    // view you can reach; keep generous headroom (128 bars) so short projects don't feel "stuck"
    // against the limit while zooming out. (The default auto-fit view stays at 64 bars.)
    const auto beatsPerBar   = static_cast<double>(juce::jmax(1, project.getNumerator()));
    const auto minVisible    = 128.0 * beatsPerBar;
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
    const auto defaultVisibleBeats = 38.0 * beatsPerBar;   // wider grid cells by default (was 64 = tiny/dense)
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

// ---- Automation editing ---------------------------------------------------------------------

void ArrangementTimelineComponent::setAutomationMode(bool shouldEdit)
{
    if (automationMode == shouldEdit)
        return;
    automationMode = shouldEdit;
    automationDragTrack = automationDragPoint = -1;
    if (onAutomationModeChanged)
        onAutomationModeChanged(automationMode);
    repaint();
}

void ArrangementTimelineComponent::setAutomationParam(AutomationParam p)
{
    // The simple Volume/Pan case: track-level target, name straight from the param.
    setAutomationTarget(p, -1, -1, automationParamName(p));
}

void ArrangementTimelineComponent::setAutomationTarget(AutomationParam p, int targetIndex,
                                                       int paramIndex, const juce::String& label)
{
    if (automationParam == p && automationTargetIndex == targetIndex && automationParamIndex == paramIndex)
        return;
    automationParam = p;
    automationTargetIndex = targetIndex;
    automationParamIndex = paramIndex;
    automationParamLabel = label.isNotEmpty() ? label : automationParamName(p);
    automationDragTrack = automationDragPoint = -1;   // any in-flight drag targeted the old lane
    repaint();
}

bool ArrangementTimelineComponent::chordKeyAtBeat(double beat, int& rootPc, bool& minor) const noexcept
{
    for (const auto& ev : project.getChordTrack())
    {
        if (beat >= ev.startBeat && beat < ev.startBeat + ev.lengthInBeats)
        {
            rootPc = ((ev.spec.rootPc % 12) + 12) % 12;
            minor = ev.spec.quality == orion::chords::Quality::minor
                 || ev.spec.quality == orion::chords::Quality::dim;
            return true;
        }
    }
    return false;
}

juce::Rectangle<int> ArrangementTimelineComponent::automationLaneGrid(int trackIndex) const noexcept
{
    return getTrackLaneBounds(trackIndex).withTrimmedLeft(trackHeaderWidth).reduced(0, 5);
}

juce::Rectangle<int> ArrangementTimelineComponent::automationParamChipBounds(int trackIndex) const noexcept
{
    // The parameter selector sits in the TRACK HEADER over the volume-fader row and REPLACES it while
    // automation mode is on (Bitwig swaps the mixer controls for the automation param/section choosers).
    // Drawn opaque in drawAutomationHeaderChips so the fader/dB underneath are hidden — a clean swap,
    // not a translucent overlay. Spans the fader + dB span of the header.
    const auto layout = computeHeaderLayout(trackIndex);
    auto strip = layout.slider.getUnion(layout.volumeValue);
    return juce::Rectangle<int>(strip.getX() - 2, strip.getCentreY() - 9,
                                strip.getWidth() + 4, 18);
}

juce::String ArrangementTimelineComponent::automationValueLabel(float value) const
{
    switch (automationParam)
    {
        case AutomationParam::trackVolume:
            return (value > -59.9f ? juce::String(value, 1) : juce::String("-inf")) + " dB";
        case AutomationParam::trackPan:
        {
            const int pct = juce::roundToInt(std::abs(value) * 100.0f);
            return pct == 0 ? juce::String("C") : (value < 0 ? "L" : "R") + juce::String(pct);
        }
        default:   // send / instrument / insert params are normalised 0..1
            return juce::String(juce::roundToInt(value * 100.0f)) + "%";
    }
}

float ArrangementTimelineComponent::automationValueToY(float value, juce::Rectangle<int> grid) const noexcept
{
    const float mn = automationParamMin(automationParam);
    const float mx = automationParamMax(automationParam);
    float t = juce::jlimit(0.0f, 1.0f, (value - mn) / juce::jmax(0.0001f, mx - mn));
    // Volume uses a fader-like taper (Ableton feel): 0 dB sits ~80% up with fine control near unity,
    // and the quiet end compresses toward the floor — instead of a linear scale pinning 0 dB at the top.
    if (automationParam == AutomationParam::trackVolume)
        t = std::pow(t, 2.3f);
    return static_cast<float>(grid.getBottom()) - t * static_cast<float>(grid.getHeight());
}

float ArrangementTimelineComponent::automationYToValue(float y, juce::Rectangle<int> grid) const noexcept
{
    const float mn = automationParamMin(automationParam);
    const float mx = automationParamMax(automationParam);
    float t = juce::jlimit(0.0f, 1.0f, (static_cast<float>(grid.getBottom()) - y) / juce::jmax(1.0f, static_cast<float>(grid.getHeight())));
    if (automationParam == AutomationParam::trackVolume)
        t = std::pow(t, 1.0f / 2.3f);
    return mn + t * (mx - mn);
}

int ArrangementTimelineComponent::automationPointAt(int trackIndex, juce::Point<int> pos) const
{
    const auto& tracks = project.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return -1;
    const auto* lane = tracks[static_cast<std::size_t>(trackIndex)]
                           .findAutomation(automationParam, automationTargetIndex, automationParamIndex);
    if (lane == nullptr)
        return -1;
    const auto grid = automationLaneGrid(trackIndex);
    for (int i = 0; i < static_cast<int>(lane->points.size()); ++i)
    {
        const auto& pt = lane->points[static_cast<std::size_t>(i)];
        const auto px = beatToX(pt.beat, grid);
        const auto py = automationValueToY(pt.value, grid);
        if (std::abs(px - pos.x) <= 7.0f && std::abs(py - pos.y) <= 7.0f)
            return i;
    }
    return -1;
}

void ArrangementTimelineComponent::drawAutomationOverlay(juce::Graphics& g)
{
    if (! automationMode)
        return;

    const auto& tracks = project.getTracks();
    const auto visible = getVisibleTrackAreaBounds(*this);

    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        const auto grid = automationLaneGrid(i);
        if (grid.getHeight() <= 2 || ! grid.intersects(visible))
            continue;

        // Bitwig does NOT darken the clip in automation mode — the clip stays fully colourful and the
        // automation (track-coloured fill + bright line) simply overlays it. So: no veil here.
        const auto& track = tracks[static_cast<std::size_t>(i)];
        // Track-coloured FILL for identity (Bitwig), but a bright near-WHITE line so the curve always
        // contrasts — a track-coloured line would vanish over a clip of the same colour. Ableton keeps
        // the line a bold, high-contrast colour for exactly this reason.
        const auto trackColour = track.colour;
        const juce::Colour line(0xfff7faff);   // bright, high-contrast curve
        const juce::Colour dot (0xffffffff);

        const auto* lane = track.findAutomation(automationParam, automationTargetIndex, automationParamIndex);
        const float staticVal = automationParam == AutomationParam::trackVolume
                                    ? static_cast<float>(track.volumeDb)
                                    : (automationParam == AutomationParam::trackPan
                                           ? static_cast<float>(track.pan)
                                           : automationParamDefault(automationParam));

        if (lane == nullptr || lane->points.empty())
        {
            const auto y = automationValueToY(staticVal, grid);
            const juce::Rectangle<float> row(static_cast<float>(grid.getX()), y,
                                             static_cast<float>(grid.getWidth()),
                                             static_cast<float>(grid.getBottom()) - y);
            // Track-coloured fill under the flat line + a clear, thick line (shadowed) so the default
            // automation level is obviously visible, exactly like Ableton's line across the clip.
            g.setColour(trackColour.withAlpha(0.3f));
            g.fillRect(row);
            g.setColour(juce::Colours::black.withAlpha(0.45f));
            g.fillRect(row.getX(), y - 2.4f, row.getWidth(), 4.8f);
            g.setColour(line);
            g.fillRect(row.getX(), y - 1.4f, row.getWidth(), 2.8f);
            continue;
        }

        // Envelope: flat before the first point, straight segments between points (tension defaults to
        // 0 for now), flat after the last. Points drawn as handles on top.
        juce::Path path;
        const auto firstX = beatToX(lane->points.front().beat, grid);
        const auto firstY = automationValueToY(lane->points.front().value, grid);
        path.startNewSubPath(static_cast<float>(grid.getX()), firstY);
        path.lineTo(firstX, firstY);
        for (const auto& pt : lane->points)
            path.lineTo(beatToX(pt.beat, grid), automationValueToY(pt.value, grid));
        const auto lastY = automationValueToY(lane->points.back().value, grid);
        path.lineTo(static_cast<float>(grid.getRight()), lastY);

        // Track-coloured fill from the curve down to the lane floor (Bitwig look).
        juce::Path fill = path;
        fill.lineTo(static_cast<float>(grid.getRight()), static_cast<float>(grid.getBottom()));
        fill.lineTo(static_cast<float>(grid.getX()),     static_cast<float>(grid.getBottom()));
        fill.closeSubPath();
        g.setColour(trackColour.withAlpha(0.3f));
        g.fillPath(fill);

        // Dark shadow under the bright line so it reads boldly over any content.
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.strokePath(path, juce::PathStrokeType(4.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(line);
        g.strokePath(path, juce::PathStrokeType(2.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        for (const auto& pt : lane->points)
        {
            const auto px = beatToX(pt.beat, grid);
            const auto py = automationValueToY(pt.value, grid);
            g.setColour(juce::Colours::black.withAlpha(0.55f));
            g.fillEllipse(px - 4.5f, py - 4.5f, 9.0f, 9.0f);
            g.setColour(dot);
            g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        }

        // Numeric value readout beside the point being dragged or hovered (Bitwig/Ableton style).
        const int activePoint = (automationDragTrack == i && automationDragPoint >= 0) ? automationDragPoint
                              : (automationHoverTrack == i ? automationHoverPoint : -1);
        if (activePoint >= 0 && activePoint < static_cast<int>(lane->points.size()))
        {
            const auto& pt = lane->points[static_cast<std::size_t>(activePoint)];
            const auto px = beatToX(pt.beat, grid);
            const auto py = automationValueToY(pt.value, grid);
            const auto label = automationValueLabel(pt.value);
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            const int w = juce::jmax(34, g.getCurrentFont().getStringWidth(label) + 12);
            juce::Rectangle<float> box(px + 9.0f, py - 20.0f, static_cast<float>(w), 16.0f);
            if (box.getRight() > static_cast<float>(grid.getRight()))
                box.setX(px - 9.0f - static_cast<float>(w));   // flip to the left near the edge
            box.setY(juce::jlimit(static_cast<float>(grid.getY()),
                                  static_cast<float>(grid.getBottom() - 16), box.getY()));
            g.setColour(juce::Colours::black.withAlpha(0.82f));
            g.fillRoundedRectangle(box, 3.0f);
            g.setColour(line);
            g.drawText(label, box, juce::Justification::centred, false);
        }
    }
}

void ArrangementTimelineComponent::drawAutomationHeaderChips(juce::Graphics& g)
{
    const auto& tracks = project.getTracks();
    const auto visible = getVisibleTrackAreaBounds(*this);
    const juce::Colour col(0xffffb454);

    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        const auto lane = getTrackLaneBounds(i);
        if (! lane.intersects(visible))
            continue;

        const auto chip = automationParamChipBounds(i);
        if (! chip.intersects(visible))
            continue;

        // Opaque: covers the fader/dB it replaces, so it reads as a clean parameter chooser in the
        // header (Bitwig swaps the mixer row for the automation selector), not a chip floating on top.
        g.setColour(juce::Colour(0xff1b2130));
        g.fillRoundedRectangle(chip.toFloat(), 4.0f);
        g.setColour(col.withAlpha(0.85f));
        g.drawRoundedRectangle(chip.toFloat().reduced(0.5f), 4.0f, 1.0f);

        // A small down-caret on the right marks it as a dropdown.
        auto text = chip.reduced(8, 0);
        const auto caret = text.removeFromRight(12);
        g.setColour(col);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(automationParamLabel, text, juce::Justification::centredLeft, true);
        g.drawText(juce::String::fromUTF8("▾"), caret, juce::Justification::centred, false);
    }
}

bool ArrangementTimelineComponent::handleAutomationMouseDown(const juce::MouseEvent& event)
{
    if (! automationMode)
        return false;

    auto& tracks = project.getTracks();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        const auto lane = getTrackLaneBounds(i);
        if (event.y < lane.getY() || event.y >= lane.getBottom())
            continue;

        // The parameter selector chip (in the header): a menu to switch what this lane automates —
        // Volume / Pan, or any parameter of the track's hosted instrument or insert effects. IDs:
        // 1=Volume, 2=Pan, 1000+p = instrument param p, 10000 + slot*1000 + p = insert slot's param p.
        if (automationParamChipBounds(i).contains(event.getPosition()))
        {
            juce::PopupMenu menu;
            menu.addItem(1, "Volume", true, automationParam == AutomationParam::trackVolume);
            menu.addItem(2, "Pan",    true, automationParam == AutomationParam::trackPan);

            juce::StringArray instNames;
            if (onRequestInstrumentParamNames)
                instNames = onRequestInstrumentParamNames(i);
            if (! instNames.isEmpty())
            {
                juce::PopupMenu sub;
                for (int p = 0; p < instNames.size(); ++p)
                    sub.addItem(1000 + p, instNames[p], true,
                                automationParam == AutomationParam::instrumentParam
                                    && automationTargetIndex == -1 && automationParamIndex == p);
                menu.addSubMenu("Instrument", sub);
            }

            const auto& inserts = project.getTracks()[static_cast<std::size_t>(i)].inserts;
            for (int s = 0; s < static_cast<int>(inserts.size()); ++s)
            {
                juce::StringArray insNames;
                if (onRequestInsertParamNames)
                    insNames = onRequestInsertParamNames(i, s);
                if (insNames.isEmpty())
                    continue;
                juce::PopupMenu sub;
                for (int p = 0; p < insNames.size(); ++p)
                    sub.addItem(10000 + s * 1000 + p, insNames[p], true,
                                automationParam == AutomationParam::insertParam
                                    && automationTargetIndex == s && automationParamIndex == p);
                const auto nm = inserts[static_cast<std::size_t>(s)].pluginName;
                menu.addSubMenu(nm.isNotEmpty() ? nm : ("Insert " + juce::String(s + 1)), sub);
            }

            // Clear the current parameter's envelope → it returns to the static fader/default value
            // (the "get back to 0" exit). Only offered when there's actually something to clear.
            const auto* curLane = project.getTracks()[static_cast<std::size_t>(i)]
                                      .findAutomation(automationParam, automationTargetIndex, automationParamIndex);
            if (curLane != nullptr && ! curLane->points.empty())
            {
                menu.addSeparator();
                menu.addItem(9999, "Clear " + automationParamLabel + " automation");
            }

            menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this)
                                   .withTargetScreenArea(localAreaToGlobal(automationParamChipBounds(i))),
                [this, i, instNames](int r)
                {
                    if (r == 9999)
                    {
                        captureUndoSnapshot();
                        {
                            const juce::ScopedLock sl(project.getAudioEditLock());
                            project.getTracks()[static_cast<std::size_t>(i)]
                                .laneFor(automationParam, automationTargetIndex, automationParamIndex).points.clear();
                        }
                        automationDragTrack = automationDragPoint = -1;
                        automationHoverTrack = automationHoverPoint = -1;
                        repaint();
                        return;
                    }
                    if (r == 1) { setAutomationParam(AutomationParam::trackVolume); }
                    else if (r == 2) { setAutomationParam(AutomationParam::trackPan); }
                    else if (r >= 1000 && r < 10000)
                    {
                        const int p = r - 1000;
                        setAutomationTarget(AutomationParam::instrumentParam, -1, p,
                            juce::isPositiveAndBelow(p, instNames.size()) ? instNames[p]
                                                                          : ("Param " + juce::String(p + 1)));
                    }
                    else if (r >= 10000)
                    {
                        const int s = (r - 10000) / 1000;
                        const int p = (r - 10000) % 1000;
                        juce::StringArray insNames;
                        if (onRequestInsertParamNames)
                            insNames = onRequestInsertParamNames(i, s);
                        setAutomationTarget(AutomationParam::insertParam, s, p,
                            juce::isPositiveAndBelow(p, insNames.size()) ? insNames[p]
                                                                        : ("Param " + juce::String(p + 1)));
                    }
                });
            return true;
        }

        const auto grid = automationLaneGrid(i);
        if (event.x < grid.getX())
            return false;   // header side — let normal handling (track select etc.) run

        const int hit = automationPointAt(i, event.getPosition());

        // Delete a point with right-click / ctrl-click.
        if (hit >= 0 && event.mods.isPopupMenu())
        {
            captureUndoSnapshot();
            {
                const juce::ScopedLock sl(project.getAudioEditLock());
                auto& pts = tracks[static_cast<std::size_t>(i)]
                                .laneFor(automationParam, automationTargetIndex, automationParamIndex).points;
                pts.erase(pts.begin() + hit);
            }
            repaint();
            return true;
        }

        int pointIndex = hit;
        if (pointIndex < 0)
        {
            // Add a new point ON the current curve (its value at this beat), like Ableton — a click just
            // inserts a point without jumping its height; you set the value by dragging afterwards.
            const double beat = juce::jmax(0.0, xToBeatPosition(event.x));
            auto& track = tracks[static_cast<std::size_t>(i)];
            const float fallback = automationParam == AutomationParam::trackVolume
                                       ? static_cast<float>(track.volumeDb)
                                       : (automationParam == AutomationParam::trackPan
                                              ? static_cast<float>(track.pan)
                                              : automationParamDefault(automationParam));
            const auto* existing = track.findAutomation(automationParam, automationTargetIndex, automationParamIndex);
            const float lineVal = existing != nullptr ? existing->valueAt(beat, fallback) : fallback;

            // Only grab the line when the click is actually ON it (within a few px). A click elsewhere in
            // the lane does nothing — so you can't nudge the level by clicking near, but not on, the line
            // (Ableton behaviour). Consume the event so it also doesn't start a clip drag.
            if (std::abs(static_cast<float>(event.y) - automationValueToY(lineVal, grid)) > 6.0f)
                return true;

            captureUndoSnapshot();
            const juce::ScopedLock sl(project.getAudioEditLock());   // laneFor may reallocate the lane vector
            auto& laneRef = track.laneFor(automationParam, automationTargetIndex, automationParamIndex);
            pointIndex = laneRef.addPoint(beat, lineVal);
        }
        else
        {
            captureUndoSnapshot();   // grabbing an existing point
        }
        automationDragTrack = i;
        automationDragPoint = pointIndex;
        repaint();
        return true;
    }
    return false;
}

bool ArrangementTimelineComponent::handleAutomationMouseDrag(const juce::MouseEvent& event)
{
    if (! automationMode || automationDragTrack < 0 || automationDragPoint < 0)
        return false;

    auto& tracks = project.getTracks();
    if (automationDragTrack >= static_cast<int>(tracks.size()))
        return false;
    const auto grid = automationLaneGrid(automationDragTrack);

    const juce::ScopedLock sl(project.getAudioEditLock());
    auto& pts = tracks[static_cast<std::size_t>(automationDragTrack)]
                    .laneFor(automationParam, automationTargetIndex, automationParamIndex).points;
    if (automationDragPoint >= static_cast<int>(pts.size()))
        return false;

    // Keep beat ordering: clamp between neighbours so the drag never reorders the list.
    double beat = juce::jmax(0.0, xToBeatPosition(event.x));
    if (automationDragPoint > 0)
        beat = juce::jmax(beat, pts[static_cast<std::size_t>(automationDragPoint - 1)].beat + 1.0e-4);
    if (automationDragPoint < static_cast<int>(pts.size()) - 1)
        beat = juce::jmin(beat, pts[static_cast<std::size_t>(automationDragPoint + 1)].beat - 1.0e-4);
    pts[static_cast<std::size_t>(automationDragPoint)].beat = beat;
    pts[static_cast<std::size_t>(automationDragPoint)].value = automationYToValue(static_cast<float>(event.y), grid);
    repaint();
    return true;
}

bool ArrangementTimelineComponent::handleAutomationMouseUp()
{
    if (! automationMode || automationDragTrack < 0)
        return false;
    automationDragTrack = automationDragPoint = -1;
    return true;
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
    bounds.removeFromTop(timelineTopChromeFor(*this));
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

double ArrangementTimelineComponent::currentGridBeats() const noexcept
{
    const double beatsPerBar = static_cast<double>(juce::jmax(1, project.getNumerator()));

    // The base division. Adaptive: the finest musical division whose on-screen spacing is still at
    // least a few pixels, so grid lines stay readable at any zoom (Ableton's zoom-adaptive grid).
    // Fixed: a constant 1/16. The user's density offset (Cmd+1/2) then shifts it finer/coarser.
    double base = snapSizeInBeats;   // fixed default = 1/16 (0.25 beat)
    if (gridAdaptive && pixelsPerBeat > 0.0)
    {
        constexpr double minSpacingPx = 16.0;   // wider grid step (≈ 1-bar cells at the default zoom, like Ableton)
        const double ladder[] = { 0.125, 0.25, 0.5, 1.0, 2.0, beatsPerBar,
                                  beatsPerBar * 2.0, beatsPerBar * 4.0 };
        base = ladder[std::size(ladder) - 1];
        for (const double d : ladder)
            if (d * pixelsPerBeat >= minSpacingPx) { base = d; break; }
    }

    base *= std::pow(2.0, static_cast<double>(-gridDensityOffset));   // +offset = finer
    if (gridTriplet)
        base *= 2.0 / 3.0;
    return juce::jmax(1.0e-4, base);
}

juce::String ArrangementTimelineComponent::gridStepName() const
{
    if (! gridSnapEnabled)
        return "Off";
    const double beatsPerBar = static_cast<double>(juce::jmax(1, project.getNumerator()));
    // Name from the straight (non-triplet) division, then append "T" for triplets, like Ableton.
    double straight = currentGridBeats();
    if (gridTriplet)
        straight *= 3.0 / 2.0;
    const juce::String suffix = gridTriplet ? "T" : "";

    if (straight >= beatsPerBar - 1.0e-6)
    {
        const int bars = juce::jmax(1, static_cast<int>(std::round(straight / beatsPerBar)));
        return juce::String(bars) + (bars == 1 ? " Bar" : " Bars") + suffix;
    }
    const int denom = juce::jmax(1, static_cast<int>(std::round(4.0 / straight)));   // 0.25 beat → 1/16
    return "1/" + juce::String(denom) + suffix;
}

void ArrangementTimelineComponent::adjustGridDensity(int steps)
{
    gridDensityOffset = juce::jlimit(-4, 6, gridDensityOffset + steps);
    repaint();
}

void ArrangementTimelineComponent::toggleGridTriplet()  { gridTriplet = ! gridTriplet; repaint(); }
void ArrangementTimelineComponent::toggleGridSnap()     { gridSnapEnabled = ! gridSnapEnabled; repaint(); }
void ArrangementTimelineComponent::toggleGridAdaptive() { gridAdaptive = ! gridAdaptive; repaint(); }

double ArrangementTimelineComponent::snapBeatValue(double beat) const noexcept
{
    if (snapBypass || ! gridSnapEnabled)
        return beat;   // Cmd held (or snap off) → free movement
    const double step = currentGridBeats();
    return std::round(beat / step) * step;
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

void ArrangementTimelineComponent::moveDraggedClipsByTrackDelta(int deltaTracks)
{
    if (! dragState.has_value() || deltaTracks == 0 || dragState->clipItems.empty())
        return;

    auto& tracks = project.getTracks();
    const int trackCount = static_cast<int>(tracks.size());

    // Validate first: every dragged clip must land on an existing, type-compatible track.
    for (const auto& item : dragState->clipItems)
    {
        const int src = item.clip.trackIndex;
        const int dst = src + deltaTracks;
        if (src < 0 || src >= trackCount || dst < 0 || dst >= trackCount)
            return;
        const auto& srcClips = tracks[static_cast<std::size_t>(src)].clips;
        if (item.clip.clipIndex < 0 || item.clip.clipIndex >= static_cast<int>(srcClips.size()))
            return;
        if (! canClipLiveOnTrack(srcClips[static_cast<std::size_t>(item.clip.clipIndex)], dst))
            return;
    }

    // Snapshot the movers (by value) BEFORE erasing, so indices stay meaningful.
    struct Moving { TimelineClip clip; int targetTrack; };
    std::vector<Moving> moving;
    moving.reserve(dragState->clipItems.size());
    for (const auto& item : dragState->clipItems)
        moving.push_back({ tracks[static_cast<std::size_t>(item.clip.trackIndex)]
                               .clips[static_cast<std::size_t>(item.clip.clipIndex)],
                           item.clip.trackIndex + deltaTracks });

    // Erase originals highest-index-first so earlier indices stay valid.
    auto ordered = dragState->clipItems;
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
    {
        if (a.clip.trackIndex != b.clip.trackIndex) return a.clip.trackIndex > b.clip.trackIndex;
        return a.clip.clipIndex > b.clip.clipIndex;
    });

    const juce::ScopedLock sl(project.getAudioEditLock());   // clips vectors reallocate — guard the audio thread
    for (const auto& item : ordered)
    {
        auto& srcClips = tracks[static_cast<std::size_t>(item.clip.trackIndex)].clips;
        srcClips.erase(srcClips.begin() + item.clip.clipIndex);
    }

    std::vector<DragState::ClipItem> newItems;
    std::vector<SelectedClip> newSelection;
    newItems.reserve(moving.size());
    newSelection.reserve(moving.size());
    for (std::size_t i = 0; i < moving.size(); ++i)
    {
        auto& dstClips = tracks[static_cast<std::size_t>(moving[i].targetTrack)].clips;
        dstClips.push_back(moving[i].clip);
        const SelectedClip sc { moving[i].targetTrack, static_cast<int>(dstClips.size()) - 1 };
        newItems.push_back(DragState::ClipItem { sc,
                                                 dragState->clipItems[i].originalStartBeat,
                                                 dragState->clipItems[i].originalLengthInBeats });
        newSelection.push_back(sc);
    }

    dragState->clipItems = std::move(newItems);
    dragState->clip = dragState->clipItems.front().clip;
    selectedClips = std::move(newSelection);
    selectedClip = selectedClips.back();
    lastClickedClip = selectedClip;
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
    undoStack.push_back(TimelineSnapshot { project.getTracks(), selectedClip, project.getChordTrack() });
    redoStack.clear();
}

void ArrangementTimelineComponent::restoreSnapshot(const TimelineSnapshot& snapshot)
{
    {
        const juce::ScopedLock sl(project.getAudioEditLock());
        auto& live = project.getTracks();

        // Ableton treats mute / solo / record-arm as performance controls, NOT undoable edits
        // (their forum confirms solo is deliberately excluded from undo). So undo/redo must LEAVE
        // these live: keep each existing track's current states across the restore. Otherwise
        // undoing a clip/gain edit silently flips a track's mute/solo or re-arms record — which is
        // exactly what deleted the clip and "jumped to R" here.
        auto restored = snapshot.tracks;
        for (std::size_t i = 0; i < restored.size() && i < live.size(); ++i)
        {
            restored[i].muted       = live[i].muted;
            restored[i].solo        = live[i].solo;
            restored[i].recordArmed = live[i].recordArmed;
        }

        live = std::move(restored);
        project.getChordTrack() = snapshot.chordTrack;
    }
    selectedChords.clear();
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
        tracksBounds.removeFromTop(timelineTopChromeFor(*this));
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
