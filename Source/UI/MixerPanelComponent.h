#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "../Core/ProjectState.h"

namespace orion
{
// A floating mixer overlay: one channel strip per track (name, M/S/R, insert &
// send slots, fader with dB scale + stereo meter, dB readout, pan knob, output
// routing) plus a master strip, and a bottom bar (link / view / width).
//
// The panel mutates TrackState fields directly (the audio engine reads them live)
// and reports edits through onTrackChanged so the timeline can repaint.
class MixerPanelComponent final : public juce::Component,
                                  private juce::Timer
{
public:
    explicit MixerPanelComponent(ProjectState& projectState);

    std::function<void()> onClose;
    std::function<void()> onTrackChanged;
    std::function<void(double)> onSetMasterGainDb;
    std::function<double()> onRequestMasterGainDb;
    std::function<float()>  onRequestMasterPeak;
    std::function<std::pair<float, float>()> onRequestMasterPeakStereo;
    // Unified readouts owned by the host (single 60 Hz measurement) so the master meter
    // matches the track meters exactly. 0..1 stereo bar level + held dB number.
    std::function<std::pair<float, float>()> onRequestMasterLevelStereo;
    std::function<float()> onRequestMasterLevelDb;
    std::function<float(int)> onRequestTrackLevel;
    std::function<std::pair<float, float>(int)> onRequestTrackLevelStereo;
    std::function<float(int)> onRequestTrackLevelDb;
    // Insert slot clicked: trackIndex + insert row index (< 0 = the "+" add slot).
    std::function<void(int, int)> onInsertClicked;
    // Insert dragged to a new place: (fromTrack, fromIndex, toTrack, toIndex).
    std::function<void(int, int, int, int)> onInsertMoved;
    // Send slot clicked on a track strip: (trackIndex, sendIndex; sendIndex < 0 = add).
    std::function<void(int, int)> onSendClicked;
    // Drag-set a send's level: (trackIndex, sendIndex, level 0..1).
    std::function<void(int, int, float)> onSendLevelChanged;
    // Stereo 0..1 level for an aux bus (mixer bus-strip meter).
    std::function<std::pair<float, float>(int)> onRequestBusLevelStereo;
    std::function<float(int)> onRequestBusLevelDb;
    std::function<void()> onAddBus;
    // Click a strip's name to select that track in the arrangement (tracks only).
    std::function<void(int)> onSelectTrack;
    // Click the OUT dropdown to choose a track's main output (Master / aux bus).
    std::function<void(int)> onOutputRouteClicked;

    void refreshStrips();   // rebuild strips from the project (tracks + buses)

    void open();
    void closePanel();

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    struct ChannelStrip;   // defined below; referenced by drawStrip()
    enum class MixerView { faders, meters, compact };

    // All sub-rectangles of one channel strip, so paint() and resized() stay aligned.
    struct StripLayout
    {
        juce::Rectangle<int> card;
        juce::Rectangle<int> nameRow;
        juce::Rectangle<int> levelReadout;
        juce::Rectangle<int> mute, solo, record;
        juce::Rectangle<int> insertsLabel, insertsPower, insertsSlot;
        juce::Rectangle<int> sendsLabel, sendsPower, sendsSlot;
        juce::Rectangle<int> peakLabel;
        juce::Rectangle<int> scaleArea;
        juce::Rectangle<int> fader;
        juce::Rectangle<int> meter;
        juce::Rectangle<int> dbBox;
        juce::Rectangle<int> panKnob;
        juce::Rectangle<int> panCaption;
        juce::Rectangle<int> outDropdown;
    };

    StripLayout computeStripLayout(juce::Rectangle<int> card) const;

    void timerCallback() override;
    void rebuildStrips();
    void syncControlsFromProject();
    void drawStereoMeter(juce::Graphics& g, juce::Rectangle<int> bounds, float levelL, float levelR) const;
    void drawStrip(juce::Graphics& g, const StripLayout& layout, ChannelStrip* strip, bool isMaster);
    juce::Rectangle<int> getPanelBounds() const;
    juce::Rectangle<int> getCloseButtonBounds() const;
    int currentStripWidth() const;

    struct ChannelStrip
    {
        std::unique_ptr<juce::Slider> volume;
        std::unique_ptr<juce::Slider> pan;
        std::unique_ptr<juce::TextButton> mute;
        std::unique_ptr<juce::TextButton> solo;
        std::unique_ptr<juce::TextButton> record;
        int trackIndex { -1 };   // for track strips
        bool isBus { false };
        int busIndex { -1 };     // for bus strips
        StripLayout layout;
        float meterDisplayL { 0.0f };
        float meterDisplayR { 0.0f };
        float levelDbDisplay { -100.0f };
        int insertScroll { 0 };  // vertical scroll offset (px) for the inserts list
        // Resolved insert-chain id: track index, or the bus key offset for bus strips.
        int insertId() const noexcept { return isBus ? (1000000 + busIndex) : trackIndex; }
    };

    ProjectState& project;
    // Declared first so it outlives every slider that uses it (members are destroyed
    // in reverse declaration order).
    std::unique_ptr<juce::LookAndFeel> mixerLnf;
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    juce::Slider masterVolume;
    juce::Slider masterPan;
    int builtTrackCount { -1 };
    int builtBusCount { -1 };

    StripLayout masterLayout;
    float masterMeterDisplayL { 0.0f };
    float masterMeterDisplayR { 0.0f };
    float masterLevelDb { -100.0f };
    float masterPeakRecentDb { -100.0f };
    int   masterPeakHoldFrames { 0 };

    MixerView view { MixerView::faders };
    bool maximized { false };   // Enter toggles full-screen mixer.

    // Insert drag-and-drop state.
    struct InsertDrag
    {
        bool armed { false };     // a chip was pressed; might become a drag
        bool dragging { false };
        int srcTrack { -1 };
        int srcIndex { -1 };
        juce::String label;
        juce::Point<int> pos;
    };
    InsertDrag insertDrag;

    // Drag-to-set a send level along its row (horizontal). Click (no drag) opens the menu.
    struct SendDrag
    {
        bool armed { false };
        bool dragging { false };
        int track { -1 };
        int row { -1 };
        float startLevel { 0.0f };
        int rowWidth { 1 };
    };
    SendDrag sendDrag;

    // Inline dB text entry (double-click a strip's dB box). target: -2 none, -1 master,
    // otherwise an index into `strips`.
    std::unique_ptr<juce::TextEditor> dbEditor;
    int dbEditTarget { -2 };
    void beginDbEdit(int target, juce::Rectangle<int> box);
    void commitDbEdit();

    static constexpr int kBusInsertKeyBase = 1000000;
    static constexpr int kMasterInsertKey  = 2000000;   // master strip insert-chain id
    int masterInsertScroll { 0 };

    // Maps a point to an insert slot. Returns true if inside one; row = chip index, or
    // -1 for the "+" add row. trackOut is the strip's track index.
    bool insertSlotAt(juce::Point<int> p, int& idOut, int& rowOut) const;
    bool sendSlotAt(juce::Point<int> p, int& trackOut, int& rowOut) const;
    const std::vector<TrackState::InsertFx>* insertChainForId(int id) const;
    juce::Rectangle<int> linkButtonBounds;
    juce::Rectangle<int> addBusButtonBounds;
    juce::Rectangle<int> viewFadersBounds, viewMetersBounds, viewCompactBounds;
    bool linkEnabled { false };
};
}  // namespace orion
