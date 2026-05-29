#include <juce_audio_processors/juce_audio_processors.h>

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc < 3)
        return 2;

    const juce::String formatName = juce::String::fromUTF8(argv[1]);
    const juce::String fileToScan = juce::String::fromUTF8(argv[2]);

    juce::AudioPluginFormatManager formatManager;
    juce::addDefaultFormatsToManager(formatManager);

    juce::AudioPluginFormat* targetFormat = nullptr;
    for (auto* format : formatManager.getFormats())
    {
        if (format != nullptr && format->getName() == formatName)
        {
            targetFormat = format;
            break;
        }
    }

    if (targetFormat == nullptr || ! targetFormat->fileMightContainThisPluginType(fileToScan))
        return 3;

    juce::KnownPluginList list;
    juce::OwnedArray<juce::PluginDescription> typesFound;
    list.scanAndAddFile(fileToScan, false, typesFound, *targetFormat);

    if (auto xml = list.createXml())
    {
        std::cout << xml->toString().toStdString();
        return 0;
    }

    return typesFound.isEmpty() ? 4 : 0;
}
