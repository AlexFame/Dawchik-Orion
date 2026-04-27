#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

#include "MainWindow.h"

namespace orion
{
class OrionApplication final : public juce::JUCEApplication
{
public:
    ~OrionApplication() override;

    const juce::String getApplicationName() override;
    const juce::String getApplicationVersion() override;
    bool moreThanOneInstanceAllowed() override;

    void initialise(const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;
    void anotherInstanceStarted(const juce::String& commandLine) override;

private:
    std::unique_ptr<MainWindow> mainWindow;
};
}  // namespace orion
