#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace orion
{
// Owns plugin discovery (scanning) and instantiation for VST3 (and, on macOS,
// AudioUnit) plugins. Lives on the message thread; scanning happens on a
// background thread and reports back via MessageManager::callAsync.
class PluginManager final
{
public:
    PluginManager();
    ~PluginManager();

    // Kick off a background scan of all default plugin locations. The callbacks
    // are always invoked on the message thread. onProgress is called with the
    // name of each plugin file as it is examined; onFinished once at the end.
    void scanForPlugins(std::function<void(const juce::String&, double)> onProgress,
                        std::function<void()> onFinished);
    bool isScanning() const noexcept { return scanning.load(); }

    // Every known plugin (instruments + effects), sorted by manufacturer.
    juce::Array<juce::PluginDescription> getAllDescriptions() const;
    // Only plugins that report themselves as instruments (synths/samplers).
    juce::Array<juce::PluginDescription> getInstrumentDescriptions() const;

    // Look up a previously scanned plugin by its stable identifier string
    // (PluginDescription::createIdentifierString()). Used when restoring a
    // project that references a plugin by id.
    std::optional<juce::PluginDescription> findDescription(const juce::String& identifier) const;

    // Synchronously create a usable plugin instance. Must be called on the
    // message thread. Returns nullptr (and fills errorMessage) on failure.
    std::unique_ptr<juce::AudioPluginInstance> createInstance(const juce::PluginDescription& description,
                                                              double sampleRate,
                                                              int blockSize,
                                                              juce::String& errorMessage);

    void loadKnownPluginsFromSettings();
    void saveKnownPluginsToSettings();

private:
    class ScanSession;

    juce::File getOrionSettingsDir() const;
    juce::File getKnownPluginsFile() const;
    juce::File getDeadMansPedalFile() const;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;

    std::unique_ptr<ScanSession> scanSession;
    std::atomic<bool> scanning { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginManager)
};
}  // namespace orion
