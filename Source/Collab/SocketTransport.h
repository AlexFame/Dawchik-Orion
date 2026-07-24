#pragma once

#include "CollabTransport.h"

#include <juce_events/juce_events.h>

#include <atomic>

// SocketTransport — a CollabTransport over a TCP connection to a CollabServer (the sequencer/hub).
// The same class is used by a host Orion (connecting to its own embedded server on localhost) and a
// guest (connecting to the host's address), and unchanged against a real remote server in Phase 3.
//
// Threading: juce::InterprocessConnection delivers messageReceived / connection callbacks on a
// background socket thread. The CollabTransport contract promises callbacks on the MESSAGE thread
// (op application touches ProjectState + the audio-edit lock), so every callback is marshalled with
// MessageManager::callAsync, guarded by a WeakReference so a teardown mid-flight can't dangle.

namespace orion::collab
{
class SocketTransport final : public CollabTransport,
                              private juce::InterprocessConnection
{
public:
    explicit SocketTransport(ActorId actor)
        : juce::InterprocessConnection(/*callbacksOnMessageThread*/ false),
          actorId(std::move(actor))
    {
    }

    ~SocketTransport() override
    {
        masterReference.clear();   // no queued callAsync may touch us after this
        disconnect();
    }

    bool connectToServer(const juce::String& host, int port, int timeoutMs = 4000)
    {
        return connectToSocket(host, port, timeoutMs);
    }

    void closeConnection() { disconnect(); }

    // ---- CollabTransport ----
    void sendOp(const Op& op) override { sendMessage(wire::encode(wire::opMessage(op))); }
    void sendSnapshot(const juce::var& project) override { sendMessage(wire::encode(wire::snapshotMessage(project))); }
    void requestBacklog() override { sendMessage(wire::encode(wire::backlogRequest())); }

    ActorId localActor() const override { return actorId; }
    bool isConnected() const override { return connectedFlag.load(); }

private:
    void connectionMade() override
    {
        connectedFlag = true;
        marshalConnection(true);
    }

    void connectionLost() override
    {
        connectedFlag = false;
        marshalConnection(false);
    }

    void messageReceived(const juce::MemoryBlock& message) override
    {
        const auto decoded = wire::decode(message);
        const auto kind = wire::kindOf(decoded);
        juce::WeakReference<SocketTransport> weak(this);

        if (kind == wire::kindOp)
        {
            auto op = Op::fromVar(decoded.getProperty("op", juce::var()));
            juce::MessageManager::callAsync([weak, op]
            {
                if (auto* self = weak.get())
                    if (self->onOpReceived)
                        self->onOpReceived(op);
            });
        }
        else if (kind == wire::kindSnapshot)
        {
            auto project = decoded.getProperty("project", juce::var());
            juce::MessageManager::callAsync([weak, project]
            {
                if (auto* self = weak.get())
                    if (self->onSnapshotReceived)
                        self->onSnapshotReceived(project);
            });
        }
    }

    void marshalConnection(bool nowConnected)
    {
        juce::WeakReference<SocketTransport> weak(this);
        juce::MessageManager::callAsync([weak, nowConnected]
        {
            if (auto* self = weak.get())
                if (self->onConnectionChanged)
                    self->onConnectionChanged(nowConnected);
        });
    }

    ActorId actorId;
    std::atomic<bool> connectedFlag { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE(SocketTransport)
};
} // namespace orion::collab
