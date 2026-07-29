// MPC Sample pad/kit logic for MainComponent.
//
// Split out of MainComponent.cpp purely to keep that file manageable — this is the SAME
// class, same members, identical logic; only the definitions live here. See MainComponent.h
// for the declarations.
#include "MainComponent.h"

#include "../Audio/PlaybackSources.h"
#include "../Sampler/SamplerEngine.h"
#include "OrionTheme.h"

#include <vector>

namespace orion
{
void MainComponent::triggerMpcPad(int padIndex, int velocity)
{
    padIndex = juce::jlimit(0, 15, padIndex);
    if (velocity > 0)
    {
        // Mirror the panel's chop-aware focus (MpcSamplePanelComponent::setPadActivity): in chop mode
        // the LCD stays on the SOURCE sample (the pressed pad just lights its slice), and we never move
        // focus onto an EMPTY pad. Jumping to an unloaded pad dropped the LCD to the "no sample"
        // hardware screen and hid the chop waveform + slice markers + active-slice highlight.
        const bool keepChopFocus = mpc.chopMode && mpcSamplePanel.isPadLoaded(mpc.selectedPad);
        if (! keepChopFocus && mpcSamplePanel.isPadLoaded(padIndex))
            mpc.selectedPad = padIndex;
        updateMpcPerformanceState();
    }

    // Play the pad's own sample through the shared sampler engine (+ record).
    playMpcPad(padIndex, velocity);

    mpcHardwareBridge.sendPadToHardware(padIndex, velocity);
    if (velocity > 0 && ! mpcHardwareBridge.getDeviceState().outputConnected && ! mpcSamplePanel.isPadLoaded(padIndex))
        statusLabel.setText("MPC MIDI OUT not found. Check USB/TRS MIDI output.", juce::dontSendNotification);
}

void MainComponent::playMpcPad(int padIndex, int velocity)
{
    if (padIndex < 0 || padIndex > 15 || arrangementPlaybackSource == nullptr)
        return;

    if (velocity > 0 && mpc.padActiveNotes.count(padIndex) > 0)
        return;

    // Tune/melodic mode: one sample pitched across the pads. For live play, keep the
    // source of truth local to the MPC surface so hardware hits don't depend on the
    // project track sync happening first.
    const int kit = findMpcKitTrack();
    const bool trackTune = kit >= 0
                        && projectState.getTracks()[static_cast<std::size_t>(kit)].isMpcTuneMode
                        && projectState.getTracks()[static_cast<std::size_t>(kit)].mpcTuneSample.isNotEmpty();
    const bool trackChop = kit >= 0
                        && projectState.getTracks()[static_cast<std::size_t>(kit)].isMpcChopMode
                        && projectState.getTracks()[static_cast<std::size_t>(kit)].mpcChopSample.isNotEmpty();
    const bool tune = mpc.sixteenLevels && (mpc.tuneSourcePath.isNotEmpty() || trackTune);
    const bool chop = ! tune && mpc.chopMode && (mpc.chopSourcePath.isNotEmpty() || trackChop);
    const int note = (velocity > 0 || mpc.padActiveNotes.count(padIndex) == 0)
        ? (tune ? mpcTuneMidiNoteForPad(padIndex) : 36 + padIndex)
        : mpc.padActiveNotes[padIndex];

    juce::String sourcePath;
    int rootNote = tune ? (mpc.tuneRootNote + mpc.tuneOctaveOffset * 12) : note;
    if (chop)
    {
        if (mpc.chopSourcePath.isNotEmpty())
            sourcePath = mpc.chopSourcePath;
        else
            sourcePath = projectState.getTracks()[static_cast<std::size_t>(kit)].mpcChopSample;
        rootNote = 36;
    }
    else if (tune)
    {
        if (mpc.tuneSourcePath.isNotEmpty())
        {
            sourcePath = mpc.tuneSourcePath;
            rootNote = mpc.tuneRootNote;
        }
        else
        {
            const auto& t = projectState.getTracks()[static_cast<std::size_t>(kit)];
            sourcePath = t.mpcTuneSample;
            rootNote = t.mpcTuneRoot;
        }
    }
    else
    {
        if (! mpcSamplePanel.isPadLoaded(padIndex))
            return;   // empty pad — nothing to sound
        sourcePath = mpcSamplePanel.getPadSourcePath(padIndex);
    }

    if (velocity > 0)
    {
        // Kit mode is drum-style: each pad is one sound. Tune/16 Levels is melodic, so let
        // Orion's shared Chord Mode expand a pad into a chord just like the sampler keyboard.
        const auto pitches = tune ? chordPitchesForNote(note) : std::vector<int>{ note };
        mpc.padActiveNotes[padIndex] = note;
        mpc.chordVoicing[note] = pitches;
        for (const auto p : pitches)
        {
            arrangementPlaybackSource->samplerNoteOn(sourcePath, p, velocity, rootNote, 0.0,
                                                     chop ? SamplerPlaybackMode::slice : SamplerPlaybackMode::oneShot,
                                                     chop ? padIndex : 0,
                                                     chop ? 16 : 1,
                                                     false, 0.0, true);
            recordNoteOn(p, velocity);
        }
    }
    else
    {
        auto pitches = std::vector<int>{ note };
        if (const auto it = mpc.chordVoicing.find(note); it != mpc.chordVoicing.end())
        {
            pitches = it->second;
            mpc.chordVoicing.erase(it);
        }

        for (const auto p : pitches)
        {
            arrangementPlaybackSource->samplerNoteOff(p, SamplerPlaybackMode::oneShot, false);
            recordNoteOff(p);
        }
        mpc.padActiveNotes.erase(padIndex);
    }
}

int MainComponent::mpcTuneMidiNoteForPad(int padIndex) const
{
    const int rootPad = juce::jlimit(0, 15, mpc.tuneRootNote - 36);
    const int degreeOffset = juce::jlimit(0, 15, padIndex) - rootPad;
    const int chromatic = mpc.tuneRootNote + degreeOffset + mpc.tuneOctaveOffset * 12;

    if (! projectState.isKeyEnabled() || ! projectState.isScaleLockEnabled())
        return juce::jlimit(0, 127, chromatic);

    static constexpr std::array<int, 7> majorScale { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr std::array<int, 7> minorScale { 0, 2, 3, 5, 7, 8, 10 };
    const auto& scale = projectState.isKeyMinor() ? minorScale : majorScale;
    const int keyRoot = ((projectState.getKeyRoot() % 12) + 12) % 12;

    const int rootPc = (((mpc.tuneRootNote - keyRoot) % 12) + 12) % 12;
    int rootDegree = 0;
    int bestDistance = 128;
    int bestDelta = 0;
    for (int i = 0; i < 7; ++i)
    {
        int delta = scale[static_cast<std::size_t>(i)] - rootPc;
        if (delta > 6) delta -= 12;
        if (delta < -6) delta += 12;
        const int distance = std::abs(delta);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestDelta = delta;
            rootDegree = i;
        }
    }

    const int snappedRoot = mpc.tuneRootNote + bestDelta + mpc.tuneOctaveOffset * 12;
    const int snappedRootOctave = (snappedRoot - keyRoot - scale[static_cast<std::size_t>(rootDegree)]) / 12;
    const int totalDegree = rootDegree + degreeOffset;
    const int octaveCarry = totalDegree >= 0 ? totalDegree / 7 : -((-totalDegree + 6) / 7);
    const int degree = ((totalDegree % 7) + 7) % 7;

    return juce::jlimit(0, 127,
                        keyRoot
                            + (snappedRootOctave + octaveCarry) * 12
                            + scale[static_cast<std::size_t>(degree)]);
}

int MainComponent::mpcKitTrackIndex()
{
    auto& tracks = projectState.getTracks();
    if (const auto selected = arrangementTimeline.getSelectedTrackIndex(); selected.has_value()
        && *selected >= 0 && *selected < static_cast<int>(tracks.size()))
    {
        auto& t = tracks[static_cast<std::size_t>(*selected)];
        if (t.isMpcKit)
            return *selected;
        if (t.isMidiTrack && t.clips.empty())
        {
            const juce::ScopedLock sl(projectState.getAudioEditLock());
            t.isMpcKit = true;
            if (! t.name.startsWithIgnoreCase("MPC"))
                t.name = "MPC " + t.name;
            return *selected;
        }
    }

    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<std::size_t>(i)].isMpcKit && tracks[static_cast<std::size_t>(i)].recordArmed)
            return i;

    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<std::size_t>(i)].isMpcKit)
            return i;

    // None yet — create a dedicated "MPC" kit track (sanctioned path: undo snapshot + select).
    arrangementTimeline.addMidiTrack();
    const int idx = static_cast<int>(projectState.getTracks().size()) - 1;
    if (idx >= 0)
    {
        const juce::ScopedLock sl(projectState.getAudioEditLock());
        auto& t = projectState.getTracks()[static_cast<std::size_t>(idx)];
        t.isMpcKit = true;
        t.name = "MPC";
    }
    return idx;
}

int MainComponent::findMpcKitTrack() const
{
    const auto& tracks = projectState.getTracks();
    if (const auto selected = arrangementTimeline.getSelectedTrackIndex(); selected.has_value()
        && *selected >= 0 && *selected < static_cast<int>(tracks.size())
        && tracks[static_cast<std::size_t>(*selected)].isMpcKit)
        return *selected;

    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<std::size_t>(i)].isMpcKit && tracks[static_cast<std::size_t>(i)].recordArmed)
            return i;

    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<std::size_t>(i)].isMpcKit)
            return i;
    return -1;
}

void MainComponent::syncMpcTuneMode()
{
    const int idx = mpcKitTrackIndex();   // ensure the kit track exists
    if (idx < 0 || idx >= static_cast<int>(projectState.getTracks().size()))
        return;

    int selPad = juce::jlimit(0, 15, mpcSamplePanel.getSelectedPad());
    if (! mpcSamplePanel.isPadLoaded(selPad))
        for (int pad = 0; pad < 16; ++pad)
            if (mpcSamplePanel.isPadLoaded(pad))
            {
                selPad = pad;
                break;
            }

    const auto tunePath = mpcSamplePanel.getPadSourcePath(selPad);
    mpc.tuneSourcePath = mpc.sixteenLevels ? tunePath : juce::String();
    mpc.tuneRootNote = 36 + selPad;

    {
        const juce::ScopedLock sl(projectState.getAudioEditLock());
        auto& t = projectState.getTracks()[static_cast<std::size_t>(idx)];
        if (tunePath.isNotEmpty())
            t.mpcKitSamples[static_cast<std::size_t>(selPad)] = tunePath;
        t.isMpcTuneMode = mpc.sixteenLevels && tunePath.isNotEmpty();
        t.mpcTuneSample = tunePath;
        t.mpcTuneRoot   = 36 + selPad;   // the selected pad plays at original pitch
        if (t.isMpcTuneMode)
        {
            t.isMpcChopMode = false;
            t.mpcChopSample = {};
        }
    }
}

void MainComponent::assignMpcKitSample(int padIndex, const juce::String& sourcePath)
{
    if (padIndex < 0 || padIndex > 15)
        return;

    const int idx = mpcKitTrackIndex();
    if (idx < 0 || idx >= static_cast<int>(projectState.getTracks().size()))
        return;

    {
        // The audio thread reads mpcKitSamples in renderMpcKitClip — guard the write.
        const juce::ScopedLock sl(projectState.getAudioEditLock());
        auto& t = projectState.getTracks()[static_cast<std::size_t>(idx)];
        t.isMpcKit = true;
        t.mpcKitSamples[static_cast<std::size_t>(padIndex)] = sourcePath;
        if (mpc.chopMode && padIndex == mpcSamplePanel.getSelectedPad())
        {
            mpc.chopSourcePath = sourcePath;
            t.isMpcTuneMode = false;
            t.mpcTuneSample = {};
            t.isMpcChopMode = true;
            t.mpcChopSample = sourcePath;
            t.mpcChopRootPad = padIndex;
            t.mpcChopSliceCount = 16;
        }
    }
    if (mpc.sixteenLevels)
    {
        mpc.repeatedRootNoteCount = 0;
        syncMpcTuneMode();
    }
    arrangementTimeline.repaint();
    mixerPanel.repaint();
}

int MainComponent::ensureMpcRecordTrack()
{
    // Record pad hits into the MPC kit track, so playback plays the pad samples back.
    const int target = mpcKitTrackIndex();
    if (target >= 0 && target < static_cast<int>(projectState.getTracks().size()))
    {
        auto& tracks = projectState.getTracks();
        for (auto& track : tracks)
            if (track.isMidiTrack)
                track.recordArmed = false;
        tracks[static_cast<std::size_t>(target)].recordArmed = true;
        arrangementTimeline.selectTrack(target);
        arrangementTimeline.repaint();
        mixerPanel.repaint();
    }
    return target;
}

void MainComponent::handleMpcCommand(MpcSamplePanelComponent::Command command)
{
    switch (command)
    {
        case MpcSamplePanelComponent::Command::sampleMode:
            statusLabel.setText("MPC: hardware shell active", juce::dontSendNotification);
            break;
        case MpcSamplePanelComponent::Command::seqMode:
            statusLabel.setText("MPC: sequence mode selected", juce::dontSendNotification);
            break;
        case MpcSamplePanelComponent::Command::padFx:
            mpc.fullLevel = ! mpc.fullLevel;
            statusLabel.setText(mpc.fullLevel ? "MPC: Full Level on" : "MPC: Full Level off", juce::dontSendNotification);
            break;
        case MpcSamplePanelComponent::Command::knobFx:
            statusLabel.setText("MPC: knob FX is hardware-side", juce::dontSendNotification);
            break;
        case MpcSamplePanelComponent::Command::shift:
            mpc.sixteenLevels = false;
            mpc.chopMode = false;
            mpc.chopSourcePath = {};
            mpc.fullLevel = false;
            mpc.padBank = 0;
            mpc.tuneOctaveOffset = 0;
            mpc.heldHardwareNoteKeys.clear();
            mpc.hardwareNoteReleaseTimes.clear();
            mpc.hardwareNotePads.clear();
            mpc.padActiveNotes.clear();
            mpc.chordVoicing.clear();
            statusLabel.setText("MPC: performance reset", juce::dontSendNotification);
            break;
        case MpcSamplePanelComponent::Command::padBank:
            mpc.padBank = (mpc.padBank + 1) % 4;
            statusLabel.setText("MPC: Pad bank " + juce::String(static_cast<juce::juce_wchar>('A' + mpc.padBank)),
                                juce::dontSendNotification);
            break;
        case MpcSamplePanelComponent::Command::chop:
        {
            int sourcePad = mpcSamplePanel.getSelectedPad();
            if (! mpcSamplePanel.isPadLoaded(sourcePad))
                for (int pad = 0; pad < 16; ++pad)
                    if (mpcSamplePanel.isPadLoaded(pad))
                    {
                        sourcePad = pad;
                        break;
                    }

            const auto chopPath = mpcSamplePanel.getPadSourcePath(sourcePad);
            if (chopPath.isEmpty())
            {
                mpc.chopMode = false;
                mpc.chopSourcePath = {};
                statusLabel.setText("MPC Chop: drop a sample on a pad first", juce::dontSendNotification);
                break;
            }

            mpc.chopMode = ! mpc.chopMode;
            mpc.sixteenLevels = false;
            mpc.tuneSourcePath = {};
            mpc.tuneOctaveOffset = 0;
            mpc.chopSourcePath = mpc.chopMode ? chopPath : juce::String();
            mpc.selectedPad = sourcePad;
            mpc.heldHardwareNoteKeys.clear();
            mpc.hardwareNoteReleaseTimes.clear();
            mpc.hardwareNotePads.clear();
            mpc.padActiveNotes.clear();
            mpc.chordVoicing.clear();

            if (const int idx = mpcKitTrackIndex(); idx >= 0 && idx < static_cast<int>(projectState.getTracks().size()))
            {
                const juce::ScopedLock sl(projectState.getAudioEditLock());
                auto& t = projectState.getTracks()[static_cast<std::size_t>(idx)];
                t.isMpcTuneMode = false;
                t.mpcTuneSample = {};
                t.isMpcChopMode = mpc.chopMode;
                t.mpcChopSample = mpc.chopSourcePath;
                t.mpcChopRootPad = sourcePad;
                t.mpcChopSliceCount = 16;
            }

            statusLabel.setText(mpc.chopMode ? "MPC Chop: pads trigger 16 slices"
                                            : "MPC Chop: off",
                                juce::dontSendNotification);
            break;
        }
        case MpcSamplePanelComponent::Command::mute:
            statusLabel.setText("MPC: mute is hardware-side", juce::dontSendNotification);
            break;
        case MpcSamplePanelComponent::Command::loop:
            toggleLoopFromUi();
            break;
        case MpcSamplePanelComponent::Command::levels16:
        {
            bool hasSampleForTune = mpcSamplePanel.isPadLoaded(mpcSamplePanel.getSelectedPad());
            if (! hasSampleForTune)
                for (int pad = 0; pad < 16; ++pad)
                    if (mpcSamplePanel.isPadLoaded(pad))
                    {
                        hasSampleForTune = true;
                        break;
                    }

            if (! hasSampleForTune)
            {
                mpc.sixteenLevels = false;
                mpc.tuneSourcePath = {};
                mpc.chopMode = false;
                mpc.chopSourcePath = {};
                mpc.heldHardwareNoteKeys.clear();
                mpc.hardwareNoteReleaseTimes.clear();
                mpc.hardwareNotePads.clear();
                mpc.padActiveNotes.clear();
                mpc.chordVoicing.clear();
                statusLabel.setText("MPC: drop a sample on a pad before 16 Levels", juce::dontSendNotification);
                break;
            }

            mpc.sixteenLevels = ! mpc.sixteenLevels;
            mpc.chopMode = false;
            mpc.chopSourcePath = {};
            mpc.repeatedRootNoteCount = 0;
            mpc.heldHardwareNoteKeys.clear();
            mpc.hardwareNoteReleaseTimes.clear();
            mpc.hardwareNotePads.clear();
            mpc.padActiveNotes.clear();
            mpc.chordVoicing.clear();
            syncMpcTuneMode();   // Tune/melodic: one sample pitched across the pads
            statusLabel.setText(mpc.sixteenLevels ? "MPC: Tune — scale pads, +/- octave"
                                                 : "MPC: Kit mode", juce::dontSendNotification);
            break;
        }
        case MpcSamplePanelComponent::Command::sampleSelect:
            statusLabel.setText("MPC: sample select is hardware-side", juce::dontSendNotification);
            break;
        case MpcSamplePanelComponent::Command::tapTempo:
            mpcTapTempo();
            break;
        case MpcSamplePanelComponent::Command::rewind:
            rewindTransportFromUi();
            break;
        case MpcSamplePanelComponent::Command::stop:
            stopTransportFromUi();
            break;
        case MpcSamplePanelComponent::Command::record:
            recordButton.triggerClick();
            break;
        case MpcSamplePanelComponent::Command::play:
            toggleTransportFromUi();
            break;
        case MpcSamplePanelComponent::Command::undo:
            if (mpc.sixteenLevels)
            {
                if (arrangementPlaybackSource != nullptr)
                    arrangementPlaybackSource->allSamplerNotesOff();
                mpc.tuneOctaveOffset = juce::jlimit(-4, 4, mpc.tuneOctaveOffset - 1);
                mpc.heldHardwareNoteKeys.clear();
                mpc.hardwareNoteReleaseTimes.clear();
                mpc.hardwareNotePads.clear();
                mpc.padActiveNotes.clear();
                mpc.chordVoicing.clear();
                statusLabel.setText("MPC: Octave " + juce::String(mpc.tuneOctaveOffset), juce::dontSendNotification);
            }
            else
            {
                arrangementTimeline.undo();
            }
            break;
        case MpcSamplePanelComponent::Command::redo:
            if (mpc.sixteenLevels)
            {
                if (arrangementPlaybackSource != nullptr)
                    arrangementPlaybackSource->allSamplerNotesOff();
                mpc.tuneOctaveOffset = juce::jlimit(-4, 4, mpc.tuneOctaveOffset + 1);
                mpc.heldHardwareNoteKeys.clear();
                mpc.hardwareNoteReleaseTimes.clear();
                mpc.hardwareNotePads.clear();
                mpc.padActiveNotes.clear();
                mpc.chordVoicing.clear();
                statusLabel.setText("MPC: Octave " + juce::String(mpc.tuneOctaveOffset), juce::dontSendNotification);
            }
            else
            {
                arrangementTimeline.redo();
            }
            break;
        case MpcSamplePanelComponent::Command::count:
            break;
    }

    updateMpcPerformanceState();
    arrangementTimeline.repaint();
}

void MainComponent::beginMpcCommandLearn(MpcSamplePanelComponent::Command command)
{
    mpc.pendingCommandLearn = command;
    statusLabel.setText("MPC Learn: press hardware control for " + mpcCommandName(command),
                        juce::dontSendNotification);
    lastLiveMidiSignalText = "MPC learn " + mpcCommandName(command);
    mpcSamplePanel.setHardwareStatus(mpcHardwareBridge.getDeviceState().inputName,
                                     mpcHardwareBridge.getDeviceState().outputName,
                                     "MPC Learn: waiting for CC -> " + mpcCommandName(command));
}

void MainComponent::updateMpcPerformanceState()
{
    mpcSamplePanel.setPerformanceState(mpc.fullLevel, mpc.sixteenLevels, mpc.chopMode, mpc.padBank, mpc.selectedPad);
}

void MainComponent::mpcTapTempo()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (mpc.lastTapMs > 0.0)
    {
        const auto interval = now - mpc.lastTapMs;
        if (interval > 250.0 && interval < 2000.0)
        {
            mpc.tapIntervalsMs.push_back(interval);
            while (mpc.tapIntervalsMs.size() > 4)
                mpc.tapIntervalsMs.erase(mpc.tapIntervalsMs.begin());

            const auto sum = std::accumulate(mpc.tapIntervalsMs.begin(), mpc.tapIntervalsMs.end(), 0.0);
            const auto bpm = 60000.0 / (sum / static_cast<double>(mpc.tapIntervalsMs.size()));
            transportController.setTempoBpm(bpm);
            updateTransportLabels();
            statusLabel.setText("MPC: Tap tempo " + juce::String(projectState.getTempoBpm(), 1) + " BPM",
                                juce::dontSendNotification);
        }
        else
            mpc.tapIntervalsMs.clear();
    }
    mpc.lastTapMs = now;
}
} // namespace orion
