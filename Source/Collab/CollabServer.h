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

    bool start(int port, const juce::String& bindAddress = "127.0.0.1")
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
        void messageReceived(const juce::MemoryBlock& m) override { owner.handleMessage(m); }
        CollabServer& owner;
    };

    juce::InterprocessConnection* createConnectionObject() override
    {
        auto* c = new Conn(*this);
        const juce::ScopedLock sl(lock);
        connections.add(c);
        return c;
    }

    void handleMessage(const juce::MemoryBlock& message)
    {
        auto parsed = juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(message.getData()),
                                                               static_cast<int>(message.getSize())));
        if (auto* obj = parsed.getDynamicObject())
            obj->setProperty("seq", juce::String(nextSeq.fetch_add(1) + 1));   // authoritative order

        const auto json = juce::JSON::toString(parsed, true);
        juce::MemoryBlock out(json.toRawUTF8(), json.getNumBytesAsUTF8());

        const juce::ScopedLock sl(lock);
        for (auto* c : connections)
            c->sendMessage(out);
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

    JUCE_DECLARE_WEAK_REFERENCEABLE(CollabServer)
};
} // namespace orion::collab
