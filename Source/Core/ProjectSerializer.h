#pragma once

#include <juce_core/juce_core.h>

#include "ProjectState.h"

namespace orion
{
class ProjectSerializer final
{
public:
    static bool saveToFile(const ProjectState& projectState,
                           const juce::File& destinationFile,
                           juce::String* errorMessage = nullptr);

    // Replaces the contents of projectState with the project stored in sourceFile.
    // Returns false (and sets errorMessage) if the file is missing or unparseable;
    // projectState is left untouched on failure.
    static bool loadFromFile(ProjectState& projectState,
                             const juce::File& sourceFile,
                             juce::String* errorMessage = nullptr);

    // The same project representation as the file format, but in memory. saveToFile/loadFromFile are
    // thin wrappers over these, so anything using them (e.g. the collab snapshot sent to a peer
    // joining a live session) stays automatically in sync with the on-disk format.
    static juce::var toVar(const ProjectState& projectState);
    static bool fromVar(ProjectState& projectState,
                        const juce::var& source,
                        juce::String* errorMessage = nullptr);

    // A single clip in the same representation. The collab layer syncs whole clips with these
    // rather than enumerating fields: a TimelineClip carries ~30 of them (warp markers, detected
    // bars, key data, fades, sample bounds…), and any one left out silently desyncs.
    static juce::var clipToVar(const TimelineClip& clip);
    static TimelineClip clipFromVar(const juce::var& source);

    // A single track in the same representation. Like clips, the collab layer syncs a track's ~40
    // properties (VST instrument + its state, sampler config, MPC kit/tune/chop, inserts, sends,
    // folder info…) as one blob rather than enumerating fields, so none silently desyncs.
    static juce::var trackToVar(const TrackState& track);
    static TrackState trackFromVar(const juce::var& source);
};
}  // namespace orion
