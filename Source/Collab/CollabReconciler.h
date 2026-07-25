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
class CollabReconciler
{
public:
    CollabReconciler(ProjectState& liveProject, CollabController& controllerToUse);

    // shadow = live. Call after connecting and after every remotely-applied change.
    void captureBaseline();

    // Diff and broadcast. Returns how many ops were sent (0 when nothing changed).
    int sync();

private:
    // A clip is tracked by a hash of its ENTIRE serialised form rather than a list of fields.
    // TimelineClip has ~30 of them (warp markers, detected bars, key data, fades, sample bounds,
    // notes…) and any one left out of a hand-written diff desyncs silently — which is exactly how
    // warp state ended up differing between peers. Hashing the whole clip means a change to any
    // field, present or future, is caught automatically.
    struct ClipShadow
    {
        EntityId id { noEntity };
        EntityId owner { noEntity };      // owning track — flat list, so cross-track moves are visible
        juce::int64 dataHash { 0 };
    };

    // Like clips, a track is tracked by a hash of its whole serialised form (minus its clips, which
    // the clip diff owns). A TrackState has ~40 properties — VST instrument + state, sampler config,
    // MPC kit/tune/chop, inserts, sends — and hand-listing them is how instrument/sampler/MPC state
    // silently failed to sync. name/isMidiTrack are kept out of the hash: they seed addTrack.
    struct TrackShadow
    {
        EntityId id { noEntity };
        juce::String name;
        bool isMidiTrack { false };
        juce::int64 propsHash { 0 };
    };

    struct Shadow
    {
        double tempoBpm { 0.0 };
        std::vector<TrackShadow> tracks;   // in project order
        std::vector<ClipShadow> clips;     // flat across all tracks
        juce::int64 busesHash { 0 };       // aux buses + their inserts, as one blob
        juce::int64 masterInsertsHash { 0 };
    };

    void stampNewEntities();               // give ids to anything the DAW just created
    Shadow snapshotOfLive() const;
    int diffTracks(const Shadow& current);
    int diffClips(const Shadow& current);

    ProjectState& live;
    CollabController& controller;
    Shadow shadow;
    bool hasShadow { false };
};
} // namespace collab
} // namespace orion
