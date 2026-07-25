#pragma once

#include "CollabTypes.h"

#include <optional>
#include <vector>

namespace orion
{
class ProjectState;
struct TrackState;
struct TimelineClip;
struct MidiNote;

namespace collab
{
// OpLog — the pure translation between Operations and ProjectState. Stateless free functions:
// every client runs the SAME apply() over the SAME server-ordered op stream, so all clients
// converge on identical state. Nothing here knows about the network, the UI, or undo history.
//
// Entities are addressed by their stable juce::uint64 id (see ProjectState). apply() holds the
// project's audio-edit lock around any op that reallocates a clips/notes vector, so the audio
// thread never reads a vector mid-resize.
namespace oplog
{
    // Give every id-less entity (id == 0) a fresh id. Called once when a collab session opens on a
    // project that was created offline, so subsequent ops have something stable to target.
    void ensureIds(ProjectState&, EntityIdGenerator&);

    // Apply a (already server-sequenced) op. Returns false if the op could not be applied
    // (e.g. its target entity no longer exists — expected under concurrent deletes, not an error).
    bool apply(ProjectState&, const Op&);

    // ---- Lookups by stable id (nullptr if gone). ----
    TrackState*    findTrack(ProjectState&, EntityId trackId);
    TimelineClip*  findClip(ProjectState&, EntityId clipId, TrackState** ownerOut = nullptr);
    MidiNote*      findNote(ProjectState&, EntityId clipId, EntityId noteId);
} // namespace oplog

// Builders — construct a well-formed Op with its payload. The caller sets ids (via the session's
// generator) and the actor; the server fills in seq. Keeps op construction in one place so the
// wire shape and apply() can never drift apart.
namespace ops
{
    Op setTempo(double bpm);

    Op addTrack(EntityId newTrackId, const juce::String& name, bool isMidiTrack);
    Op removeTrack(EntityId trackId);

    // Replace a track's properties (not its clips) from ProjectSerializer::trackToVar data.
    Op updateTrackProps(EntityId trackId, const juce::var& trackData);
    Op moveTrack(EntityId trackId, int newIndex);
    Op setTrackField(EntityId trackId, const juce::String& field, const juce::var& value);

    // An audio clip is useless to a peer without the source it plays: without sourcePath the clip
    // arrives as an empty block — no waveform, no sound. The audio fields ride along with the
    // creation op so the clip is complete the moment it lands.
    struct ClipSource
    {
        int    type { 0 };                    // ClipType: 0 = audio, 1 = midi
        juce::String sourcePath;
        double sourceDurationSeconds { 0.0 };
        double sourceBpm { 0.0 };
        double sampleStartRatio { 0.0 };
        double sampleEndRatio { 1.0 };
        int    transposeSemitones { 0 };
        bool   warpEnabled { false };
        double warpTargetLengthInBeats { 0.0 };
    };

    Op addClip(EntityId trackId, EntityId newClipId, const juce::String& name,
               double startBeat, double lengthInBeats, const ClipSource& source = {});
    Op removeClip(EntityId clipId);

    // Create-or-replace the clip wholesale on trackId, from ProjectSerializer::clipToVar data.
    Op replaceClip(EntityId trackId, EntityId clipId, const juce::var& clipData);
    Op moveClip(EntityId clipId, double startBeat, EntityId toTrack = noEntity);
    Op resizeClip(EntityId clipId, double lengthInBeats);
    Op setClipField(EntityId clipId, const juce::String& field, const juce::var& value);

    Op addNote(EntityId clipId, EntityId newNoteId, int pitch, double startBeat,
               double lengthInBeats, int velocity);
    Op removeNote(EntityId clipId, EntityId noteId);
    Op editNote(EntityId clipId, EntityId noteId, int pitch, double startBeat,
                double lengthInBeats, int velocity);

    // Sync a clip's ENTIRE note vector in one op. Coarser than per-note ops (two people editing the
    // same clip at once resolve last-writer-wins rather than merging), but it lets a complex editor
    // like the piano roll broadcast from a single place after any edit instead of instrumenting
    // every add/remove/drag/quantise path.
    Op replaceClipNotes(EntityId clipId, const std::vector<MidiNote>& notes);
} // namespace ops
} // namespace collab
} // namespace orion
