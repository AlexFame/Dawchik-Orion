#pragma once

#include "../Core/ProjectState.h"

#include <functional>

// AssetRefs — the bridge between machine-specific file PATHS and machine-independent content HASHES
// for the audio a clip or track references.
//
// The rule: the wire and the reconciler's shadow always carry audio references as "asset://<hash>";
// the live ProjectState carries real local paths. encodeAssetRefs() turns a copy's paths into hash
// sentinels before it is serialised/broadcast; resolveAssetRefs() turns received sentinels back into
// a local path (fetching the file if we don't have it yet). Because both sides converge on the same
// hash for the same audio, their shadows match and nothing is needlessly re-broadcast.

namespace orion::collab
{
inline const juce::String assetPrefix { "asset://" };

inline bool isAssetRef(const juce::String& s) { return s.startsWith(assetPrefix); }
inline juce::String assetRefHash(const juce::String& s) { return isAssetRef(s) ? s.substring(assetPrefix.length()) : juce::String(); }
inline juce::String makeAssetRef(const juce::String& hash) { return hash.isEmpty() ? juce::String() : assetPrefix + hash; }

// pathToHash: local path -> content hash (registers the asset); returns "" if unhashable, in which
// case the path is left as-is (best effort). Already-encoded refs are passed through untouched.
using PathToHash = std::function<juce::String(const juce::String&)>;

// hashToPath: content hash -> a local path if we have the file (else "" and it is requested).
using HashToPath = std::function<juce::String(const juce::String&)>;

namespace detail
{
    inline void encodeField(juce::String& field, const PathToHash& toHash)
    {
        if (field.isEmpty() || isAssetRef(field))
            return;
        const auto hash = toHash(field);
        if (hash.isNotEmpty())
            field = makeAssetRef(hash);
    }

    inline void resolveField(juce::String& field, const HashToPath& toPath)
    {
        if (! isAssetRef(field))
            return;
        const auto local = toPath(assetRefHash(field));
        if (local.isNotEmpty())
            field = local;   // else leave the sentinel; it resolves once the file arrives
    }
}

inline void encodeAssetRefs(TimelineClip& clip, const PathToHash& toHash)
{
    detail::encodeField(clip.sourcePath, toHash);
}

inline void resolveAssetRefs(TimelineClip& clip, const HashToPath& toPath)
{
    detail::resolveField(clip.sourcePath, toPath);
}

inline void encodeAssetRefs(TrackState& track, const PathToHash& toHash)
{
    detail::encodeField(track.samplerSourcePath, toHash);
    detail::encodeField(track.mpcTuneSample, toHash);
    detail::encodeField(track.mpcChopSample, toHash);
    for (auto& pad : track.mpcKitSamples)
        detail::encodeField(pad, toHash);
}

inline void resolveAssetRefs(TrackState& track, const HashToPath& toPath)
{
    detail::resolveField(track.samplerSourcePath, toPath);
    detail::resolveField(track.mpcTuneSample, toPath);
    detail::resolveField(track.mpcChopSample, toPath);
    for (auto& pad : track.mpcKitSamples)
        detail::resolveField(pad, toPath);
}
} // namespace orion::collab
