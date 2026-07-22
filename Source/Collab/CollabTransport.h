#pragma once

#include "CollabTypes.h"

#include <functional>
#include <vector>

// The transport abstraction — how ops leave and arrive at this client. The rest of the Collab
// module talks only to this interface, so the same session/op-log code runs unchanged over:
//   • LoopbackTransport   — in-process, for unit tests and a two-window localhost bring-up
//   • WebSocketTransport  — the real server-sequenced connection (Phase 3)
// Nothing above this line knows which one is plugged in.

namespace orion::collab
{
// A connected participant as the transport sees them (identity + presence only; audio/video and
// chat live in higher layers).
struct PeerInfo
{
    ActorId      id;
    juce::String displayName;
    juce::uint32 colourArgb { 0xff9e9e9e };   // ARGB; kept as a raw int so the transport layer
                                              // needs only juce_core (no juce_graphics dependency).
};

class CollabTransport
{
public:
    virtual ~CollabTransport() = default;

    // Send a locally-created op to the server/peers. The op's seq is still 0 here; the server
    // assigns the authoritative sequence and echoes it back via onOpReceived.
    virtual void sendOp(const Op& op) = 0;

    // Our own actor id on this connection (assigned on join). Empty until connected.
    virtual ActorId localActor() const = 0;

    virtual bool isConnected() const = 0;

    // ---- Callbacks the session installs. All are delivered on the message thread. ----

    // A sequenced op arrived (either a peer's edit, or the server-confirmed echo of one of ours).
    std::function<void(const Op&)> onOpReceived;

    // The roster changed (someone joined/left, name/colour updated).
    std::function<void(const std::vector<PeerInfo>&)> onPeersChanged;

    // Connection state flipped.
    std::function<void(bool /*connected*/)> onConnectionChanged;
};
} // namespace orion::collab
