#pragma once

#include "CollabTransport.h"
#include "CollabTypes.h"

#include <functional>

namespace orion
{
class ProjectState;

namespace collab
{
// CollabSession — the object the DAW holds (via CollabController later). It binds one ProjectState
// to one transport: local edits are turned into ops, applied optimistically, and sent; sequenced
// ops from peers are applied to the ProjectState. This is the ONE type MainComponent needs to know
// about; everything below (OpLog, transport, wire format) stays inside the module.
//
// Phase-1 model (deliberately simple): local ops are applied immediately and their server echo is
// ignored (we authored them, already applied). Peer ops are applied in the order the server
// delivered them. No rollback/rebase yet — that arrives with real conflict handling in a later phase.
class CollabSession
{
public:
    // actorSalt seeds this client's EntityIdGenerator so two clients never mint the same id.
    CollabSession(ProjectState& state, CollabTransport& transport, EntityId actorSalt);
    ~CollabSession();

    // Mint a fresh, globally-unique id for a new entity the local user is creating.
    EntityId newId() noexcept { return ids.next(); }

    // Stamp an op as ours, apply it locally, and send it. The caller builds the op via ops:: and
    // sets any new-entity ids from newId(). Use when the op is the source of truth (tests / flows
    // that don't already mutate the project themselves).
    void submitLocal(Op op);

    // Stamp an op as ours and send it WITHOUT applying locally — for the DAW integration, where the
    // app's own edit code already mutated the project and we only need to broadcast a description of
    // it. The peer applies it via OpLog; our own echo is ignored on return.
    void sendLocal(Op op);

    // Bring every id-less entity in the project up to a stable id (call once at session start).
    void assignInitialIds();

    // Publish the current project as the session baseline (the host does this once, after ids are
    // assigned) so anyone joining later starts from the real project instead of an empty one.
    void publishSnapshot();

    // Ask the server for the baseline + every op since. Called by a joining client once its
    // callbacks are wired.
    void requestBacklog();

    ActorId localActor() const;

    // Fired after a PEER op has been applied to the ProjectState, so the UI can refresh.
    std::function<void()> onRemoteApplied;

private:
    void handleIncoming(const Op& op);
    void handleSnapshot(const juce::var& project);

    ProjectState& state;
    CollabTransport& transport;
    EntityIdGenerator ids;
};
} // namespace collab
} // namespace orion
