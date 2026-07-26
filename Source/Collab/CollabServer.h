#pragma once

#include "CollabTypes.h"

#include <juce_events/juce_events.h>

// CollabServer — the sequencer/hub. Accepts TCP connections, stamps every incoming op with the next
// global sequence number, and rebroadcasts it to every connected client (including the sender, whose
// CollabSession ignores its own echo by actor id). It never touches a ProjectState, so it runs
// entirely on the socket threads. Embedded in a host Orion for localhost jams now; the identical
// class runs standalone as the remote server in Phase 3.

namespace orion::collab
{
class CollabServer final : private juce::InterprocessConnectionServer
{
public:
    ~CollabServer() override { stop(); }

    // An empty bind address listens on every interface, so a peer on the LAN can reach the host —
    // binding to 127.0.0.1 would have accepted only same-machine connections while the UI happily
    // advertised the LAN address.
    bool start(int port, const juce::String& bindAddress = {})
    {
        return beginWaitingForSocket(port, bindAddress);
    }

    void stop()
    {
        masterReference.clear();
        juce::InterprocessConnectionServer::stop();
        const juce::ScopedLock sl(lock);
        for (auto* c : connections)
            c->disconnect();
        connections.clear(true);
    }

    int numConnections() const { const juce::ScopedLock sl(lock); return connections.size(); }

private:
    struct Conn final : juce::InterprocessConnection
    {
        explicit Conn(CollabServer& o) : juce::InterprocessConnection(false), owner(o) {}
        void connectionMade() override {}
        void connectionLost() override { owner.handleLost(this); }
        void messageReceived(const juce::MemoryBlock& m) override { owner.handleMessage(*this, m); }
        CollabServer& owner;
    };

    juce::InterprocessConnection* createConnectionObject() override
    {
        auto* c = new Conn(*this);
        const juce::ScopedLock sl(lock);
        connections.add(c);
        return c;
    }

    void handleMessage(Conn& from, const juce::MemoryBlock& message)
    {
        const auto parsed = wire::decode(message);
        const auto kind = wire::kindOf(parsed);

        if (kind == wire::kindOp)
        {
            // Stamp the authoritative order, remember it for late joiners, fan it out.
            auto opVar = parsed.getProperty("op", juce::var());
            if (auto* opObj = opVar.getDynamicObject())
                opObj->setProperty("seq", juce::String(nextSeq.fetch_add(1) + 1));

            const auto out = wire::encode(wire::opMessage(Op::fromVar(opVar)));
            const juce::ScopedLock sl(lock);
            opLog.add(out);
            for (auto* c : connections)
                c->sendMessage(out);
            return;
        }

        if (kind == wire::kindPresence || kind == wire::kindAssetRequest || kind == wire::kindAssetData)
        {
            // Forward to everyone else and forget it: presence must never enter the op log, or a
            // late joiner would be replayed a stream of stale mouse positions.
            const auto out = wire::encode(parsed);
            const juce::ScopedLock sl(lock);
            for (auto* c : connections)
                if (c != &from)
                    c->sendMessage(out);
            return;
        }

        if (kind == wire::kindSnapshot)
        {
            // The host published a fresh baseline: it supersedes every op logged before it.
            const juce::ScopedLock sl(lock);
            baseline = wire::encode(parsed);
            hasBaseline = true;
            opLog.clear();
            return;
        }

        if (kind == wire::kindBacklog)
        {
            // A client just joined and is ready to listen: hand it the baseline, then the ops since.
            const juce::ScopedLock sl(lock);
            if (hasBaseline)
                from.sendMessage(baseline);
            for (const auto& logged : opLog)
                from.sendMessage(logged);
        }
    }

    void handleLost(Conn* c)
    {
        // connectionLost() fires on the socket thread; deleting the connection there (from inside its
        // own callback) is unsafe, so defer removal to the message thread.
        juce::WeakReference<CollabServer> weak(this);
        juce::MessageManager::callAsync([weak, c]
        {
            if (auto* self = weak.get())
            {
                const juce::ScopedLock sl(self->lock);
                self->connections.removeObject(c, true);
            }
        });
    }

    mutable juce::CriticalSection lock;
    juce::OwnedArray<Conn> connections;
    std::atomic<Seq> nextSeq { 0 };

    // Session history, so someone joining a jam already in progress gets the full picture rather
    // than only the edits made after they arrived.
    juce::MemoryBlock baseline;
    bool hasBaseline { false };
    juce::Array<juce::MemoryBlock> opLog;

    JUCE_DECLARE_WEAK_REFERENCEABLE(CollabServer)
};
} // namespace orion::collab
