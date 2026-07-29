// Live MIDI routing for MainComponent: hardware-MIDI input, routing a played note through the
// active track's instrument/sampler (with chord-mode expansion), and the chord-lane audition.
//
// Split out of MainComponent.cpp purely to keep that file manageable — this is the SAME
// class, same members, identical logic; only the definitions live here. See MainComponent.h
// for the declarations.
#include "MainComponent.h"

#include "../Audio/PlaybackSources.h"
#include "../Sampler/SamplerEngine.h"
#include "OrionTheme.h"
#include "MainComponentInternal.h"

#include <algorithm>
#include <vector>

namespace orion
{
void MainComponent::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    // Called on the MIDI thread. Copy the message and hand it to the message
    // thread, where it's safe to touch the project state and UI.
    juce::Component::SafePointer<MainComponent> safeThis(this);
    const juce::MidiMessage msg(message);
    const juce::String sourceName = source != nullptr ? source->getName() : juce::String();
    juce::MessageManager::callAsync([safeThis, msg, sourceName]
    {
        if (safeThis != nullptr)
            safeThis->routeLiveMidiMessage(msg, sourceName);
    });
}

void MainComponent::appendLiveMidiDebugLog(const juce::MidiMessage& message,
                                           const juce::String& sourceName,
                                           int mappedPadIndex)
{
    juce::String line = juce::Time::getCurrentTime().formatted("%H:%M:%S.%ms")
        + " src=\"" + sourceName + "\""
        + " ch=" + juce::String(message.getChannel())
        + " mappedPad=" + (mappedPadIndex >= 0 ? juce::String(mappedPadIndex + 1) : juce::String("-"));

    if (message.isNoteOnOrOff())
        line += " note=" + juce::String(message.getNoteNumber())
              + " vel=" + juce::String(message.isNoteOn() ? static_cast<int>(message.getVelocity()) : 0)
              + (message.isNoteOn() ? " on" : " off");
    else if (message.isController())
        line += " cc=" + juce::String(message.getControllerNumber())
              + " val=" + juce::String(message.getControllerValue());
    else if (message.isPitchWheel())
        line += " pitchWheel=" + juce::String(message.getPitchWheelValue());
    else if (message.isAftertouch())
        line += " aftertouch note=" + juce::String(message.getNoteNumber())
              + " val=" + juce::String(message.getAfterTouchValue());
    else if (message.isChannelPressure())
        line += " pressure=" + juce::String(message.getChannelPressureValue());
    else
        line += " other";

    line += "\n";
    juce::File("/tmp/orion-midi.log").appendText(line, false, false, "\n");
}

void MainComponent::routeLiveMidiMessage(const juce::MidiMessage& message, const juce::String& sourceName)
{
    lastLiveMidiActivityMs = juce::Time::getMillisecondCounterHiRes();
    if (message.isNoteOn())
    {
        liveMidiDisplayNotes.insert(message.getNoteNumber());
        lastLiveMidiSignalText = liveMidiDisplayText(liveMidiDisplayNotes);
    }
    else if (message.isNoteOff())
    {
        liveMidiDisplayNotes.erase(message.getNoteNumber());
        lastLiveMidiSignalText = liveMidiDisplayText(liveMidiDisplayNotes);
    }
    else if (message.isController() && liveMidiDisplayNotes.empty())
        lastLiveMidiSignalText = "CC " + juce::String(message.getControllerNumber())
            + "  " + juce::String(message.getControllerValue());
    else
        lastLiveMidiSignalText = liveMidiDisplayText(liveMidiDisplayNotes);

    if (mpc.pendingCommandLearn && message.isController()
        && mpcHardwareBridge.shouldHandleInput(message, sourceName, mpcSamplePanel.isVisible()))
    {
        const auto command = *mpc.pendingCommandLearn;
        mpc.pendingCommandLearn.reset();
        mpc.ccCommandMap[mpcCcKey(message.getChannel(), message.getControllerNumber())] = command;
        appendLiveMidiDebugLog(message, sourceName, -1);
        lastLiveMidiSignalText = "MPC map cc " + juce::String(message.getControllerNumber())
            + " -> " + mpcCommandName(command);
        statusLabel.setText("MPC Learn: CC " + juce::String(message.getControllerNumber())
                            + " mapped to " + mpcCommandName(command),
                            juce::dontSendNotification);
        mpcSamplePanel.setHardwareStatus(mpcHardwareBridge.getDeviceState().inputName,
                                         mpcHardwareBridge.getDeviceState().outputName,
                                         lastLiveMidiSignalText);
        return;
    }

    // The MPC surface only *visualises* the live MIDI stream (pad glow + hardware status).
    // It must never consume the message: earlier this returned while the panel was visible,
    // which swallowed every note from every source — so both the pads and a plain MIDI
    // keyboard went silent. Let the message fall through to the musical routing below so
    // pads actually play (and record) through the armed track / sampler.
    if (mpcHardwareBridge.shouldHandleInput(message, sourceName, mpcSamplePanel.isVisible()))
    {
        const auto pad = mpcHardwareBridge.handleIncomingMessage(message, sourceName);
        if (message.isController())
        {
            const auto mapped = mpc.ccCommandMap.find(mpcCcKey(message.getChannel(), message.getControllerNumber()));
            if (mapped != mpc.ccCommandMap.end())
            {
                appendLiveMidiDebugLog(message, sourceName, -1);
                if (message.getControllerValue() >= 64)
                {
                    statusLabel.setText("MPC CC: " + mpcCommandName(mapped->second), juce::dontSendNotification);
                    handleMpcCommand(mapped->second);
                }
                mpcSamplePanel.setHardwareStatus(mpcHardwareBridge.getDeviceState().inputName,
                                                 mpcHardwareBridge.getDeviceState().outputName,
                                                 mpcHardwareBridge.getLastMidiDescription());
                return;
            }
        }
        if (pad)
        {
            appendLiveMidiDebugLog(message, sourceName, pad->padIndex);
            mpcSamplePanel.setPadActivity(pad->padIndex, pad->velocity);
            if (message.isNoteOnOrOff())
                lastLiveMidiSignalText = liveMidiDisplayText(liveMidiDisplayNotes);
        }
        mpcSamplePanel.setHardwareStatus(mpcHardwareBridge.getDeviceState().inputName,
                                         mpcHardwareBridge.getDeviceState().outputName,
                                         mpcHardwareBridge.getLastMidiDescription());

        // Kit mode: only loaded pads sound. 16 Levels/Tune mode: one selected sample
        // is pitched across all 16 pad notes, so empty pads must still be consumed here.
        const bool tuneMode = mpc.sixteenLevels && mpc.tuneSourcePath.isNotEmpty();
        const bool chopMode = mpc.chopMode && mpc.chopSourcePath.isNotEmpty();
        if (mpcSamplePanel.isVisible() && pad && (tuneMode || chopMode || mpcSamplePanel.isPadLoaded(pad->padIndex)))
        {
            int tunePadIndex = pad->padIndex;
            if (tuneMode)
            {
                // Some MPC 16-Levels modes output the same MIDI note for every physical pad,
                // encoding the "level" as channel or velocity variation instead of chromatic notes.
                // If all hits map to the selected root pad, use channel first and velocity buckets
                // so Orion still produces 16 pitched levels instead of replaying one note.
                const int rootPad = juce::jlimit(0, 15, mpc.tuneRootNote - 36);
                if (pad->padIndex == rootPad)
                {
                    if (message.getChannel() >= 1 && message.getChannel() <= 16)
                        tunePadIndex = message.getChannel() - 1;
                    else if (message.isNoteOn())
                        tunePadIndex = juce::jlimit(0, 15, static_cast<int>(std::round((pad->velocity - 1) * 15.0 / 126.0)));

                    if (message.isNoteOn() && tunePadIndex == rootPad && pad->velocity == 127)
                    {
                        ++mpc.repeatedRootNoteCount;
                        if (mpc.repeatedRootNoteCount >= 4)
                            statusLabel.setText("MPC 16 Levels: hardware is sending only note "
                                                + juce::String(message.getNoteNumber())
                                                + " / pad " + juce::String(rootPad + 1)
                                                + ". Turn off MPC hardware 16 Levels; use Orion 16 Levels with normal pad MIDI.",
                                                juce::dontSendNotification);
                    }
                    else
                    {
                        mpc.repeatedRootNoteCount = 0;
                    }
                }
            }

            static constexpr double mpcPadRearmDelayMs = 140.0;
            const int hardwareNoteKey = message.getChannel() * 128 + message.getNoteNumber();
            if (message.isNoteOn())
            {
                if (mpc.heldHardwareNoteKeys.count(hardwareNoteKey) > 0)
                {
                    const auto releaseIt = mpc.hardwareNoteReleaseTimes.find(hardwareNoteKey);
                    const auto now = juce::Time::getMillisecondCounterHiRes();
                    if (releaseIt == mpc.hardwareNoteReleaseTimes.end()
                        || now - releaseIt->second < mpcPadRearmDelayMs)
                    {
                        mpc.hardwareNoteReleaseTimes.erase(hardwareNoteKey);
                        return;
                    }

                    mpc.heldHardwareNoteKeys.erase(hardwareNoteKey);
                    mpc.hardwareNoteReleaseTimes.erase(hardwareNoteKey);
                    mpc.hardwareNotePads.erase(hardwareNoteKey);
                }

                mpc.heldHardwareNoteKeys.insert(hardwareNoteKey);
                mpc.hardwareNotePads[hardwareNoteKey] = tunePadIndex;
                if (tuneMode)
                {
                    liveMidiDisplayNotes.erase(message.getNoteNumber());
                    liveMidiDisplayNotes.insert(mpc.tuneRootNote + (tunePadIndex - juce::jlimit(0, 15, mpc.tuneRootNote - 36)));
                    lastLiveMidiSignalText = liveMidiDisplayText(liveMidiDisplayNotes);
                }
                playMpcPad(tunePadIndex, pad->velocity);
            }
            else if (message.isNoteOff())
            {
                if (tuneMode)
                {
                    liveMidiDisplayNotes.erase(mpc.tuneRootNote + (tunePadIndex - juce::jlimit(0, 15, mpc.tuneRootNote - 36)));
                    lastLiveMidiSignalText = liveMidiDisplayText(liveMidiDisplayNotes);
                }
                mpc.hardwareNoteReleaseTimes[hardwareNoteKey] = juce::Time::getMillisecondCounterHiRes();
            }

            return;
        }
        else
        {
            appendLiveMidiDebugLog(message, sourceName, -1);
        }
    }

    const auto targetTrack = resolveLiveMidiTargetTrack();

    // Note on (a note-on with velocity 0 is a note-off by MIDI convention, which
    // isNoteOn() correctly reports as false / isNoteOff() as true).
    if (message.isNoteOn())
    {
        const auto note     = message.getNoteNumber();
        const auto velocity = juce::jlimit(1, 127, static_cast<int>(message.getVelocity()));

        // Sampler open → route through it so hardware MIDI maps slices + highlights the key,
        // exactly like the on-screen/typing keyboard (its onNoteOn callback plays + records).
        if (samplerPanel.isVisible())
        {
            samplerPanel.externalMidiNoteOn(note, velocity);
            return;
        }

        // Step-write into the MIDI editor when it's armed for it. It returns true
        // only when it consumed the note, in which case it has already previewed
        // the (possibly scale-snapped) pitch — so don't also play it directly.
        const bool consumedByEditor = midiEditorOverlay.isVisible()
                                      && midiEditorOverlay.stepWriteMidiNoteOn(note, velocity);

        // Chord mode expands one key into a diatonic chord for live play + recording. Step-write
        // keeps its own single-note behaviour (the editor already handled the note).
        const auto pitches = consumedByEditor ? std::vector<int>{ note } : chordPitchesForNote(note);
        if (! consumedByEditor)
        {
            liveChordVoicing[note] = pitches;
            liveMidiDisplayNotes.erase(note);
            for (const auto p : pitches)
                liveMidiDisplayNotes.insert(p);
            lastLiveMidiSignalText = liveMidiDisplayText(liveMidiDisplayNotes);
            if (targetTrack >= 0)
                for (const auto p : pitches)
                    liveMidiNoteOn(targetTrack, p, velocity);
        }

        for (const auto p : pitches)
            recordNoteOn(p, velocity);
        return;
    }

    if (message.isNoteOff())
    {
        const auto note = message.getNoteNumber();

        if (samplerPanel.isVisible())
        {
            samplerPanel.externalMidiNoteOff(note);
            return;
        }

        const bool consumedByEditor = midiEditorOverlay.isVisible()
                                      && midiEditorOverlay.stepWriteMidiNoteOff(note);

        // Release exactly the pitches this key sounded (remembered at note-on), so a chord is
        // fully released even if chord mode was toggled off while the key was held.
        std::vector<int> pitches { note };
        if (const auto it = liveChordVoicing.find(note); it != liveChordVoicing.end())
        {
            pitches = it->second;
            liveChordVoicing.erase(it);
        }

        liveMidiDisplayNotes.erase(note);
        for (const auto p : pitches)
            liveMidiDisplayNotes.erase(p);
        lastLiveMidiSignalText = liveMidiDisplayText(liveMidiDisplayNotes);

        if (! consumedByEditor && targetTrack >= 0)
            for (const auto p : pitches)
                liveMidiNoteOff(targetTrack, p);

        for (const auto p : pitches)
            recordNoteOff(p);
        return;
    }

    // Controllers / pitch bend / aftertouch only mean something to a hosted VST
    // instrument; forward them through verbatim. (The sampler has no modulation
    // inputs, so there's nothing to route them to there.)
    if (targetTrack >= 0
        && (message.isController() || message.isPitchWheel()
            || message.isAftertouch() || message.isChannelPressure()
            || message.isProgramChange()))
    {
        if (arrangementPlaybackSource != nullptr
            && arrangementPlaybackSource->hasTrackInstrument(targetTrack))
            arrangementPlaybackSource->instrumentLiveMidiMessage(targetTrack, message);
    }
}

// Resolves which track a hardware MIDI keyboard should play/record into. Priority
// follows where the user's attention is: an open recording take, then an open
// sampler/MIDI-editor panel, then the armed/selected MIDI track.
int MainComponent::resolveLiveMidiTargetTrack()
{
    auto& tracks = projectState.getTracks();
    const auto valid = [&](int idx) { return idx >= 0 && idx < static_cast<int>(tracks.size()); };

    // While a take is rolling, always sound the track we're recording into so the
    // player hears exactly what's being captured.
    if (recordingSession.has_value() && valid(recordingSession->trackIndex))
        return recordingSession->trackIndex;

    if (samplerPanel.isVisible())
    {
        const auto idx = samplerPanel.getActiveTrackIndex();
        if (valid(idx))
            return idx;
    }

    if (midiEditorOverlay.isVisible() && selectedArrangementClip.has_value()
        && valid(selectedArrangementClip->first))
        return selectedArrangementClip->first;

    return resolveArmedMidiTrack();
}

// The record-target MIDI track: an explicitly R-armed MIDI track, else the
// selected clip's track, else a selected MIDI track header. -1 if none.
int MainComponent::resolveArmedMidiTrack()
{
    auto& tracks = projectState.getTracks();

    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].isMidiTrack && tracks[i].recordArmed)
            return static_cast<int>(i);

    if (selectedArrangementClip.has_value())
    {
        const auto idx = selectedArrangementClip->first;
        if (idx >= 0 && idx < static_cast<int>(tracks.size()) && tracks[static_cast<std::size_t>(idx)].isMidiTrack)
            return idx;
    }

    const auto sel = arrangementTimeline.getSelectedTrackIndex();
    if (sel.has_value() && *sel >= 0 && *sel < static_cast<int>(tracks.size())
        && tracks[static_cast<std::size_t>(*sel)].isMidiTrack)
        return *sel;

    return -1;
}

std::vector<int> MainComponent::chordPitchesForNote(int midiNote) const
{
    // Chord mode needs a project key to be diatonic. Off → the note plays as-is.
    if (! projectState.isChordModeEnabled() || ! projectState.isKeyEnabled())
        return { midiNote };

    // Diatonic scales as semitone offsets from the tonic. Minor = natural minor.
    static constexpr int majorScale[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int minorScale[7] = { 0, 2, 3, 5, 7, 8, 10 };
    const int* scale = projectState.isKeyMinor() ? minorScale : majorScale;
    const int root = ((projectState.getKeyRoot() % 12) + 12) % 12;

    // Snap the played note to the nearest scale tone, then find its scale degree + octave.
    const int pc = (((midiNote - root) % 12) + 12) % 12;
    int degree = 0, best = 128;
    for (int i = 0; i < 7; ++i)
    {
        const int dist = std::abs(scale[i] - pc);
        if (dist < best) { best = dist; degree = i; }
    }
    const int snapped = midiNote + (scale[degree] - pc);
    const int octave = (snapped - root - scale[degree]) / 12;

    // Stack diatonic thirds (degree, +2, +4, …) so each chord's quality is correct for the key.
    const int size = juce::jlimit(3, 7, projectState.getChordSizeNotes());
    std::vector<int> pitches;
    pitches.reserve(static_cast<std::size_t>(size));
    for (int k = 0; k < size; ++k)
    {
        const int d = degree + 2 * k;
        const int o = octave + d / 7;
        const int midi = root + 12 * o + scale[d % 7];
        pitches.push_back(juce::jlimit(0, 127, midi));
    }
    return pitches;
}

bool MainComponent::samplerTrackGatesByNoteLength(int trackIndex) const
{
    const auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return false;
    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    return track.samplerMode == SamplerPlaybackMode::classic && ! track.samplerFullSampleTrigger;
}

void MainComponent::syncChordModeToSurfaces()
{
    const bool on   = projectState.isChordModeEnabled();
    const int  size = projectState.getChordSizeNotes();
    midiEditorOverlay.setChordModeExternally(on, size);
    updateTransportLabels();
    arrangementTimeline.repaint();   // track headers reflect the shared state
}

void MainComponent::liveMidiNoteOn(int trackIndex, int midiNote, int velocity)
{
    if (arrangementPlaybackSource == nullptr)
        return;

    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (arrangementPlaybackSource->hasTrackInstrument(trackIndex))
    {
        arrangementPlaybackSource->instrumentLiveNoteOn(trackIndex, midiNote, velocity);
        return;
    }

    if (track.isMpcKit)
    {
        if (track.isMpcChopMode && track.mpcChopSample.isNotEmpty())
        {
            const int slice = juce::jlimit(0, juce::jmax(0, track.mpcChopSliceCount - 1), midiNote - 36);
            arrangementPlaybackSource->samplerNoteOn(track.mpcChopSample,
                                                     midiNote,
                                                     velocity,
                                                     36,
                                                     track.volumeDb,
                                                     SamplerPlaybackMode::slice,
                                                     slice,
                                                     juce::jlimit(1, 64, track.mpcChopSliceCount),
                                                     false,
                                                     0.0,
                                                     true);
            return;
        }

        if (track.isMpcTuneMode && track.mpcTuneSample.isNotEmpty())
        {
            arrangementPlaybackSource->samplerNoteOn(track.mpcTuneSample,
                                                     midiNote,
                                                     velocity,
                                                     track.mpcTuneRoot,
                                                     track.volumeDb,
                                                     SamplerPlaybackMode::oneShot,
                                                     0,
                                                     1,
                                                     false,
                                                     0.0,
                                                     true);
            return;
        }

        const int pad = midiNote - 36;
        if (pad >= 0 && pad < 16)
        {
            const auto& sample = track.mpcKitSamples[static_cast<std::size_t>(pad)];
            if (sample.isNotEmpty())
                arrangementPlaybackSource->samplerNoteOn(sample,
                                                         midiNote,
                                                         velocity,
                                                         midiNote,
                                                         track.volumeDb,
                                                         SamplerPlaybackMode::oneShot,
                                                         0,
                                                         1,
                                                         false,
                                                         0.0,
                                                         true);
        }
        return;
    }

    if (track.samplerSourcePath.isNotEmpty())
        arrangementPlaybackSource->samplerNoteOn(track.samplerSourcePath,
                                                 midiNote,
                                                 velocity,
                                                 track.samplerRootMidiNote,
                                                 track.volumeDb,
                                                 track.samplerMode,
                                                 0,
                                                 track.samplerSliceCount,
                                                 track.samplerWarpEnabled,
                                                 track.samplerSourceBpm,
                                                 track.samplerFullSampleTrigger);
}

void MainComponent::liveMidiNoteOff(int trackIndex, int midiNote)
{
    if (arrangementPlaybackSource == nullptr)
        return;

    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (arrangementPlaybackSource->hasTrackInstrument(trackIndex))
        arrangementPlaybackSource->instrumentLiveNoteOff(trackIndex, midiNote);
    else
        arrangementPlaybackSource->samplerNoteOff(midiNote, track.samplerMode,
                                                  samplerTrackGatesByNoteLength(trackIndex));
}

void MainComponent::auditionArrangementChord(const std::vector<int>& pitches)
{
    stopArrangementChordAudition();
    if (pitches.empty() || arrangementPlaybackSource == nullptr)
        return;

    // Play through the built-in chord preview synth so it's always audible, regardless of tracks.
    for (int p : pitches)
    {
        const int note = juce::jlimit(0, 127, p);
        arrangementPlaybackSource->chordPreviewNoteOn(note, 0.85f);
        activeChordAuditionNotes.emplace_back(-1, note);
    }
    const int gen = ++chordAuditionGeneration;
    juce::Timer::callAfterDelay(750, [this, gen] { if (gen == chordAuditionGeneration) stopArrangementChordAudition(); });
}

void MainComponent::stopArrangementChordAudition()
{
    if (arrangementPlaybackSource != nullptr)
        for (auto& [t, p] : activeChordAuditionNotes)
            arrangementPlaybackSource->chordPreviewNoteOff(p);
    activeChordAuditionNotes.clear();
}
} // namespace orion
