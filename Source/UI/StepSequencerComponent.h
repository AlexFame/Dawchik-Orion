#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "../Audio/TransportEngine.h"
#include "../Core/ProjectState.h"

namespace orion
{
// FL-style step sequencer (Channel Rack): one row per MIDI/sampler track ("channel"), a grid
// of steps per bar. Toggling a step writes/removes a MIDI note in that track's 1-bar pattern
// clip (at the start of the timeline); right-clicking a row or step opens the piano roll for
// finer editing. Purely a view over the project — playback happens through the normal render.
class StepSequencerComponent final : public juce::Component,
                                     public juce::DragAndDropTarget,
                                     private juce::Timer
{
public:
    StepSequencerComponent(ProjectState& projectState, TransportEngine& transportEngine);
    ~StepSequencerComponent() override;

    // Host opens the piano roll for (trackIndex, clipIndex).
    std::function<void(int, int)> onOpenPianoRoll;
    // Fired after the pattern changes, so the host can repaint the arrangement / re-prep audio.
    std::function<void()> onPatternEdited;
    // A browser sample was dropped on empty space → create a new sampler channel from it.
    std::function<void(juce::File)> onAddChannelFromSample;
    // A browser sample was dropped on an existing channel row → load it into that track.
    std::function<void(int, juce::File)> onAssignSampleToChannel;
    // A channel name was clicked → host selects that track (so Enter-replace targets it).
    std::function<void(int)> onChannelSelected;
    // A channel sample was clicked → host opens the sampler panel (manipulators) for it.
    std::function<void(int)> onOpenSampler;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void visibilityChanged() override;

    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override;
    void itemDragEnter(const SourceDetails& dragSourceDetails) override;
    void itemDragMove(const SourceDetails& dragSourceDetails) override;
    void itemDragExit(const SourceDetails& dragSourceDetails) override;
    void itemDropped(const SourceDetails& dragSourceDetails) override;

    // Track index of the channel selected in the rack (-1 if none) — used for Enter-replace.
    int selectedChannelTrackIndex() const noexcept { return selectedTrackIndex; }

    static constexpr int kStepsPerBar = 16;

private:
    void timerCallback() override;

    int    beatsPerBar() const noexcept;
    int    patternSteps() const noexcept;        // kStepsPerBar * bars
    double stepLengthBeats() const noexcept;      // beatsPerBar / kStepsPerBar
    double patternBeats() const noexcept;         // bars * beatsPerBar
    int    channelNotePitch(const TrackState& track) const noexcept;

    void rebuildChannels();
    int  findPatternClip(int trackIndex, bool createIfMissing);
    bool stepActive(int trackIndex, int step) const;
    void toggleStep(int trackIndex, int step);

    // The bar-0 MIDI clip that a channel row reflects (nullptr if none).
    const TimelineClip* rowClip(int trackIndex) const;
    // True when the row's clip holds a real melody (multiple pitches / off-grid / long notes)
    // rather than a plain single-pitch step pattern — then we draw a note preview, not steps.
    bool rowIsMelodic(int trackIndex) const;

    // Header "16 / 32" step-count toggle (32 steps = 2 bars of 16). Returns the two clickable halves.
    std::pair<juce::Rectangle<int>, juce::Rectangle<int>> stepCountToggleBounds() const;
    juce::Rectangle<int> gridArea() const;        // below the header
    juce::Rectangle<int> rowBounds(int row) const;
    juce::Rectangle<int> nameBounds(int row) const;
    juce::Rectangle<int> muteBounds(int row) const;
    juce::Rectangle<int> stepsBounds(int row) const;
    juce::Rectangle<int> stepCellBounds(int row, int step) const;
    int  currentPlayStep() const;                 // -1 if not playing / outside pattern

    ProjectState& project;
    TransportEngine& transport;

    std::vector<int> channelTrackIndices;         // tracks shown as channels
    int  bars { 1 };
    int  hoveredRow { -1 };
    int  hoveredStep { -1 };
    bool dragOver { false };
    // Signal indicator next to each channel name: lights when the playhead hits an active step
    // on that channel and decays. Keyed by track index; value = trigger time (ms).
    int  lastPlayStep { -1 };
    std::map<int, double> lastTriggerMsByTrack;
    int  selectedTrackIndex { -1 };   // channel selected in the rack (for Enter-replace)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepSequencerComponent)
};
}  // namespace orion
