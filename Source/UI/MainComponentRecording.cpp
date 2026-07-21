// Recording for MainComponent: MIDI take capture, audio-input configuration, and audio
// take capture.
//
// Split out of MainComponent.cpp purely to keep that file manageable — this is the SAME
// class, same members, identical logic; only the definitions live here. See MainComponent.h
// for the declarations.
#include "MainComponent.h"

#include "../Audio/AudioInputRecorder.h"
#include "../Audio/PlaybackSources.h"
#include "../Sampler/SamplerEngine.h"
#include "OrionTheme.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace orion
{
void MainComponent::startMidiRecordingFromRecordButtonIfNeeded()
{
    if (! transportEngine.isRecordArmed()
        || transportEngine.isPlaying()
        || transportEngine.isCountInActive())
        return;

    auto& tracks = projectState.getTracks();
    auto targetTrack = resolveArmedMidiTrack();
    if (targetTrack < 0)
    {
        const auto audioArmed = std::any_of(tracks.begin(), tracks.end(), [](const TrackState& track)
        {
            return ! track.isMidiTrack && track.recordArmed;
        });
        if (audioArmed)
        {
            // Audio-only take: there is no MIDI track to arm, but the transport still has to
            // roll. Returning here left Record armed with nothing happening — no playhead, no
            // metronome, no recording.
            toggleTransportFromUi();
            return;
        }

        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        {
            if (tracks[static_cast<std::size_t>(i)].isMidiTrack)
            {
                targetTrack = i;
                break;
            }
        }
    }

    if (targetTrack < 0)
        return;

    if (targetTrack < static_cast<int>(tracks.size()))
    {
        tracks[static_cast<std::size_t>(targetTrack)].recordArmed = true;
        arrangementTimeline.repaint();
        mixerPanel.repaint();
    }

    toggleTransportFromUi();
}

void MainComponent::stopTransportFromUi()
{
    finishRecordingAndDisarm();
    if (masterStripSource != nullptr)
        masterStripSource->requestStopFade();
    stopClipEditorPreview(true);
    samplerPanel.stopPreviewPlayback(); // halt the simpler audition + waveform playhead
    if (arrangementPlaybackSource != nullptr)
    {
        arrangementPlaybackSource->allInstrumentNotesOff(); // flush any hung instrument notes
        arrangementPlaybackSource->allSamplerNotesOff();    // and any sampler audition voices
    }
    transportController.stop(
        [this]() { stopBrowserPreview(true); },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    if (clipEditorPanel.isVisible())
    {
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->clearClipEditorPreviewTrim();
        setClipEditorLocalPreviewPosition(clipEditorPreviewStartRatio);
        refreshClipEditor();
    }
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::finishRecordingAndDisarm()
{
    const auto hadRecording = recordingSession.has_value() || audioRecordingSession.has_value();

    finalizeRecordingClip(); // close any in-flight MIDI recording session
    finalizeAudioRecordingClip();

    if (hadRecording || transportEngine.isRecordArmed())
    {
        transportController.setRecordArmed(false);
        recordButton.setToggleState(false, juce::dontSendNotification);
    }

    updateTransportLabels();
}

void MainComponent::ensureMidiRecordingSession(int armedTrack)
{
    auto& tracks = projectState.getTracks();
    if (armedTrack < 0 || armedTrack >= static_cast<int>(tracks.size()))
        return;
    if (recordingSession.has_value() && recordingSession->trackIndex == armedTrack)
        return;   // already recording this track

    finalizeRecordingClip();
    const auto playheadBeat = transportEngine.getPlayheadBeat();
    auto& track = tracks[static_cast<std::size_t>(armedTrack)];
    arrangementTimeline.captureUndoSnapshot();

    // Overdub: if the playhead is inside an existing MIDI clip on this track, record INTO it
    // (merge the new notes) instead of stacking a separate overlapping clip.
    int targetClip = -1;
    for (int i = 0; i < static_cast<int>(track.clips.size()); ++i)
    {
        const auto& c = track.clips[i];
        if (c.type == ClipType::midi
            && playheadBeat >= c.startBeat - 1.0e-6
            && playheadBeat < c.startBeat + c.lengthInBeats - 1.0e-6)
        {
            targetClip = i;
            break;
        }
    }

    RecordingSession session;
    session.trackIndex = armedTrack;
    if (targetClip >= 0)
    {
        track.clips[static_cast<std::size_t>(targetClip)].recording = true;
        session.clipIndex     = targetClip;
        session.clipStartBeat = track.clips[static_cast<std::size_t>(targetClip)].startBeat;
    }
    else
    {
        const auto clipStart = std::floor(playheadBeat);
        track.clips.push_back(TimelineClip {
            "Recording",
            ClipType::midi,
            clipStart,
            juce::jmax(0.25, playheadBeat - clipStart),
            track.colour.brighter(0.1f),
            {}, {}, "", 0.0, false, false,
            0.0, 0.0, 0,
            false, false, 0.0,
            -1, false, true
        });
        track.clips.back().recording = true;
        session.clipIndex     = static_cast<int>(track.clips.size()) - 1;
        session.clipStartBeat = clipStart;
    }
    recordingSession = std::move(session);
}

void MainComponent::recordNoteOn(int pitch, int velocity)
{
    if (! transportEngine.isRecordArmed())
        return;
    if (! transportEngine.isPlaying())
    {
        // Not playing yet. Capture ONLY the anticipated first chord struck in the LAST BEAT of the
        // count-in (it lands at the clip start, beat 0) — players hit the downbeat a hair early. Notes
        // played earlier in the count-in aren't part of the take, so they're ignored: that avoids a
        // whole rushed progression piling up at beat 0 (which read as "only one chord recorded").
        if (! transportEngine.isCountInActive())
            return;
        const double countInBeats = static_cast<double>(juce::jmax(1, projectState.getNumerator()));
        if (transportEngine.getClickBeat() < countInBeats - 1.0)
            return;
    }
    // Open the session on the spot if the timer hasn't yet — otherwise the very first note
    // (played in the gap between count-in ending and the next timer tick) was dropped.
    if (! recordingSession.has_value())
        ensureMidiRecordingSession(resolveArmedMidiTrack());
    if (! recordingSession.has_value())
        return;

    const auto playheadBeat = transportEngine.getPlayheadBeat();
    const auto beatInClip   = juce::jmax(0.0, playheadBeat - recordingSession->clipStartBeat);
    recordingSession->pendingNotes[pitch] = { velocity, beatInClip };
}

void MainComponent::recordNoteOff(int pitch)
{
    if (! recordingSession.has_value()) return;

    auto it = recordingSession->pendingNotes.find(pitch);
    if (it == recordingSession->pendingNotes.end()) return;

    const auto playheadBeat = transportEngine.getPlayheadBeat();
    const auto endBeatInClip = juce::jmax(it->second.startBeatInClip + 0.05,
                                          playheadBeat - recordingSession->clipStartBeat);
    const auto lengthBeats = endBeatInClip - it->second.startBeatInClip;

    auto& tracks = projectState.getTracks();
    if (recordingSession->trackIndex < static_cast<int>(tracks.size()))
    {
        auto& clips = tracks[static_cast<std::size_t>(recordingSession->trackIndex)].clips;
        if (recordingSession->clipIndex < static_cast<int>(clips.size()))
        {
            auto& clip = clips[static_cast<std::size_t>(recordingSession->clipIndex)];
            const juce::ScopedLock sl(projectState.getAudioEditLock());
            clip.midiNotes.push_back(MidiNote { pitch, it->second.startBeatInClip, lengthBeats, it->second.velocity });
            if (endBeatInClip + 0.5 > clip.lengthInBeats)
                clip.lengthInBeats = std::ceil(endBeatInClip + 0.25); // expand to next quarter
        }
    }

    recordingSession->pendingNotes.erase(it);
    arrangementTimeline.repaint();
}

void MainComponent::finalizeRecordingClip()
{
    if (! recordingSession.has_value()) return;

    // Close any notes that are still held when recording ends.
    const auto playheadBeat = transportEngine.getPlayheadBeat();
    auto& tracks = projectState.getTracks();

    if (recordingSession->trackIndex < static_cast<int>(tracks.size()))
    {
        auto& clips = tracks[static_cast<std::size_t>(recordingSession->trackIndex)].clips;
        if (recordingSession->clipIndex < static_cast<int>(clips.size()))
        {
            auto& clip = clips[static_cast<std::size_t>(recordingSession->clipIndex)];
            const juce::ScopedLock sl(projectState.getAudioEditLock());
            for (const auto& [pitch, pending] : recordingSession->pendingNotes)
            {
                const auto endBeatInClip = juce::jmax(pending.startBeatInClip + 0.05,
                                                      playheadBeat - recordingSession->clipStartBeat);
                clip.midiNotes.push_back(MidiNote {
                    pitch,
                    pending.startBeatInClip,
                    endBeatInClip - pending.startBeatInClip,
                    pending.velocity
                });
                if (endBeatInClip + 0.5 > clip.lengthInBeats)
                    clip.lengthInBeats = std::ceil(endBeatInClip + 0.25);
            }
            // If the recording captured nothing, drop the empty clip so we don't pollute the timeline.
            if (clip.midiNotes.empty())
            {
                clips.erase(clips.begin() + recordingSession->clipIndex);
            }
            else
            {
                clip.recording = false;
                // Round the clip length UP to the next bar boundary so playback always
                // covers a full bar — matches FL's behaviour where the pattern length
                // snaps to the bar grid even if you stopped recording mid-bar.
                const auto beatsPerBar = static_cast<double>(juce::jmax(1, projectState.getNumerator()));
                const auto bars        = std::ceil(clip.lengthInBeats / beatsPerBar);
                clip.lengthInBeats     = juce::jmax(beatsPerBar, bars * beatsPerBar);
            }
        }
    }

    recordingSession.reset();
    arrangementTimeline.repaint();
}

juce::File MainComponent::getAudioRecordingDirectory() const
{
    if (currentProjectFile.existsAsFile())
        return currentProjectFile.getParentDirectory().getChildFile("Recordings");

    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Orion Recordings");
}

void MainComponent::updateInputMonitoring()
{
    if (audioInputRecorder == nullptr)
        return;

    bool anyAudioArmed = false;
    for (const auto& t : projectState.getTracks())
        if (! t.isMidiTrack && t.recordArmed) { anyAudioArmed = true; break; }

    const bool wantInput = anyAudioArmed || audioInputRecorder->isRecording();
    if (wantInput && ! audioRecorderCallbackAttached)
    {
        if (! ensureAudioInputReady(true))
            return;

        audioDeviceManager.addAudioCallback(audioInputRecorder.get());
        audioRecorderCallbackAttached = true;
    }
    else if (! wantInput && audioRecorderCallbackAttached)
    {
        audioDeviceManager.removeAudioCallback(audioInputRecorder.get());
        audioRecorderCallbackAttached = false;
    }
}

bool MainComponent::ensureAudioInputReady(bool requestPermission)
{
    if (audioInputUnavailable)
        return false;

    if (juce::RuntimePermissions::isRequired(juce::RuntimePermissions::recordAudio)
        && ! juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio))
    {
        if (requestPermission && ! audioInputPermissionRequestInFlight)
        {
            audioInputPermissionRequestInFlight = true;
            juce::Component::SafePointer<MainComponent> safeThis(this);
            juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
                                              [safeThis](bool granted)
                                              {
                                                  if (safeThis == nullptr)
                                                      return;

                                                  safeThis->audioInputPermissionRequestInFlight = false;
                                                  if (granted)
                                                      safeThis->updateInputMonitoring();
                                                  else
                                                      safeThis->statusLabel.setText("Microphone permission denied. Enable it in macOS Privacy settings.",
                                                                                    juce::dontSendNotification);
                                              });
        }
        return false;
    }

    auto* currentDevice = audioDeviceManager.getCurrentAudioDevice();

    // Already have input channels running → attach immediately.
    if (currentDevice != nullptr
        && currentDevice->getActiveInputChannels().countNumberOfSetBits() > 0)
        return true;

    // Not ready yet: configure the device on a BACKGROUND thread and return false for now —
    // the caller re-checks when it completes (via updateInputMonitoring). Starting a CoreAudio
    // device — especially JUCE's combiner for a separate input vs output device (e.g. the MPC
    // Sample as input + your monitors as output) — can block, and doing it synchronously on the
    // message thread SELF-DEADLOCKS: CoreAudio needs the main run loop to service the start, but
    // the main thread is stuck inside it (confirmed by a process sample: setAudioDeviceSetup →
    // AudioIODeviceCombiner::start → HALB_IOThread::_WaitForState). Off-thread start keeps the
    // run loop free, so separate input/output devices just work (Ableton-style, no aggregate)
    // and the UI never freezes.
    beginAudioInputConfiguration();
    return false;
}

void MainComponent::beginAudioInputConfiguration()
{
    if (audioInputConfiguring.exchange(true))
        return;   // a configuration attempt is already in flight

    const auto previousSetup = audioDeviceManager.getAudioDeviceSetup();
    auto setup = previousSetup;
    if (setup.inputDeviceName.isEmpty())
    {
        if (auto* type = audioDeviceManager.getCurrentDeviceTypeObject())
        {
            const auto inputNames = type->getDeviceNames(true);
            if (inputNames.isEmpty())
            {
                statusLabel.setText("No audio input device found.", juce::dontSendNotification);
                audioInputConfiguring = false;
                return;
            }

            const auto defaultIndex = juce::jlimit(0, inputNames.size() - 1, type->getDefaultDeviceIndex(true));
            setup.inputDeviceName = inputNames[defaultIndex];
        }
    }

    setup.inputChannels.clear();
    setup.inputChannels.setRange(0, 2, true);

    statusLabel.setText("Starting audio input " + setup.inputDeviceName + juce::String::fromUTF8(" \xE2\x80\xA6"),
                        juce::dontSendNotification);

    auto* adm = &audioDeviceManager;
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Thread::launch([adm, setup, previousSetup, safeThis]
    {
        auto error = adm->setAudioDeviceSetup(setup, true);
        auto* currentDevice = adm->getCurrentAudioDevice();
        const auto inputReady = currentDevice != nullptr
            && currentDevice->getActiveInputChannels().countNumberOfSetBits() > 0;
        const auto outputReady = currentDevice != nullptr
            && currentDevice->getActiveOutputChannels().countNumberOfSetBits() > 0;

        // Input setup is optional. If CoreAudio cannot open it, restore the last known-good
        // setup so asking for a microphone can never take the main output down with it.
        bool restoredOutput = false;
        if (error.isNotEmpty() || ! inputReady || ! outputReady)
        {
            const auto restoreError = adm->setAudioDeviceSetup(previousSetup, true);
            restoredOutput = restoreError.isEmpty();
            if (error.isEmpty() && restoreError.isNotEmpty())
                error = restoreError;
        }

        juce::MessageManager::callAsync([safeThis, error, inputReady, outputReady, restoredOutput]
        {
            if (safeThis == nullptr)
                return;

            safeThis->audioInputConfiguring = false;

            if (error.isNotEmpty() || ! inputReady || ! outputReady)
            {
                safeThis->audioInputUnavailable = true;
                safeThis->statusLabel.setText(
                    restoredOutput ? "Microphone unavailable; audio output preserved."
                                   : "Audio input failed; check Audio Settings.",
                    juce::dontSendNotification);
                return;
            }

            safeThis->updateInputMonitoring();   // attach the recorder now the device is up
        });
    });
}

void MainComponent::startAudioRecordingClip(int trackIndex)
{
    if (audioInputRecorder == nullptr)
        return;

    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (track.isMidiTrack)
        return;

    if (! ensureAudioInputReady(true))
        return;

    auto* currentDevice = audioDeviceManager.getCurrentAudioDevice();
    const auto inputChannels = currentDevice != nullptr
        ? juce::jmin(2, currentDevice->getActiveInputChannels().countNumberOfSetBits())
        : 0;
    if (inputChannels <= 0)
    {
        statusLabel.setText("No audio input selected. Open Settings and enable a microphone/input.",
                            juce::dontSendNotification);
        return;
    }

    const auto playheadBeat = transportEngine.getPlayheadBeat();
    const auto clipStart = std::floor(playheadBeat);
    const auto sampleRate = currentDevice != nullptr && currentDevice->getCurrentSampleRate() > 0.0
        ? currentDevice->getCurrentSampleRate()
        : 44100.0;
    const auto directory = getAudioRecordingDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    const auto file = directory.getNonexistentChildFile("Orion_Take_" + stamp, ".wav", false);

    juce::String error;
    if (! audioInputRecorder->start(file, sampleRate, inputChannels, error))
    {
        statusLabel.setText("Audio recording failed: " + error, juce::dontSendNotification);
        return;
    }

    if (! audioRecorderCallbackAttached)
    {
        audioDeviceManager.addAudioCallback(audioInputRecorder.get());
        audioRecorderCallbackAttached = true;
    }

    TimelineClip clip;
    clip.name = "Audio Recording";
    clip.type = ClipType::audio;
    clip.startBeat = clipStart;
    clip.lengthInBeats = juce::jmax(0.25, playheadBeat - clipStart);
    clip.colour = track.colour.brighter(0.1f);
    clip.sourcePath = file.getFullPathName();
    clip.sourceDurationSeconds = 0.0;
    clip.sourceBpm = projectState.getTempoBpm();
    clip.detectedBars = 0;
    clip.warpEnabled = false;
    clip.bpmGuessed = false;
    clip.warpTargetLengthInBeats = 0.0;
    clip.sourceKeyRoot = -1;
    clip.sourceKeyIsMinor = false;
    clip.keyShiftEnabled = false;
    clip.recording = true;

    // Checkpoint so the finished take can be removed with Cmd+Z (and cleaned up on cancel).
    arrangementTimeline.captureUndoSnapshot();
    track.clips.push_back(std::move(clip));

    AudioRecordingSession session;
    session.trackIndex = trackIndex;
    session.clipIndex = static_cast<int>(track.clips.size()) - 1;
    session.clipStartBeat = clipStart;
    session.sampleRate = sampleRate;
    session.file = file;
    audioRecordingSession = std::move(session);

    statusLabel.setText("Recording audio: " + file.getFileName(), juce::dontSendNotification);
    arrangementTimeline.repaint();
}

void MainComponent::finalizeAudioRecordingClip()
{
    // Note: the input callback stays attached for monitoring while a track is armed;
    // updateInputMonitoring() (and shutdown) own detaching it. Here we only stop the
    // writer so the WAV file is flushed/closed.
    if (! audioRecordingSession.has_value())
    {
        if (audioInputRecorder != nullptr)
            audioInputRecorder->stop();
        return;
    }

    const auto session = *audioRecordingSession;
    audioRecordingSession.reset();
    arrangementTimeline.clearLiveRecordingWaveform();

    const auto samplesWritten = audioInputRecorder != nullptr ? audioInputRecorder->stop() : 0;
    const auto durationSeconds = session.sampleRate > 0.0
        ? static_cast<double>(samplesWritten) / session.sampleRate
        : 0.0;
    const auto lengthBeats = durationSeconds * projectState.getTempoBpm() / 60.0;

    auto& tracks = projectState.getTracks();
    if (session.trackIndex >= 0 && session.trackIndex < static_cast<int>(tracks.size()))
    {
        auto& clips = tracks[static_cast<std::size_t>(session.trackIndex)].clips;
        if (session.clipIndex >= 0 && session.clipIndex < static_cast<int>(clips.size()))
        {
            auto& clip = clips[static_cast<std::size_t>(session.clipIndex)];
            if (samplesWritten <= static_cast<juce::int64>(session.sampleRate * 0.05))
            {
                clips.erase(clips.begin() + session.clipIndex);
                session.file.deleteFile();
                arrangementTimeline.dropLastUndoSnapshot();   // nothing kept — no undo entry
                statusLabel.setText("Audio recording discarded: no input captured", juce::dontSendNotification);
            }
            else
            {
                clip.recording = false;
                clip.sourceDurationSeconds = durationSeconds;
                clip.lengthInBeats = juce::jmax(0.25, lengthBeats);
                clip.warpTargetLengthInBeats = clip.lengthInBeats;
                statusLabel.setText("Recorded audio: " + session.file.getFileName(), juce::dontSendNotification);
            }
        }
    }

    arrangementTimeline.repaint();
}

void MainComponent::cancelRecording()
{
    auto& tracks = projectState.getTracks();

    // Discard the in-progress audio take: stop the recorder, remove its clip + file.
    if (audioRecordingSession.has_value())
    {
        const auto session = *audioRecordingSession;
        audioRecordingSession.reset();
        arrangementTimeline.clearLiveRecordingWaveform();
        if (audioInputRecorder != nullptr)
            audioInputRecorder->stop();
        if (session.trackIndex >= 0 && session.trackIndex < static_cast<int>(tracks.size()))
        {
            auto& clips = tracks[static_cast<std::size_t>(session.trackIndex)].clips;
            if (session.clipIndex >= 0 && session.clipIndex < static_cast<int>(clips.size()))
                clips.erase(clips.begin() + session.clipIndex);
        }
        session.file.deleteFile();
        arrangementTimeline.dropLastUndoSnapshot();
    }

    // Discard the in-progress MIDI take.
    if (recordingSession.has_value())
    {
        const auto session = *recordingSession;
        recordingSession.reset();
        if (session.trackIndex >= 0 && session.trackIndex < static_cast<int>(tracks.size()))
        {
            auto& clips = tracks[static_cast<std::size_t>(session.trackIndex)].clips;
            if (session.clipIndex >= 0 && session.clipIndex < static_cast<int>(clips.size()))
                clips.erase(clips.begin() + session.clipIndex);
        }
        arrangementTimeline.dropLastUndoSnapshot();
    }

    stopTransportFromUi();
    statusLabel.setText("Recording cancelled", juce::dontSendNotification);
    arrangementTimeline.repaint();
}
} // namespace orion
