#include "CollabReconciler.h"

#include "CollabController.h"
#include "OpLog.h"
#include "../Core/ProjectSerializer.h"
#include "../Core/ProjectState.h"

#include <map>

namespace orion::collab
{
namespace
{
    constexpr double epsilon = 1.0e-9;
    bool differs(double a, double b) noexcept { return std::abs(a - b) > epsilon; }
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
        // Hash everything EXCEPT the clips, which the clip diff syncs — otherwise a note edit would
        // also fire a whole-track op. Cheapest way to exclude them is to hash a clip-less copy.
        {
            TrackState propsOnly = t;
            propsOnly.clips.clear();
            ts.propsHash = juce::JSON::toString(ProjectSerializer::trackToVar(propsOnly), true).hashCode64();
        }
        s.tracks.push_back(std::move(ts));

        for (const auto& c : t.clips)
        {
            ClipShadow cs;
            cs.id = c.id;
            cs.owner = t.id;
            cs.dataHash = juce::JSON::toString(ProjectSerializer::clipToVar(c), true).hashCode64();
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

    // Added, and any property change on an existing track — both ship the whole track's properties
    // (VST/sampler/MPC/inserts included) via one op, so no field can be forgotten.
    for (const auto& t : current.tracks)
    {
        const auto it = before.find(t.id);
        const bool isNew = it == before.end();

        if (isNew)
        {
            controller.broadcast(ops::addTrack(t.id, t.name, t.isMidiTrack));
            ++sent;
        }

        if (isNew || it->second->propsHash != t.propsHash)
            if (const auto* liveTrack = oplog::findTrack(live, t.id))
            {
                controller.broadcast(ops::updateTrackProps(t.id, ProjectSerializer::trackToVar(*liveTrack)));
                ++sent;
            }
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
        const bool isNew = it == before.end();

        // New, moved to another track, or changed in any way at all — ship the whole clip.
        if (isNew || it->second->owner != c.owner || it->second->dataHash != c.dataHash)
            if (const auto* liveClip = oplog::findClip(live, c.id))
            {
                controller.broadcast(ops::replaceClip(c.owner, c.id, ProjectSerializer::clipToVar(*liveClip)));
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
