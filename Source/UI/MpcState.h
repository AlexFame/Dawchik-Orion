#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <optional>
#include <set>
#include <vector>

#include "MpcSamplePanelComponent.h"   // for the nested MpcSamplePanelComponent::Command enum

namespace orion
{
// Runtime state for the MPC Sample workflow: pad performance modes, tune/chop source samples,
// hardware note-latch bookkeeping, tap-tempo, and command-learn. Grouped out of MainComponent so the
// MPC subsystem's scattered state reads as one thing — it already has companions MpcSampleMapping and
// MpcSampleHardwareBridge. Behaviour is unchanged; fields were only renamed to drop the redundant
// `mpc` prefix now that they live under a `mpc.` member.
struct MpcState
{
    // Performance modes.
    bool fullLevel { false };
    bool sixteenLevels { false };
    bool chopMode { false };

    // Tune (16-Levels) + chop source samples and root.
    juce::String tuneSourcePath;
    juce::String chopSourcePath;
    int tuneRootNote { 36 };
    int tuneOctaveOffset { 0 };
    int repeatedRootNoteCount { 0 };

    // Pad selection / bank.
    int padBank { 0 };
    int selectedPad { 0 };

    // Tap tempo.
    double lastTapMs { 0.0 };
    std::vector<double> tapIntervalsMs;

    // Command-learn: map a hardware CC to a panel Command.
    std::optional<MpcSamplePanelComponent::Command> pendingCommandLearn;
    std::map<int, MpcSamplePanelComponent::Command> ccCommandMap;

    // Live 16-Levels chord + hardware note-latch bookkeeping (filters MPC pressure/note-repeat chatter).
    std::map<int, std::vector<int>> chordVoicing;             // held pads → sounded pitches
    std::map<int, int>              padActiveNotes;           // pad index → exact note sounded at note-on
    std::set<int>                   heldHardwareNoteKeys;     // channel/note latch: ignore repeats while held
    std::map<int, double>           hardwareNoteReleaseTimes; // delayed re-arm times
    std::map<int, int>              hardwareNotePads;         // channel/note → Orion pad index for delayed note-off

    // MPC-local tonality for pad highlighting. Independent of the project key: lets you pick a key
    // right on the MPC (even when the project tonality is off) and light the in-key pads among the
    // full chromatic pad layout. When inactive, the highlight falls back to the project key.
    bool highlightKeyActive { false };
    int  highlightKeyRoot { 0 };      // 0 = C … 11 = B
    bool highlightKeyMinor { true };

    // MPC hardware connection status.
    bool inputConnected { false };
    juce::String inputName;
};
} // namespace orion
