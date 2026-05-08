#include "ArrangementTimelineComponent.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
const auto timelineBackground = juce::Colour(0xff11181f);
const auto laneColour = juce::Colour(0xff172029);
const auto emptyCanvasColour = juce::Colour(0xff12181f);
const auto majorGridColour = juce::Colour(0xff31404d);
const auto minorGridColour = juce::Colour(0xff202a33);
const auto markerColour = juce::Colours::white.withAlpha(0.64f);
const auto textColour = juce::Colours::white.withAlpha(0.88f);
const auto playheadColour = juce::Colour(0xfff25454);
const auto loopRangeColour = juce::Colour(0xff7ecb6f);
constexpr auto resizeHandleWidth = 12;
constexpr auto minimumClipLengthInBeats = 1.0;
constexpr auto snapSizeInBeats = 0.25;
constexpr auto trackHeaderWidth = 214;
constexpr auto beatEpsilon = 0.0001;
constexpr auto defaultLaneHeight = 104;
constexpr auto bottomCanvasPadding = 280;
constexpr auto loopLaneHeight = 11;
constexpr auto loopHandleHitWidth = 8;
constexpr auto playheadHitWidth = 8;
constexpr auto inspectorResizeHandleHeight = 12;
constexpr auto minExpandedLaneHeight = 104;
constexpr auto maxExpandedLaneHeight = 240;
constexpr auto minHorizontalZoom = 1.0;
constexpr auto maxHorizontalZoom = 16.0;
const std::array<juce::Colour, 6> trackPalette {
    juce::Colour(0xffd97a2b),
    juce::Colour(0xffca5d54),
    juce::Colour(0xff5b84d6),
    juce::Colour(0xff7b68b5),
    juce::Colour(0xff9db0c3),
    juce::Colour(0xff48a999)
};

struct AudioImportAnalysis
{
    double durationSeconds { 0.0 };
    double clipLengthInBeats { minimumClipLengthInBeats };
    double sourceBpm { 0.0 };
    int detectedBars { 0 };
    juce::String bpmSource { "none" };
    double bpmConfidence { 0.0 };
    bool bpmGuessed { false };
};

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
    g.fillAll(timelineBackground);

    auto bounds = getLocalBounds().reduced(18);
    auto rulerArea = bounds.removeFromTop(42);
    auto tracksArea = bounds;
    auto gridArea = tracksArea;
    gridArea.removeFromLeft(trackHeaderWidth);
    auto rulerGridArea = rulerArea;
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    const auto trackCount = static_cast<int>(project.getTracks().size());
    const auto totalTracksHeight = getTotalTrackHeight();

    g.setColour(juce::Colours::black.withAlpha(0.18f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 18.0f);

    const auto beatsPerBar = static_cast<double>(project.getNumerator());
    const auto totalBeats = project.getProjectLengthInBeats();
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
    }

    // Calculate the visible beat range so the ruler extends beyond the project length
    const auto gridWidth = static_cast<double>(gridArea.getWidth());
    const auto pixelsPerBeat = totalBeats > 0.0 ? (gridWidth * horizontalZoom) / totalBeats : gridWidth;
    const int firstVisibleBeat = pixelsPerBeat > 0.0 ? juce::jmax(0, static_cast<int>(scrollX / pixelsPerBeat)) : 0;
    const int lastVisibleBeat = pixelsPerBeat > 0.0 ? static_cast<int>((scrollX + gridWidth) / pixelsPerBeat) + 1 : static_cast<int>(totalBeats);
    // Draw beyond project length so the ruler always fills the screen
    const int maxBeat = juce::jmax(lastVisibleBeat, static_cast<int>(totalBeats) + 64);

    const auto barPixelWidth = pixelsPerBeat * beatsPerBar;
    const auto labelBarStep = barPixelWidth > 96.0 ? 1
        : barPixelWidth > 48.0 ? 2
        : barPixelWidth > 24.0 ? 4
        : 8;

    for (int beat = firstVisibleBeat; beat <= maxBeat; ++beat)
    {
        const auto x = beatToX(static_cast<double>(beat), gridArea);
        if (x > static_cast<float>(gridArea.getRight() + 50))
            break;
        if (x < static_cast<float>(gridArea.getX() - 50))
            continue;

        const auto isBarLine = beat % static_cast<int>(beatsPerBar) == 0;

        // Grid lines only extend down to the bottom of the last track, not the whole panel
        const auto gridBottom = juce::jmin(static_cast<float>(tracksArea.getBottom()),
                                           static_cast<float>(tracksArea.getY() + totalTracksHeight - static_cast<int>(scrollY)));

        g.setColour(isBarLine ? majorGridColour : minorGridColour);
        g.drawLine(x, static_cast<float>(rulerGridArea.getY()), x, gridBottom, isBarLine ? 2.0f : 1.0f);

        if (isBarLine && ((beat / static_cast<int>(beatsPerBar)) % labelBarStep == 0))
        {
            const auto barNumber = 1 + beat / static_cast<int>(beatsPerBar);
            g.setColour(markerColour);
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            g.drawText(juce::String(barNumber), static_cast<int>(x) + 6, markerLane.getY(), 48, markerLane.getHeight(), juce::Justification::centredLeft);
        }
    }

    const auto& tracks = project.getTracks();

    const auto trackCanvasBottom = tracksArea.getY() + totalTracksHeight - static_cast<int>(scrollY);

    g.saveState();
    g.reduceClipRegion(tracksArea);

    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        const auto trackArrayIndex = static_cast<std::size_t>(trackIndex);
        auto lane = getTrackLaneBounds(trackIndex);

        g.setColour(laneColour);
        g.fillRoundedRectangle(lane.toFloat(), 12.0f);

        auto trackNameArea = lane.removeFromLeft(trackHeaderWidth);
        const auto trackSelected = selectedClip.has_value() && selectedClip->trackIndex == trackIndex;
        const auto headerSelected = selectedTrackIndex.has_value() && *selectedTrackIndex == trackIndex;
        g.setColour((trackSelected ? tracks[trackArrayIndex].colour.withAlpha(0.20f) : tracks[trackArrayIndex].colour.withAlpha(0.14f)));
        g.fillRoundedRectangle(trackNameArea.reduced(8, 6).toFloat(), 10.0f);
        if (headerSelected)
        {
            g.setColour(juce::Colours::white.withAlpha(0.46f));
            g.drawRoundedRectangle(trackNameArea.reduced(8, 6).toFloat(), 10.0f, 1.2f);
        }

        auto trackInner = trackNameArea.reduced(12, 10);
        auto titleRow = trackInner.removeFromTop(24);
        auto iconBox = titleRow.removeFromLeft(24);
        g.setColour(tracks[trackArrayIndex].colour.withAlpha(0.95f));
        g.fillRoundedRectangle(iconBox.toFloat(), 6.0f);
        g.setColour(textColour);
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
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

    if (trackCanvasBottom < tracksArea.getBottom())
    {
        auto emptyArea = tracksArea.withY(juce::jmax(tracksArea.getY(), trackCanvasBottom))
                           .withBottom(tracksArea.getBottom());
        g.setColour(emptyCanvasColour);
        g.fillRect(emptyArea);
    }

    g.reduceClipRegion(gridArea);

    // Draw Clips
    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        const auto trackArrayIndex = static_cast<std::size_t>(trackIndex);
        for (const auto& clip : tracks[trackArrayIndex].clips)
        {
            const auto clipIndex = static_cast<int>(&clip - tracks[trackArrayIndex].clips.data());
            const auto clipBoundsInt = getClipBounds(clip, trackIndex);
            const auto clipBounds = clipBoundsInt.toFloat();
            auto clipContentBounds = clipBoundsInt.reduced(0, 8);
            const auto clipHeaderHeight = juce::jmin(22, juce::jmax(16, clipContentBounds.getHeight() / 3));
            auto clipHeaderBounds = clipContentBounds.removeFromTop(clipHeaderHeight);
            auto clipBodyBounds = clipContentBounds;

            const auto isSelected = isClipSelected(SelectedClip { trackIndex, clipIndex });
            g.setColour(clip.colour.withSaturation(0.78f));
            g.fillRoundedRectangle(clipBounds, 10.0f);

            if (clip.type == ClipType::audio)
            {
                g.setColour(juce::Colours::white.withAlpha(0.45f));
                const float barWidth = 2.5f;
                const float centerY = clipBodyBounds.getY() + clipBodyBounds.getHeight() * 0.5f;
                const float maxBarHeight = juce::jmax(8.0f, clipBodyBounds.getHeight() - 6.0f);

                // Fixed number of waveform samples per beat so shape never changes
                constexpr double samplesPerBeat = 4.0;
                const int totalSamples = static_cast<int>(clip.lengthInBeats * samplesPerBeat);
                const auto seed = static_cast<int>(clip.name.hashCode());

                for (int i = 0; i < totalSamples; ++i)
                {
                    const auto progress = totalSamples > 1 ? static_cast<float>(i) / static_cast<float>(totalSamples - 1) : 0.0f;
                    const float x = static_cast<float>(clipBodyBounds.getX()) + progress * static_cast<float>(clipBodyBounds.getWidth());

                    // Deterministic height from sample index + clip seed
                    juce::Random rand(seed + i * 7919);
                    float intensity = 0.2f + 0.8f * rand.nextFloat();
                    float modulation = std::sin(static_cast<float>(i) * 0.35f) * 0.5f + 0.5f;
                    float height = 4.0f + (intensity * modulation * maxBarHeight);

                    g.fillRoundedRectangle(x - barWidth * 0.5f, centerY - height * 0.5f, barWidth, height, barWidth * 0.5f);
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
                g.setColour(juce::Colours::white.withAlpha(0.85f));
                g.drawRoundedRectangle(clipBounds.expanded(0.5f, 0.5f), 10.0f, 1.5f);
            }

            g.setFont(juce::FontOptions(14.0f, juce::Font::plain));
            
            // Keep clip names readable by pinning them to a dedicated top header strip.
            g.setColour(juce::Colours::black.withAlpha(0.4f));
            g.drawText(clip.name, clipHeaderBounds.reduced(8, 0).translated(1, 1), juce::Justification::centredLeft, true);
            
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            g.drawText(clip.name, clipHeaderBounds.reduced(8, 0), juce::Justification::centredLeft, true);
        }
    }

    g.restoreState();

    // Draw Playhead with a top cap in the ruler
    const auto playheadX = beatToX(transport.getPlayheadBeat(), gridArea);
    const auto isPlaying = transport.isPlaying();
    juce::ColourGradient gradient(playheadColour.withAlpha(0.0f), playheadX - 8.0f, 0.0f,
                                  playheadColour.withAlpha(0.0f), playheadX + 8.0f, 0.0f, false);
    gradient.addColour(0.5, playheadColour.withAlpha(isPlaying ? 0.35f : 0.15f));
    g.setGradientFill(gradient);
    g.fillRect(playheadX - 8.0f, static_cast<float>(tracksArea.getY()), 16.0f, static_cast<float>(tracksArea.getHeight()));

    g.setColour(playheadColour.withAlpha(isPlaying ? 0.95f : 0.78f));
    g.drawLine(playheadX, static_cast<float>(tracksArea.getY()), playheadX, static_cast<float>(tracksArea.getBottom()), 2.0f);
    g.fillEllipse(playheadX - 5.0f, static_cast<float>(tracksArea.getY()) - 5.0f, 10.0f, 10.0f);

    if (browserDropPreviewBounds.has_value())
    {
        g.setColour(browserDropPreviewColour.withAlpha(0.28f));
        g.fillRoundedRectangle(browserDropPreviewBounds->toFloat(), 10.0f);
        g.setColour(browserDropPreviewColour.withAlpha(0.95f));
        g.drawRoundedRectangle(browserDropPreviewBounds->toFloat(), 10.0f, 1.5f);
    }

    if (selectionBoxState.active)
    {
        const auto selectionBounds = getSelectionBoxBounds();
        if (! selectionBounds.isEmpty())
        {
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.fillRect(selectionBounds);
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.drawRect(selectionBounds, 1);
        }
    }
}

void ArrangementTimelineComponent::resized()
{
}

void ArrangementTimelineComponent::mouseDown(const juce::MouseEvent& event)
{
    auto bounds = getLocalBounds().reduced(18);
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
        if (event.mods.isShiftDown())
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
        dragState = DragState {
            hit->clip,
            hit->overResizeHandle ? DragMode::resizeRight : DragMode::move,
            event.getPosition(),
            clip.startBeat,
            clip.lengthInBeats,
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
        clip.startBeat = juce::jlimit(0.0, juce::jmax(0.0, project.getProjectLengthInBeats() - clip.lengthInBeats), snapBeatValue(dragState->originalStartBeat + beatDelta));
    }
    else
    {
        const auto proposedLength = dragState->originalLengthInBeats + beatDelta;
        clip.lengthInBeats = juce::jlimit(
            minimumClipLengthInBeats,
            juce::jmax(minimumClipLengthInBeats, project.getProjectLengthInBeats() - clip.startBeat),
            snapBeatValue(proposedLength));
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
    auto bounds = getLocalBounds().reduced(18);
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
            juce::jmax(0.0, project.getProjectLengthInBeats() - duplicateClip.lengthInBeats),
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
    auto bounds = getLocalBounds().reduced(18);
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
    const auto clippedStart = juce::jlimit(0.0, juce::jmax(0.0, project.getProjectLengthInBeats() - clipLength), startBeat);
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
    auto bounds = getLocalBounds().reduced(18);
    auto gridArea = bounds.withTrimmedTop(42);
    gridArea.removeFromLeft(trackHeaderWidth);

    if (! gridArea.contains(event.getPosition()))
        return;

    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto totalDelta = std::abs(wheel.deltaX) + std::abs(wheel.deltaY);

    if (nowMs < ignoreWheelUntilMs && totalDelta < 0.045f)
        return;

    if (std::abs(wheel.deltaX) > 0.0001f || std::abs(wheel.deltaY) > 0.0001f)
    {
        auto horizontalDelta = static_cast<double>(wheel.deltaX);
        auto verticalDelta = static_cast<double>(wheel.deltaY);
        const auto absHorizontal = std::abs(horizontalDelta);
        const auto absVertical = std::abs(verticalDelta);

        // Trackpads often report a tiny cross-axis delta. Lock to the dominant axis
        // so a vertical two-finger scroll does not drift the playlist horizontally.
        if (absVertical > absHorizontal * 1.35)
            horizontalDelta = 0.0;
        else if (absHorizontal > absVertical * 1.35)
            verticalDelta = 0.0;

        scrollX -= horizontalDelta * 600.0;
        scrollY -= verticalDelta * 600.0;
        clampScrollOffsets();
        repaint();
    }
}

void ArrangementTimelineComponent::mouseMagnify(const juce::MouseEvent& event, float scaleFactor)
{
    auto bounds = getLocalBounds().reduced(18);
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
        auto tracksBounds = getLocalBounds().reduced(18);
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
        juce::jmax(0.0, project.getProjectLengthInBeats() - lengthBeats),
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
        analysis.bpmGuessed
    });

    setSingleSelection(SelectedClip { targetTrackIndex, static_cast<int>(tracks[static_cast<std::size_t>(targetTrackIndex)].clips.size()) - 1 });

    clearBrowserDropPreview();
    repaint();
}

void ArrangementTimelineComponent::adjustZoom(double horizontalDelta, std::optional<juce::Point<int>> focusPoint)
{
    auto bounds = getLocalBounds().reduced(18);
    auto gridArea = bounds.withTrimmedTop(42);
    gridArea.removeFromLeft(trackHeaderWidth);

    const auto fullWidth = static_cast<double>(gridArea.getWidth());
    const auto oldContentWidth = juce::jmax(fullWidth, fullWidth * horizontalZoom);

    double focusXInView = fullWidth * 0.5;
    double focusXRatio = 0.5;

    if (focusPoint.has_value())
    {
        focusXInView = juce::jlimit(0.0, fullWidth, static_cast<double>(focusPoint->x - gridArea.getX()));
        focusXRatio = oldContentWidth > 0.0 ? (scrollX + focusXInView) / oldContentWidth : 0.5;
    }

    horizontalZoom = juce::jlimit(minHorizontalZoom, maxHorizontalZoom, horizontalZoom + horizontalDelta);

    const auto newContentWidth = juce::jmax(fullWidth, fullWidth * horizontalZoom);
    scrollX = (focusXRatio * newContentWidth) - focusXInView;
    clampScrollOffsets();
    repaint();
}

void ArrangementTimelineComponent::timerCallback()
{
    repaint();
}

float ArrangementTimelineComponent::beatToX(double beat, juce::Rectangle<int> laneArea) const noexcept
{
    const auto totalBeats = project.getProjectLengthInBeats();
    if (totalBeats <= 0.0) return static_cast<float>(laneArea.getX());
    
    const auto pixelsPerBeat = (static_cast<double>(laneArea.getWidth()) * horizontalZoom) / totalBeats;
    return static_cast<float>(laneArea.getX() + (beat * pixelsPerBeat) - scrollX);
}

void ArrangementTimelineComponent::clampScrollOffsets()
{
    auto bounds = getLocalBounds().reduced(18);
    auto rulerGridArea = bounds.withTrimmedTop(42);
    rulerGridArea.removeFromLeft(trackHeaderWidth);
    
    const auto fullWidth = static_cast<double>(rulerGridArea.getWidth());
    const auto zoomedWidth = fullWidth * horizontalZoom;
    const auto maxScroll = juce::jmax(0.0, zoomedWidth - fullWidth);
    scrollX = juce::jlimit(0.0, maxScroll, scrollX);
    
    // Vertical scroll: clamp based on total track height vs visible height
    auto vBounds = getLocalBounds().reduced(18);
    vBounds.removeFromTop(42);
    const auto totalTrackHeight = static_cast<double>(getTotalTrackHeight() + bottomCanvasPadding);
    const auto visibleHeight = static_cast<double>(vBounds.getHeight());
    scrollY = juce::jlimit(0.0, juce::jmax(0.0, totalTrackHeight - visibleHeight), scrollY);
}

int ArrangementTimelineComponent::getLaneHeightForTrack(int trackIndex) const noexcept
{
    if (selectedClip.has_value() && selectedClip->trackIndex == trackIndex)
        return selectedTrackExpandedHeight;

    return defaultLaneHeight;
}

int ArrangementTimelineComponent::getTrackTopForIndex(int trackIndex) const noexcept
{
    auto top = 0;
    for (int i = 0; i < trackIndex; ++i)
        top += getLaneHeightForTrack(i);
    return top;
}

int ArrangementTimelineComponent::getTotalTrackHeight() const noexcept
{
    const auto trackCount = static_cast<int>(project.getTracks().size());
    auto total = 0;
    for (int i = 0; i < trackCount; ++i)
        total += getLaneHeightForTrack(i);
    return total;
}

juce::Rectangle<int> ArrangementTimelineComponent::getTrackLaneBounds(int trackIndex) const noexcept
{
    auto bounds = getLocalBounds().reduced(18);
    bounds.removeFromTop(42);

    const int laneTop = bounds.getY() + getTrackTopForIndex(trackIndex) - static_cast<int>(scrollY);
    auto lane = juce::Rectangle<int>(bounds.getX(), laneTop, bounds.getWidth(), getLaneHeightForTrack(trackIndex));
    lane.removeFromBottom(6);
    return lane;
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
        lane.getY() + 12,
        static_cast<int>(juce::jmax(48.0f, clipEndX - clipX - 2.0f)),
        lane.getHeight() - 24);
}

double ArrangementTimelineComponent::xToBeatDelta(int xDelta) const noexcept
{
    auto bounds = getLocalBounds().reduced(18);
    bounds.removeFromTop(42);
    bounds.removeFromLeft(trackHeaderWidth);
    const auto totalBeats = project.getProjectLengthInBeats();
    if (totalBeats <= 0.0) return 0.0;
    
    const auto pixelsPerBeat = (static_cast<double>(bounds.getWidth()) * horizontalZoom) / totalBeats;
    return pixelsPerBeat > 0.0 ? static_cast<double>(xDelta) / pixelsPerBeat : 0.0;
}

double ArrangementTimelineComponent::xToBeatPosition(int x) const noexcept
{
    auto bounds = getLocalBounds().reduced(18);
    bounds.removeFromTop(42);
    bounds.removeFromLeft(trackHeaderWidth);
    const auto totalBeats = project.getProjectLengthInBeats();
    if (totalBeats <= 0.0)
        return 0.0;

    const auto pixelsPerBeat = (static_cast<double>(bounds.getWidth()) * horizontalZoom) / totalBeats;
    const auto localX = static_cast<double>(x - bounds.getX());
    return pixelsPerBeat > 0.0 ? (scrollX + localX) / pixelsPerBeat : 0.0;
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
        auto tracksBounds = getLocalBounds().reduced(18);
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
        juce::jmax(0.0, project.getProjectLengthInBeats() - lengthBeats),
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
        analysis.bpmGuessed
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
}  // namespace orion
