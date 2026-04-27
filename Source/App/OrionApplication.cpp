#include "OrionApplication.h"

#include "MainWindow.h"

namespace orion
{
OrionApplication::~OrionApplication() = default;

const juce::String OrionApplication::getApplicationName()
{
    return "Orion";
}

const juce::String OrionApplication::getApplicationVersion()
{
    return "0.1.0";
}

bool OrionApplication::moreThanOneInstanceAllowed()
{
    return true;
}

void OrionApplication::initialise(const juce::String&)
{
    mainWindow = std::make_unique<MainWindow>(getApplicationName());
}

void OrionApplication::shutdown()
{
    mainWindow.reset();
}

void OrionApplication::systemRequestedQuit()
{
    quit();
}

void OrionApplication::anotherInstanceStarted(const juce::String&)
{
}
}  // namespace orion
