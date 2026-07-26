#pragma once

#include <juce_core/juce_core.h>

// AssetStore — content-addressed cache of the audio files shared in a jam.
//
// Clips and tracks reference audio by file PATH, which is meaningless on another machine. The store
// keys each file by a hash of its BYTES instead, so the same sample is one entry no matter where it
// lived on each computer. A sender registers its local originals; a receiver writes incoming bytes
// into a cache folder and can then resolve the hash to a real local file to play. Nothing here
// touches the network — the transport moves the bytes, this just addresses and stores them.

namespace orion::collab
{
class AssetStore
{
public:
    // cacheDir is where received assets are written (created if missing). A per-session subfolder
    // keeps them tidy and lets them be cleared later.
    explicit AssetStore(juce::File cacheDirectory) : cacheDir(std::move(cacheDirectory))
    {
        cacheDir.createDirectory();
    }

    static juce::String hashOfBytes(const juce::MemoryBlock& bytes)
    {
        // FNV-1a 64-bit over the raw bytes + the length. Dependency-free (juce_core only), and for a
        // session's handful of samples a 64-bit content hash collides with vanishing probability.
        juce::uint64 h = 0xcbf29ce484222325ULL;
        const auto* p = static_cast<const juce::uint8*>(bytes.getData());
        for (size_t i = 0; i < bytes.getSize(); ++i)
        {
            h ^= p[i];
            h *= 0x100000001b3ULL;
        }
        return juce::String::toHexString(static_cast<juce::int64>(h)) + "-" + juce::String(bytes.getSize());
    }

    static juce::String hashOfFile(const juce::File& file)
    {
        juce::MemoryBlock mb;
        if (! file.existsAsFile() || ! file.loadFileAsData(mb))
            return {};
        return hashOfBytes(mb);
    }

    // Remember that a local file IS the given hash, so we can serve its bytes without copying it
    // into the cache. Original filename is kept so a received copy gets a sensible name.
    void registerLocal(const juce::String& hash, const juce::File& original)
    {
        if (hash.isNotEmpty() && original.existsAsFile())
            locals[hash] = original;
    }

    // The best local file for a hash: a registered original if we have one, else the cached copy.
    juce::File localFor(const juce::String& hash) const
    {
        if (auto it = locals.find(hash); it != locals.end() && it->second.existsAsFile())
            return it->second;
        const auto cached = cachePathFor(hash);
        return cached.existsAsFile() ? cached : juce::File();
    }

    bool has(const juce::String& hash) const { return localFor(hash).existsAsFile(); }

    // Load an asset's bytes to send it (from the original or the cache).
    bool loadBytes(const juce::String& hash, juce::MemoryBlock& out) const
    {
        const auto f = localFor(hash);
        return f.existsAsFile() && f.loadFileAsData(out);
    }

    // Write received bytes into the cache under their hash (verified). Returns the cached file, or
    // an invalid File if the bytes don't match the claimed hash.
    juce::File store(const juce::String& hash, const juce::MemoryBlock& bytes, const juce::String& originalName = {})
    {
        if (hashOfBytes(bytes) != hash)
            return {};   // integrity check: never trust a mismatched payload

        // Keep the original extension so format detection / display still work.
        const auto ext = juce::File(originalName).getFileExtension();
        auto dest = cacheDir.getChildFile(hash + ext);
        if (! dest.existsAsFile())
            dest.replaceWithData(bytes.getData(), bytes.getSize());
        return dest;
    }

private:
    juce::File cachePathFor(const juce::String& hash) const
    {
        // Any file whose name starts with the hash (extension unknown up front).
        for (const auto& f : cacheDir.findChildFiles(juce::File::findFiles, false, hash + "*"))
            return f;
        return cacheDir.getChildFile(hash);
    }

    juce::File cacheDir;
    std::map<juce::String, juce::File> locals;
};
} // namespace orion::collab
