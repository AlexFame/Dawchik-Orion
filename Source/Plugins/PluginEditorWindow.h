#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace orion
{
// A desktop window that hosts a plugin's own editor GUI (or a generic
// parameter editor if the plugin has none). The plugin instance is owned
// elsewhere (the audio engine); this window only borrows it for display.
class PluginEditorWindow final : public juce::DocumentWindow
{
public:
    PluginEditorWindow(juce::AudioPluginInstance& instance,
                       const juce::String& windowTitle,
                       std::function<void()> onCloseRequested,
                       std::function<bool(const juce::KeyPress&)> onKeyPressedRequested = {},
                       std::function<bool(bool)> onKeyStateChangedRequested = {})
        : juce::DocumentWindow(windowTitle,
                               juce::Colours::black,
                               juce::DocumentWindow::closeButton),
          onClose(std::move(onCloseRequested)),
          onKeyPressed(std::move(onKeyPressedRequested)),
          onKeyStateChanged(std::move(onKeyStateChangedRequested))
    {
        setUsingNativeTitleBar(true);

        if (instance.hasEditor())
        {
            if (auto* editor = instance.createEditorIfNeeded())
            {
                setContentOwned(editor, true);
                setResizable(editor->isResizable(), false);
            }
        }

        if (getContentComponent() == nullptr)
        {
            setContentOwned(new juce::GenericAudioProcessorEditor(instance), true);
            setResizable(true, false);
        }

        centreWithSize(juce::jmax(320, getWidth()), juce::jmax(200, getHeight()));
        setVisible(true);
    }

    ~PluginEditorWindow() override
    {
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        return onKeyPressed != nullptr && onKeyPressed(key);
    }

    bool keyStateChanged(bool isKeyDown) override
    {
        return onKeyStateChanged != nullptr && onKeyStateChanged(isKeyDown);
    }

private:
    std::function<void()> onClose;
    std::function<bool(const juce::KeyPress&)> onKeyPressed;
    std::function<bool(bool)> onKeyStateChanged;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorWindow)
};
}  // namespace orion
