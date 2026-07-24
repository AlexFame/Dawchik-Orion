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
    struct NoteShadow
    {
        EntityId id { noEntity };
        int pitch { 0 };
        double startBeat { 0.0 };
        double lengthInBeats { 0.0 };
        int velocity { 0 };
    };

    struct ClipShadow
    {
        EntityId id { noEntity };
        EntityId owner { noEntity };      // owning track — flat list, so cross-track moves are visible
        juce::String name;
        double startBeat { 0.0 };
        double lengthInBeats { 0.0 };
        double gainDb { 0.0 };
        bool muted { false };
        bool solo { false };
        bool warpEnabled { false };
        juce::uint32 colour { 0 };
        std::vector<NoteShadow> notes;
    };

    struct TrackShadow
    {
        EntityId id { noEntity };
        juce::String name;
        bool isMidiTrack { false };
        bool muted { false };
        bool solo { false };
        bool recordArmed { false };
        double volumeDb { 0.0 };
        double pan { 0.0 };
        double trackGainDb { 0.0 };
        juce::uint32 colour { 0 };
    };

    struct Shadow
    {
        double tempoBpm { 0.0 };
        std::vector<TrackShadow> tracks;   // in project order
        std::vector<ClipShadow> clips;     // flat across all tracks
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
