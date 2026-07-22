// Headless tests for the Collab op-log (Phase 1): two independent ProjectStates, wired through a
// loopback "server", must converge to identical state as ops flow between them — plus op JSON
// round-trip and id assignment. No network, no UI.
//   cmake --build <dir> --target OrionCollabTest && ./OrionCollabTest

#include "../Collab/CollabController.h"
#include "../Collab/CollabSession.h"
#include "../Collab/LoopbackTransport.h"
#include "../Collab/OpLog.h"
#include "../Core/ProjectState.h"

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
    CollabSession sessA(projA, transA, /*salt*/ 0x00A1);
    CollabSession sessB(projB, transB, /*salt*/ 0x00B2);

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

    std::cout << std::endl;
    if (failures == 0)
        std::cout << "all checks passed" << std::endl;
    else
        std::cout << failures << " check(s) FAILED" << std::endl;
    return failures == 0 ? 0 : 1;
}
