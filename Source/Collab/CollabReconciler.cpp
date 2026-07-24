#include "CollabReconciler.h"

#include "CollabController.h"
#include "OpLog.h"
#include "../Core/ProjectState.h"

#include <map>

namespace orion::collab
{
namespace
{
    constexpr double epsilon = 1.0e-9;
    bool differs(double a, double b) noexcept { return std::abs(a - b) > epsilon; }

    juce::var colourVar(juce::uint32 argb) { return juce::var(static_cast<juce::int64>(argb)); }
}

CollabReconciler::CollabReconciler(ProjectState& liveProject, CollabController& controllerToUse)
    : live(liveProject), controller(controllerToUse)
{
}

void CollabReconciler::stampNewEntities()
{
    // The DAW creates entities with id 0 because it knows nothing about collab. Give them stable
    // ids here, before diffing, so every op can address them. Writing the id field doesn't
    // reallocate anything and the audio thread never reads it, so no lock is needed.
    for (auto& t : live.getTracks())
    {
        if (t.id == noEntity)
            t.id = controller.newId();

        for (auto& c : t.clips)
        {
            if (c.id == noEntity)
                c.id = controller.newId();

            for (auto& n : c.midiNotes)
                if (n.id == noEntity)
                    n.id = controller.newId();
        }
    }
}

CollabReconciler::Shadow CollabReconciler::snapshotOfLive() const
{
    Shadow s;
    s.tempoBpm = live.getTempoBpm();

    for (const auto& t : live.getTracks())
    {
        TrackShadow ts;
        ts.id = t.id;
        ts.name = t.name;
        ts.isMidiTrack = t.isMidiTrack;
        ts.muted = t.muted;
        ts.solo = t.solo;
        ts.recordArmed = t.recordArmed;
        ts.volumeDb = t.volumeDb;
        ts.pan = t.pan;
        ts.trackGainDb = t.trackGainDb;
        ts.colour = t.colour.getARGB();
        s.tracks.push_back(std::move(ts));

        for (const auto& c : t.clips)
        {
            ClipShadow cs;
            cs.id = c.id;
            cs.owner = t.id;
            cs.name = c.name;
            cs.startBeat = c.startBeat;
            cs.lengthInBeats = c.lengthInBeats;
            cs.gainDb = c.gainDb;
            cs.muted = c.muted;
            cs.solo = c.solo;
            cs.warpEnabled = c.warpEnabled;
            cs.colour = c.colour.getARGB();

            cs.notes.reserve(c.midiNotes.size());
            for (const auto& n : c.midiNotes)
                cs.notes.push_back({ n.id, n.pitch, n.startBeat, n.lengthInBeats, n.velocity });

            s.clips.push_back(std::move(cs));
        }
    }

    return s;
}

void CollabReconciler::captureBaseline()
{
    stampNewEntities();
    shadow = snapshotOfLive();
    hasShadow = true;
}

int CollabReconciler::diffTracks(const Shadow& current)
{
    int sent = 0;

    std::map<EntityId, const TrackShadow*> before, after;
    for (const auto& t : shadow.tracks)  before[t.id] = &t;
    for (const auto& t : current.tracks) after[t.id] = &t;

    // Removed.
    for (const auto& [id, t] : before)
        if (after.find(id) == after.end())
        {
            controller.broadcast(ops::removeTrack(id));
            ++sent;
        }

    // Added.
    for (const auto& t : current.tracks)
        if (before.find(t.id) == before.end())
        {
            controller.broadcast(ops::addTrack(t.id, t.name, t.isMidiTrack));
            ++sent;

            // Carry the non-default properties of a freshly created track.
            const TrackShadow fresh {};
            if (t.muted != fresh.muted)             { controller.broadcast(ops::setTrackField(t.id, "muted", t.muted)); ++sent; }
            if (t.solo != fresh.solo)               { controller.broadcast(ops::setTrackField(t.id, "solo", t.solo)); ++sent; }
            if (differs(t.volumeDb, fresh.volumeDb)){ controller.broadcast(ops::setTrackField(t.id, "volumeDb", t.volumeDb)); ++sent; }
            if (differs(t.pan, fresh.pan))          { controller.broadcast(ops::setTrackField(t.id, "pan", t.pan)); ++sent; }
            if (differs(t.trackGainDb, fresh.trackGainDb)) { controller.broadcast(ops::setTrackField(t.id, "trackGainDb", t.trackGainDb)); ++sent; }
            controller.broadcast(ops::setTrackField(t.id, "colour", colourVar(t.colour)));
            ++sent;
        }

    // Field changes on tracks present in both.
    for (const auto& t : current.tracks)
    {
        const auto it = before.find(t.id);
        if (it == before.end())
            continue;

        const auto& was = *it->second;
        if (t.name != was.name)                   { controller.broadcast(ops::setTrackField(t.id, "name", t.name)); ++sent; }
        if (t.muted != was.muted)                 { controller.broadcast(ops::setTrackField(t.id, "muted", t.muted)); ++sent; }
        if (t.solo != was.solo)                   { controller.broadcast(ops::setTrackField(t.id, "solo", t.solo)); ++sent; }
        if (t.recordArmed != was.recordArmed)     { controller.broadcast(ops::setTrackField(t.id, "recordArmed", t.recordArmed)); ++sent; }
        if (differs(t.volumeDb, was.volumeDb))    { controller.broadcast(ops::setTrackField(t.id, "volumeDb", t.volumeDb)); ++sent; }
        if (differs(t.pan, was.pan))              { controller.broadcast(ops::setTrackField(t.id, "pan", t.pan)); ++sent; }
        if (differs(t.trackGainDb, was.trackGainDb)) { controller.broadcast(ops::setTrackField(t.id, "trackGainDb", t.trackGainDb)); ++sent; }
        if (t.colour != was.colour)               { controller.broadcast(ops::setTrackField(t.id, "colour", colourVar(t.colour))); ++sent; }
    }

    // Order. Compare the id sequences that survive on both sides; if they disagree, walk the live
    // order and move each track into place.
    std::vector<EntityId> beforeOrder, afterOrder;
    for (const auto& t : shadow.tracks)  if (after.count(t.id))  beforeOrder.push_back(t.id);
    for (const auto& t : current.tracks) if (before.count(t.id)) afterOrder.push_back(t.id);

    if (beforeOrder != afterOrder)
        for (std::size_t i = 0; i < current.tracks.size(); ++i)
        {
            controller.broadcast(ops::moveTrack(current.tracks[i].id, static_cast<int>(i)));
            ++sent;
        }

    return sent;
}

int CollabReconciler::diffClips(const Shadow& current)
{
    int sent = 0;

    std::map<EntityId, const ClipShadow*> before, after;
    for (const auto& c : shadow.clips)  before[c.id] = &c;
    for (const auto& c : current.clips) after[c.id] = &c;

    // Removed.
    for (const auto& [id, c] : before)
        if (after.find(id) == after.end())
        {
            controller.broadcast(ops::removeClip(id));
            ++sent;
        }

    for (const auto& c : current.clips)
    {
        const auto it = before.find(c.id);

        if (it == before.end())
        {
            // Added: create it on its owning track, then carry its properties and notes.
            controller.broadcast(ops::addClip(c.owner, c.id, c.name, c.startBeat, c.lengthInBeats));
            ++sent;

            const ClipShadow fresh {};
            if (differs(c.gainDb, fresh.gainDb)) { controller.broadcast(ops::setClipField(c.id, "gainDb", c.gainDb)); ++sent; }
            if (c.muted != fresh.muted)         { controller.broadcast(ops::setClipField(c.id, "muted", c.muted)); ++sent; }
            if (c.solo != fresh.solo)           { controller.broadcast(ops::setClipField(c.id, "solo", c.solo)); ++sent; }
            if (c.warpEnabled != fresh.warpEnabled) { controller.broadcast(ops::setClipField(c.id, "warpEnabled", c.warpEnabled)); ++sent; }
            controller.broadcast(ops::setClipField(c.id, "colour", colourVar(c.colour)));
            ++sent;

            if (! c.notes.empty())
                if (const auto* liveClip = oplog::findClip(live, c.id))
                {
                    controller.broadcast(ops::replaceClipNotes(c.id, liveClip->midiNotes));
                    ++sent;
                }
            continue;
        }

        const auto& was = *it->second;

        // Moved (to a new position and/or a different track) / resized.
        if (c.owner != was.owner || differs(c.startBeat, was.startBeat))
        {
            controller.broadcast(ops::moveClip(c.id, c.startBeat, c.owner != was.owner ? c.owner : noEntity));
            ++sent;
        }
        if (differs(c.lengthInBeats, was.lengthInBeats))
        {
            controller.broadcast(ops::resizeClip(c.id, c.lengthInBeats));
            ++sent;
        }

        if (c.name != was.name)                 { controller.broadcast(ops::setClipField(c.id, "name", c.name)); ++sent; }
        if (differs(c.gainDb, was.gainDb))      { controller.broadcast(ops::setClipField(c.id, "gainDb", c.gainDb)); ++sent; }
        if (c.muted != was.muted)               { controller.broadcast(ops::setClipField(c.id, "muted", c.muted)); ++sent; }
        if (c.solo != was.solo)                 { controller.broadcast(ops::setClipField(c.id, "solo", c.solo)); ++sent; }
        if (c.warpEnabled != was.warpEnabled)   { controller.broadcast(ops::setClipField(c.id, "warpEnabled", c.warpEnabled)); ++sent; }
        if (c.colour != was.colour)             { controller.broadcast(ops::setClipField(c.id, "colour", colourVar(c.colour))); ++sent; }

        // Notes: any difference at all resyncs the whole vector in one op.
        bool notesChanged = c.notes.size() != was.notes.size();
        for (std::size_t i = 0; ! notesChanged && i < c.notes.size(); ++i)
        {
            const auto& a = c.notes[i];
            const auto& b = was.notes[i];
            notesChanged = a.id != b.id || a.pitch != b.pitch || a.velocity != b.velocity
                        || differs(a.startBeat, b.startBeat) || differs(a.lengthInBeats, b.lengthInBeats);
        }

        if (notesChanged)
            if (const auto* liveClip = oplog::findClip(live, c.id))
            {
                controller.broadcast(ops::replaceClipNotes(c.id, liveClip->midiNotes));
                ++sent;
            }
    }

    return sent;
}

int CollabReconciler::sync()
{
    if (! controller.isActive())
        return 0;

    if (! hasShadow)
    {
        captureBaseline();
        return 0;
    }

    stampNewEntities();
    const auto current = snapshotOfLive();

    int sent = 0;
    if (differs(current.tempoBpm, shadow.tempoBpm))
    {
        controller.broadcast(ops::setTempo(current.tempoBpm));
        ++sent;
    }

    sent += diffTracks(current);
    sent += diffClips(current);

    shadow = current;
    return sent;
}
} // namespace orion::collab
