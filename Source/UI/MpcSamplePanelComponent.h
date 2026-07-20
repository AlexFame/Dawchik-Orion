#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace orion
{
// Skeuomorphic MPC Sample panel. The chrome (case, branding, screen bezel, knob/pad bodies)
// is a baked product render drawn as the background; only the live elements — pad-hit glow,
// screen content, control state — are painted on top, at coordinates measured off the render.
class MpcSamplePanelComponent final : public juce::Component,
                                      public juce::FileDragAndDropTarget,
                                      public juce::DragAndDropTarget,
                                      private juce::Timer
{
public:
    enum class Command
    {
        sampleMode,
        seqMode,
        padFx,
        knobFx,
        shift,
        padBank,
        chop,
        mute,
        loop,
        levels16,
        sampleSelect,
        tapTempo,
        rewind,
        stop,
        record,
        play,
        undo,
        redo,
        count
    };

    MpcSamplePanelComponent();

    std::function<void()> onClose;
    std::function<void(int, int)> onPadTriggered;
    std::function<void(Command)> onCommand;
    std::function<void(Command)> onCommandLearnRequested;
    // Fired when a sample is loaded onto a pad — the host mirrors it into the MPC kit track
    // so recorded MIDI plays the pad samples back through the engine.
    std::function<void(int padIndex, const juce::String& sourcePath)> onPadSampleAssigned;

    // Sample Mode (Stage 1): each pad can hold a sample. onPadPlay drives Orion's engine
    // (velocity>0 = note on, 0 = note off); the panel owns the sample data + LCD waveform.
    void loadSampleOntoPad(int padIndex, const juce::File& file);
    bool isPadLoaded(int padIndex) const noexcept;
    juce::String getPadSourcePath(int padIndex) const;
    int getSelectedPad() const noexcept { return selectedPad; }

    void setConnectionState(bool connected, const juce::String& deviceName = {});
    void setHardwareStatus(const juce::String& midiInput, const juce::String& midiOutput, const juce::String& lastMidi);
    void setPerformanceState(bool fullLevelEnabled, bool sixteenLevelsEnabled, bool chopEnabled, int bankIndex, int selectedPadIndex);
    void handlePadEvent(int padIndex, int velocity);
    void setPadActivity(int padIndex, int velocity);
    void handleMidiMessage(const juce::MidiMessage& message);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    // Sample drops onto a pad — from the OS (Finder) and from Orion's internal browser.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDragEnter(const SourceDetails& details) override;
    void itemDragMove(const SourceDetails& details) override;
    void itemDragExit(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;

private:
    bool hasAnyLoadedPad() const noexcept;
    void timerCallback() override;
    // Where the baked render is drawn (aspect-preserved), and the mapping helpers from the
    // render's own pixel space into on-screen coordinates.
    juce::Rectangle<float> imageArea;
    juce::Rectangle<float> mapNorm(float x, float y, float w, float h) const;
    void rebuildHotspots();
    bool isPadPoint(juce::Point<float>) const noexcept;
    bool isCommandPoint(juce::Point<float>) const noexcept;
    static std::size_t commandIndex(Command command) noexcept;
    static bool commandUsesLocalLatch(Command command) noexcept;
    int padIndexAt(juce::Point<float>) const noexcept;

    // A sample loaded onto a pad, plus a cached min/max waveform for the LCD.
    struct PadSample
    {
        juce::String sourcePath;
        juce::String name;
        std::vector<float> peaks;   // 2 floats (min,max) per horizontal bucket, mono
        bool loaded { false };
    };
    std::array<PadSample, 16> padSamples {};
    juce::AudioFormatManager audioFormatManager;
    static std::vector<float> buildPeaks(const juce::File&, juce::AudioFormatManager&, int buckets);
    void drawScreen(juce::Graphics&);   // live LCD overlay (waveform + name) for the selected pad

    juce::Image panelImage;
    std::array<juce::Rectangle<float>, 16> padBounds {};
    std::array<juce::Rectangle<float>, static_cast<std::size_t>(Command::count)> commandBounds {};
    std::array<bool, static_cast<std::size_t>(Command::count)> commandLatched {};
    juce::Rectangle<float> closeBounds;
    std::array<int, 16> padVelocity {};
    bool connected { false };
    juce::String deviceName;
    juce::String midiInputName;
    juce::String midiOutputName;
    juce::String lastMidiText { "MPC MIDI: waiting for pads" };
    bool fullLevel { false };
    bool sixteenLevels { false };
    bool chopMode { false };
    int activeChopSlice { -1 };
    int padBank { 0 };
    int selectedPad { 0 };
    int pressedPad { -1 };
    int dragHoverPad { -1 };   // pad highlighted while a sample is dragged over it
    bool draggingPanel { false };
    juce::ComponentDragger dragger;
    juce::ComponentBoundsConstrainer dragConstrainer;
};

// Human-readable name for a panel command (status text, MIDI-learn messages). Lives here
// beside the enum so every translation unit that handles commands can share one definition.
inline juce::String mpcCommandName(MpcSamplePanelComponent::Command command)
{
    using Command = MpcSamplePanelComponent::Command;
    switch (command)
    {
        case Command::sampleMode:   return "Sample";
        case Command::seqMode:      return "Seq";
        case Command::padFx:        return "Pad FX";
        case Command::knobFx:       return "Knob FX";
        case Command::shift:        return "Shift";
        case Command::padBank:      return "Pad Bank";
        case Command::chop:         return "Chop";
        case Command::mute:         return "Mute";
        case Command::loop:         return "Loop";
        case Command::levels16:     return "16 Levels";
        case Command::sampleSelect: return "Sample Select";
        case Command::tapTempo:     return "Tap Tempo";
        case Command::rewind:       return "Rewind";
        case Command::stop:         return "Stop";
        case Command::record:       return "Record";
        case Command::play:         return "Play";
        case Command::undo:         return "Undo";
        case Command::redo:         return "Redo";
        case Command::count:        break;
    }
    return "Command";
}
} // namespace orion
