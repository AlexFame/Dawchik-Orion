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
    // Global UI scale: Orion's controls/text are designed a touch large. Scaling the whole
    // interface down makes it read denser (Logic-like) without touching every layout value.
    // Tweak kUiScale (0.75–0.9) to taste — smaller = denser.
    constexpr float kUiScale = 0.8f;
    juce::Desktop::getInstance().setGlobalScaleFactor(kUiScale);

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
