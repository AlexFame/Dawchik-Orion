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

juce::var pitchSlideToVar(const orion::PitchSlide& slide)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("sourcePitch", slide.sourcePitch);
    object->setProperty("sourceNoteStartBeat", slide.sourceNoteStartBeat);

    juce::Array<juce::var> points;
    for (const auto& point : slide.points)
    {
        auto* pointObject = new juce::DynamicObject();
        pointObject->setProperty("beat", point.beat);
        pointObject->setProperty("pitch", point.pitch);
        pointObject->setProperty("curve", point.curve);
        pointObject->setProperty("lfoShape", point.lfoShape);
        pointObject->setProperty("lfoDepth", point.lfoDepth);
        pointObject->setProperty("lfoRate", point.lfoRate);
        points.add(juce::var(pointObject));
    }

    object->setProperty("points", juce::var(points));
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
    object->setProperty("transposeSemitones", clip.transposeSemitones);
    object->setProperty("sampleStartRatio", clip.sampleStartRatio);
    object->setProperty("sampleEndRatio", clip.sampleEndRatio);
    object->setProperty("fadeInBeats", clip.fadeInBeats);
    object->setProperty("fadeOutBeats", clip.fadeOutBeats);
    object->setProperty("fadeInCurve", clip.fadeInCurve);
    object->setProperty("fadeOutCurve", clip.fadeOutCurve);

    juce::Array<juce::var> notes;
    for (const auto& note : clip.midiNotes)
        notes.add(midiNoteToVar(note));

    object->setProperty("midiNotes", juce::var(notes));

    juce::Array<juce::var> slides;
    for (const auto& slide : clip.pitchSlides)
        slides.add(pitchSlideToVar(slide));

    object->setProperty("pitchSlides", juce::var(slides));

    juce::Array<juce::var> warpMarkers;
    for (const auto& m : clip.warpMarkers)
    {
        auto* wm = new juce::DynamicObject();
        wm->setProperty("sourceRatio", m.sourceRatio);
        wm->setProperty("beat", m.beat);
        warpMarkers.add(juce::var(wm));
    }
    object->setProperty("warpMarkers", juce::var(warpMarkers));
    return juce::var(object);
}

juce::var trackStateToVar(const orion::TrackState& track)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("name", track.name);
    object->setProperty("isMidiTrack", track.isMidiTrack);
    object->setProperty("isFolder", track.isFolder);
    object->setProperty("folderCollapsed", track.folderCollapsed);
    object->setProperty("groupId", track.groupId);
    object->setProperty("parentGroup", track.parentGroup);
    object->setProperty("folderBusIndex", track.folderBusIndex);
    object->setProperty("colour", track.colour.toString());
    object->setProperty("muted", track.muted);
    object->setProperty("solo", track.solo);
    object->setProperty("recordArmed", track.recordArmed);
    object->setProperty("volumeDb", track.volumeDb);
    object->setProperty("trackGainDb", track.trackGainDb);
    object->setProperty("pan", track.pan);
    object->setProperty("samplerSourcePath", track.samplerSourcePath);
    object->setProperty("isMpcKit", track.isMpcKit);
    {
        juce::Array<juce::var> kit;
        for (const auto& s : track.mpcKitSamples)
            kit.add(s);
        object->setProperty("mpcKitSamples", kit);
    }
    object->setProperty("isMpcTuneMode", track.isMpcTuneMode);
    object->setProperty("mpcTuneSample", track.mpcTuneSample);
    object->setProperty("mpcTuneRoot", track.mpcTuneRoot);
    object->setProperty("isMpcChopMode", track.isMpcChopMode);
    object->setProperty("mpcChopSample", track.mpcChopSample);
    object->setProperty("mpcChopRootPad", track.mpcChopRootPad);
    object->setProperty("mpcChopSliceCount", track.mpcChopSliceCount);
    object->setProperty("samplerRootMidiNote", track.samplerRootMidiNote);
    object->setProperty("samplerMode", samplerModeToString(track.samplerMode));
    object->setProperty("samplerKeyboardOctaveOffset", track.samplerKeyboardOctaveOffset);
    object->setProperty("samplerTransposeSemitones", track.samplerTransposeSemitones);
    object->setProperty("samplerSliceCount", track.samplerSliceCount);
    {
        juce::Array<juce::var> pts;
        for (const auto r : track.samplerSlicePoints)
            pts.add(r);
        object->setProperty("samplerSlicePoints", pts);
    }
    object->setProperty("samplerSliceSensitivity", track.samplerSliceSensitivity);
    object->setProperty("samplerWarpEnabled", track.samplerWarpEnabled);
    object->setProperty("samplerFullSampleTrigger", track.samplerFullSampleTrigger);
    object->setProperty("samplerStepGateBeats", track.samplerStepGateBeats);
    object->setProperty("samplerAmpAttackSeconds", track.samplerAmpAttackSeconds);
    object->setProperty("samplerAmpDecaySeconds", track.samplerAmpDecaySeconds);
    object->setProperty("samplerAmpSustain", track.samplerAmpSustain);
    object->setProperty("samplerAmpReleaseSeconds", track.samplerAmpReleaseSeconds);
    object->setProperty("samplerSourceBpm", track.samplerSourceBpm);
    object->setProperty("samplerSourceDurationSeconds", track.samplerSourceDurationSeconds);
    object->setProperty("samplerDetectedBars", track.samplerDetectedBars);
    object->setProperty("instrumentPluginId", track.instrumentPluginId);
    object->setProperty("instrumentPluginName", track.instrumentPluginName);
    object->setProperty("instrumentStateBase64", track.instrumentStateBase64);

    juce::Array<juce::var> inserts;
    for (const auto& fx : track.inserts)
    {
        auto* fxObj = new juce::DynamicObject();
        fxObj->setProperty("pluginId", fx.pluginId);
        fxObj->setProperty("pluginName", fx.pluginName);
        fxObj->setProperty("stateBase64", fx.stateBase64);
        fxObj->setProperty("bypassed", fx.bypassed);
        inserts.add(juce::var(fxObj));
    }
    object->setProperty("inserts", juce::var(inserts));

    juce::Array<juce::var> sends;
    for (const auto& s : track.sends)
    {
        auto* sObj = new juce::DynamicObject();
        sObj->setProperty("busIndex", s.busIndex);
        sObj->setProperty("level", s.level);
        sObj->setProperty("prefader", s.prefader);
        sends.add(juce::var(sObj));
    }
    object->setProperty("sends", juce::var(sends));
    object->setProperty("outputBus", track.outputBus);

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

orion::PitchSlide pitchSlideFromVar(const juce::var& value)
{
    orion::PitchSlide slide;
    auto* obj = value.getDynamicObject();
    if (obj == nullptr)
        return slide;

    slide.sourcePitch         = getInt(*obj, "sourcePitch", slide.sourcePitch);
    slide.sourceNoteStartBeat = getDouble(*obj, "sourceNoteStartBeat", slide.sourceNoteStartBeat);

    if (auto* points = obj->getProperty("points").getArray())
    {
        for (const auto& pointVar : *points)
        {
            if (auto* pointObj = pointVar.getDynamicObject())
            {
                orion::PitchSlidePoint point;
                point.beat     = getDouble(*pointObj, "beat", point.beat);
                point.pitch    = getDouble(*pointObj, "pitch", point.pitch);
                point.curve    = getDouble(*pointObj, "curve", point.curve);
                point.lfoShape = getInt(*pointObj, "lfoShape", point.lfoShape);
                point.lfoDepth = getDouble(*pointObj, "lfoDepth", point.lfoDepth);
                point.lfoRate  = getDouble(*pointObj, "lfoRate", point.lfoRate);
                slide.points.push_back(point);
            }
        }
    }

    return slide;
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
    clip.transposeSemitones      = getInt(*obj, "transposeSemitones", clip.transposeSemitones);
    clip.sampleStartRatio        = juce::jlimit(0.0, 0.999, getDouble(*obj, "sampleStartRatio", clip.sampleStartRatio));
    clip.sampleEndRatio          = juce::jlimit(0.001, 1.0, getDouble(*obj, "sampleEndRatio", clip.sampleEndRatio));
    if (clip.sampleEndRatio <= clip.sampleStartRatio)
        clip.sampleEndRatio = juce::jmin(1.0, clip.sampleStartRatio + 0.001);
    clip.fadeInBeats             = juce::jmax(0.0, getDouble(*obj, "fadeInBeats", clip.fadeInBeats));
    clip.fadeOutBeats            = juce::jmax(0.0, getDouble(*obj, "fadeOutBeats", clip.fadeOutBeats));
    clip.fadeInCurve             = juce::jlimit(-1.0, 1.0, getDouble(*obj, "fadeInCurve", clip.fadeInCurve));
    clip.fadeOutCurve            = juce::jlimit(-1.0, 1.0, getDouble(*obj, "fadeOutCurve", clip.fadeOutCurve));

    if (auto* notes = obj->getProperty("midiNotes").getArray())
        for (const auto& noteVar : *notes)
            clip.midiNotes.push_back(midiNoteFromVar(noteVar));

    if (auto* slides = obj->getProperty("pitchSlides").getArray())
        for (const auto& slideVar : *slides)
            clip.pitchSlides.push_back(pitchSlideFromVar(slideVar));

    if (auto* markers = obj->getProperty("warpMarkers").getArray())
        for (const auto& mVar : *markers)
            if (auto* mObj = mVar.getDynamicObject())
                clip.warpMarkers.push_back({ getDouble(*mObj, "sourceRatio", 0.0),
                                             getDouble(*mObj, "beat", 0.0) });

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
    track.isFolder                    = getBool(*obj, "isFolder", track.isFolder);
    track.folderCollapsed             = getBool(*obj, "folderCollapsed", track.folderCollapsed);
    track.groupId                     = getInt(*obj, "groupId", track.groupId);
    track.parentGroup                 = getInt(*obj, "parentGroup", track.parentGroup);
    track.folderBusIndex              = getInt(*obj, "folderBusIndex", track.folderBusIndex);
    track.colour                      = getColour(*obj, "colour", track.colour);
    track.muted                       = getBool(*obj, "muted", track.muted);
    track.solo                        = getBool(*obj, "solo", track.solo);
    track.recordArmed                 = getBool(*obj, "recordArmed", track.recordArmed);
    track.volumeDb                    = getDouble(*obj, "volumeDb", track.volumeDb);
    track.trackGainDb                 = getDouble(*obj, "trackGainDb", track.trackGainDb);
    track.pan                         = juce::jlimit(-1.0, 1.0, getDouble(*obj, "pan", track.pan));
    track.samplerSourcePath           = getString(*obj, "samplerSourcePath");
    track.isMpcKit                    = getBool(*obj, "isMpcKit", track.isMpcKit);
    if (const auto* kit = obj->getProperty("mpcKitSamples").getArray())
        for (int i = 0; i < juce::jmin(kit->size(), 16); ++i)
            track.mpcKitSamples[static_cast<std::size_t>(i)] = (*kit)[i].toString();
    track.isMpcTuneMode               = getBool(*obj, "isMpcTuneMode", track.isMpcTuneMode);
    track.mpcTuneSample               = getString(*obj, "mpcTuneSample");
    track.mpcTuneRoot                 = getInt(*obj, "mpcTuneRoot", track.mpcTuneRoot);
    track.isMpcChopMode               = getBool(*obj, "isMpcChopMode", track.isMpcChopMode);
    track.mpcChopSample               = getString(*obj, "mpcChopSample");
    track.mpcChopRootPad              = juce::jlimit(0, 15, getInt(*obj, "mpcChopRootPad", track.mpcChopRootPad));
    track.mpcChopSliceCount           = juce::jlimit(1, 64, getInt(*obj, "mpcChopSliceCount", track.mpcChopSliceCount));
    track.samplerRootMidiNote         = getInt(*obj, "samplerRootMidiNote", track.samplerRootMidiNote);
    track.samplerMode                 = samplerModeFromString(getString(*obj, "samplerMode"));
    track.samplerKeyboardOctaveOffset = getInt(*obj, "samplerKeyboardOctaveOffset", track.samplerKeyboardOctaveOffset);
    track.samplerTransposeSemitones   = getInt(*obj, "samplerTransposeSemitones", track.samplerTransposeSemitones);
    track.samplerSliceCount           = getInt(*obj, "samplerSliceCount", track.samplerSliceCount);
    track.samplerSlicePoints.clear();
    if (const auto* pts = obj->getProperty("samplerSlicePoints").getArray())
        for (const auto& v : *pts)
            track.samplerSlicePoints.push_back(static_cast<double>(v));
    track.samplerSliceSensitivity     = juce::jlimit(0.0, 1.0, getDouble(*obj, "samplerSliceSensitivity", track.samplerSliceSensitivity));
    track.samplerWarpEnabled          = getBool(*obj, "samplerWarpEnabled", track.samplerWarpEnabled);
    track.samplerFullSampleTrigger    = getBool(*obj, "samplerFullSampleTrigger", track.samplerFullSampleTrigger);
    track.samplerStepGateBeats        = juce::jmax(0.0, getDouble(*obj, "samplerStepGateBeats", track.samplerStepGateBeats));
    track.samplerAmpAttackSeconds     = juce::jmax(0.0, getDouble(*obj, "samplerAmpAttackSeconds", track.samplerAmpAttackSeconds));
    track.samplerAmpDecaySeconds      = juce::jmax(0.0, getDouble(*obj, "samplerAmpDecaySeconds", track.samplerAmpDecaySeconds));
    track.samplerAmpSustain           = juce::jlimit(0.0, 1.0, getDouble(*obj, "samplerAmpSustain", track.samplerAmpSustain));
    track.samplerAmpReleaseSeconds    = juce::jmax(0.0, getDouble(*obj, "samplerAmpReleaseSeconds", track.samplerAmpReleaseSeconds));
    track.samplerSourceBpm            = getDouble(*obj, "samplerSourceBpm", track.samplerSourceBpm);
    track.samplerSourceDurationSeconds = getDouble(*obj, "samplerSourceDurationSeconds", track.samplerSourceDurationSeconds);
    track.samplerDetectedBars         = getInt(*obj, "samplerDetectedBars", track.samplerDetectedBars);
    track.instrumentPluginId          = getString(*obj, "instrumentPluginId");
    track.instrumentPluginName        = getString(*obj, "instrumentPluginName");
    track.instrumentStateBase64       = getString(*obj, "instrumentStateBase64");

    if (auto* inserts = obj->getProperty("inserts").getArray())
    {
        for (const auto& fxVar : *inserts)
        {
            if (auto* fxObj = fxVar.getDynamicObject())
            {
                orion::TrackState::InsertFx fx;
                fx.pluginId    = getString(*fxObj, "pluginId");
                fx.pluginName  = getString(*fxObj, "pluginName");
                fx.stateBase64 = getString(*fxObj, "stateBase64");
                fx.bypassed    = getBool(*fxObj, "bypassed", false);
                track.inserts.push_back(std::move(fx));
            }
        }
    }

    if (auto* sends = obj->getProperty("sends").getArray())
    {
        for (const auto& sVar : *sends)
        {
            if (auto* sObj = sVar.getDynamicObject())
            {
                orion::TrackState::SendFx s;
                s.busIndex = getInt(*sObj, "busIndex", 0);
                s.level    = juce::jlimit(0.0, 1.0, getDouble(*sObj, "level", 0.25));
                s.prefader = getBool(*sObj, "prefader", false);
                track.sends.push_back(s);
            }
        }
    }

    track.outputBus = getInt(*obj, "outputBus", -1);

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
    rootObject->setProperty("keyEnabled", projectState.isKeyEnabled());
    rootObject->setProperty("scaleLockEnabled", projectState.isScaleLockEnabled());
    rootObject->setProperty("chordModeEnabled", projectState.isChordModeEnabled());
    rootObject->setProperty("chordSizeNotes", projectState.getChordSizeNotes());
    rootObject->setProperty("recordWithMetronome", projectState.isRecordWithMetronome());
    rootObject->setProperty("recordWithCountIn", projectState.isRecordWithCountIn());
    rootObject->setProperty("loopLengthInBeats", projectState.getLoopLengthInBeats());
    rootObject->setProperty("loopRangeActive", projectState.hasLoopRange());
    rootObject->setProperty("loopStartBeat", projectState.getLoopStartBeat());
    rootObject->setProperty("loopEndBeat", projectState.getLoopEndBeat());

    juce::Array<juce::var> trackArray;
    for (const auto& track : projectState.getTracks())
        trackArray.add(trackStateToVar(track));

    rootObject->setProperty("tracks", juce::var(trackArray));

    juce::Array<juce::var> busArray;
    for (const auto& bus : projectState.getBuses())
    {
        auto* b = new juce::DynamicObject();
        b->setProperty("name", bus.name);
        b->setProperty("colour", static_cast<int>(bus.colour.getARGB()));
        b->setProperty("volumeDb", bus.volumeDb);
        b->setProperty("pan", bus.pan);
        b->setProperty("muted", bus.muted);
        juce::Array<juce::var> busInserts;
        for (const auto& fx : bus.inserts)
        {
            auto* fxObj = new juce::DynamicObject();
            fxObj->setProperty("pluginId", fx.pluginId);
            fxObj->setProperty("pluginName", fx.pluginName);
            fxObj->setProperty("stateBase64", fx.stateBase64);
            fxObj->setProperty("bypassed", fx.bypassed);
            busInserts.add(juce::var(fxObj));
        }
        b->setProperty("inserts", juce::var(busInserts));
        busArray.add(juce::var(b));
    }
    rootObject->setProperty("buses", juce::var(busArray));

    juce::Array<juce::var> masterInserts;
    for (const auto& fx : projectState.getMasterInserts())
    {
        auto* fxObj = new juce::DynamicObject();
        fxObj->setProperty("pluginId", fx.pluginId);
        fxObj->setProperty("pluginName", fx.pluginName);
        fxObj->setProperty("stateBase64", fx.stateBase64);
        fxObj->setProperty("bypassed", fx.bypassed);
        masterInserts.add(juce::var(fxObj));
    }
    rootObject->setProperty("masterInserts", juce::var(masterInserts));

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
    projectState.setKeyEnabled(getBool(*root, "keyEnabled", projectState.isKeyEnabled()));
    projectState.setScaleLockEnabled(getBool(*root, "scaleLockEnabled", projectState.isScaleLockEnabled()));
    projectState.setChordModeEnabled(getBool(*root, "chordModeEnabled", projectState.isChordModeEnabled()));
    projectState.setChordSizeNotes(getInt(*root, "chordSizeNotes", projectState.getChordSizeNotes()));
    projectState.setRecordWithMetronome(getBool(*root, "recordWithMetronome", projectState.isRecordWithMetronome()));
    projectState.setRecordWithCountIn(getBool(*root, "recordWithCountIn", projectState.isRecordWithCountIn()));
    projectState.setLoopLengthInBeats(getDouble(*root, "loopLengthInBeats", projectState.getLoopLengthInBeats()));

    if (getBool(*root, "loopRangeActive", false))
        projectState.setLoopRange(getDouble(*root, "loopStartBeat", 0.0),
                                  getDouble(*root, "loopEndBeat", 8.0));
    else
        projectState.clearLoopRange();

    projectState.getTracks() = std::move(newTracks);
    // Restore the group-id allocator so new folders don't collide with loaded ones.
    for (const auto& t : projectState.getTracks())
        if (t.isFolder && t.groupId >= 0)
            projectState.noteUsedGroupId(t.groupId);

    std::vector<orion::BusState> newBuses;
    if (auto* buses = root->getProperty("buses").getArray())
    {
        for (const auto& busVar : *buses)
        {
            if (auto* b = busVar.getDynamicObject())
            {
                orion::BusState bus;
                bus.name = getString(*b, "name");
                if (bus.name.isEmpty()) bus.name = "Bus";
                bus.colour = juce::Colour(static_cast<juce::uint32>(getInt(*b, "colour", static_cast<int>(bus.colour.getARGB()))));
                bus.volumeDb = getDouble(*b, "volumeDb", 0.0);
                bus.pan = juce::jlimit(-1.0, 1.0, getDouble(*b, "pan", 0.0));
                bus.muted = getBool(*b, "muted", false);
                if (auto* ins = b->getProperty("inserts").getArray())
                    for (const auto& fxVar : *ins)
                        if (auto* fxObj = fxVar.getDynamicObject())
                        {
                            orion::TrackState::InsertFx fx;
                            fx.pluginId = getString(*fxObj, "pluginId");
                            fx.pluginName = getString(*fxObj, "pluginName");
                            fx.stateBase64 = getString(*fxObj, "stateBase64");
                            fx.bypassed = getBool(*fxObj, "bypassed", false);
                            bus.inserts.push_back(std::move(fx));
                        }
                newBuses.push_back(std::move(bus));
            }
        }
    }
    projectState.getBuses() = std::move(newBuses);

    std::vector<orion::TrackState::InsertFx> newMasterInserts;
    if (auto* ins = root->getProperty("masterInserts").getArray())
        for (const auto& fxVar : *ins)
            if (auto* fxObj = fxVar.getDynamicObject())
            {
                orion::TrackState::InsertFx fx;
                fx.pluginId = getString(*fxObj, "pluginId");
                fx.pluginName = getString(*fxObj, "pluginName");
                fx.stateBase64 = getString(*fxObj, "stateBase64");
                fx.bypassed = getBool(*fxObj, "bypassed", false);
                newMasterInserts.push_back(std::move(fx));
            }
    projectState.getMasterInserts() = std::move(newMasterInserts);
    return true;
}
}  // namespace orion
