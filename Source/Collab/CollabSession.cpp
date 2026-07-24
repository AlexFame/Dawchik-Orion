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
}

CollabSession::~CollabSession()
{
    transport.onOpReceived = nullptr;
    transport.onSnapshotReceived = nullptr;
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
    if (oplog::apply(state, op))
    {
        ++opsApplied;
        if (onRemoteApplied)
            onRemoteApplied();
    }
}
} // namespace orion::collab
