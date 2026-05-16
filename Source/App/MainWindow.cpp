#include "MainWindow.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/MainComponent.h"

namespace orion
{
MainWindow::MainWindow(juce::String name)
    : juce::DocumentWindow(
          std::move(name),
          juce::Colour(0xff101417),
          juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setResizeLimits(1200, 760, 3840, 2160);
    setContentOwned(new MainComponent(), true);
    centreWithSize(1440, 900);
    setVisible(true);

    if (auto* mainComponent = dynamic_cast<MainComponent*>(getContentComponent()))
    {
        mainComponent->resetToPlaylistView();

        juce::Component::SafePointer<MainComponent> safeMainComponent(mainComponent);
        juce::Timer::callAfterDelay(150, [safeMainComponent]
        {
            if (safeMainComponent != nullptr)
                safeMainComponent->resetToPlaylistView();
        });
    }
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}
}  // namespace orion
