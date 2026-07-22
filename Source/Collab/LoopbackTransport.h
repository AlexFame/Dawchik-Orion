#pragma once

#include "CollabTransport.h"

#include <algorithm>
#include <memory>
#include <vector>

// In-process transport used for unit tests and a two-window localhost bring-up. A LoopbackHub plays
// the role of the server: it assigns each op a global sequence number and broadcasts it to every
// connected transport — including the original sender, which is exactly how the real server echoes
// your own op back with its authoritative seq. No sockets, no threads: delivery is synchronous, so
// tests are deterministic.

namespace orion::collab
{
class LoopbackTransport;

class LoopbackHub
{
public:
    void join(LoopbackTransport* t) { members.push_back(t); }
    void leave(LoopbackTransport* t) { members.erase(std::remove(members.begin(), members.end(), t), members.end()); }

    // Sequence + fan-out. Defined out-of-line below (needs LoopbackTransport's full type).
    void publish(Op op);

private:
    std::vector<LoopbackTransport*> members;
    Seq nextSeq { 0 };
};

class LoopbackTransport final : public CollabTransport
{
public:
    LoopbackTransport(LoopbackHub& hubToJoin, ActorId actor)
        : hub(hubToJoin), actorId(std::move(actor))
    {
        hub.join(this);
    }

    ~LoopbackTransport() override { hub.leave(this); }

    void sendOp(const Op& op) override { hub.publish(op); }
    ActorId localActor() const override { return actorId; }
    bool isConnected() const override { return true; }

    // Called by the hub when a sequenced op arrives.
    void deliver(const Op& op)
    {
        if (onOpReceived)
            onOpReceived(op);
    }

private:
    LoopbackHub& hub;
    ActorId actorId;
};

inline void LoopbackHub::publish(Op op)
{
    op.seq = ++nextSeq;                // server assigns the authoritative order
    for (auto* m : members)
        m->deliver(op);
}
} // namespace orion::collab
