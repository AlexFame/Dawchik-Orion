#pragma once

#include "AssetRefs.h"
#include "AssetStore.h"
#include "CollabServer.h"
#include "OpLog.h"
#include "CollabSession.h"
#include "CollabTransport.h"
#include "CollabTypes.h"
#include "SocketTransport.h"

#include <functional>
#include <map>
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
    explicit CollabController(ProjectState& stateToBind, juce::File assetCacheDir = {})
        : state(stateToBind),
          assets(assetCacheDir != juce::File()
                     ? assetCacheDir
                     : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Orion/collab-assets"))
    {}

    // Go live over the given transport (ownership taken). Stamps stable ids onto the existing
    // project so subsequent ops have something to target, then starts relaying.
    void connect(std::unique_ptr<CollabTransport> transportToOwn, EntityId actorSalt)
    {
        transport = std::move(transportToOwn);
        session = std::make_unique<CollabSession>(state, *transport, assets, actorSalt);
        session->onAssetReady = [this](const juce::String& hash, const juce::File& file) { if (onAssetReady) onAssetReady(hash, file); };
        session->onRemoteApplied = [this] { if (onProjectChanged) onProjectChanged(); };
        session->onRemoteTransport = [this](bool playing, double beat) { if (onRemoteTransport) onRemoteTransport(playing, beat); };
        session->onChat = [this](const juce::String& name, const juce::String& text) { if (onRemoteChat) onRemoteChat(name, text); };
        session->onVoice = [this](const juce::String& actor, int rate, const juce::MemoryBlock& pcm) { if (onVoiceReceived) onVoiceReceived(actor, rate, pcm); };
        session->assignInitialIds();
    }

    void disconnect()
    {
        session.reset();     // detaches the transport callback first (session dtor)
        transport.reset();
        embeddedServer.reset();
        reconnectAddress = {};   // an explicit Leave must not trigger auto-reconnect
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

        // Remember how we got in so a dropped guest can silently reconnect.
        reconnectAddress = address;
        reconnectPort = port;
        reconnectActor = me;
        reconnectSalt = actorSalt;

        connect(std::move(socket), actorSalt);
        // Callbacks are wired now, so it is safe to ask for the baseline + everything since.
        session->requestBacklog();
        return true;
    }

    // A guest whose socket dropped is still "active" (the session object lives) but not connected.
    bool isConnected() const noexcept { return transport != nullptr && transport->isConnected(); }
    bool canReconnect() const noexcept { return reconnectAddress.isNotEmpty(); }
    juce::String reconnectDisplay() const { return reconnectAddress + ":" + juce::String(reconnectPort); }

    // Re-establish a dropped guest connection: fresh socket to the same host, then re-request the
    // backlog so we catch up on everything missed. Returns false if the host still isn't reachable.
    bool tryReconnect()
    {
        if (reconnectAddress.isEmpty())
            return false;   // host doesn't auto-reconnect (its server is local)

        // Keep the reconnect params: disconnect() clears the session but we re-join with the same id.
        const auto address = reconnectAddress;
        const auto port = reconnectPort;
        const auto me = reconnectActor;
        const auto salt = reconnectSalt;

        session.reset();
        transport.reset();
        return joinSession(address, port, me, salt);
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

    // A peer sent a chat line (or history is replaying on join).
    std::function<void(const juce::String& name, const juce::String& text)> onRemoteChat;

    // Fired when the socket connects/drops (guest). MainComponent shows status + drives reconnect.
    std::function<void(bool connected)> onConnectionChanged;

    // ---- Audio assets (content-addressed sample transfer) ----
    // Register a local audio file we reference, returning its content hash (empty if unreadable).
    juce::String registerAsset(const juce::File& file)
    {
        const auto hash = AssetStore::hashOfFile(file);
        assets.registerLocal(hash, file);
        return hash;
    }

    // Local path -> content hash, memoised by (path, modtime) so a WAV isn't re-hashed on every
    // sync tick. Registers the file as a local original. Empty if the path is missing/unreadable.
    juce::String assetHashForPath(const juce::String& path)
    {
        if (path.isEmpty())
            return {};

        juce::File f(path);
        if (! f.existsAsFile())
            return {};

        const auto stamp = f.getLastModificationTime().toMilliseconds();
        if (auto it = pathHashCache.find(path); it != pathHashCache.end() && it->second.first == stamp)
            return it->second.second;

        const auto hash = registerAsset(f);
        pathHashCache[path] = { stamp, hash };
        return hash;
    }

    // A hash -> a local path if we already have the file; otherwise request it and return empty so
    // the caller keeps the sentinel until the bytes arrive.
    juce::String assetPathOrRequest(const juce::String& hash)
    {
        const auto f = assets.localFor(hash);
        if (f.existsAsFile())
            return f.getFullPathName();
        requestAsset(hash);
        return {};
    }
    bool hasAsset(const juce::String& hash) const { return assets.has(hash); }
    juce::File assetPath(const juce::String& hash) const { return assets.localFor(hash); }
    void requestAsset(const juce::String& hash) { if (session != nullptr) session->requestAsset(hash); }
    // A missing asset finished downloading (hash -> local cache file).
    std::function<void(const juce::String& hash, const juce::File& file)> onAssetReady;

    // ---- Live voice ----
    void sendVoice(int sampleRate, const juce::MemoryBlock& pcm) { if (session != nullptr) session->sendVoice(sampleRate, pcm); }
    std::function<void(const juce::String& actor, int sampleRate, const juce::MemoryBlock& pcm)> onVoiceReceived;

    void sendChat(const juce::String& name, const juce::String& text)
    {
        if (session != nullptr)
            broadcast(collab::ops::chat(name, text));
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
    AssetStore assets;
    std::map<juce::String, std::pair<juce::int64, juce::String>> pathHashCache;   // path -> (modtime, hash)

    juce::String reconnectAddress;   // set only for a guest, so it can silently reconnect on a drop
    int reconnectPort { 0 };
    ActorId reconnectActor;
    EntityId reconnectSalt { 0 };
};
} // namespace collab
} // namespace orion
