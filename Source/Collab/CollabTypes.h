#pragma once

#include <juce_core/juce_core.h>

// Collab module — foundation types for real-time multiplayer editing.
//
// The whole feature is built on a server-sequenced op-log (the Figma/Google-Docs model):
// every edit is expressed as a small Operation instead of shipping the whole project. Ops are
// applied to ProjectState on every client; a server assigns each op a global sequence number so
// all clients converge on the same order. This header defines the data an op carries — nothing
// here touches the DAW, the UI, or the network, so the module stays fully decoupled.
//
// CRITICAL PREREQUISITE (Phase 0): ops address entities by stable EntityId, never by vector index.
// Indices drift the instant another participant inserts/deletes above you; ids do not. TrackState /
// TimelineClip / MidiNote must each carry an EntityId before ops can target them reliably.

namespace orion::collab
{
// Globally-unique-across-clients id for a track / clip / note / bus. 0 means "none".
// Uniqueness is guaranteed by minting ids with a per-actor salt in the high bits (see
// EntityIdGenerator) so two clients editing offline never collide.
using EntityId = juce::uint64;

// Stable per-connection participant id (who authored an op). Assigned by the session on join.
using ActorId = juce::String;

// Server-assigned global order. 0 = locally created but not yet sequenced by the server
// (an "optimistic" op the client already applied and is waiting to have confirmed/reordered).
using Seq = juce::int64;

constexpr EntityId noEntity = 0;

// Where a collaborator currently is. Expressed in PROJECT coordinates (beat + track row), never in
// pixels — each client has its own scroll and zoom, so a screen position would land somewhere else
// on their timeline.
struct PeerPresence
{
    ActorId      actor;
    juce::String name;
    juce::uint32 colourArgb { 0xff9e9e9e };
    double       beat { 0.0 };
    // Vertical position in the timeline's CONTENT space (scroll already removed), so it lands at
    // the same musical row on a peer who has scrolled differently. A track index alone would pin
    // the cursor to one height and lose all vertical movement between/below tracks.
    double       contentY { 0.0 };
    bool         overTimeline { false };
    // Whether this peer is currently in the A/V call (Zoom-style): drives whether their video tile
    // shows. Leaving the call (red hang-up) flips this off while they stay in the jam.
    bool         inCall { false };
    juce::int64  lastSeenMs { 0 };
};

// Mints process-unique EntityIds that also never collide with other clients: the high 16 bits are
// a random per-actor salt, the low 48 bits a monotonic counter. 48 bits of counter is ~281e12
// ids per session — never exhausted in practice.
class EntityIdGenerator
{
public:
    EntityIdGenerator() : salt(static_cast<EntityId>(juce::Random::getSystemRandom().nextInt64()) & 0xffffULL) {}
    explicit EntityIdGenerator(EntityId actorSalt) : salt(actorSalt & 0xffffULL) {}

    EntityId next() noexcept
    {
        const auto n = ++counter & 0x0000ffffffffffffULL;   // low 48 bits
        return (salt << 48) | n;
    }

private:
    EntityId salt { 0 };
    EntityId counter { 0 };
};

enum class OpType
{
    // Track lifecycle / properties.
    addTrack,
    removeTrack,
    moveTrack,          // reorder within the track list
    setTrackField,      // name / mute / solo / recordArmed / volumeDb / pan / trackGainDb / colour…

    // Clip lifecycle / geometry / properties.
    addClip,
    removeClip,
    moveClip,           // startBeat and/or owning track
    resizeClip,         // lengthInBeats
    setClipField,       // gainDb / muted / solo / colour / name / warpEnabled…

    // Whole-clip sync: create or fully replace a clip from its serialised form (every field,
    // including warp markers and key data). Coarser than the per-field ops but immune to a field
    // being forgotten, which silently desyncs.
    replaceClip,

    // Sync a track's properties wholesale (VST instrument+state, sampler, MPC, inserts, sends…)
    // from its serialised form. Its CLIPS are handled by the clip ops, so this never touches them —
    // avoids clips racing down two paths at once.
    updateTrackProps,

    // Notes within a clip.
    addNote,
    removeNote,
    editNote,           // pitch / startBeat / lengthInBeats / velocity

    // Pitch slides within a clip.
    addSlide,
    removeSlide,
    editSlide,

    // Mixer: aux buses (+ their inserts) and the master insert chain, each synced as one blob.
    replaceBuses,
    replaceMasterInserts,

    // Project-wide / transport (shared clock).
    setTempo,
    setTransport,       // play / stop / position — keeps everyone phase-locked
    chatMessage,        // routed to the chat handler (not ProjectState); logged so joiners see history
    endCallForAll,      // host ends the video call for every participant (routed, not ProjectState)

    // Bring-up escape hatch: replace one clip's whole note vector in a single op. Coarser than
    // per-note ops (loses fine-grained merging) but trivially correct — used before per-note
    // editNote is wired, then retired.
    replaceClipNotes,

    unknown
};

// One atomic edit. Data-driven on purpose: target ids pick the entity, `payload` carries the
// changed fields as a key/value bag. That keeps the wire format and OpLog::apply() open to new
// fields without a class per operation.
struct Op
{
    OpType   type { OpType::unknown };
    Seq      seq { 0 };
    ActorId  actor;

    EntityId track { noEntity };
    EntityId clip { noEntity };
    EntityId note { noEntity };

    juce::var payload;   // e.g. { "startBeat": 4.0, "track": <newTrackId as string> }

    // ---- JSON (juce::var) round-trip. EntityIds are serialised as decimal strings because
    // juce::var's integer is 32-bit-ish for JSON and would truncate a 64-bit id. ----
    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("type", static_cast<int>(type));
        obj->setProperty("seq", juce::String(seq));
        obj->setProperty("actor", actor);
        obj->setProperty("track", juce::String(track));
        obj->setProperty("clip", juce::String(clip));
        obj->setProperty("note", juce::String(note));
        obj->setProperty("payload", payload);
        return juce::var(obj);
    }

    static Op fromVar(const juce::var& v)
    {
        Op op;
        op.type    = static_cast<OpType>(static_cast<int>(v.getProperty("type", static_cast<int>(OpType::unknown))));
        op.seq     = v.getProperty("seq", "0").toString().getLargeIntValue();
        op.actor   = v.getProperty("actor", juce::String()).toString();
        op.track   = static_cast<EntityId>(v.getProperty("track", "0").toString().getLargeIntValue());
        op.clip    = static_cast<EntityId>(v.getProperty("clip", "0").toString().getLargeIntValue());
        op.note    = static_cast<EntityId>(v.getProperty("note", "0").toString().getLargeIntValue());
        op.payload = v.getProperty("payload", juce::var());
        return op;
    }
};

// The wire envelope. Every message on the socket is a JSON object with a "kind" so ops, the project
// baseline and control messages can share one channel. Defined here so the transport and the server
// can never drift apart on the format.
namespace wire
{
    inline constexpr const char* kindOp       = "op";
    inline constexpr const char* kindSnapshot = "snapshot";
    inline constexpr const char* kindBacklog  = "requestBacklog";
    // Ephemeral "where is everyone" traffic: cursors, selections, who's live. Deliberately NOT an
    // op — presence is high-frequency and worthless after the moment it describes, so the server
    // forwards it but never logs it and never replays it to someone joining later.
    inline constexpr const char* kindPresence = "presence";
    // Content-addressed audio transfer: "I need this file" / "here are its bytes". Not ops — bulk,
    // request/response, forwarded by the server but not logged into the project history.
    inline constexpr const char* kindAssetRequest = "assetReq";
    inline constexpr const char* kindAssetData    = "assetData";
    // Live voice: a short chunk of the speaker's mic audio. Ephemeral like presence — forwarded,
    // never logged, never replayed. Mono int16 PCM as base64.
    inline constexpr const char* kindVoice        = "voice";

    inline juce::String kindOf(const juce::var& message)
    {
        return message.getProperty("kind", juce::String()).toString();
    }

    inline juce::var opMessage(const Op& op)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kind", kindOp);
        obj->setProperty("op", op.toVar());
        return juce::var(obj);
    }

    inline juce::var snapshotMessage(const juce::var& project)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kind", kindSnapshot);
        obj->setProperty("project", project);
        return juce::var(obj);
    }

    inline juce::var backlogRequest()
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kind", kindBacklog);
        return juce::var(obj);
    }

    inline juce::var presenceMessage(const juce::var& presence)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kind", kindPresence);
        obj->setProperty("presence", presence);
        return juce::var(obj);
    }

    inline juce::var assetRequestMessage(const juce::String& hash)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kind", kindAssetRequest);
        obj->setProperty("hash", hash);
        return juce::var(obj);
    }

    inline juce::var assetDataMessage(const juce::String& hash, const juce::String& name, const juce::MemoryBlock& bytes)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kind", kindAssetData);
        obj->setProperty("hash", hash);
        obj->setProperty("name", name);
        obj->setProperty("bytes", bytes.toBase64Encoding());
        return juce::var(obj);
    }

    inline juce::var voiceMessage(const juce::String& actor, int sampleRate, const juce::MemoryBlock& pcm)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kind", kindVoice);
        obj->setProperty("actor", actor);
        obj->setProperty("rate", sampleRate);
        obj->setProperty("pcm", pcm.toBase64Encoding());
        return juce::var(obj);
    }

    inline juce::MemoryBlock encode(const juce::var& message)
    {
        const auto json = juce::JSON::toString(message, true);
        return juce::MemoryBlock(json.toRawUTF8(), json.getNumBytesAsUTF8());
    }

    inline juce::var decode(const juce::MemoryBlock& block)
    {
        return juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(block.getData()),
                                                        static_cast<int>(block.getSize())));
    }
} // namespace wire

inline juce::String toString(OpType t)
{
    switch (t)
    {
        case OpType::addTrack:         return "addTrack";
        case OpType::updateTrackProps: return "updateTrackProps";
        case OpType::removeTrack:      return "removeTrack";
        case OpType::moveTrack:        return "moveTrack";
        case OpType::setTrackField:    return "setTrackField";
        case OpType::addClip:          return "addClip";
        case OpType::replaceClip:      return "replaceClip";
        case OpType::removeClip:       return "removeClip";
        case OpType::moveClip:         return "moveClip";
        case OpType::resizeClip:       return "resizeClip";
        case OpType::setClipField:     return "setClipField";
        case OpType::addNote:          return "addNote";
        case OpType::removeNote:       return "removeNote";
        case OpType::editNote:         return "editNote";
        case OpType::addSlide:         return "addSlide";
        case OpType::removeSlide:      return "removeSlide";
        case OpType::editSlide:        return "editSlide";
        case OpType::replaceBuses:     return "replaceBuses";
        case OpType::replaceMasterInserts: return "replaceMasterInserts";
        case OpType::setTempo:         return "setTempo";
        case OpType::setTransport:     return "setTransport";
        case OpType::chatMessage:      return "chatMessage";
        case OpType::endCallForAll:    return "endCallForAll";
        case OpType::replaceClipNotes: return "replaceClipNotes";
        case OpType::unknown:          break;
    }
    return "unknown";
}
} // namespace orion::collab
