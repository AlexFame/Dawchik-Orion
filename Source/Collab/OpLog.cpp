#include "OpLog.h"

#include "../Core/ProjectState.h"

namespace orion::collab
{
namespace
{
    juce::var makePayload(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* obj = new juce::DynamicObject();
        for (const auto& f : fields)
            obj->setProperty(f.first, f.second);
        return juce::var(obj);
    }

    // EntityIds ride in the payload as decimal strings — juce::var's JSON integer would truncate a
    // 64-bit id. Matches Op::toVar's own id encoding.
    juce::var idToVar(EntityId id) { return juce::String(id); }
    EntityId  varToId(const juce::var& v) { return static_cast<EntityId>(v.toString().getLargeIntValue()); }

    double asNum(const juce::var& p, const char* key, double fallback)
    {
        return p.hasProperty(key) ? static_cast<double>(p.getProperty(key, fallback)) : fallback;
    }

    void applyTrackField(TrackState& t, const juce::String& field, const juce::var& v)
    {
        if (field == "name")             t.name = v.toString();
        else if (field == "muted")       t.muted = static_cast<bool>(v);
        else if (field == "solo")        t.solo = static_cast<bool>(v);
        else if (field == "recordArmed") t.recordArmed = static_cast<bool>(v);
        else if (field == "volumeDb")    t.volumeDb = static_cast<double>(v);
        else if (field == "trackGainDb") t.trackGainDb = static_cast<double>(v);
        else if (field == "pan")         t.pan = static_cast<double>(v);
        else if (field == "colour")      t.colour = juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(v)));
    }

    void applyClipField(TimelineClip& c, const juce::String& field, const juce::var& v)
    {
        if (field == "name")             c.name = v.toString();
        else if (field == "gainDb")      c.gainDb = static_cast<double>(v);
        else if (field == "muted")       c.muted = static_cast<bool>(v);
        else if (field == "solo")        c.solo = static_cast<bool>(v);
        else if (field == "warpEnabled") c.warpEnabled = static_cast<bool>(v);
        else if (field == "colour")      c.colour = juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(v)));
    }
} // namespace

namespace oplog
{
    TrackState* findTrack(ProjectState& state, EntityId trackId)
    {
        if (trackId == noEntity)
            return nullptr;
        for (auto& t : state.getTracks())
            if (t.id == trackId)
                return &t;
        return nullptr;
    }

    TimelineClip* findClip(ProjectState& state, EntityId clipId, TrackState** ownerOut)
    {
        if (clipId == noEntity)
            return nullptr;
        for (auto& t : state.getTracks())
            for (auto& c : t.clips)
                if (c.id == clipId)
                {
                    if (ownerOut != nullptr)
                        *ownerOut = &t;
                    return &c;
                }
        return nullptr;
    }

    MidiNote* findNote(ProjectState& state, EntityId clipId, EntityId noteId)
    {
        if (auto* c = findClip(state, clipId))
            for (auto& n : c->midiNotes)
                if (n.id == noteId)
                    return &n;
        return nullptr;
    }

    void ensureIds(ProjectState& state, EntityIdGenerator& gen)
    {
        const juce::ScopedLock sl(state.getAudioEditLock());
        for (auto& t : state.getTracks())
        {
            if (t.id == noEntity)
                t.id = gen.next();
            for (auto& c : t.clips)
            {
                if (c.id == noEntity)
                    c.id = gen.next();
                for (auto& n : c.midiNotes)
                    if (n.id == noEntity)
                        n.id = gen.next();
            }
        }
        for (auto& b : state.getBuses())
            if (b.id == noEntity)
                b.id = gen.next();
    }

    bool apply(ProjectState& state, const Op& op)
    {
        switch (op.type)
        {
            case OpType::setTempo:
                state.setTempoBpm(asNum(op.payload, "bpm", state.getTempoBpm()));
                return true;

            case OpType::addTrack:
            {
                const juce::ScopedLock sl(state.getAudioEditLock());
                TrackState t;
                t.id = op.track;
                t.name = op.payload.getProperty("name", "Track").toString();
                t.isMidiTrack = static_cast<bool>(op.payload.getProperty("isMidiTrack", false));
                state.getTracks().push_back(std::move(t));
                return true;
            }

            case OpType::removeTrack:
            {
                auto& tracks = state.getTracks();
                const juce::ScopedLock sl(state.getAudioEditLock());
                for (auto it = tracks.begin(); it != tracks.end(); ++it)
                    if (it->id == op.track)
                    {
                        tracks.erase(it);
                        return true;
                    }
                return false;
            }

            case OpType::moveTrack:
            {
                auto& tracks = state.getTracks();
                const auto target = static_cast<int>(asNum(op.payload, "newIndex", -1.0));
                if (target < 0 || target >= static_cast<int>(tracks.size()))
                    return false;

                const juce::ScopedLock sl(state.getAudioEditLock());
                for (std::size_t i = 0; i < tracks.size(); ++i)
                    if (tracks[i].id == op.track)
                    {
                        if (static_cast<int>(i) == target)
                            return true;
                        auto moved = std::move(tracks[i]);
                        tracks.erase(tracks.begin() + static_cast<long>(i));
                        tracks.insert(tracks.begin() + target, std::move(moved));
                        return true;
                    }
                return false;
            }

            case OpType::replaceClipNotes:
            {
                auto* c = findClip(state, op.clip);
                if (c == nullptr)
                    return false;

                std::vector<MidiNote> rebuilt;
                if (const auto* arr = op.payload.getProperty("notes", juce::var()).getArray())
                {
                    rebuilt.reserve(static_cast<std::size_t>(arr->size()));
                    for (const auto& v : *arr)
                    {
                        MidiNote n;
                        n.id = varToId(v.getProperty("id", "0"));
                        n.pitch = static_cast<int>(v.getProperty("pitch", 60));
                        n.startBeat = static_cast<double>(v.getProperty("startBeat", 0.0));
                        n.lengthInBeats = static_cast<double>(v.getProperty("lengthInBeats", 1.0));
                        n.velocity = static_cast<int>(v.getProperty("velocity", 100));
                        rebuilt.push_back(n);
                    }
                }

                const juce::ScopedLock sl(state.getAudioEditLock());
                c->midiNotes = std::move(rebuilt);
                return true;
            }

            case OpType::setTrackField:
            {
                auto* t = findTrack(state, op.track);
                if (t == nullptr)
                    return false;
                applyTrackField(*t, op.payload.getProperty("field", "").toString(), op.payload.getProperty("value", {}));
                return true;
            }

            case OpType::addClip:
            {
                auto* t = findTrack(state, op.track);
                if (t == nullptr)
                    return false;
                const juce::ScopedLock sl(state.getAudioEditLock());
                TimelineClip c;
                c.id = op.clip;
                c.name = op.payload.getProperty("name", "Clip").toString();
                c.startBeat = asNum(op.payload, "startBeat", 0.0);
                c.lengthInBeats = asNum(op.payload, "lengthInBeats", 4.0);
                t->clips.push_back(std::move(c));
                return true;
            }

            case OpType::removeClip:
            {
                TrackState* owner = nullptr;
                if (findClip(state, op.clip, &owner) == nullptr || owner == nullptr)
                    return false;
                const juce::ScopedLock sl(state.getAudioEditLock());
                auto& clips = owner->clips;
                for (auto it = clips.begin(); it != clips.end(); ++it)
                    if (it->id == op.clip)
                    {
                        clips.erase(it);
                        return true;
                    }
                return false;
            }

            case OpType::moveClip:
            {
                TrackState* owner = nullptr;
                auto* c = findClip(state, op.clip, &owner);
                if (c == nullptr)
                    return false;

                const auto newStart = asNum(op.payload, "startBeat", c->startBeat);
                const auto toTrack = varToId(op.payload.getProperty("toTrack", "0"));

                if (toTrack != noEntity && (owner == nullptr || owner->id != toTrack))
                {
                    auto* dest = findTrack(state, toTrack);
                    if (dest == nullptr)
                        return false;
                    const juce::ScopedLock sl(state.getAudioEditLock());
                    TimelineClip moved = *c;               // copy (keeps id + contents)
                    moved.startBeat = newStart;
                    auto& src = owner->clips;
                    for (auto it = src.begin(); it != src.end(); ++it)
                        if (it->id == op.clip) { src.erase(it); break; }
                    dest->clips.push_back(std::move(moved));
                    return true;
                }

                c->startBeat = newStart;   // in-place move (no reallocation)
                return true;
            }

            case OpType::resizeClip:
            {
                auto* c = findClip(state, op.clip);
                if (c == nullptr)
                    return false;
                c->lengthInBeats = asNum(op.payload, "lengthInBeats", c->lengthInBeats);
                return true;
            }

            case OpType::setClipField:
            {
                auto* c = findClip(state, op.clip);
                if (c == nullptr)
                    return false;
                applyClipField(*c, op.payload.getProperty("field", "").toString(), op.payload.getProperty("value", {}));
                return true;
            }

            case OpType::addNote:
            {
                auto* c = findClip(state, op.clip);
                if (c == nullptr)
                    return false;
                const juce::ScopedLock sl(state.getAudioEditLock());
                MidiNote n;
                n.pitch = static_cast<int>(op.payload.getProperty("pitch", 60));
                n.startBeat = asNum(op.payload, "startBeat", 0.0);
                n.lengthInBeats = asNum(op.payload, "lengthInBeats", 1.0);
                n.velocity = static_cast<int>(op.payload.getProperty("velocity", 100));
                n.id = op.note;
                c->midiNotes.push_back(n);
                return true;
            }

            case OpType::removeNote:
            {
                auto* c = findClip(state, op.clip);
                if (c == nullptr)
                    return false;
                const juce::ScopedLock sl(state.getAudioEditLock());
                auto& notes = c->midiNotes;
                for (auto it = notes.begin(); it != notes.end(); ++it)
                    if (it->id == op.note)
                    {
                        notes.erase(it);
                        return true;
                    }
                return false;
            }

            case OpType::editNote:
            {
                auto* n = findNote(state, op.clip, op.note);
                if (n == nullptr)
                    return false;
                n->pitch = static_cast<int>(op.payload.getProperty("pitch", n->pitch));
                n->startBeat = asNum(op.payload, "startBeat", n->startBeat);
                n->lengthInBeats = asNum(op.payload, "lengthInBeats", n->lengthInBeats);
                n->velocity = static_cast<int>(op.payload.getProperty("velocity", n->velocity));
                return true;
            }

            case OpType::addSlide:
            case OpType::removeSlide:
            case OpType::editSlide:
            case OpType::setTransport:
            case OpType::unknown:
            default:
                // Not yet wired (pitch slides / shared transport land in later phases).
                return false;
        }
    }
} // namespace oplog

namespace ops
{
    Op setTempo(double bpm)
    {
        Op op; op.type = OpType::setTempo;
        op.payload = makePayload({ { "bpm", bpm } });
        return op;
    }

    Op addTrack(EntityId newTrackId, const juce::String& name, bool isMidiTrack)
    {
        Op op; op.type = OpType::addTrack; op.track = newTrackId;
        op.payload = makePayload({ { "name", name }, { "isMidiTrack", isMidiTrack } });
        return op;
    }

    Op removeTrack(EntityId trackId)
    {
        Op op; op.type = OpType::removeTrack; op.track = trackId;
        return op;
    }

    Op moveTrack(EntityId trackId, int newIndex)
    {
        Op op; op.type = OpType::moveTrack; op.track = trackId;
        op.payload = makePayload({ { "newIndex", newIndex } });
        return op;
    }

    Op setTrackField(EntityId trackId, const juce::String& field, const juce::var& value)
    {
        Op op; op.type = OpType::setTrackField; op.track = trackId;
        op.payload = makePayload({ { "field", field }, { "value", value } });
        return op;
    }

    Op addClip(EntityId trackId, EntityId newClipId, const juce::String& name,
               double startBeat, double lengthInBeats)
    {
        Op op; op.type = OpType::addClip; op.track = trackId; op.clip = newClipId;
        op.payload = makePayload({ { "name", name }, { "startBeat", startBeat }, { "lengthInBeats", lengthInBeats } });
        return op;
    }

    Op removeClip(EntityId clipId)
    {
        Op op; op.type = OpType::removeClip; op.clip = clipId;
        return op;
    }

    Op moveClip(EntityId clipId, double startBeat, EntityId toTrack)
    {
        Op op; op.type = OpType::moveClip; op.clip = clipId;
        op.payload = makePayload({ { "startBeat", startBeat }, { "toTrack", idToVar(toTrack) } });
        return op;
    }

    Op resizeClip(EntityId clipId, double lengthInBeats)
    {
        Op op; op.type = OpType::resizeClip; op.clip = clipId;
        op.payload = makePayload({ { "lengthInBeats", lengthInBeats } });
        return op;
    }

    Op setClipField(EntityId clipId, const juce::String& field, const juce::var& value)
    {
        Op op; op.type = OpType::setClipField; op.clip = clipId;
        op.payload = makePayload({ { "field", field }, { "value", value } });
        return op;
    }

    Op addNote(EntityId clipId, EntityId newNoteId, int pitch, double startBeat,
               double lengthInBeats, int velocity)
    {
        Op op; op.type = OpType::addNote; op.clip = clipId; op.note = newNoteId;
        op.payload = makePayload({ { "pitch", pitch }, { "startBeat", startBeat },
                                   { "lengthInBeats", lengthInBeats }, { "velocity", velocity } });
        return op;
    }

    Op removeNote(EntityId clipId, EntityId noteId)
    {
        Op op; op.type = OpType::removeNote; op.clip = clipId; op.note = noteId;
        return op;
    }

    Op editNote(EntityId clipId, EntityId noteId, int pitch, double startBeat,
                double lengthInBeats, int velocity)
    {
        Op op; op.type = OpType::editNote; op.clip = clipId; op.note = noteId;
        op.payload = makePayload({ { "pitch", pitch }, { "startBeat", startBeat },
                                   { "lengthInBeats", lengthInBeats }, { "velocity", velocity } });
        return op;
    }

    Op replaceClipNotes(EntityId clipId, const std::vector<MidiNote>& notes)
    {
        Op op; op.type = OpType::replaceClipNotes; op.clip = clipId;

        juce::Array<juce::var> encoded;
        encoded.ensureStorageAllocated(static_cast<int>(notes.size()));
        for (const auto& n : notes)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("id", idToVar(n.id));
            o->setProperty("pitch", n.pitch);
            o->setProperty("startBeat", n.startBeat);
            o->setProperty("lengthInBeats", n.lengthInBeats);
            o->setProperty("velocity", n.velocity);
            encoded.add(juce::var(o));
        }

        op.payload = makePayload({ { "notes", juce::var(encoded) } });
        return op;
    }
} // namespace ops
} // namespace orion::collab
