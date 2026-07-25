#pragma once

#include "CollabServer.h"
#include "OpLog.h"
#include "CollabSession.h"
#include "CollabTransport.h"
#include "CollabTypes.h"
#include "SocketTransport.h"

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
        session->onRemoteTransport = [this](bool playing, double beat) { if (onRemoteTransport) onRemoteTransport(playing, beat); };
        session->assignInitialIds();
    }

    void disconnect()
    {
        session.reset();     // detaches the transport callback first (session dtor)
        transport.reset();
        embeddedServer.reset();
    }

    // ---- One-call session bootstrap, so the DAW's wiring stays a single line. ----

    // Host a jam: start an embedded sequencing server on `port` and connect to it locally.
    // Returns false if the port is already taken.
    bool hostSession(int port, const ActorId& me, EntityId actorSalt)
    {
        auto server = std::make_unique<CollabServer>();
        if (! server->start(port))
            return false;

        auto socket = std::make_unique<SocketTransport>(me);
        if (! socket->connectToServer("127.0.0.1", port))
            return false;

        embeddedServer = std::move(server);
        connect(std::move(socket), actorSalt);
        // The host's project IS the session's starting point — publish it so anyone joining later
        // receives the real arrangement instead of an empty timeline.
        session->publishSnapshot();
        return true;
    }

    // Join someone else's jam (their host address / a standalone server later).
    bool joinSession(const juce::String& address, int port, const ActorId& me, EntityId actorSalt)
    {
        auto socket = std::make_unique<SocketTransport>(me);
        if (! socket->connectToServer(address, port))
            return false;

        connect(std::move(socket), actorSalt);
        // Callbacks are wired now, so it is safe to ask for the baseline + everything since.
        session->requestBacklog();
        return true;
    }

    bool isHosting() const noexcept { return embeddedServer != nullptr; }

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

    // ---- Live presence (Figma-style cursors) ----
    void publishPresence(const juce::String& displayName, juce::uint32 colourArgb,
                         double beat, double contentY, bool overTimeline)
    {
        if (session != nullptr)
            session->publishPresence(displayName, colourArgb, beat, contentY, overTimeline);
    }

    std::vector<PeerPresence> peers() const
    {
        return session != nullptr ? session->peers() : std::vector<PeerPresence> {};
    }

    // Fired after a peer's op mutated the ProjectState — MainComponent repaints / refreshes here.
    std::function<void()> onProjectChanged;

    // A peer started/stopped the shared transport. MainComponent drives its own transport to match.
    std::function<void(bool playing, double beat)> onRemoteTransport;

    void sendTransport(bool playing, double beat)
    {
        if (session != nullptr)
            broadcast(collab::ops::setTransport(playing, beat));
    }

    // One-line health summary for the Jam panel. A stalled session is otherwise invisible: this
    // says whether the socket is up, how many clients the server sees, and whether ops are
    // actually flowing in each direction.
    juce::String diagnosticsLine() const
    {
        if (session == nullptr)
            return "not connected";

        juce::String s;
        s << (transport != nullptr && transport->isConnected() ? "link up" : "LINK DOWN")
          << "  out " << session->opsSent
          << "  in " << session->opsReceived
          << "  applied " << session->opsApplied
          << "  snap " << session->snapshotsReceived;

        s << "  presOut " << session->presenceSent
          << "  presIn " << session->presenceReceived
          << "  peers " << static_cast<int>(session->peers().size());

        if (embeddedServer != nullptr)
            s << "  clients " << embeddedServer->numConnections();

        return s;
    }

private:
    ProjectState& state;
    std::unique_ptr<CollabServer> embeddedServer;   // only when hosting
    std::unique_ptr<CollabTransport> transport;
    std::unique_ptr<CollabSession> session;
};
} // namespace collab
} // namespace orion
