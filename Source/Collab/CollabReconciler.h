#pragma once

#include "CollabTypes.h"

#include <vector>

namespace orion
{
class ProjectState;

namespace collab
{
class CollabController;

// CollabReconciler — turns "the project changed somehow" into ops WITHOUT instrumenting the DAW's
// edit code.
//
// The app edits ProjectState exactly as it always has and knows nothing about collab. The
// reconciler keeps a shadow of the last-synced project; sync() stamps ids onto anything newly
// created, diffs the live project against the shadow BY STABLE ID, broadcasts one op per
// difference, and re-shadows. The app only has to call sync() periodically (a timer) — no edit path
// is touched, so single-user behaviour is unchanged.
//
// CRITICAL: after a remote op or snapshot is applied, captureBaseline() must be called. Otherwise
// the reconciler would see the peer's change as a local edit and broadcast it straight back —
// an echo storm.
//
// COLLABORATIVE UNDO lives here too. Because captureBaseline() folds in every peer change, each
// sync() diff contains ONLY the local user's own edits — so the ops it produces, and their
// inverses, target exactly this user's entities and never a peer's. Every diff that produces ops is
// recorded as an undo entry {forward, inverse}; undo()/redo() replay the inverse/forward ops (which
// are targeted by id, so they revert one user's change without disturbing anyone else's) and
// broadcast them like any edit. This is real multiplayer undo, not a project-snapshot restore
// (which would clobber concurrent peer edits).
class CollabReconciler
{
public:
    CollabReconciler(ProjectState& liveProject, CollabController& controllerToUse);

    // shadow = live. Call after connecting and after every remotely-applied change. Clears the
    // undo/redo history because the local baseline just moved (a peer op / snapshot / reconnect).
    void captureBaseline();

    // Diff and broadcast. Returns how many ops were sent (0 when nothing changed).
    int sync();

    // Re-shadow after a PEER change so it isn't diffed as ours — but KEEP the local undo history,
    // since the peer's edit doesn't invalidate our own entries (they still target our entities).
    // Use this after a remote op mid-session; use captureBaseline() for session start / resync.
    void foldRemoteChange();

    // ---- Collaborative undo/redo. Apply this user's inverse/forward ops and broadcast them. ----
    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }
    bool undo();
    bool redo();

private:
    // Each tracked entity keeps its full serialised form (JSON), not just a hash: change detection
    // is a string compare, and — crucially for undo — the stored "before" JSON is what an inverse op
    // restores the entity to.
    struct ClipShadow
    {
        EntityId id { noEntity };
        EntityId owner { noEntity };   // owning track — flat list, so cross-track moves are visible
        juce::String json;             // ProjectSerializer::clipToVar, serialised
    };

    struct TrackShadow
    {
        EntityId id { noEntity };
        juce::String name;
        bool isMidiTrack { false };
        juce::String propsJson;        // clip-less track (clips are owned by the clip diff)
    };

    struct Shadow
    {
        double tempoBpm { 0.0 };
        std::vector<TrackShadow> tracks;   // in project order
        std::vector<ClipShadow> clips;     // flat across all tracks
        juce::String busesJson;
        juce::String masterInsertsJson;
    };

    // One user gesture's worth of ops and the ops that reverse it.
    struct UndoEntry
    {
        std::vector<Op> forward;   // what was applied (for redo)
        std::vector<Op> inverse;   // what undoes it (for undo)
    };

    void stampNewEntities();               // give ids to anything the DAW just created
    Shadow snapshotOfLive() const;

    // Build the forward ops (what changed) and the inverse ops (how to undo it) between the current
    // shadow and `current`. Forward ops are broadcast; both are recorded for undo/redo.
    void diffTempo(const Shadow& current, std::vector<Op>& forward, std::vector<Op>& inverse) const;
    void diffMixer(const Shadow& current, std::vector<Op>& forward, std::vector<Op>& inverse) const;
    void diffTracks(const Shadow& current, std::vector<Op>& forward, std::vector<Op>& inverse) const;
    void diffClips(const Shadow& current, std::vector<Op>& forward, std::vector<Op>& inverse) const;

    // Apply a batch to `live` (under the audio lock), broadcast each, then re-shadow. Shared by
    // undo() and redo().
    void applyAndBroadcast(const std::vector<Op>& ops);

    ProjectState& live;
    CollabController& controller;
    Shadow shadow;
    bool hasShadow { false };

    std::vector<UndoEntry> undoStack;
    std::vector<UndoEntry> redoStack;
};
} // namespace collab
} // namespace orion
