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
    object->setProperty("sourceKeyRoot", clip.sourceKeyRoot);
    object->setProperty("sourceKeyIsMinor", clip.sourceKeyIsMinor);
    object->setProperty("keyShiftEnabled", clip.keyShiftEnabled);

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
    object->setProperty("instrumentPluginId", track.instrumentPluginId);
    object->setProperty("instrumentPluginName", track.instrumentPluginName);
    object->setProperty("instrumentStateBase64", track.instrumentStateBase64);

    juce::Array<juce::var> clips;
    for (const auto& clip : track.clips)
        clips.add(timelineClipToVar(clip));

    object->setProperty("clips", juce::var(clips));
    return juce::var(object);
}

// ---- Deserialization helpers -------------------------------------------------

orion::ClipType clipTypeFromString(const juce::String& text)
{
    return text == "midi" ? orion::ClipType::midi : orion::ClipType::audio;
}

orion::SamplerPlaybackMode samplerModeFromString(const juce::String& text)
{
    if (text == "oneShot")
        return orion::SamplerPlaybackMode::oneShot;
    if (text == "slice")
        return orion::SamplerPlaybackMode::slice;
    return orion::SamplerPlaybackMode::classic;
}

// Reads a property if present, otherwise returns fallback unchanged.
double getDouble(const juce::DynamicObject& obj, const char* key, double fallback)
{
    return obj.hasProperty(key) ? static_cast<double>(obj.getProperty(key)) : fallback;
}

int getInt(const juce::DynamicObject& obj, const char* key, int fallback)
{
    return obj.hasProperty(key) ? static_cast<int>(obj.getProperty(key)) : fallback;
}

bool getBool(const juce::DynamicObject& obj, const char* key, bool fallback)
{
    return obj.hasProperty(key) ? static_cast<bool>(obj.getProperty(key)) : fallback;
}

juce::String getString(const juce::DynamicObject& obj, const char* key)
{
    return obj.hasProperty(key) ? obj.getProperty(key).toString() : juce::String();
}

juce::Colour getColour(const juce::DynamicObject& obj, const char* key, juce::Colour fallback)
{
    if (! obj.hasProperty(key))
        return fallback;
    return juce::Colour::fromString(obj.getProperty(key).toString());
}

orion::MidiNote midiNoteFromVar(const juce::var& value)
{
    orion::MidiNote note;
    if (auto* obj = value.getDynamicObject())
    {
        note.pitch         = getInt(*obj, "pitch", note.pitch);
        note.startBeat     = getDouble(*obj, "startBeat", note.startBeat);
        note.lengthInBeats = getDouble(*obj, "lengthInBeats", note.lengthInBeats);
        note.velocity      = getInt(*obj, "velocity", note.velocity);
    }
    return note;
}

orion::TimelineClip timelineClipFromVar(const juce::var& value)
{
    orion::TimelineClip clip;
    auto* obj = value.getDynamicObject();
    if (obj == nullptr)
        return clip;

    clip.name                    = getString(*obj, "name");
    clip.type                    = clipTypeFromString(getString(*obj, "type"));
    clip.startBeat               = getDouble(*obj, "startBeat", clip.startBeat);
    clip.lengthInBeats           = getDouble(*obj, "lengthInBeats", clip.lengthInBeats);
    clip.colour                  = getColour(*obj, "colour", clip.colour);
    clip.sourcePath              = getString(*obj, "sourcePath");
    clip.gainDb                  = getDouble(*obj, "gainDb", clip.gainDb);
    clip.muted                   = getBool(*obj, "muted", clip.muted);
    clip.solo                    = getBool(*obj, "solo", clip.solo);
    clip.sourceDurationSeconds   = getDouble(*obj, "sourceDurationSeconds", clip.sourceDurationSeconds);
    clip.sourceBpm               = getDouble(*obj, "sourceBpm", clip.sourceBpm);
    clip.detectedBars            = getInt(*obj, "detectedBars", clip.detectedBars);
    clip.warpEnabled             = getBool(*obj, "warpEnabled", clip.warpEnabled);
    clip.bpmGuessed              = getBool(*obj, "bpmGuessed", clip.bpmGuessed);
    clip.warpTargetLengthInBeats = getDouble(*obj, "warpTargetLengthInBeats", clip.warpTargetLengthInBeats);
    clip.sourceKeyRoot           = getInt(*obj, "sourceKeyRoot", clip.sourceKeyRoot);
    clip.sourceKeyIsMinor        = getBool(*obj, "sourceKeyIsMinor", clip.sourceKeyIsMinor);
    clip.keyShiftEnabled         = getBool(*obj, "keyShiftEnabled", clip.keyShiftEnabled);

    if (auto* notes = obj->getProperty("midiNotes").getArray())
        for (const auto& noteVar : *notes)
            clip.midiNotes.push_back(midiNoteFromVar(noteVar));

    return clip;
}

orion::TrackState trackStateFromVar(const juce::var& value)
{
    orion::TrackState track;
    auto* obj = value.getDynamicObject();
    if (obj == nullptr)
        return track;

    track.name                        = getString(*obj, "name");
    track.isMidiTrack                 = getBool(*obj, "isMidiTrack", track.isMidiTrack);
    track.colour                      = getColour(*obj, "colour", track.colour);
    track.muted                       = getBool(*obj, "muted", track.muted);
    track.solo                        = getBool(*obj, "solo", track.solo);
    track.recordArmed                 = getBool(*obj, "recordArmed", track.recordArmed);
    track.volumeDb                    = getDouble(*obj, "volumeDb", track.volumeDb);
    track.samplerSourcePath           = getString(*obj, "samplerSourcePath");
    track.samplerRootMidiNote         = getInt(*obj, "samplerRootMidiNote", track.samplerRootMidiNote);
    track.samplerMode                 = samplerModeFromString(getString(*obj, "samplerMode"));
    track.samplerKeyboardOctaveOffset = getInt(*obj, "samplerKeyboardOctaveOffset", track.samplerKeyboardOctaveOffset);
    track.samplerTransposeSemitones   = getInt(*obj, "samplerTransposeSemitones", track.samplerTransposeSemitones);
    track.samplerSliceCount           = getInt(*obj, "samplerSliceCount", track.samplerSliceCount);
    track.samplerWarpEnabled          = getBool(*obj, "samplerWarpEnabled", track.samplerWarpEnabled);
    track.samplerSourceBpm            = getDouble(*obj, "samplerSourceBpm", track.samplerSourceBpm);
    track.samplerSourceDurationSeconds = getDouble(*obj, "samplerSourceDurationSeconds", track.samplerSourceDurationSeconds);
    track.samplerDetectedBars         = getInt(*obj, "samplerDetectedBars", track.samplerDetectedBars);
    track.instrumentPluginId          = getString(*obj, "instrumentPluginId");
    track.instrumentPluginName        = getString(*obj, "instrumentPluginName");
    track.instrumentStateBase64       = getString(*obj, "instrumentStateBase64");

    if (auto* clips = obj->getProperty("clips").getArray())
        for (const auto& clipVar : *clips)
            track.clips.push_back(timelineClipFromVar(clipVar));

    return track;
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
    rootObject->setProperty("keyRoot", projectState.getKeyRoot());
    rootObject->setProperty("keyIsMinor", projectState.isKeyMinor());
    rootObject->setProperty("scaleLockEnabled", projectState.isScaleLockEnabled());
    rootObject->setProperty("recordWithMetronome", projectState.isRecordWithMetronome());
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

bool ProjectSerializer::loadFromFile(ProjectState& projectState,
                                     const juce::File& sourceFile,
                                     juce::String* errorMessage)
{
    if (! sourceFile.existsAsFile())
    {
        if (errorMessage != nullptr)
            *errorMessage = "Project file not found";
        return false;
    }

    auto parsed = juce::JSON::parse(sourceFile.loadFileAsString());
    auto* root = parsed.getDynamicObject();
    if (root == nullptr)
    {
        if (errorMessage != nullptr)
            *errorMessage = "Project file is not valid JSON";
        return false;
    }

    // Parse tracks into a temporary vector first, so a malformed file can't leave
    // the project half-overwritten.
    std::vector<TrackState> newTracks;
    if (auto* tracks = root->getProperty("tracks").getArray())
    {
        newTracks.reserve(static_cast<std::size_t>(tracks->size()));
        for (const auto& trackVar : *tracks)
            newTracks.push_back(trackStateFromVar(trackVar));
    }

    projectState.setTempoBpm(getDouble(*root, "tempoBpm", projectState.getTempoBpm()));
    projectState.setTimeSignature(getInt(*root, "timeSigNumerator", projectState.getNumerator()),
                                  getInt(*root, "timeSigDenominator", projectState.getDenominator()));
    projectState.setKey(getInt(*root, "keyRoot", projectState.getKeyRoot()),
                        getBool(*root, "keyIsMinor", projectState.isKeyMinor()));
    projectState.setScaleLockEnabled(getBool(*root, "scaleLockEnabled", projectState.isScaleLockEnabled()));
    projectState.setRecordWithMetronome(getBool(*root, "recordWithMetronome", projectState.isRecordWithMetronome()));
    projectState.setLoopLengthInBeats(getDouble(*root, "loopLengthInBeats", projectState.getLoopLengthInBeats()));

    if (getBool(*root, "loopRangeActive", false))
        projectState.setLoopRange(getDouble(*root, "loopStartBeat", 0.0),
                                  getDouble(*root, "loopEndBeat", 8.0));
    else
        projectState.clearLoopRange();

    projectState.getTracks() = std::move(newTracks);
    return true;
}
}  // namespace orion
