#pragma once

#include "AssetStore.h"
#include "CollabTransport.h"
#include "CollabTypes.h"

#include <functional>
#include <map>
#include <vector>

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
    CollabSession(ProjectState& state, CollabTransport& transport, AssetStore& assets, EntityId actorSalt);
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

    // Tell the others where we are (Figma-style live cursor). Cheap and frequent; never logged.
    void publishPresence(const juce::String& displayName, juce::uint32 colourArgb,
                         double beat, double contentY, bool overTimeline, bool inCall);

    // Peers seen recently, stalest entries dropped. Safe to call every frame.
    std::vector<PeerPresence> peers() const;

    ActorId localActor() const;

    // Fired after a PEER op has been applied to the ProjectState, so the UI can refresh.
    std::function<void()> onRemoteApplied;

    // A PEER changed the shared transport (play/stop + start beat). Routed here instead of the
    // op-log because the transport lives outside ProjectState.
    std::function<void(bool playing, double beat)> onRemoteTransport;

    // A chat line arrived (peer message live, or session history replayed on join).
    std::function<void(const juce::String& name, const juce::String& text)> onChat;

    // The host ended the video call for everyone — drop our own call locally.
    std::function<void()> onCallEnded;

    // A requested audio asset finished downloading into the cache (hash -> local file).
    std::function<void(const juce::String& hash, const juce::File& file)> onAssetReady;

    // Ask peers for an audio file by content hash.
    void requestAsset(const juce::String& hash) { transport.sendAssetRequest(hash); }

    // Live voice: send our mic chunk; onVoice fires when a peer's chunk arrives.
    void sendVoice(int sampleRate, const juce::MemoryBlock& pcm) { transport.sendVoice(transport.localActor(), sampleRate, pcm); }
    std::function<void(const juce::String& actor, int sampleRate, const juce::MemoryBlock& pcm)> onVoice;

    // Live counters — surfaced in the Jam panel so a stalled session can be diagnosed from the UI
    // instead of guessing which link in the chain is broken.
    int opsSent { 0 };
    int opsReceived { 0 };      // peer ops that arrived (whether or not they applied)
    int opsApplied { 0 };
    int snapshotsReceived { 0 };
    int presenceSent { 0 };
    int presenceReceived { 0 };

private:
    void handleIncoming(const Op& op);
    void handleSnapshot(const juce::var& project);
    void handlePresence(const juce::var& presence);
    // Turn received "asset://<hash>" audio refs into local paths, requesting any file we lack.
    void resolveAllAssetRefs();

    ProjectState& state;
    CollabTransport& transport;
    AssetStore& assets;
    EntityIdGenerator ids;
    std::map<ActorId, PeerPresence> presenceByActor;
};
} // namespace collab
} // namespace orion
