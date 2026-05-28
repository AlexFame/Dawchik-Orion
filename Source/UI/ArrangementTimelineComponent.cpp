#include "ArrangementTimelineComponent.h"

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
constexpr auto minimumClipLengthInBeats = 1.0;
constexpr auto snapSizeInBeats = 0.25;
constexpr auto trackHeaderWidth = 214;
constexpr auto beatEpsilon = 0.0001;
constexpr auto defaultLaneHeight = 78;
constexpr auto minimumCompressedLaneHeight = 28;
constexpr auto loopLaneHeight = 11;
constexpr auto loopHandleHitWidth = 8;
constexpr auto playheadHitWidth = 8;
constexpr auto inspectorResizeHandleHeight = 12;
constexpr auto minExpandedLaneHeight = 104;
constexpr auto maxExpandedLaneHeight = 240;
constexpr auto minPixelsPerBeat = 4.0;
constexpr auto maxPixelsPerBeat = 160.0;
constexpr auto minTimelineLengthInBeats = 256.0;
constexpr auto timelinePaddingInBeats = 64.0;
const std::array<juce::Colour, 7> trackPalette {
    juce::Colour(0xffd47a35),
    juce::Colour(0xffc85f57),
    juce::Colour(0xff5d82d8),
    juce::Colour(0xff7866b9),
    juce::Colour(0xff46a997),
    juce::Colour(0xffc99b42),
    juce::Colour(0xff6fae67)
};

juce::Rectangle<int> getTimelineContentBounds(const juce::Component& component)
{
    auto bounds = component.getLocalBounds();
    bounds.removeFromLeft(18);
    bounds.removeFromRight(18);
    bounds.removeFromTop(18);
    return bounds;
}

juce::Rectangle<int> getVisibleTrackAreaBounds(const juce::Component& component)
{
    auto bounds = getTimelineContentBounds(component);
    bounds.removeFromTop(42);
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
};

// Parses musical key from a filename. Strict — only accepts standalone token candidates
// (rejects words like "Bass", "Drum" that start with a note letter). Handles "Cm",
// "C minor", "C_minor", "Cmin", "Cmaj", "C# minor", "Db", "Fsharp", "Bbm" etc.
struct ParsedClipKey { int root { -1 }; bool minor { false }; };
ParsedClipKey parseKeyFromClipFileName(const juce::File& file)
{
    auto raw  = file.getFileNameWithoutExtension();
    auto text = juce::String(' ') + raw.toLowerCase().replaceCharacters("_-.()[]", "       ") + juce::String(' ');

    auto letterToSemi = [](juce::juce_wchar c) -> int
    {
        switch (c)
        {
            case 'c': return 0;
            case 'd': return 2;
            case 'e': return 4;
            case 'f': return 5;
            case 'g': return 7;
            case 'a': return 9;
            case 'b': return 11;
        }
        return -1;
    };
    auto isLetter = [](juce::juce_wchar c) { return c >= 'a' && c <= 'z'; };

    ParsedClipKey best { -1, false };
    int bestScore = 0;

    for (int i = 1; i + 1 < text.length(); ++i)
    {
        const auto prev = text[i - 1];
        if (prev != ' ' && prev != '\t') continue;       // word boundary
        const auto c = text[i];
        const auto semi = letterToSemi(c);
        if (semi < 0) continue;

        int rootSemi = semi;
        int pos = i + 1;
        bool hadAccidental = false;

        // Optional sharp/flat
        if (pos < text.length())
        {
            const auto a = text[pos];
            if (a == '#')
            {
                rootSemi = (rootSemi + 1) % 12; ++pos; hadAccidental = true;
            }
            else if (a == 'b' && pos + 1 < text.length())
            {
                // Accept 'b' as flat only when followed by 'm' (Cbm), space, or digit (Cb 120bpm).
                const auto next = text[pos + 1];
                if (next == 'm' || next == ' ' || next == '\t' || (next >= '0' && next <= '9'))
                {
                    rootSemi = (rootSemi + 11) % 12; ++pos; hadAccidental = true;
                }
            }
        }

        // Look at what immediately follows root+accidental.
        int modePos = pos;
        while (modePos < text.length() && (text[modePos] == ' ' || text[modePos] == '\t')) ++modePos;
        const bool skippedSpace = modePos > pos;

        bool isMinor = false, modeKnown = false, isValid = false;

        if (modePos >= text.length())
        {
            // End of name — standalone note → default major.
            isValid = true;
        }
        else
        {
            const auto rem = text.substring(modePos, juce::jmin(modePos + 6, text.length()));
            if      (rem.startsWith("minor")) { isMinor = true;  modeKnown = true; isValid = true; }
            else if (rem.startsWith("major")) { isMinor = false; modeKnown = true; isValid = true; }
            else if (rem.startsWith("min"))   { isMinor = true;  modeKnown = true; isValid = true; }
            else if (rem.startsWith("maj"))   { isMinor = false; modeKnown = true; isValid = true; }
            else if (! skippedSpace && text[pos] == 'm')
            {
                // "Cm" — 'm' immediately after root (no space). Reject if followed by another letter
                // that's not part of a mode suffix.
                if (pos + 1 >= text.length() || ! isLetter(text[pos + 1]))
                {
                    isMinor = true; modeKnown = true; isValid = true;
                }
            }
            else if (skippedSpace)
            {
                // Standalone note followed by space and something non-mode → default major.
                isValid = true;
            }
            // else: root immediately followed by a letter that's not m/maj/min → false positive (e.g. "Bass", "Drum"). Reject.
        }

        if (! isValid) continue;

        // Explicit mode strongly preferred. Accidentals also score higher.
        const int score = (modeKnown ? 1000 : 100) + (hadAccidental ? 50 : 0) + juce::jmax(0, 100 - i);
        if (score > bestScore)
        {
            bestScore = score;
            best.root = rootSemi;
            best.minor = isMinor;
        }
    }
    return best;
}

double parseBpmFromFileName(const juce::File& file)
{
    auto text = file.getFileNameWithoutExtension().toLowerCase();
    const auto bpmIndex = text.indexOf("bpm");
    if (bpmIndex < 0)
    {
        if (! (text.contains("loop") || text.contains("break") || text.contains("drum") || text.contains("beat")))
            return 0.0;

        double bestBpm = 0.0;
        int bestDigitCount = 0;
        juce::String currentNumber;

        const auto textWithDelimiter = text + " ";
        for (int i = 0; i < textWithDelimiter.length(); ++i)
        {
            const auto character = textWithDelimiter[i];
            if (juce::CharacterFunctions::isDigit(character))
            {
                currentNumber += juce::String::charToString(character);
                continue;
            }

            if (currentNumber.isNotEmpty())
            {
                const auto candidateBpm = currentNumber.getDoubleValue();
                if (candidateBpm >= 40.0 && candidateBpm <= 260.0 && currentNumber.length() >= bestDigitCount)
                {
                    bestBpm = candidateBpm;
                    bestDigitCount = currentNumber.length();
                }

                currentNumber.clear();
            }
        }

        return bestBpm;
    }

    auto start = bpmIndex - 1;
    while (start >= 0 && (juce::CharacterFunctions::isDigit(text[start]) || text[start] == ' ' || text[start] == '_' || text[start] == '-'))
        --start;

    auto numberText = text.substring(start + 1, bpmIndex).retainCharacters("0123456789").trim();
    if (numberText.isEmpty())
        return 0.0;

    const auto bpm = numberText.getDoubleValue();
    return (bpm >= 40.0 && bpm <= 260.0) ? bpm : 0.0;
}

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

    static juce::AudioFormatManager audioFormatManager;
    static bool formatsRegistered = false;
    if (! formatsRegistered)
    {
        audioFormatManager.registerBasicFormats();
        formatsRegistered = true;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return result;

    const auto durationSeconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    const auto beatsPerSecond = tempoBpm / 60.0;
    const auto durationInBeats = durationSeconds * beatsPerSecond;
    result.durationSeconds = durationSeconds;
    result.clipLengthInBeats = juce::jmax(minimumClipLengthInBeats, durationInBeats);

    // Parse key from filename so the dropped clip is auto-pitched to the project key.
    const auto parsedKey = parseKeyFromClipFileName(file);
    result.sourceKeyRoot    = parsedKey.root;
    result.sourceKeyIsMinor = parsedKey.minor;

    const auto nameBpm = parseBpmFromFileName(file);
    if (nameBpm > 0.0)
    {
        result.sourceBpm = nameBpm;
        result.bpmSource = "filename";
        result.bpmConfidence = 1.0;
        result.bpmGuessed = false;
        const auto sourceBeats = durationSeconds * (nameBpm / 60.0);
        const auto snappedBeats = std::round(sourceBeats / snapSizeInBeats) * snapSizeInBeats;
        result.clipLengthInBeats = juce::jmax(minimumClipLengthInBeats, snappedBeats);

        const auto roundedBeats = std::round(sourceBeats);
        const auto beatsPerBar = static_cast<double>(juce::jmax(1, numerator));
        const auto roundedBars = static_cast<int>(std::round(roundedBeats / beatsPerBar));
        if (roundedBars > 0 && std::abs(roundedBeats - static_cast<double>(roundedBars) * beatsPerBar) <= 0.25)
            result.detectedBars = roundedBars;

        DBG("[Warp-Import] " + file.getFileName() + " | bpmSource=filename | confidence=1.0 | sourceBpm=" + juce::String(nameBpm, 1));
        return result;
    }

    constexpr double neutralCenter = 128.0;
    constexpr int commonLoopBars[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
    double bestBpm = 0.0;
    int bestBars = 0;
    double bestScore = std::numeric_limits<double>::max();

    for (const auto bars : commonLoopBars)
    {
        const auto loopBeats = static_cast<double>(bars * juce::jmax(1, numerator));
        const auto candidateBpm = (loopBeats / durationSeconds) * 60.0;
        if (candidateBpm < 70.0 || candidateBpm > 180.0)
            continue;

        const auto score = std::abs(candidateBpm - neutralCenter);
        if (score < bestScore)
        {
            bestScore = score;
            bestBpm = candidateBpm;
            bestBars = bars;
        }
    }

    if (bestBars > 0)
    {
        result.sourceBpm = bestBpm;
        result.detectedBars = bestBars;
        result.clipLengthInBeats = juce::jmax(minimumClipLengthInBeats, static_cast<double>(bestBars * juce::jmax(1, numerator)));
        result.bpmSource = "duration-bars";
        result.bpmGuessed = true;

        // Confidence: how close the detected beats align to perfect bar boundaries
        const auto beatsPerBar = static_cast<double>(juce::jmax(1, numerator));
        const auto sourceBeats = durationSeconds * (bestBpm / 60.0);
        const auto barFraction = std::abs(sourceBeats - std::round(sourceBeats / beatsPerBar) * beatsPerBar);
        result.bpmConfidence = juce::jlimit(0.0, 1.0, 1.0 - barFraction / beatsPerBar);

        DBG("[Warp-Import] " + file.getFileName()
            + " | bpmSource=duration-bars | confidence=" + juce::String(result.bpmConfidence, 2)
            + " | sourceBpm=" + juce::String(bestBpm, 1)
            + " | bars=" + juce::String(bestBars));
    }
    else
    {
        // No usable BPM candidate — warp cannot be applied
        DBG("[Warp-Import] " + file.getFileName() + " | bpmSource=none | confidence=0 | sourceBpm=0");
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
    startTimerHz(120);
}

ArrangementTimelineComponent::~ArrangementTimelineComponent() = default;

bool ArrangementTimelineComponent::canUndo() const noexcept
{
    return ! undoStack.empty();
}

std::optional<int> ArrangementTimelineComponent::getSelectedTrackIndex() const noexcept
{
    return selectedTrackIndex;
}

bool ArrangementTimelineComponent::canRedo() const noexcept
{
    return ! redoStack.empty();
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

void ArrangementTimelineComponent::addAudioTrack()
{
    pushUndoSnapshot();
    const auto index = static_cast<int>(project.getTracks().size());
    project.getTracks().push_back(TrackState {
        makeUniqueTrackName("Audio Track"),
        false,
        trackPalette[static_cast<std::size_t>(index % static_cast<int>(trackPalette.size()))],
        false,
        false,
        false,
        0.0,
        {}
    });

    setSingleSelection(std::nullopt);
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::addMidiTrack()
{
    pushUndoSnapshot();
    const auto index = static_cast<int>(project.getTracks().size());
    project.getTracks().push_back(TrackState {
        makeUniqueTrackName("MIDI Track"),
        true,
        trackPalette[static_cast<std::size_t>(index % static_cast<int>(trackPalette.size()))],
        false,
        false,
        false,
        0.0,
        {}
    });

    setSingleSelection(std::nullopt);
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
    auto rulerArea = bounds.removeFromTop(42);
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
    const auto addTrackButton = getAddTrackButtonBounds();

    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.fillRoundedRectangle(addTrackButton.toFloat(), 6.0f);
    g.setColour(juce::Colours::white.withAlpha(0.70f));
    g.drawRoundedRectangle(addTrackButton.toFloat(), 6.0f, 1.0f);
    g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    g.drawText("+", addTrackButton, juce::Justification::centred);

    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.fillRoundedRectangle(loopLane.toFloat(), 4.0f);

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
    const auto barPixelWidth = pixelsPerBeat * beatsPerBar;
    const auto minGridSpacingPixels = 28.0;
    const auto minLabelSpacingPixels = 104.0;
    const auto sectionStepBeats = beatsPerBarInt * 16;
    const auto majorStepBeats = beatsPerBarInt * 4;
    const auto barStepBeats = beatsPerBarInt;
    const auto beatStepBeats = 1;
    auto labelStepBars = 1;
    while (static_cast<double>(labelStepBars) * barPixelWidth < minLabelSpacingPixels)
        labelStepBars *= 2;
    const auto labelStepBeats = beatsPerBarInt * labelStepBars;

    auto drawGridLayer = [&](juce::Rectangle<int> verticalArea, int stepBeats, juce::Colour colour, float thickness)
    {
        if (stepBeats <= 0 || pixelsPerBeat * static_cast<double>(stepBeats) < minGridSpacingPixels)
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
    drawGridLayer(rulerGridArea, beatStepBeats, minorGridColour.withAlpha(0.22f), 1.0f);
    drawGridLayer(rulerGridArea, barStepBeats, majorGridColour.withAlpha(0.36f), 1.0f);
    drawGridLayer(rulerGridArea, majorStepBeats, majorGridColour.withAlpha(0.56f), 1.3f);
    drawGridLayer(rulerGridArea, sectionStepBeats, majorGridColour.withAlpha(0.72f), 1.6f);

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
        const auto trackSelected = selectedClip.has_value() && selectedClip->trackIndex == trackIndex;
        const auto headerSelected = selectedTrackIndex.has_value() && *selectedTrackIndex == trackIndex;
        const auto rowSelected = trackSelected || headerSelected;

        if (rowSelected)
        {
            g.setColour(tracks[trackArrayIndex].colour.withAlpha(0.10f));
            g.fillRect(rowBounds);
        }

        g.setColour(tracks[trackArrayIndex].colour.withAlpha(rowSelected ? 0.95f : 0.72f));
        g.fillRect(trackNameArea.withWidth(4));

        g.setColour(juce::Colours::white.withAlpha(0.095f));
        g.drawHorizontalLine(rowBounds.getBottom(), static_cast<float>(rowBounds.getX()), static_cast<float>(rowBounds.getRight()));

        auto trackInner = trackNameArea.reduced(12, 10);
        auto titleRow = trackInner.removeFromTop(24);
        auto iconBox = titleRow.removeFromLeft(24);
        g.setColour(tracks[trackArrayIndex].colour.withAlpha(0.95f));
        g.fillRoundedRectangle(iconBox.toFloat(), 6.0f);
        g.setColour(textColour);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        titleRow.removeFromLeft(10);
        g.drawText(tracks[trackArrayIndex].name, titleRow, juce::Justification::centredLeft, true);

        trackInner.removeFromTop(10);
        auto controlsRow = trackInner.removeFromTop(20);

        const std::array<juce::String, 4> buttons { "M", "S", "R", "I" };
        for (const auto& buttonText : buttons)
        {
            auto buttonBounds = controlsRow.removeFromLeft(20);
            const auto active = (buttonText == "M" && tracks[trackArrayIndex].muted)
                || (buttonText == "S" && tracks[trackArrayIndex].solo)
                || (buttonText == "R" && tracks[trackArrayIndex].recordArmed)
                || (buttonText == "I" && headerSelected);
            g.setColour(active ? tracks[trackArrayIndex].colour.withAlpha(0.72f) : juce::Colours::white.withAlpha(0.12f));
            g.fillRoundedRectangle(buttonBounds.toFloat(), 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(buttonText, buttonBounds, juce::Justification::centred);
            controlsRow.removeFromLeft(6);
        }

        auto sliderBounds = controlsRow.removeFromLeft(72);
        g.setColour(juce::Colours::white.withAlpha(0.10f));
        g.fillRoundedRectangle(sliderBounds.toFloat(), 8.0f);
        g.setColour(juce::Colours::white.withAlpha(0.30f));
        const auto volumeRatio = juce::jmap(static_cast<float>(tracks[trackArrayIndex].volumeDb), -24.0f, 12.0f, 0.0f, 1.0f);
        auto thumbBounds = sliderBounds.withWidth(18).withX(sliderBounds.getX() + static_cast<int>(std::round(volumeRatio * static_cast<float>(sliderBounds.getWidth() - 18))));
        g.fillRoundedRectangle(thumbBounds.toFloat(), 8.0f);

        if (trackSelected)
        {
            auto resizeHandle = trackNameArea.reduced(24, 0);
            resizeHandle = resizeHandle.withY(lane.getBottom() - inspectorResizeHandleHeight - 2).withHeight(inspectorResizeHandleHeight);
            resizeHandle = resizeHandle.withX(resizeHandle.getX() + 20).withWidth(74);
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRoundedRectangle(resizeHandle.toFloat(), 5.0f);
            g.setColour(juce::Colours::white.withAlpha(0.30f));
            auto grip = resizeHandle.reduced(18, 4);
            g.fillRoundedRectangle(grip.toFloat(), 4.0f);
        }
    }

    g.reduceClipRegion(gridArea);

    drawGridLayer(visibleTracksArea, beatStepBeats, minorGridColour.withAlpha(0.20f), 1.0f);
    drawGridLayer(visibleTracksArea, barStepBeats, majorGridColour.withAlpha(0.30f), 1.0f);
    drawGridLayer(visibleTracksArea, majorStepBeats, majorGridColour.withAlpha(0.48f), 1.15f);
    drawGridLayer(visibleTracksArea, sectionStepBeats, majorGridColour.withAlpha(0.62f), 1.4f);

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
            auto clipContentBounds = clipBoundsInt.reduced(0, 8);
            const auto shouldDrawLabel = clipBoundsInt.getWidth() > 36;
            const auto shouldDrawWaveform = clipBoundsInt.getWidth() > 16;
            const auto clipHeaderHeight = juce::jmin(22, juce::jmax(16, clipContentBounds.getHeight() / 3));
            auto clipHeaderBounds = clipContentBounds.removeFromTop(clipHeaderHeight);
            auto clipBodyBounds = clipContentBounds;

            const auto isSelected = isClipSelected(SelectedClip { trackIndex, clipIndex });
            g.saveState();
            g.reduceClipRegion(clipBoundsInt);
            g.setColour(clip.colour.withSaturation(0.78f));
            g.fillRoundedRectangle(clipBounds, 10.0f);

            if (clip.type == ClipType::audio && shouldDrawWaveform)
            {
                const auto* peaks = getOrComputePeaks(clip.sourcePath);
                if (peaks != nullptr && ! peaks->minVals.empty())
                {
                    g.saveState();
                    g.reduceClipRegion(clipBoundsInt);
                    g.setColour(juce::Colours::white.withAlpha(0.7f));

                    const auto bodyX     = clipBodyBounds.getX();
                    const auto bodyWidth = juce::jmax(1, clipBodyBounds.getWidth());
                    const auto centerY   = static_cast<float>(clipBodyBounds.getY()) + clipBodyBounds.getHeight() * 0.5f;
                    const auto halfH     = juce::jmax(2.0f, static_cast<float>(clipBodyBounds.getHeight()) * 0.45f);
                    const auto numBuckets = static_cast<int>(peaks->minVals.size());

                    for (int px = 0; px < bodyWidth; ++px)
                    {
                        const auto bStart = static_cast<int>(static_cast<double>(px) * numBuckets / bodyWidth);
                        const auto bEnd   = static_cast<int>(static_cast<double>(px + 1) * numBuckets / bodyWidth);
                        const auto safeEnd = juce::jmax(bStart + 1, bEnd);

                        float minVal = 0.0f, maxVal = 0.0f;
                        for (int b = bStart; b < safeEnd && b < numBuckets; ++b)
                        {
                            minVal = juce::jmin(minVal, peaks->minVals[static_cast<size_t>(b)]);
                            maxVal = juce::jmax(maxVal, peaks->maxVals[static_cast<size_t>(b)]);
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

            if (isSelected)
            {
                g.setColour(juce::Colours::white.withAlpha(0.58f));
                g.drawRoundedRectangle(clipBounds.reduced(0.5f, 0.5f), 9.5f, 1.2f);
            }

            if (shouldDrawLabel)
            {
                g.setFont(juce::FontOptions(14.0f, juce::Font::plain));

                // Keep clip names readable by pinning them to a dedicated top header strip.
                g.setColour(juce::Colours::black.withAlpha(0.4f));
                g.drawText(clip.name, clipHeaderBounds.reduced(8, 0).translated(1, 1), juce::Justification::centredLeft, true);

                g.setColour(juce::Colours::white.withAlpha(0.92f));
                g.drawText(clip.name, clipHeaderBounds.reduced(8, 0), juce::Justification::centredLeft, true);
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

    if (browserDropPreviewBounds.has_value())
    {
        g.saveState();
        g.reduceClipRegion(visibleGridArea);
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

}

void ArrangementTimelineComponent::resized()
{
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::mouseDown(const juce::MouseEvent& event)
{
        auto bounds = getTimelineContentBounds(*this);
    auto rulerArea = bounds.removeFromTop(42);
    auto tracksArea = bounds;
    auto gridArea = tracksArea;
    gridArea.removeFromLeft(trackHeaderWidth);

    if (getAddTrackButtonBounds().contains(event.getPosition()))
    {
        showAddTrackMenu();
        return;
    }

    const auto playheadX = beatToX(transport.getPlayheadBeat(), gridArea);
    if ((rulerArea.contains(event.getPosition()) || tracksArea.contains(event.getPosition()))
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

    if (selectedClip.has_value())
    {
        auto selectedLane = getTrackLaneBounds(selectedClip->trackIndex);
        auto selectedHeader = selectedLane.removeFromLeft(trackHeaderWidth).reduced(8, 6);
        auto resizeHandle = selectedHeader.reduced(24, 0);
        resizeHandle = resizeHandle.withY(selectedLane.getBottom() - inspectorResizeHandleHeight - 2).withHeight(inspectorResizeHandleHeight);
        resizeHandle = resizeHandle.withX(resizeHandle.getX() + 20).withWidth(74);
        if (resizeHandle.contains(event.getPosition()))
        {
            inspectorResizeState.active = true;
            inspectorResizeState.mouseDownY = event.getPosition().y;
            inspectorResizeState.originalHeight = selectedTrackExpandedHeight;
            return;
        }
    }

    const auto trackHeaderHit = hitTestTrackHeader(event.getPosition());
    if (trackHeaderHit.has_value())
    {
        selectedTrackIndex = trackHeaderHit->trackIndex;
        setSingleSelection(std::nullopt);

        auto& track = project.getTracks()[static_cast<std::size_t>(trackHeaderHit->trackIndex)];
        if (trackHeaderHit->control == TrackHeaderControl::mute)
            track.muted = ! track.muted;
        else if (trackHeaderHit->control == TrackHeaderControl::solo)
            track.solo = ! track.solo;
        else if (trackHeaderHit->control == TrackHeaderControl::record)
            track.recordArmed = ! track.recordArmed;
        else if (trackHeaderHit->control == TrackHeaderControl::volume)
        {
            trackVolumeDragState = TrackVolumeDragState { true, trackHeaderHit->trackIndex, trackHeaderHit->bounds };
            updateTrackVolumeFromPoint(trackHeaderHit->trackIndex, trackHeaderHit->bounds, event.getPosition().x);
        }

        grabKeyboardFocus();
        repaint();
        return;
    }

    const auto hit = hitTestClipDetailed(event.getPosition(), false);
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
        const auto dragMode = hit->overResizeHandle
            ? (event.mods.isShiftDown() ? DragMode::stretchRight : DragMode::resizeRight)
            : DragMode::move;
        dragState = DragState {
            hit->clip,
            dragMode,
            event.getPosition(),
            clip.startBeat,
            clip.lengthInBeats,
            clip.warpTargetLengthInBeats > 0.0 ? clip.warpTargetLengthInBeats : clip.lengthInBeats,
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

    if (inspectorResizeState.active)
    {
        const auto deltaY = event.getPosition().y - inspectorResizeState.mouseDownY;
        selectedTrackExpandedHeight = juce::jlimit(minExpandedLaneHeight,
                                                   maxExpandedLaneHeight,
                                                   inspectorResizeState.originalHeight + deltaY);
        clampScrollOffsets();
        repaint();
        return;
    }

    if (playheadDragState.active)
    {
        transport.setPlayheadBeat(snapBeatValue(xToBeatPosition(event.getPosition().x)));
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

    if (dragState->mode == DragMode::move)
    {
        clip.startBeat = juce::jlimit(0.0, juce::jmax(0.0, getTimelineEndBeats() - clip.lengthInBeats), snapBeatValue(dragState->originalStartBeat + beatDelta));
    }
    else
    {
        const auto proposedLength = dragState->originalLengthInBeats + beatDelta;
        clip.lengthInBeats = juce::jlimit(
            minimumClipLengthInBeats,
            juce::jmax(minimumClipLengthInBeats, getTimelineEndBeats() - clip.startBeat),
            snapBeatValue(proposedLength));

        if (dragState->mode == DragMode::stretchRight && clip.type == ClipType::audio)
        {
            const auto proposedWarpLength = dragState->originalWarpTargetLengthInBeats + beatDelta;
            clip.warpTargetLengthInBeats = juce::jlimit(
                minimumClipLengthInBeats,
                juce::jmax(minimumClipLengthInBeats, getTimelineEndBeats() - clip.startBeat),
                snapBeatValue(proposedWarpLength));
        }
    }

    repaint();
}

void ArrangementTimelineComponent::mouseMove(const juce::MouseEvent& event)
{
    if (selectedClip.has_value())
    {
        auto selectedLane = getTrackLaneBounds(selectedClip->trackIndex);
        auto selectedHeader = selectedLane.removeFromLeft(trackHeaderWidth).reduced(8, 6);
        auto resizeHandle = selectedHeader.reduced(24, 0);
        resizeHandle = resizeHandle.withY(selectedLane.getBottom() - inspectorResizeHandleHeight - 2).withHeight(inspectorResizeHandleHeight);
        resizeHandle = resizeHandle.withX(resizeHandle.getX() + 20).withWidth(74);
        if (resizeHandle.contains(event.getPosition()))
        {
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            return;
        }
    }

    hoverClip = hitTestClipDetailed(event.getPosition(), false);
    setMouseCursor(hoverClip.has_value() && hoverClip->overResizeHandle
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : juce::MouseCursor::NormalCursor);
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
    playheadDragState.active = false;
    trackVolumeDragState.active = false;
    loopSelectionState.reset();
    selectionBoxState.active = false;

    if (dragState.has_value() && dragState->historyCaptured && ! undoStack.empty() && ! hasTimelineChangedSince(undoStack.back()))
    {
        undoStack.pop_back();
    }

    dragState.reset();
    setMouseCursor(hoverClip.has_value() && hoverClip->overResizeHandle
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : juce::MouseCursor::NormalCursor);
}

void ArrangementTimelineComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    auto bounds = getTimelineContentBounds(*this);
    auto rulerArea = bounds.removeFromTop(42);
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

    const auto hit = hitTestClip(event.getPosition(), true);
    if (hit.has_value())
    {
        setSingleSelection(hit);
        grabKeyboardFocus();
        repaint();

        if (onMidiClipDoubleClick)
            onMidiClipDoubleClick(hit->trackIndex, hit->clipIndex);
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

    if (clip.type != ClipType::midi || ! onMidiClipDoubleClick)
        return false;

    onMidiClipDoubleClick(selectedClip->trackIndex, selectedClip->clipIndex);
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

        for (int clipIndex = 0; clipIndex < static_cast<int>(clips.size()); ++clipIndex)
        {
            const auto& clip = clips[static_cast<std::size_t>(clipIndex)];
            const auto clipBounds = getClipBounds(clip, trackIndex);

            if (! clipBounds.contains(position))
                continue;

            if (midiOnly && clip.type != ClipType::midi)
                return std::nullopt;

            return ClipHit {
                SelectedClip { trackIndex, clipIndex },
                clipBounds,
                position.x >= clipBounds.getRight() - resizeHandleWidth
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
        auto lane = getTrackLaneBounds(trackIndex);
        auto trackNameArea = lane.removeFromLeft(trackHeaderWidth).reduced(8, 6);
        if (! trackNameArea.contains(position))
            continue;

        auto trackInner = trackNameArea.reduced(12, 10);
        trackInner.removeFromTop(24);
        trackInner.removeFromTop(10);
        auto controlsRow = trackInner.removeFromTop(20);
        const std::array<TrackHeaderControl, 4> controls {
            TrackHeaderControl::mute,
            TrackHeaderControl::solo,
            TrackHeaderControl::record,
            TrackHeaderControl::inspector
        };

        for (const auto control : controls)
        {
            auto buttonBounds = controlsRow.removeFromLeft(20);
            if (buttonBounds.contains(position))
                return TrackHeaderHit { trackIndex, control, buttonBounds };

            controlsRow.removeFromLeft(6);
        }

        auto sliderBounds = controlsRow.removeFromLeft(72);
        if (sliderBounds.contains(position))
            return TrackHeaderHit { trackIndex, TrackHeaderControl::volume, sliderBounds };

        return TrackHeaderHit { trackIndex, TrackHeaderControl::none, trackNameArea };
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
    auto rulerArea = bounds.removeFromTop(42);
    auto headerArea = rulerArea.removeFromLeft(trackHeaderWidth);
    return headerArea.withSizeKeepingCentre(28, 28);
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
    bounds.removeFromTop(42);

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

    if (isVerticalIntent)
        return;

    if (isExplicitHorizontal && visibleGridArea.contains(event.getPosition()))
    {
        const auto previousScrollX = scrollX;
        scrollX -= horizontalDelta * 600.0;
        clampScrollOffsets();

        if (std::abs(scrollX - previousScrollX) > 0.01)
            repaint();
    }
}

void ArrangementTimelineComponent::mouseMagnify(const juce::MouseEvent& event, float scaleFactor)
{
    auto bounds = getTimelineContentBounds(*this);
    auto gridArea = bounds.withTrimmedTop(42);
    gridArea.removeFromLeft(trackHeaderWidth);

    if (! gridArea.contains(event.getPosition()))
        return;

    const auto rawDelta = (static_cast<double>(scaleFactor) - 1.0) * 3.0;
    pendingMagnifyDelta += rawDelta;
    ignoreWheelUntilMs = juce::Time::getMillisecondCounterHiRes() + 120.0;

    if (std::abs(pendingMagnifyDelta) < 0.005)
        return;

    const auto stableDelta = pendingMagnifyDelta;
    pendingMagnifyDelta = 0.0;
    adjustZoom(stableDelta, event.getPosition());
}

bool ArrangementTimelineComponent::isInterestedInDragSource(const SourceDetails& dragSourceDetails)
{
    const auto* payload = dragSourceDetails.description.getDynamicObject();
    return payload != nullptr && payload->getProperty("type") == "browser-item";
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

    if (targetTrackIndex < 0)
    {
        auto tracksBounds = getTimelineContentBounds(*this);
        tracksBounds.removeFromTop(42);

        const auto newTrackIndex = static_cast<int>(project.getTracks().size());
        const auto newTrackLane = getTrackLaneBounds(newTrackIndex);
        if (dragSourceDetails.localPosition.y >= newTrackLane.getY()
            && dragSourceDetails.localPosition.y < newTrackLane.getBottom()
            && tracksBounds.contains(tracksBounds.getX() + 1, dragSourceDetails.localPosition.y))
        {
            targetTrackIndex = newTrackIndex;
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
    const auto fallbackLengthBeats = fallbackClipLengthInBeats(*payload);
    const auto analysis = analyzeImportedAudioClip(sourceFile, project.getTempoBpm(), project.getNumerator(), fallbackLengthBeats);
    const auto lengthBeats = juce::jmax(minimumClipLengthInBeats, analysis.clipLengthInBeats);
    const auto startBeat = juce::jlimit(
        0.0,
        juce::jmax(0.0, getTimelineEndBeats() - lengthBeats),
        snapBeatValue(xToBeatPosition(dragSourceDetails.localPosition.x)));

    pushUndoSnapshot();
    const auto clipColour = juce::Colour(static_cast<juce::uint32>(static_cast<int>(payload->getProperty("colour"))));
    const auto clipName = clipNameForImportedFile(sourceFile, *payload);

    if (createNewTrack)
    {
        const auto categoryName = payload->getProperty("category").toString();
        tracks.push_back(TrackState {
            categoryName + " Track",
            false,
            clipColour,
            false,
            false,
            false,
            0.0,
            {}
        });
        targetTrackIndex = static_cast<int>(tracks.size()) - 1;
    }

    tracks[static_cast<std::size_t>(targetTrackIndex)].clips.push_back(TimelineClip {
        clipName,
        ClipType::audio,
        startBeat,
        lengthBeats,
        clipColour,
        {},
        sourceFile.getFullPathName(),
        0.0,
        false,
        false,
        analysis.durationSeconds,
        analysis.sourceBpm,
        analysis.detectedBars,
        true,
        analysis.bpmGuessed,
        lengthBeats,
        analysis.sourceKeyRoot,
        analysis.sourceKeyIsMinor,
        true   // keyShiftEnabled — auto pitch to project key by default
    });

    setSingleSelection(SelectedClip { targetTrackIndex, static_cast<int>(tracks[static_cast<std::size_t>(targetTrackIndex)].clips.size()) - 1 });

    clearBrowserDropPreview();
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::adjustZoom(double horizontalDelta, std::optional<juce::Point<int>> focusPoint)
{
    auto bounds = getTimelineContentBounds(*this);
    auto gridArea = bounds.withTrimmedTop(42);
    gridArea.removeFromLeft(trackHeaderWidth);

    const auto fullWidth = static_cast<double>(gridArea.getWidth());
    double focusXInView = fullWidth * 0.5;
    if (focusPoint.has_value())
        focusXInView = juce::jlimit(0.0, fullWidth, static_cast<double>(focusPoint->x - gridArea.getX()));

    const auto oldPixelsPerBeat = pixelsPerBeat;
    const auto focusBeat = oldPixelsPerBeat > 0.0 ? (scrollX + focusXInView) / oldPixelsPerBeat : 0.0;
    const auto keepTimelineStartAnchored = scrollX <= 0.0;
    const auto zoomFactor = std::pow(1.2, horizontalDelta);

    pixelsPerBeat = juce::jlimit(minPixelsPerBeat, maxPixelsPerBeat, pixelsPerBeat * zoomFactor);
    scrollX = keepTimelineStartAnchored ? 0.0 : (focusBeat * pixelsPerBeat) - focusXInView;
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::timerCallback()
{
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
    
    // Tracks compress into the current visible area instead of scrolling behind the docked sampler.
    scrollY = 0.0;
}

double ArrangementTimelineComponent::getTimelineEndBeats() const noexcept
{
    return juce::jmax(minTimelineLengthInBeats, project.getContentEndInBeats() + timelinePaddingInBeats);
}

int ArrangementTimelineComponent::getLaneHeightForTrack(int trackIndex) const noexcept
{
    juce::ignoreUnused(trackIndex);

    const auto trackCount = static_cast<int>(project.getTracks().size());
    if (trackCount <= 0)
        return defaultLaneHeight;

    const auto visibleHeight = getVisibleTrackAreaBounds(*this).getHeight();
    if (visibleHeight <= 0)
        return defaultLaneHeight;

    const auto compressedHeight = visibleHeight / trackCount;
    return juce::jlimit(minimumCompressedLaneHeight, defaultLaneHeight, compressedHeight);
}

int ArrangementTimelineComponent::getTrackTopForIndex(int trackIndex) const noexcept
{
    return juce::jmax(0, trackIndex) * getLaneHeightForTrack(trackIndex);
}

int ArrangementTimelineComponent::getTotalTrackHeight() const noexcept
{
    const auto trackCount = static_cast<int>(project.getTracks().size());
    return trackCount * getLaneHeightForTrack(0);
}

juce::Rectangle<int> ArrangementTimelineComponent::getTrackLaneBounds(int trackIndex) const noexcept
{
    const auto bounds = getVisibleTrackAreaBounds(*this);
    const auto laneHeight = getLaneHeightForTrack(trackIndex);
    const int laneTop = bounds.getY() + getTrackTopForIndex(trackIndex);
    return juce::Rectangle<int>(bounds.getX(), laneTop, bounds.getWidth(), laneHeight);
}

juce::Rectangle<int> ArrangementTimelineComponent::getClipBounds(const TimelineClip& clip, int trackIndex) const noexcept
{
    auto lane = getTrackLaneBounds(trackIndex);
    auto clipLane = lane;
    clipLane.removeFromLeft(trackHeaderWidth);
    const auto clipX = beatToX(clip.startBeat, clipLane);
    const auto clipEndX = beatToX(clip.startBeat + clip.lengthInBeats, clipLane);
    return juce::Rectangle<int>(
        static_cast<int>(clipX + 1.0f),
        lane.getY() + 1,
        static_cast<int>(juce::jmax(1.0f, clipEndX - clipX - 2.0f)),
        juce::jmax(1, lane.getHeight() - 2));
}

double ArrangementTimelineComponent::xToBeatDelta(int xDelta) const noexcept
{
    return pixelsPerBeat > 0.0 ? static_cast<double>(xDelta) / pixelsPerBeat : 0.0;
}

double ArrangementTimelineComponent::xToBeatPosition(int x) const noexcept
{
    auto bounds = getTimelineContentBounds(*this);
    bounds.removeFromTop(42);
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

    auto targetTrackIndex = trackIndexFromY(position.y);
    bool createNewTrack = false;

    if (targetTrackIndex < 0)
    {
        auto tracksBounds = getTimelineContentBounds(*this);
        tracksBounds.removeFromTop(42);

        const auto newTrackIndex = static_cast<int>(project.getTracks().size());
        const auto newTrackLane = getTrackLaneBounds(newTrackIndex);
        if (position.y >= newTrackLane.getY()
            && position.y < newTrackLane.getBottom()
            && tracksBounds.contains(tracksBounds.getX() + 1, position.y))
        {
            targetTrackIndex = newTrackIndex;
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

    const auto sourceFile = juce::File(payload->getProperty("path").toString());
    const auto fallbackLengthBeats = fallbackClipLengthInBeats(*payload);
    const auto analysis = analyzeImportedAudioClip(sourceFile, project.getTempoBpm(), project.getNumerator(), fallbackLengthBeats);
    const auto lengthBeats = juce::jmax(minimumClipLengthInBeats, analysis.clipLengthInBeats);
    const auto startBeat = juce::jlimit(
        0.0,
        juce::jmax(0.0, getTimelineEndBeats() - lengthBeats),
        snapBeatValue(xToBeatPosition(position.x)));

    const TimelineClip previewClip {
        payload->getProperty("name").toString(),
        ClipType::audio,
        startBeat,
        lengthBeats,
        juce::Colour(static_cast<juce::uint32>(static_cast<int>(payload->getProperty("colour")))),
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
    browserDropPreviewBounds = getClipBounds(previewClip, targetTrackIndex);
    repaint();
}

void ArrangementTimelineComponent::notifyClipSelectionChanged()
{
    if (onClipSelectionChanged == nullptr)
        return;

    if (selectedClip.has_value())
        onClipSelectionChanged(selectedClip->trackIndex, selectedClip->clipIndex);
    else
        onClipSelectionChanged(-1, -1);
}

std::optional<juce::Rectangle<int>> ArrangementTimelineComponent::getSelectedTrackInspectorBounds() const noexcept
{
    if (! selectedClip.has_value())
        return std::nullopt;

    auto lane = getTrackLaneBounds(selectedClip->trackIndex);
    return lane.removeFromLeft(trackHeaderWidth).reduced(8, 6);
}

const ArrangementTimelineComponent::AudioPeaks* ArrangementTimelineComponent::getOrComputePeaks(const juce::String& path)
{
    if (path.isEmpty())
        return nullptr;

    const auto key = path.toStdString();
    if (const auto it = waveformCache.find(key); it != waveformCache.end())
        return &it->second;

    juce::File file(path);
    if (! file.existsAsFile())
        return nullptr;

    auto& fm = getSharedWaveformFormatManager();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
        return nullptr;

    constexpr int samplesPerBucket = 256;
    const auto totalSamples = static_cast<int>(juce::jmin<juce::int64>(reader->lengthInSamples,
                                                                       static_cast<juce::int64>(std::numeric_limits<int>::max())));
    const auto numChannels = static_cast<int>(reader->numChannels);
    const auto numBuckets = (totalSamples + samplesPerBucket - 1) / samplesPerBucket;

    AudioPeaks peaks;
    peaks.samplesPerBucket = samplesPerBucket;
    peaks.minVals.assign(static_cast<size_t>(numBuckets), 0.0f);
    peaks.maxVals.assign(static_cast<size_t>(numBuckets), 0.0f);

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

            auto& mn = peaks.minVals[static_cast<size_t>(bucketIdx)];
            auto& mx = peaks.maxVals[static_cast<size_t>(bucketIdx)];
            mn = juce::jmin(mn, val);
            mx = juce::jmax(mx, val);
        }
        samplesProcessed += toRead;
    }

    auto [it, inserted] = waveformCache.emplace(key, std::move(peaks));
    juce::ignoreUnused(inserted);
    return &it->second;
}
}  // namespace orion
