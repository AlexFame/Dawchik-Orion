#include "PluginManager.h"

#include <vector>

namespace orion
{
//==============================================================================
// Scan plugins in a helper process. Some plugins assert if scanned off the main
// thread, and others can hang while their bundle is inspected. Keeping each scan
// in a short-lived process prevents a bad VST3 from freezing or terminating Orion.
class PluginManager::ScanSession final : private juce::Timer
{
public:
    ScanSession(juce::AudioPluginFormatManager& fmRef,
                juce::File deadMansPedalFileIn,
                std::function<void(const juce::String&, double)> onProgressIn,
                std::function<void(std::unique_ptr<juce::XmlElement>)> onFinishedIn)
        : formatManager(fmRef),
          deadMansPedalFile(std::move(deadMansPedalFileIn)),
          onProgress(std::move(onProgressIn)),
          onFinished(std::move(onFinishedIn))
    {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        helperExecutable = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                               .getSiblingFile("OrionPluginScanner");
        collectFilesToScan();
    }

    ~ScanSession() override
    {
        stopTimer();
    }

    void start()
    {
        startTimer(50);
    }

private:
    struct PendingPlugin
    {
        juce::String formatName;
        juce::String fileName;
    };

    void timerCallback() override
    {
        if (currentProcess != nullptr)
        {
            if (currentProcess->isRunning())
            {
                if (juce::Time::getMillisecondCounterHiRes() - currentProcessStartMs > pluginScanTimeoutMs)
                {
                    currentProcess->kill();
                    currentProcess->waitForProcessToFinish(500);
                    finishCurrentProcess(false);
                }

                return;
            }

            finishCurrentProcess(true);
            return;
        }

        if (nextIndex >= pendingPlugins.size())
        {
            finish();
            return;
        }

        startNextProcess();
    }

    void collectFilesToScan()
    {
        juce::StringArray seenFiles;
        for (auto* format : formatManager.getFormats())
        {
            if (format == nullptr || ! format->canScanForPlugins())
                continue;

            if (! format->getName().containsIgnoreCase("VST3"))
                continue;

            auto files = format->searchPathsForPlugins(format->getDefaultLocationsToSearch(), true, false);
            for (const auto& file : files)
            {
                if (file.isEmpty() || seenFiles.contains(file))
                    continue;

                seenFiles.add(file);
                pendingPlugins.push_back(PendingPlugin { format->getName(), file });
            }
        }
    }

    void startNextProcess()
    {
        if (! helperExecutable.existsAsFile())
        {
            finish();
            return;
        }

        currentPlugin = pendingPlugins[static_cast<std::size_t>(nextIndex)];
        ++nextIndex;

        if (onProgress)
            onProgress(currentPlugin.fileName, getOverallProgress());

        currentProcess = std::make_unique<juce::ChildProcess>();
        const juce::StringArray args { helperExecutable.getFullPathName(),
                                       currentPlugin.formatName,
                                       currentPlugin.fileName };
        currentProcessStartMs = juce::Time::getMillisecondCounterHiRes();

        if (! currentProcess->start(args, juce::ChildProcess::wantStdOut))
            finishCurrentProcess(false);
    }

    void finishCurrentProcess(bool completed)
    {
        if (completed && currentProcess != nullptr)
        {
            const auto output = currentProcess->readAllProcessOutput();
            if (auto xml = juce::XmlDocument::parse(output))
            {
                juce::KnownPluginList childList;
                childList.recreateFromXml(*xml);
                for (const auto& type : childList.getTypes())
                    localList.addType(type);
            }
        }

        if (! completed && currentPlugin.fileName.isNotEmpty())
            localList.addToBlacklist(currentPlugin.fileName);

        currentProcess.reset();
        currentPlugin = {};

        if (onProgress)
            onProgress(nextIndex < pendingPlugins.size()
                           ? pendingPlugins[static_cast<std::size_t>(nextIndex)].fileName
                           : juce::String(),
                       getOverallProgress());
    }

    double getOverallProgress() const noexcept
    {
        if (pendingPlugins.empty())
            return 1.0;

        return juce::jlimit(0.0, 1.0, static_cast<double>(nextIndex) / static_cast<double>(pendingPlugins.size()));
    }

    void finish()
    {
        stopTimer();
        if (currentProcess != nullptr && currentProcess->isRunning())
            currentProcess->kill();
        currentProcess.reset();

        if (onProgress)
            onProgress({}, 1.0);

        auto finished = std::move(onFinished);
        auto xml = localList.createXml();
        juce::MessageManager::callAsync([cb = std::move(finished), payload = std::move(xml)]() mutable
        {
            if (cb)
                cb(std::move(payload));
        });
    }

    juce::AudioPluginFormatManager& formatManager;
    juce::File helperExecutable;
    juce::File deadMansPedalFile;
    juce::KnownPluginList localList;
    std::vector<PendingPlugin> pendingPlugins;
    std::unique_ptr<juce::ChildProcess> currentProcess;
    PendingPlugin currentPlugin;
    std::size_t nextIndex { 0 };
    double currentProcessStartMs { 0.0 };
    static constexpr double pluginScanTimeoutMs { 15000.0 };
    std::function<void(const juce::String&, double)> onProgress;
    std::function<void(std::unique_ptr<juce::XmlElement>)> onFinished;
};

//==============================================================================
PluginManager::PluginManager()
{
    juce::addDefaultFormatsToManager(formatManager);
    loadKnownPluginsFromSettings();
}

PluginManager::~PluginManager()
{
    scanSession.reset();
}

juce::File PluginManager::getOrionSettingsDir() const
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("Orion");
    dir.createDirectory();
    return dir;
}

juce::File PluginManager::getKnownPluginsFile() const
{
    return getOrionSettingsDir().getChildFile("PluginList.xml");
}

juce::File PluginManager::getDeadMansPedalFile() const
{
    return getOrionSettingsDir().getChildFile("PluginScanDeadMansPedal");
}

void PluginManager::loadKnownPluginsFromSettings()
{
    if (auto xml = juce::XmlDocument::parse(getKnownPluginsFile()))
        knownPlugins.recreateFromXml(*xml);
}

void PluginManager::saveKnownPluginsToSettings()
{
    if (auto xml = knownPlugins.createXml())
        xml->writeTo(getKnownPluginsFile());
}

void PluginManager::scanForPlugins(std::function<void(const juce::String&, double)> onProgress,
                                   std::function<void()> onFinished)
{
    if (scanning.exchange(true))
        return;  // already scanning

    // A crash during a previous background scan can leave a false-positive
    // "dead man's pedal" entry. Start clean now that scanning happens safely
    // on the message thread.
    getDeadMansPedalFile().deleteFile();

    scanSession = std::make_unique<ScanSession>(
        formatManager,
        getDeadMansPedalFile(),
        std::move(onProgress),
        [this, finishedCb = std::move(onFinished)](std::unique_ptr<juce::XmlElement> resultXml) mutable
        {
            if (resultXml != nullptr)
            {
                juce::KnownPluginList scanned;
                scanned.recreateFromXml(*resultXml);
                for (const auto& type : scanned.getTypes())
                    knownPlugins.addType(type);

                saveKnownPluginsToSettings();
            }

            scanning.store(false);
            scanSession.reset();
            if (finishedCb)
                finishedCb();
        });

    scanSession->start();
}

juce::Array<juce::PluginDescription> PluginManager::getAllDescriptions() const
{
    return knownPlugins.getTypes();
}

juce::Array<juce::PluginDescription> PluginManager::getInstrumentDescriptions() const
{
    juce::Array<juce::PluginDescription> result;
    for (const auto& type : knownPlugins.getTypes())
        if (type.isInstrument)
            result.add(type);
    return result;
}

std::optional<juce::PluginDescription> PluginManager::findDescription(const juce::String& identifier) const
{
    for (const auto& type : knownPlugins.getTypes())
        if (type.createIdentifierString() == identifier)
            return type;
    return std::nullopt;
}

std::unique_ptr<juce::AudioPluginInstance> PluginManager::createInstance(const juce::PluginDescription& description,
                                                                         double sampleRate,
                                                                         int blockSize,
                                                                         juce::String& errorMessage)
{
    return formatManager.createPluginInstance(description,
                                              sampleRate > 0.0 ? sampleRate : 44100.0,
                                              blockSize > 0 ? blockSize : 512,
                                              errorMessage);
}
}  // namespace orion
