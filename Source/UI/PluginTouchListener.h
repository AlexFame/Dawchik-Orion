#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

namespace orion
{
// Watches a hosted plugin's parameters and fires when the user GRABS one (a gesture begin) in the
// plugin's own GUI. The host uses this to auto-map the automation lane onto whatever knob/fader was
// just touched — the Ableton "touch a control and it's mapped" workflow, no menu digging.
//
// Only the gesture-begin edge is forwarded, never plain value changes: playback pushing automation
// into the plugin also emits value-changed callbacks, and reacting to those would remap the lane to
// itself on every block. A gesture begin only happens from real user interaction.
class PluginTouchListener : private juce::AudioProcessorParameter::Listener
{
public:
    explicit PluginTouchListener(std::function<void(int paramIndex)> onTouched)
        : callback(std::move(onTouched)) {}

    ~PluginTouchListener() override { detach(); }

    void attachTo(juce::AudioPluginInstance* inst)
    {
        detach();
        instance = inst;
        if (instance != nullptr)
            for (auto* p : instance->getParameters())
                p->addListener(this);
    }

    void detach()
    {
        if (instance != nullptr)
            for (auto* p : instance->getParameters())
                p->removeListener(this);
        instance = nullptr;
    }

private:
    void parameterValueChanged(int, float) override {}

    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override
    {
        if (! gestureIsStarting)
            return;
        // Gesture callbacks may arrive off the message thread; hop back before touching the UI.
        auto cb = callback;
        juce::MessageManager::callAsync([cb, parameterIndex]
        {
            if (cb)
                cb(parameterIndex);
        });
    }

    std::function<void(int paramIndex)> callback;
    juce::AudioPluginInstance* instance { nullptr };
};
} // namespace orion
