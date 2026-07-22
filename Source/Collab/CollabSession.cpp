#include "CollabSession.h"

#include "OpLog.h"
#include "../Core/ProjectState.h"

namespace orion::collab
{
CollabSession::CollabSession(ProjectState& stateToBind, CollabTransport& transportToUse, EntityId actorSalt)
    : state(stateToBind), transport(transportToUse), ids(actorSalt)
{
    transport.onOpReceived = [this](const Op& op) { handleIncoming(op); };
}

CollabSession::~CollabSession()
{
    transport.onOpReceived = nullptr;
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

void CollabSession::handleIncoming(const Op& op)
{
    // Our own op coming back from the server — already applied optimistically, skip.
    if (op.actor == transport.localActor())
        return;

    if (oplog::apply(state, op) && onRemoteApplied)
        onRemoteApplied();
}
} // namespace orion::collab
