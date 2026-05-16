#include "ProjectSerializer.h"

namespace
{
juce::String clipTypeToString(orion::ClipType type)
{
    return type == orion::ClipType::midi ? "midi" : "audio";
}

juce::String samplerModeToString(orion::SamplerPlaybackMode mode)
{
    switch (mode)
    {
        case orion::SamplerPlaybackMode::oneShot:
            return "oneShot";
        case orion::SamplerPlaybackMode::slice:
            return "slice";
        case orion::SamplerPlaybackMode::classic:
        default:
            return "classic";
    }
}

juce::var midiNoteToVar(const orion::MidiNote& note)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("pitch", note.pitch);
    object->setProperty("startBeat", note.startBeat);
    object->setProperty("lengthInBeats", note.lengthInBeats);
    object->setProperty("velocity", note.velocity);
    return juce::var(object);
}

juce::var timelineClipToVar(const orion::TimelineClip& clip)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("name", clip.name);
    object->setProperty("type", clipTypeToString(clip.type));
    object->setProperty("startBeat", clip.startBeat);
    object->setProperty("lengthInBeats", clip.lengthInBeats);
    object->setProperty("colour", clip.colour.toString());
    object->setProperty("sourcePath", clip.sourcePath);
    object->setProperty("gainDb", clip.gainDb);
    object->setProperty("muted", clip.muted);
    object->setProperty("solo", clip.solo);
    object->setProperty("sourceDurationSeconds", clip.sourceDurationSeconds);
    object->setProperty("sourceBpm", clip.sourceBpm);
    object->setProperty("detectedBars", clip.detectedBars);
    object->setProperty("warpEnabled", clip.warpEnabled);
    object->setProperty("bpmGuessed", clip.bpmGuessed);
    object->setProperty("warpTargetLengthInBeats", clip.warpTargetLengthInBeats);

    juce::Array<juce::var> notes;
    for (const auto& note : clip.midiNotes)
        notes.add(midiNoteToVar(note));

    object->setProperty("midiNotes", juce::var(notes));
    return juce::var(object);
}

juce::var trackStateToVar(const orion::TrackState& track)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("name", track.name);
    object->setProperty("isMidiTrack", track.isMidiTrack);
    object->setProperty("colour", track.colour.toString());
    object->setProperty("muted", track.muted);
    object->setProperty("solo", track.solo);
    object->setProperty("recordArmed", track.recordArmed);
    object->setProperty("volumeDb", track.volumeDb);
    object->setProperty("samplerSourcePath", track.samplerSourcePath);
    object->setProperty("samplerRootMidiNote", track.samplerRootMidiNote);
    object->setProperty("samplerMode", samplerModeToString(track.samplerMode));
    object->setProperty("samplerKeyboardOctaveOffset", track.samplerKeyboardOctaveOffset);
    object->setProperty("samplerTransposeSemitones", track.samplerTransposeSemitones);
    object->setProperty("samplerSliceCount", track.samplerSliceCount);
    object->setProperty("samplerWarpEnabled", track.samplerWarpEnabled);
    object->setProperty("samplerSourceBpm", track.samplerSourceBpm);
    object->setProperty("samplerSourceDurationSeconds", track.samplerSourceDurationSeconds);
    object->setProperty("samplerDetectedBars", track.samplerDetectedBars);

    juce::Array<juce::var> clips;
    for (const auto& clip : track.clips)
        clips.add(timelineClipToVar(clip));

    object->setProperty("clips", juce::var(clips));
    return juce::var(object);
}
}  // namespace

namespace orion
{
bool ProjectSerializer::saveToFile(const ProjectState& projectState,
                                   const juce::File& destinationFile,
                                   juce::String* errorMessage)
{
    auto* rootObject = new juce::DynamicObject();
    rootObject->setProperty("app", "Orion");
    rootObject->setProperty("formatVersion", 1);
    rootObject->setProperty("tempoBpm", projectState.getTempoBpm());
    rootObject->setProperty("timeSigNumerator", projectState.getNumerator());
    rootObject->setProperty("timeSigDenominator", projectState.getDenominator());
    rootObject->setProperty("loopLengthInBeats", projectState.getLoopLengthInBeats());
    rootObject->setProperty("loopRangeActive", projectState.hasLoopRange());
    rootObject->setProperty("loopStartBeat", projectState.getLoopStartBeat());
    rootObject->setProperty("loopEndBeat", projectState.getLoopEndBeat());

    juce::Array<juce::var> trackArray;
    for (const auto& track : projectState.getTracks())
        trackArray.add(trackStateToVar(track));

    rootObject->setProperty("tracks", juce::var(trackArray));

    const auto json = juce::JSON::toString(juce::var(rootObject), true);
    if (! destinationFile.replaceWithText(json))
    {
        if (errorMessage != nullptr)
            *errorMessage = "Could not write project file";
        return false;
    }

    return true;
}
}  // namespace orion
