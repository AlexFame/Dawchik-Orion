#include "CollabSession.h"

#include "OpLog.h"
#include "../Core/ProjectSerializer.h"
#include "../Core/ProjectState.h"

namespace orion::collab
{
CollabSession::CollabSession(ProjectState& stateToBind, CollabTransport& transportToUse, EntityId actorSalt)
    : state(stateToBind), transport(transportToUse), ids(actorSalt)
{
    transport.onOpReceived = [this](const Op& op) { handleIncoming(op); };
    transport.onSnapshotReceived = [this](const juce::var& project) { handleSnapshot(project); };
    transport.onPresenceReceived = [this](const juce::var& presence) { handlePresence(presence); };
}

CollabSession::~CollabSession()
{
    transport.onOpReceived = nullptr;
    transport.onSnapshotReceived = nullptr;
    transport.onPresenceReceived = nullptr;
}

ActorId CollabSession::localActor() const
{
    return transport.localActor();
}

void CollabSession::assignInitialIds()
{
    oplog::ensureIds(state, ids);
}

void CollabSession::submitLocal(Op op)
{
    op.actor = transport.localActor();
    oplog::apply(state, op);   // optimistic: apply now for instant local feedback
    transport.sendOp(op);      // server sequences it and echoes to peers (and back to us, ignored)
}

void CollabSession::sendLocal(Op op)
{
    op.actor = transport.localActor();
    ++opsSent;
    transport.sendOp(op);      // the DAW already mutated the project; only broadcast, never re-apply
}

void CollabSession::publishSnapshot()
{
    transport.sendSnapshot(ProjectSerializer::toVar(state));
}

void CollabSession::requestBacklog()
{
    transport.requestBacklog();
}

void CollabSession::publishPresence(const juce::String& displayName, juce::uint32 colourArgb,
                                   double beat, double contentY, bool overTimeline)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("actor", transport.localActor());
    obj->setProperty("name", displayName);
    obj->setProperty("colour", static_cast<juce::int64>(colourArgb));
    obj->setProperty("beat", beat);
    obj->setProperty("y", contentY);
    obj->setProperty("over", overTimeline);
    ++presenceSent;
    transport.sendPresence(juce::var(obj));
}

void CollabSession::handlePresence(const juce::var& presence)
{
    PeerPresence p;
    p.actor = presence.getProperty("actor", juce::String()).toString();
    if (p.actor.isEmpty() || p.actor == transport.localActor())
        return;

    p.name = presence.getProperty("name", juce::String()).toString();
    p.colourArgb = static_cast<juce::uint32>(static_cast<juce::int64>(presence.getProperty("colour", 0)));
    p.beat = static_cast<double>(presence.getProperty("beat", 0.0));
    p.contentY = static_cast<double>(presence.getProperty("y", 0.0));
    p.overTimeline = static_cast<bool>(presence.getProperty("over", false));
    p.lastSeenMs = juce::Time::currentTimeMillis();
    ++presenceReceived;
    presenceByActor[p.actor] = p;
}

std::vector<PeerPresence> CollabSession::peers() const
{
    // Drop anyone we haven't heard from recently: a peer that quit or froze should stop haunting
    // the timeline rather than leaving a cursor parked forever.
    constexpr juce::int64 staleAfterMs = 5000;
    const auto now = juce::Time::currentTimeMillis();

    std::vector<PeerPresence> out;
    out.reserve(presenceByActor.size());
    for (const auto& [actor, p] : presenceByActor)
        if (now - p.lastSeenMs < staleAfterMs)
            out.push_back(p);
    return out;
}

void CollabSession::handleSnapshot(const juce::var& project)
{
    // Replacing the whole project reallocates every clips/notes vector the audio thread may be
    // reading, so this must happen under the audio-edit lock like any structural op.
    ++snapshotsReceived;
    {
        const juce::ScopedLock sl(state.getAudioEditLock());
        ProjectSerializer::fromVar(state, project);
    }

    if (onRemoteApplied)
        onRemoteApplied();
}

void CollabSession::handleIncoming(const Op& op)
{
    // Our own op coming back from the server — already applied optimistically, skip.
    if (op.actor == transport.localActor())
        return;

    ++opsReceived;

    // Transport isn't ProjectState — route it to the dedicated handler.
    if (op.type == OpType::setTransport)
    {
        if (onRemoteTransport)
            onRemoteTransport(static_cast<bool>(op.payload.getProperty("playing", false)),
                              static_cast<double>(op.payload.getProperty("beat", 0.0)));
        return;
    }

    if (oplog::apply(state, op))
    {
        ++opsApplied;
        if (onRemoteApplied)
            onRemoteApplied();
    }
}
} // namespace orion::collab
