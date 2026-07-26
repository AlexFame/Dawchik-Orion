// Headless tests for the Collab op-log (Phase 1): two independent ProjectStates, wired through a
// loopback "server", must converge to identical state as ops flow between them — plus op JSON
// round-trip and id assignment. No network, no UI.
//   cmake --build <dir> --target OrionCollabTest && ./OrionCollabTest

#include "../Collab/CollabController.h"
#include "../Collab/CollabReconciler.h"
#include "../Collab/CollabServer.h"
#include "../Collab/CollabSession.h"
#include "../Collab/LoopbackTransport.h"
#include "../Collab/OpLog.h"
#include "../Collab/SocketTransport.h"
#include "../Core/ProjectState.h"

#include <cstring>
#include <iostream>

using namespace orion;
using namespace orion::collab;

namespace
{
int failures = 0;

void check(bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (! ok)
        ++failures;
}

// Compare the two projects on every collab-relevant field. Vectors are compared positionally:
// because ops are applied in one server order, converged states hold entities in the same order.
bool projectsEqual(const ProjectState& a, const ProjectState& b)
{
    if (std::abs(a.getTempoBpm() - b.getTempoBpm()) > 1.0e-9)
        return false;

    const auto& ta = a.getTracks();
    const auto& tb = b.getTracks();
    if (ta.size() != tb.size())
        return false;

    for (std::size_t i = 0; i < ta.size(); ++i)
    {
        if (ta[i].id != tb[i].id || ta[i].name != tb[i].name
            || ta[i].muted != tb[i].muted || ta[i].isMidiTrack != tb[i].isMidiTrack
            || ta[i].clips.size() != tb[i].clips.size())
            return false;

        for (std::size_t c = 0; c < ta[i].clips.size(); ++c)
        {
            const auto& ca = ta[i].clips[c];
            const auto& cb = tb[i].clips[c];
            if (ca.id != cb.id || ca.name != cb.name
                || std::abs(ca.startBeat - cb.startBeat) > 1.0e-9
                || std::abs(ca.lengthInBeats - cb.lengthInBeats) > 1.0e-9
                || ca.midiNotes.size() != cb.midiNotes.size())
                return false;

            for (std::size_t n = 0; n < ca.midiNotes.size(); ++n)
            {
                const auto& na = ca.midiNotes[n];
                const auto& nb = cb.midiNotes[n];
                if (na.id != nb.id || na.pitch != nb.pitch || na.velocity != nb.velocity
                    || std::abs(na.startBeat - nb.startBeat) > 1.0e-9
                    || std::abs(na.lengthInBeats - nb.lengthInBeats) > 1.0e-9)
                    return false;
            }
        }
    }
    return true;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // socket tests need a message manager to pump
    std::cout << "Orion collab tests" << std::endl;

    // ---- Op JSON round-trip survives a real serialise → parse → deserialise. ----
    {
        auto op = ops::addNote(/*clip*/ 123456789012345ULL, /*note*/ 987654321098765ULL, 64, 2.5, 0.75, 118);
        op.actor = "userA";
        op.seq = 42;
        const auto json = juce::JSON::toString(op.toVar());
        const auto back = Op::fromVar(juce::JSON::parse(json));
        check(back.type == OpType::addNote && back.clip == op.clip && back.note == op.note
                  && back.seq == 42 && back.actor == "userA"
                  && static_cast<int>(back.payload.getProperty("pitch", 0)) == 64
                  && static_cast<int>(back.payload.getProperty("velocity", 0)) == 118,
              "op survives JSON round-trip (incl. 64-bit ids)");
    }

    // ---- Two sessions over a loopback hub converge. ----
    ProjectState projA, projB;
    LoopbackHub hub;
    LoopbackTransport transA(hub, "A"), transB(hub, "B");
    const auto assetTmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("OrionCollabTest-assets");
    AssetStore assetsA(assetTmp.getChildFile("A")), assetsB(assetTmp.getChildFile("B"));
    CollabSession sessA(projA, transA, assetsA, /*salt*/ 0x00A1);
    CollabSession sessB(projB, transB, assetsB, /*salt*/ 0x00B2);

    // A builds an arrangement.
    const auto trackId = sessA.newId();
    sessA.submitLocal(ops::addTrack(trackId, "Drums", /*midi*/ true));
    sessA.submitLocal(ops::setTempo(140.0));

    const auto clipId = sessA.newId();
    sessA.submitLocal(ops::addClip(trackId, clipId, "Loop", /*start*/ 0.0, /*len*/ 8.0));

    const auto note1 = sessA.newId();
    const auto note2 = sessA.newId();
    sessA.submitLocal(ops::addNote(clipId, note1, 36, 0.0, 1.0, 100));
    sessA.submitLocal(ops::addNote(clipId, note2, 38, 1.0, 0.5, 90));

    check(projB.getTracks().size() == 1 && projB.getTracks()[0].id == trackId,
          "peer B received the track A created");
    check(std::abs(projB.getTempoBpm() - 140.0) < 1.0e-9, "peer B received the tempo change");
    check(projB.getTracks()[0].clips.size() == 1 && projB.getTracks()[0].clips[0].id == clipId,
          "peer B received the clip");
    check(projB.getTracks()[0].clips[0].midiNotes.size() == 2, "peer B received both notes");
    check(projectsEqual(projA, projB), "projects converged after A's edits");

    // ---- Edits from A: field change, note edit, move — all mirror on B. ----
    sessA.submitLocal(ops::setTrackField(trackId, "muted", true));
    sessA.submitLocal(ops::editNote(clipId, note2, 40, 1.0, 0.25, 70));
    sessA.submitLocal(ops::removeNote(clipId, note1));
    check(projB.getTracks()[0].muted == true, "track mute mirrored on B");
    check(projB.getTracks()[0].clips[0].midiNotes.size() == 1
              && projB.getTracks()[0].clips[0].midiNotes[0].pitch == 40,
          "note edit + delete mirrored on B");
    check(projectsEqual(projA, projB), "projects still converged after edits");

    // ---- Bidirectional: B creates a track, A receives it. ----
    const auto bTrack = sessB.newId();
    sessB.submitLocal(ops::addTrack(bTrack, "Bass", true));
    check(projA.getTracks().size() == 2 && projA.getTracks()[1].id == bTrack,
          "A received the track B created (bidirectional)");
    check(projectsEqual(projA, projB), "projects converged after B's edit");

    // ---- moveClip across tracks mirrors the reparent. ----
    sessA.submitLocal(ops::moveClip(clipId, /*start*/ 4.0, /*toTrack*/ bTrack));
    check(projB.getTracks()[0].clips.empty() && projB.getTracks()[1].clips.size() == 1
              && projB.getTracks()[1].clips[0].id == clipId
              && std::abs(projB.getTracks()[1].clips[0].startBeat - 4.0) < 1.0e-9,
          "clip moved to another track mirrored on B");
    check(projectsEqual(projA, projB), "projects converged after cross-track move");

    // ---- ids never collide across clients (different salts). ----
    check((sessA.newId() >> 48) != (sessB.newId() >> 48), "A and B mint ids in disjoint id-spaces");

    // ---- ensureIds gives id-less offline content stable ids. ----
    {
        ProjectState offline;
        TrackState t; t.name = "Old";
        TimelineClip c; c.name = "Old clip";
        c.midiNotes.push_back(MidiNote { 60, 0.0, 1.0, 100 });   // id defaults to 0
        t.clips.push_back(std::move(c));
        offline.getTracks().push_back(std::move(t));

        EntityIdGenerator gen(0x00C3);
        oplog::ensureIds(offline, gen);
        const auto& tk = offline.getTracks()[0];
        check(tk.id != 0 && tk.clips[0].id != 0 && tk.clips[0].midiNotes[0].id != 0
                  && tk.id != tk.clips[0].id && tk.clips[0].id != tk.clips[0].midiNotes[0].id,
              "ensureIds assigns unique non-zero ids to offline entities");
    }

    // ---- CollabController: the DAW-edits-then-broadcasts integration model. ----
    {
        ProjectState pa, pb;
        LoopbackHub chub;
        CollabController ctrlA(pa), ctrlB(pb);
        int bChangedCount = 0;
        ctrlB.onProjectChanged = [&bChangedCount] { ++bChangedCount; };
        ctrlA.connect(std::make_unique<LoopbackTransport>(chub, "A"), 0x0A1);
        ctrlB.connect(std::make_unique<LoopbackTransport>(chub, "B"), 0x0B2);

        check(ctrlA.isActive() && ctrlB.isActive(), "controllers are active after connect");

        // Simulate a local DAW edit on A: mint an id, mutate the project (oplog::apply stands in for
        // the DAW's own edit code), then broadcast the matching op send-only.
        const auto tId = ctrlA.newId();
        auto trackOp = ops::addTrack(tId, "Keys", true);
        oplog::apply(pa, trackOp);   // the "DAW edit"
        ctrlA.broadcast(trackOp);    // send-only

        check(pa.getTracks().size() == 1, "broadcast did NOT re-apply locally (no double-add)");
        check(pb.getTracks().size() == 1 && pb.getTracks()[0].id == tId,
              "peer B applied the broadcast track");
        check(bChangedCount == 1, "onProjectChanged fired once on B");

        // A second edit, and a clip under the new track.
        const auto cId = ctrlA.newId();
        auto clipOp = ops::addClip(tId, cId, "Riff", 0.0, 4.0);
        oplog::apply(pa, clipOp);
        ctrlA.broadcast(clipOp);
        check(pb.getTracks()[0].clips.size() == 1 && pb.getTracks()[0].clips[0].id == cId,
              "peer B applied the broadcast clip");
        check(bChangedCount == 2, "onProjectChanged fired again on B");

        // After disconnect the controller is inert — broadcast is a no-op and no id is minted.
        ctrlA.disconnect();
        check(! ctrlA.isActive() && ctrlA.newId() == noEntity, "disconnected controller is inert");
        auto lateOp = ops::setTempo(200.0);
        ctrlA.broadcast(lateOp);   // must do nothing
        check(std::abs(pb.getTempoBpm() - 200.0) > 1.0e-9, "broadcast after disconnect is a no-op");
    }

    // ---- Real sockets: two controllers over a CollabServer on localhost converge. ----
    {
        auto pump = [](int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); };

        const int port = 54000 + juce::Random::getSystemRandom().nextInt(800);
        CollabServer server;
        check(server.start(port), "collab server started on localhost");

        ProjectState pa, pb;
        CollabController ca(pa), cb(pb);
        auto ta = std::make_unique<SocketTransport>("SocketA");
        auto tb = std::make_unique<SocketTransport>("SocketB");
        const bool aConnected = ta->connectToServer("127.0.0.1", port);
        const bool bConnected = tb->connectToServer("127.0.0.1", port);
        check(aConnected && bConnected, "both clients connected to the server over TCP");
        ca.connect(std::move(ta), 0x5A);
        cb.connect(std::move(tb), 0x5B);
        pump(120);

        // Local DAW edit on A, broadcast across the wire.
        const auto tid = ca.newId();
        auto op = ops::addTrack(tid, "Net Drums", true);
        oplog::apply(pa, op);
        ca.broadcast(op);
        for (int i = 0; i < 40 && pb.getTracks().empty(); ++i) pump(25);

        check(pb.getTracks().size() == 1 && pb.getTracks()[0].id == tid,
              "op crossed a real socket: B received A's track");
        check(pa.getTracks().size() == 1, "sender did not double-apply its own socket echo");

        // The other direction.
        const auto tid2 = cb.newId();
        auto op2 = ops::addTrack(tid2, "Net Bass", true);
        oplog::apply(pb, op2);
        cb.broadcast(op2);
        for (int i = 0; i < 40 && pa.getTracks().size() < 2; ++i) pump(25);

        check(pa.getTracks().size() == 2 && pa.getTracks()[1].id == tid2,
              "bidirectional over socket: A received B's track");

        ca.disconnect();
        cb.disconnect();
        server.stop();
        pump(40);
    }

    // ---- replaceClipNotes + moveTrack over the loopback. ----
    {
        ProjectState pa, pb;
        LoopbackHub h2;
        CollabController ca(pa), cb(pb);
        ca.connect(std::make_unique<LoopbackTransport>(h2, "RA"), 0x0C1);
        cb.connect(std::make_unique<LoopbackTransport>(h2, "RB"), 0x0C2);

        // Two tracks + a clip, created on A.
        const auto t1 = ca.newId(), t2 = ca.newId(), cl = ca.newId();
        for (auto& op : { ops::addTrack(t1, "One", true), ops::addTrack(t2, "Two", true),
                          ops::addClip(t1, cl, "C", 0.0, 4.0) })
        {
            oplog::apply(pa, op);
            ca.broadcast(op);
        }
        check(pb.getTracks().size() == 2 && pb.getTracks()[0].clips.size() == 1,
              "two tracks + clip mirrored before note sync");

        // The piano roll edited the clip: broadcast the whole note vector in one op.
        auto* clipA = oplog::findClip(pa, cl);
        clipA->midiNotes.push_back(MidiNote { 60, 0.0, 1.0, 100, ca.newId() });
        clipA->midiNotes.push_back(MidiNote { 67, 1.0, 0.5, 88, ca.newId() });
        auto notesOp = ops::replaceClipNotes(cl, clipA->midiNotes);
        ca.broadcast(notesOp);

        const auto* clipB = oplog::findClip(pb, cl);
        check(clipB != nullptr && clipB->midiNotes.size() == 2
                  && clipB->midiNotes[0].pitch == 60 && clipB->midiNotes[1].pitch == 67
                  && clipB->midiNotes[1].id == clipA->midiNotes[1].id,
              "replaceClipNotes synced the whole clip in one op (ids preserved)");

        // Deleting a note is the same single op.
        clipA->midiNotes.pop_back();
        auto notesOp2 = ops::replaceClipNotes(cl, clipA->midiNotes);
        ca.broadcast(notesOp2);
        check(oplog::findClip(pb, cl)->midiNotes.size() == 1,
              "replaceClipNotes also propagates deletions");

        // Reorder tracks.
        auto mv = ops::moveTrack(t2, 0);
        oplog::apply(pa, mv);
        ca.broadcast(mv);
        check(pa.getTracks()[0].id == t2 && pb.getTracks()[0].id == t2,
              "moveTrack reordered on both sides");
    }

    // ---- hostSession / joinSession: the one-call bootstrap the DAW will use. ----
    {
        auto pump = [](int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); };
        const int port = 55000 + juce::Random::getSystemRandom().nextInt(800);

        ProjectState ph, pg;
        CollabController host(ph), guest(pg);
        check(host.hostSession(port, "Host", 0x7A), "hostSession started and connected");
        check(host.isHosting(), "controller reports it is hosting");
        check(guest.joinSession("127.0.0.1", port, "Guest", 0x7B), "joinSession connected to the host");
        pump(120);

        const auto tid = host.newId();
        auto op = ops::addTrack(tid, "Hosted", true);
        oplog::apply(ph, op);
        host.broadcast(op);
        for (int i = 0; i < 40 && pg.getTracks().empty(); ++i) pump(25);

        check(pg.getTracks().size() == 1 && pg.getTracks()[0].id == tid,
              "guest received the host's track through the hosted session");

        guest.disconnect();
        host.disconnect();
        pump(40);
        check(! host.isHosting(), "embedded server torn down on disconnect");
    }

    // ---- Shared transport: routed to a handler, never applied to ProjectState. ----
    {
        ProjectState pa, pb;
        LoopbackHub h4;
        CollabController ca(pa), cb(pb);

        bool bPlaying = false;
        double bBeat = -1.0;
        cb.onRemoteTransport = [&](bool playing, double beat) { bPlaying = playing; bBeat = beat; };

        ca.connect(std::make_unique<LoopbackTransport>(h4, "Ta"), 0x0E1);
        cb.connect(std::make_unique<LoopbackTransport>(h4, "Tb"), 0x0E2);

        ca.sendTransport(true, 12.5);
        check(bPlaying && std::abs(bBeat - 12.5) < 1.0e-9, "peer received play at the shared beat");
        check(pb.getTracks().empty(), "transport op did not touch ProjectState");

        ca.sendTransport(false, 20.0);
        check(! bPlaying && std::abs(bBeat - 20.0) < 1.0e-9, "peer received stop at position");

        // Chat rides the same routed channel.
        juce::String gotName, gotText;
        cb.onRemoteChat = [&](const juce::String& n, const juce::String& t) { gotName = n; gotText = t; };
        ca.sendChat("Alex", "yo drop the 808");
        check(gotName == "Alex" && gotText == "yo drop the 808", "peer received chat line");
        check(pb.getTracks().empty(), "chat op did not touch ProjectState");
    }

    // ---- Joining a session already in progress: the newcomer must get the WHOLE project. ----
    {
        auto pump = [](int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); };
        const int port = 56000 + juce::Random::getSystemRandom().nextInt(800);

        // The host already has an arrangement before anyone joins.
        ProjectState ph, pg;
        {
            TrackState t;
            t.name = "Pre-existing";
            TimelineClip c;
            c.name = "Old Loop";
            c.midiNotes.push_back(MidiNote { 72, 0.0, 2.0, 90 });
            t.clips.push_back(std::move(c));
            ph.getTracks().push_back(std::move(t));
        }
        ph.setTempoBpm(93.0);

        CollabController host(ph), guest(pg);
        check(host.hostSession(port, "H", 0x9A), "host started a session on an existing project");

        // An edit made after hosting but BEFORE the guest arrives — must be replayed to them.
        const auto midId = host.newId();
        auto midOp = ops::addTrack(midId, "Added Before Join", true);
        oplog::apply(ph, midOp);
        host.broadcast(midOp);
        pump(80);

        check(guest.joinSession("127.0.0.1", port, "G", 0x9B), "guest joined the running session");
        for (int i = 0; i < 60 && pg.getTracks().size() < 2; ++i) pump(25);

        check(pg.getTracks().size() == 2, "joiner got the baseline project AND the op made since");
        check(pg.getTracks()[0].name == "Pre-existing", "snapshot carried the pre-existing track");
        check(pg.getTracks()[0].clips.size() == 1
                  && pg.getTracks()[0].clips[0].midiNotes.size() == 1
                  && pg.getTracks()[0].clips[0].midiNotes[0].pitch == 72,
              "snapshot carried clips and notes");
        check(std::abs(pg.getTempoBpm() - 93.0) < 1.0e-9, "snapshot carried the tempo");
        check(pg.getTracks()[0].id == ph.getTracks()[0].id && pg.getTracks()[0].id != 0,
              "snapshot preserved entity ids (later ops can target them)");
        check(pg.getTracks()[1].id == midId, "the pre-join op was replayed in order");

        // And editing still works across the pair after the catch-up.
        const auto afterId = host.newId();
        auto afterOp = ops::addTrack(afterId, "After Join", true);
        oplog::apply(ph, afterOp);
        host.broadcast(afterOp);
        for (int i = 0; i < 40 && pg.getTracks().size() < 3; ++i) pump(25);
        check(pg.getTracks().size() == 3 && pg.getTracks()[2].id == afterId,
              "ops keep flowing after the initial catch-up");

        guest.disconnect();
        host.disconnect();
        pump(40);
    }

    // ---- Reconciler: the DAW edits normally (no collab awareness) and the diff produces ops. ----
    {
        ProjectState pa, pb;
        LoopbackHub h3;
        CollabController ca(pa), cb(pb);
        ca.connect(std::make_unique<LoopbackTransport>(h3, "RecA"), 0x0D1);
        cb.connect(std::make_unique<LoopbackTransport>(h3, "RecB"), 0x0D2);

        CollabReconciler rec(pa, ca);
        rec.captureBaseline();
        check(rec.sync() == 0, "reconciler stays silent when nothing changed");

        // The app creates a track exactly as it always has — no ids, no knowledge of collab.
        {
            TrackState t;
            t.name = "Reco";
            t.isMidiTrack = true;
            pa.getTracks().push_back(std::move(t));
        }
        pa.setTempoBpm(101.0);

        check(rec.sync() > 0, "reconciler emitted ops for the new track + tempo");
        check(pa.getTracks()[0].id != 0, "reconciler stamped an id on the DAW-created track");
        check(pb.getTracks().size() == 1 && pb.getTracks()[0].name == "Reco",
              "peer received a track the DAW created without touching collab code");
        check(std::abs(pb.getTempoBpm() - 101.0) < 1.0e-9, "peer received the tempo change");

        // A clip with notes, again created the plain way.
        {
            TimelineClip c;
            c.name = "Cl";
            c.startBeat = 2.0;
            c.lengthInBeats = 4.0;
            c.midiNotes.push_back(MidiNote { 64, 0.0, 1.0, 99 });
            pa.getTracks()[0].clips.push_back(std::move(c));
        }
        rec.sync();
        check(pb.getTracks()[0].clips.size() == 1
                  && pb.getTracks()[0].clips[0].midiNotes.size() == 1
                  && pb.getTracks()[0].clips[0].midiNotes[0].pitch == 64,
              "peer received the clip and its notes");

        // Mixed edits in one pass: mute a track, move a clip, retune a note.
        pa.getTracks()[0].muted = true;
        pa.getTracks()[0].clips[0].startBeat = 8.0;
        pa.getTracks()[0].clips[0].midiNotes[0].pitch = 70;
        rec.sync();
        check(pb.getTracks()[0].muted, "track mute synced");
        check(std::abs(pb.getTracks()[0].clips[0].startBeat - 8.0) < 1.0e-9, "clip move synced");
        check(pb.getTracks()[0].clips[0].midiNotes[0].pitch == 70, "note edit synced");

        // Audio-clip payload: the source it plays and its warp state must ride along, or the peer
        // gets a silent empty block (no waveform) and a differently-warped one at that.
        {
            auto& hostClip = pa.getTracks()[0].clips[0];
            hostClip.type = ClipType::audio;
            hostClip.sourcePath = "/tmp/loop.wav";
            hostClip.sourceBpm = 161.0;
            hostClip.sourceDurationSeconds = 7.5;
            hostClip.warpEnabled = true;
            hostClip.warpTargetLengthInBeats = 8.0;
            hostClip.warpMarkers.push_back({ 0.25, 2.0 });
            hostClip.sampleEndRatio = 0.75;
            rec.sync();

            const auto& peerClip = pb.getTracks()[0].clips[0];
            check(peerClip.sourcePath == "/tmp/loop.wav" && std::abs(peerClip.sourceBpm - 161.0) < 1.0e-9,
                  "clip audio source synced (waveform + playback)");
            check(peerClip.warpEnabled
                      && std::abs(peerClip.warpTargetLengthInBeats - 8.0) < 1.0e-9
                      && peerClip.warpMarkers.size() == 1
                      && std::abs(peerClip.warpMarkers[0].beat - 2.0) < 1.0e-9,
                  "clip warp state incl. markers synced");
            check(std::abs(peerClip.sampleEndRatio - 0.75) < 1.0e-9, "clip sample bounds synced");
        }

        // Track-level state beyond the basic fields — VST instrument, sampler, MPC kit — must sync,
        // or a collaborator hears a track with no instrument. Whole-track sync covers all of it.
        {
            auto& t = pa.getTracks()[0];
            t.instrumentPluginId = "Arturia:AnalogLab";
            t.instrumentPluginName = "Analog Lab";
            t.instrumentStateBase64 = "AAECAwQF";
            t.samplerSourcePath = "/tmp/kit.wav";
            t.isMpcKit = true;
            t.mpcKitSamples[0] = "/tmp/kick.wav";
            t.mpcKitSamples[3] = "/tmp/snare.wav";
            rec.sync();

            const auto& pt = pb.getTracks()[0];
            check(pt.instrumentPluginId == "Arturia:AnalogLab"
                      && pt.instrumentStateBase64 == "AAECAwQF",
                  "VST instrument + its saved state synced");
            check(pt.samplerSourcePath == "/tmp/kit.wav", "sampler source synced");
            check(pt.isMpcKit && pt.mpcKitSamples[0] == "/tmp/kick.wav"
                      && pt.mpcKitSamples[3] == "/tmp/snare.wav",
                  "MPC kit pad samples synced");

            // And this must NOT have disturbed the clip that lives on the same track.
            check(pb.getTracks()[0].clips.size() == 1, "track-props sync left the clip intact");
        }

        // Pitch slides (piano-roll glides) ride inside the clip, so whole-clip sync carries them
        // for free — including curve and LFO/vibrato on each point.
        {
            PitchSlide slide;
            slide.sourcePitch = 60;
            slide.sourceNoteStartBeat = 0.0;
            slide.points.push_back({ 0.0, 60.0, 0.5, 2, 1.5, 4.0 });
            slide.points.push_back({ 2.0, 67.0, -0.3, 0, 0.0, 3.0 });
            pa.getTracks()[0].clips[0].pitchSlides.push_back(slide);
            rec.sync();

            const auto& ps = pb.getTracks()[0].clips[0].pitchSlides;
            check(ps.size() == 1 && ps[0].points.size() == 2
                      && ps[0].points[0].lfoShape == 2
                      && std::abs(ps[0].points[0].curve - 0.5) < 1.0e-9
                      && std::abs(ps[0].points[1].pitch - 67.0) < 1.0e-9,
                  "pitch slide (glide) synced incl. curve + LFO");
        }

        pa.getTracks()[0].clips.clear();
        rec.sync();
        check(pb.getTracks()[0].clips.empty(), "clip deletion synced");
        check(rec.sync() == 0, "reconciler silent again once caught up");

        // Mixer: aux buses (with an insert) and the master chain sync live.
        {
            BusState bus;
            bus.name = "Reverb Bus";
            bus.volumeDb = -3.0;
            TrackState::InsertFx fx;
            fx.pluginId = "Valhalla:Room";
            fx.pluginName = "ValhallaRoom";
            fx.stateBase64 = "Zm9vYmFy";
            bus.inserts.push_back(fx);
            pa.getBuses().push_back(std::move(bus));

            TrackState::InsertFx masterFx;
            masterFx.pluginId = "FabFilter:ProL2";
            masterFx.pluginName = "Pro-L 2";
            pa.getMasterInserts().push_back(masterFx);

            rec.sync();

            check(pb.getBuses().size() == 1 && pb.getBuses()[0].name == "Reverb Bus"
                      && pb.getBuses()[0].inserts.size() == 1
                      && pb.getBuses()[0].inserts[0].pluginId == "Valhalla:Room"
                      && pb.getBuses()[0].inserts[0].stateBase64 == "Zm9vYmFy",
                  "aux bus + its insert synced");
            check(pb.getMasterInserts().size() == 1
                      && pb.getMasterInserts()[0].pluginId == "FabFilter:ProL2",
                  "master insert synced");
        }

        // Echo-storm guard: a change that arrived FROM the peer must not be broadcast back.
        const auto remoteId = cb.newId();
        auto remoteOp = ops::addTrack(remoteId, "FromPeer", true);
        oplog::apply(pb, remoteOp);
        cb.broadcast(remoteOp);
        check(pa.getTracks().size() == 2, "A applied the peer's track");
        rec.foldRemoteChange();   // exactly what MainComponent does after a remote op
        check(rec.sync() == 0, "a remotely-applied change is never echoed back");
    }

    // ---- Collaborative undo: my undo reverts only MY change; a concurrent peer edit survives. ----
    {
        ProjectState pa, pb;
        LoopbackHub h5;
        CollabController ca(pa), cb(pb);
        ca.connect(std::make_unique<LoopbackTransport>(h5, "UA"), 0x0F1);
        cb.connect(std::make_unique<LoopbackTransport>(h5, "UB"), 0x0F2);

        CollabReconciler rec(pa, ca);
        rec.captureBaseline();
        check(! rec.canUndo(), "no undo history at the start");

        // I add a track.
        { TrackState t; t.name = "Mine"; pa.getTracks().push_back(std::move(t)); }
        rec.sync();
        const auto myTrackId = pa.getTracks()[0].id;
        check(pb.getTracks().size() == 1 && rec.canUndo(), "my track synced and is undoable");

        // Meanwhile the peer adds their own track (arrives as a remote op on my side).
        const auto peerTrackId = cb.newId();
        auto peerOp = ops::addTrack(peerTrackId, "Theirs", true);
        oplog::apply(pb, peerOp);
        cb.broadcast(peerOp);
        rec.foldRemoteChange();   // fold the peer change in, keep my undo history
        check(pa.getTracks().size() == 2, "peer's track landed on my project");
        check(rec.canUndo(), "peer edit did not wipe my undo history");

        // I undo. Only MY track goes; the peer's stays — on both projects.
        check(rec.undo(), "undo ran");
        check(pa.getTracks().size() == 1 && pa.getTracks()[0].id == peerTrackId,
              "my undo removed only my track, kept the peer's");
        check(pb.getTracks().size() == 1 && pb.getTracks()[0].id == peerTrackId,
              "the undo propagated to the peer (their view matches)");

        // Redo brings my track back.
        check(rec.redo(), "redo ran");
        bool mineBack = false;
        for (const auto& t : pb.getTracks()) if (t.id == myTrackId) mineBack = true;
        check(pa.getTracks().size() == 2 && mineBack, "redo restored my track on both sides");

        // Undo of a whole clip-with-notes edit round-trips its content.
        rec.captureBaseline();
        {
            TimelineClip c; c.name = "Loop"; c.startBeat = 4.0;
            c.midiNotes.push_back(MidiNote { 64, 0.0, 1.0, 100 });
            // put it on my track
            for (auto& t : pa.getTracks()) if (t.id == myTrackId) t.clips.push_back(std::move(c));
        }
        rec.sync();
        EntityId addedClipId = 0;
        for (auto& t : pa.getTracks()) if (t.id == myTrackId && ! t.clips.empty()) addedClipId = t.clips[0].id;
        check(addedClipId != 0 && oplog::findClip(pb, addedClipId) != nullptr, "my new clip synced");
        check(rec.undo(), "undo the clip add");
        check(oplog::findClip(pa, addedClipId) == nullptr && oplog::findClip(pb, addedClipId) == nullptr,
              "undo removed the clip on both sides");
    }

    // ---- Asset transfer: a sample the peer lacks is fetched by content hash. ----
    {
        auto pump = [](int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); };
        const int port = 57000 + juce::Random::getSystemRandom().nextInt(800);

        // A writes a real file (the "sample") only it has.
        const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("OrionAssetTest");
        tmp.createDirectory();
        const auto sampleFile = tmp.getChildFile("kick_" + juce::String(port) + ".wav");
        juce::MemoryBlock payload;
        for (int i = 0; i < 4096; ++i) payload.append(&i, 1);
        sampleFile.replaceWithData(payload.getData(), payload.getSize());

        // Isolated, freshly-cleared cache dirs so the test doesn't see a prior run's cached copy.
        const auto cacheH = tmp.getChildFile("cacheHost");
        const auto cacheG = tmp.getChildFile("cacheGuest");
        cacheH.deleteRecursively();
        cacheG.deleteRecursively();

        ProjectState ph, pg;
        CollabController host(ph, cacheH), guest(pg, cacheG);
        check(host.hostSession(port, "AH", 0xB1), "asset host up");
        check(guest.joinSession("127.0.0.1", port, "AG", 0xB2), "asset guest joined");
        pump(120);

        // A registers its local sample -> content hash.
        const auto hash = host.registerAsset(sampleFile);
        check(hash.isNotEmpty() && host.hasAsset(hash), "host registered its sample by hash");
        check(! guest.hasAsset(hash), "guest does not have the sample yet");

        // Guest asks for it; A serves the bytes; guest caches them.
        juce::File delivered;
        guest.onAssetReady = [&](const juce::String& h, const juce::File& f) { if (h == hash) delivered = f; };
        guest.requestAsset(hash);
        for (int i = 0; i < 40 && ! guest.hasAsset(hash); ++i) pump(25);

        check(guest.hasAsset(hash), "guest received and cached the sample");
        check(delivered.existsAsFile(), "onAssetReady fired with the local cache file");
        juce::MemoryBlock got;
        check(delivered.loadFileAsData(got) && got.getSize() == payload.getSize()
                  && std::memcmp(got.getData(), payload.getData(), got.getSize()) == 0,
              "cached bytes match the original exactly");
        check(AssetStore::hashOfFile(delivered) == hash, "cached file re-hashes to the same content hash");

        guest.disconnect();
        host.disconnect();
        pump(30);
        sampleFile.deleteFile();
    }

    std::cout << std::endl;
    if (failures == 0)
        std::cout << "all checks passed" << std::endl;
    else
        std::cout << failures << " check(s) FAILED" << std::endl;
    return failures == 0 ? 0 : 1;
}
