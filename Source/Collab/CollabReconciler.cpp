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

    juce::String toJson(const juce::var& v) { return juce::JSON::toString(v, true); }
    juce::var fromJson(const juce::String& s) { return juce::JSON::parse(s); }
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
    s.busesJson = toJson(ProjectSerializer::busesToVar(live));
    s.masterInsertsJson = toJson(ProjectSerializer::masterInsertsToVar(live));

    for (const auto& t : live.getTracks())
    {
        TrackShadow ts;
        ts.id = t.id;
        ts.name = t.name;
        ts.isMidiTrack = t.isMidiTrack;
        {
            TrackState propsOnly = t;   // hash/store everything EXCEPT clips (the clip diff owns them)
            propsOnly.clips.clear();
            ts.propsJson = toJson(ProjectSerializer::trackToVar(propsOnly));
        }
        s.tracks.push_back(std::move(ts));

        for (const auto& c : t.clips)
        {
            ClipShadow cs;
            cs.id = c.id;
            cs.owner = t.id;
            cs.json = toJson(ProjectSerializer::clipToVar(c));
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
    // The local baseline just moved (session start / snapshot / reconnect): old undo entries refer
    // to a world that no longer exists, so drop them.
    undoStack.clear();
    redoStack.clear();
}

void CollabReconciler::foldRemoteChange()
{
    stampNewEntities();
    shadow = snapshotOfLive();
    hasShadow = true;
    // Deliberately keep undoStack/redoStack: a peer's edit doesn't undo ours.
}

void CollabReconciler::diffTempo(const Shadow& current, std::vector<Op>& forward, std::vector<Op>& inverse) const
{
    if (differs(current.tempoBpm, shadow.tempoBpm))
    {
        forward.push_back(ops::setTempo(current.tempoBpm));
        inverse.push_back(ops::setTempo(shadow.tempoBpm));
    }
}

void CollabReconciler::diffMixer(const Shadow& current, std::vector<Op>& forward, std::vector<Op>& inverse) const
{
    if (current.busesJson != shadow.busesJson)
    {
        forward.push_back(ops::replaceBuses(fromJson(current.busesJson)));
        inverse.push_back(ops::replaceBuses(fromJson(shadow.busesJson)));
    }
    if (current.masterInsertsJson != shadow.masterInsertsJson)
    {
        forward.push_back(ops::replaceMasterInserts(fromJson(current.masterInsertsJson)));
        inverse.push_back(ops::replaceMasterInserts(fromJson(shadow.masterInsertsJson)));
    }
}

void CollabReconciler::diffTracks(const Shadow& current, std::vector<Op>& forward, std::vector<Op>& inverse) const
{
    std::map<EntityId, const TrackShadow*> before, after;
    for (const auto& t : shadow.tracks)  before[t.id] = &t;
    for (const auto& t : current.tracks) after[t.id] = &t;

    // Removed: forward removes it; inverse rebuilds the track + its clips from the shadow.
    for (const auto& t : shadow.tracks)
        if (after.find(t.id) == after.end())
        {
            forward.push_back(ops::removeTrack(t.id));

            inverse.push_back(ops::addTrack(t.id, t.name, t.isMidiTrack));
            inverse.push_back(ops::updateTrackProps(t.id, fromJson(t.propsJson)));
            for (const auto& c : shadow.clips)
                if (c.owner == t.id)
                    inverse.push_back(ops::replaceClip(t.id, c.id, fromJson(c.json)));
        }

    // Added / changed props.
    for (const auto& t : current.tracks)
    {
        const auto it = before.find(t.id);
        if (it == before.end())
        {
            forward.push_back(ops::addTrack(t.id, t.name, t.isMidiTrack));
            forward.push_back(ops::updateTrackProps(t.id, fromJson(t.propsJson)));
            inverse.push_back(ops::removeTrack(t.id));
        }
        else if (t.propsJson != it->second->propsJson)
        {
            forward.push_back(ops::updateTrackProps(t.id, fromJson(t.propsJson)));
            inverse.push_back(ops::updateTrackProps(t.id, fromJson(it->second->propsJson)));
        }
    }

    // Order: if the surviving id sequence changed, restate every track's index (both directions).
    std::vector<EntityId> beforeOrder, afterOrder;
    for (const auto& t : shadow.tracks)  if (after.count(t.id))  beforeOrder.push_back(t.id);
    for (const auto& t : current.tracks) if (before.count(t.id)) afterOrder.push_back(t.id);

    if (beforeOrder != afterOrder)
    {
        for (std::size_t i = 0; i < current.tracks.size(); ++i)
            forward.push_back(ops::moveTrack(current.tracks[i].id, static_cast<int>(i)));
        for (std::size_t i = 0; i < shadow.tracks.size(); ++i)
            inverse.push_back(ops::moveTrack(shadow.tracks[i].id, static_cast<int>(i)));
    }
}

void CollabReconciler::diffClips(const Shadow& current, std::vector<Op>& forward, std::vector<Op>& inverse) const
{
    std::map<EntityId, const ClipShadow*> before, after;
    for (const auto& c : shadow.clips)  before[c.id] = &c;
    for (const auto& c : current.clips) after[c.id] = &c;

    // Removed: forward removes; inverse re-creates from the shadow.
    for (const auto& c : shadow.clips)
        if (after.find(c.id) == after.end())
        {
            forward.push_back(ops::removeClip(c.id));
            inverse.push_back(ops::replaceClip(c.owner, c.id, fromJson(c.json)));
        }

    // Added / moved-to-another-track / changed: whole-clip replace either way.
    for (const auto& c : current.clips)
    {
        const auto it = before.find(c.id);
        if (it == before.end())
        {
            forward.push_back(ops::replaceClip(c.owner, c.id, fromJson(c.json)));
            inverse.push_back(ops::removeClip(c.id));
        }
        else if (c.owner != it->second->owner || c.json != it->second->json)
        {
            forward.push_back(ops::replaceClip(c.owner, c.id, fromJson(c.json)));
            inverse.push_back(ops::replaceClip(it->second->owner, c.id, fromJson(it->second->json)));
        }
    }
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

    std::vector<Op> forward, inverse;
    diffTempo(current, forward, inverse);
    diffMixer(current, forward, inverse);
    diffTracks(current, forward, inverse);
    diffClips(current, forward, inverse);

    for (const auto& op : forward)
        controller.broadcast(op);

    shadow = current;

    if (! forward.empty())
    {
        undoStack.push_back({ forward, inverse });
        redoStack.clear();   // a new local edit invalidates the redo branch
    }

    return static_cast<int>(forward.size());
}

void CollabReconciler::applyAndBroadcast(const std::vector<Op>& batch)
{
    for (const auto& op : batch)
        oplog::apply(live, op);       // OpLog holds the audio-edit lock around each structural op
    for (const auto& op : batch)
        controller.broadcast(op);

    // Re-shadow so the next sync() doesn't re-diff what we just applied. Undo/redo history is
    // managed by undo()/redo(), so we deliberately do NOT clear it here.
    shadow = snapshotOfLive();
    hasShadow = true;
}

bool CollabReconciler::undo()
{
    if (undoStack.empty())
        return false;

    auto entry = undoStack.back();
    undoStack.pop_back();
    applyAndBroadcast(entry.inverse);
    redoStack.push_back(std::move(entry));
    return true;
}

bool CollabReconciler::redo()
{
    if (redoStack.empty())
        return false;

    auto entry = redoStack.back();
    redoStack.pop_back();
    applyAndBroadcast(entry.forward);
    undoStack.push_back(std::move(entry));
    return true;
}
} // namespace orion::collab
