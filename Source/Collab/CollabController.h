#pragma once

#include "CollabSession.h"
#include "CollabTransport.h"
#include "CollabTypes.h"

#include <functional>
#include <memory>

namespace orion
{
class ProjectState;

namespace collab
{
// CollabController — the ONE object the DAW (MainComponent) holds. It is the whole seam between the
// app and the collab module: everything below it (session, op-log, transport, wire format) stays
// inside Source/Collab.
//
// Integration model = "the DAW edits, the controller broadcasts". The app keeps its existing edit
// code untouched; at each edit site it simply asks the controller for an id (when a session is
// live) to stamp the new entity, then calls broadcast() with a matching op. The controller sends it
// (never re-applies — the DAW already mutated the project). Incoming PEER ops are applied to the
// ProjectState via OpLog and onProjectChanged fires so the UI refreshes.
//
// When no session is connected the controller is inert: isActive() is false, broadcast() is a
// no-op, and the DAW behaves exactly as it did before. This is what keeps collab strictly additive.
class CollabController
{
public:
    explicit CollabController(ProjectState& stateToBind) : state(stateToBind) {}

    // Go live over the given transport (ownership taken). Stamps stable ids onto the existing
    // project so subsequent ops have something to target, then starts relaying.
    void connect(std::unique_ptr<CollabTransport> transportToOwn, EntityId actorSalt)
    {
        transport = std::move(transportToOwn);
        session = std::make_unique<CollabSession>(state, *transport, actorSalt);
        session->onRemoteApplied = [this] { if (onProjectChanged) onProjectChanged(); };
        session->assignInitialIds();
    }

    void disconnect()
    {
        session.reset();     // detaches the transport callback first (session dtor)
        transport.reset();
    }

    bool isActive() const noexcept { return session != nullptr; }

    // Mint a globally-unique id for a new entity the local user is creating. Returns noEntity when
    // no session is live (the DAW then just leaves the entity's id at 0, as before).
    EntityId newId() { return session != nullptr ? session->newId() : noEntity; }

    // Broadcast a local edit the DAW has ALREADY applied to the project. No-op when inactive.
    void broadcast(const Op& op)
    {
        if (session != nullptr)
            session->sendLocal(op);
    }

    // Fired after a peer's op mutated the ProjectState — MainComponent repaints / refreshes here.
    std::function<void()> onProjectChanged;

private:
    ProjectState& state;
    std::unique_ptr<CollabTransport> transport;
    std::unique_ptr<CollabSession> session;
};
} // namespace collab
} // namespace orion
