#pragma once

#include "CollabTransport.h"

#include <algorithm>
#include <vector>

// In-process transport used for unit tests and a two-window localhost bring-up. A LoopbackHub plays
// the role of the server: it assigns each op a global sequence number, keeps the session history
// (baseline snapshot + ops since) for late joiners, and broadcasts to every connected transport —
// including the original sender, which is exactly how the real server echoes your own op back with
// its authoritative seq. No sockets, no threads: delivery is synchronous, so tests are deterministic.

namespace orion::collab
{
class LoopbackTransport;

class LoopbackHub
{
public:
    void join(LoopbackTransport* t) { members.push_back(t); }
    void leave(LoopbackTransport* t) { members.erase(std::remove(members.begin(), members.end(), t), members.end()); }

    // Defined out-of-line below (they need LoopbackTransport's full type).
    void publish(Op op);
    void publishSnapshot(const juce::var& project);
    void publishPresence(LoopbackTransport* from, const juce::var& presence);
    void publishAssetRequest(LoopbackTransport* from, const juce::String& hash);
    void publishAssetData(LoopbackTransport* from, const juce::String& hash, const juce::String& name, const juce::MemoryBlock& bytes);
    void publishVoice(LoopbackTransport* from, const juce::String& actor, int rate, const juce::MemoryBlock& pcm);
    void sendBacklogTo(LoopbackTransport* t);

private:
    std::vector<LoopbackTransport*> members;
    Seq nextSeq { 0 };

    juce::var baseline;
    bool hasBaseline { false };
    std::vector<Op> opLog;
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
    void sendSnapshot(const juce::var& project) override { hub.publishSnapshot(project); }
    void requestBacklog() override { hub.sendBacklogTo(this); }
    void sendPresence(const juce::var& presence) override { hub.publishPresence(this, presence); }
    void sendAssetRequest(const juce::String& hash) override { hub.publishAssetRequest(this, hash); }
    void sendAssetData(const juce::String& h, const juce::String& n, const juce::MemoryBlock& b) override { hub.publishAssetData(this, h, n, b); }
    void sendVoice(const juce::String& a, int r, const juce::MemoryBlock& p) override { hub.publishVoice(this, a, r, p); }
    ActorId localActor() const override { return actorId; }
    bool isConnected() const override { return true; }

    // Called by the hub when a sequenced op / baseline arrives.
    void deliver(const Op& op)
    {
        if (onOpReceived)
            onOpReceived(op);
    }

    void deliverPresence(const juce::var& presence)
    {
        if (onPresenceReceived)
            onPresenceReceived(presence);
    }

    void deliverAssetRequest(const juce::String& hash) { if (onAssetRequested) onAssetRequested(hash); }
    void deliverAssetData(const juce::String& h, const juce::String& n, const juce::MemoryBlock& b) { if (onAssetData) onAssetData(h, n, b); }
    void deliverVoice(const juce::String& a, int r, const juce::MemoryBlock& p) { if (onVoice) onVoice(a, r, p); }

    void deliverSnapshot(const juce::var& project)
    {
        if (onSnapshotReceived)
            onSnapshotReceived(project);
    }

private:
    LoopbackHub& hub;
    ActorId actorId;
};

inline void LoopbackHub::publish(Op op)
{
    op.seq = ++nextSeq;                // server assigns the authoritative order
    opLog.push_back(op);
    for (auto* m : members)
        m->deliver(op);
}

inline void LoopbackHub::publishSnapshot(const juce::var& project)
{
    baseline = project;                // a fresh baseline supersedes everything logged before it
    hasBaseline = true;
    opLog.clear();
}

inline void LoopbackHub::publishPresence(LoopbackTransport* from, const juce::var& presence)
{
    for (auto* m : members)          // everyone except the sender; presence is never logged
        if (m != from)
            m->deliverPresence(presence);
}

inline void LoopbackHub::publishAssetRequest(LoopbackTransport* from, const juce::String& hash)
{
    for (auto* m : members) if (m != from) m->deliverAssetRequest(hash);
}

inline void LoopbackHub::publishAssetData(LoopbackTransport* from, const juce::String& hash, const juce::String& name, const juce::MemoryBlock& bytes)
{
    for (auto* m : members) if (m != from) m->deliverAssetData(hash, name, bytes);
}

inline void LoopbackHub::publishVoice(LoopbackTransport* from, const juce::String& actor, int rate, const juce::MemoryBlock& pcm)
{
    for (auto* m : members) if (m != from) m->deliverVoice(actor, rate, pcm);
}

inline void LoopbackHub::sendBacklogTo(LoopbackTransport* t)
{
    if (t == nullptr)
        return;
    if (hasBaseline)
        t->deliverSnapshot(baseline);
    for (const auto& op : opLog)
        t->deliver(op);
}
} // namespace orion::collab
