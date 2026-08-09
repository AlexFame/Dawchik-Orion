#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_video/juce_video.h>
#include "../Core/ChordTheory.h"
#include "../CameraChordWheel/HandPoseTracker.h"
#include <array>
#include <functional>
#include <optional>
#include <vector>

// A self-contained chord picker: Circle-of-Fifths wheel + quality + extensions + a live keyboard
// preview, seeded with the seven diatonic chords of the project key. Clicking a chord auditions it;
// dragging it out drops the chord onto the grid (wired by the host). An optional camera mode lets
// the user point at chords with a fingertip (Vision hand-pose).
class ChordSelectorComponent final : public juce::Component,
                                     private juce::CameraDevice::Listener
{
public:
    ChordSelectorComponent();

    void setProjectKey (int rootPc, const std::array<int, 7>& pattern, const juce::String& keyName);
    void setChord (const orion::chords::ChordSpec& newSpec, bool audition);
    orion::chords::ChordSpec getChord() const noexcept { return spec; }
    std::vector<int> currentPitches() const;

    std::function<void (const std::vector<int>&)> onAudition;                 // play the chord
    std::function<void (const orion::chords::ChordSpec&)> onChordChanged;     // spec changed
    std::function<void (const std::vector<int>&)> onDragChordOut;             // begin drag-to-grid
    std::function<void (juce::Rectangle<int>)> onRequestKeyMenu;             // clicking the key label
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    ~ChordSelectorComponent() override;

private:
    // ---- Camera (finger-pointing) mode ------------------------------------------------------
    void imageReceived (const juce::Image&) override;
    void toggleCamera();
    void startCamera();
    void stopCamera();
    void cameraOpened (juce::CameraDevice*, const juce::String& error);
    void detectionLoop (juce::Thread& self);            // background: fingertip → callAsync applyPointing
    void applyPointing (const std::optional<juce::Point<float>>& normTip);  // message thread only
    juce::Rectangle<int> cameraButtonBounds() const;

    struct FrameWorker final : juce::Thread
    {
        explicit FrameWorker (ChordSelectorComponent& o) : juce::Thread ("orion-chord-cam"), owner (o) {}
        void run() override { owner.detectionLoop (*this); }
        ChordSelectorComponent& owner;
    };

    // A transparent top layer that draws ONLY the fast-moving camera cursor + self-view, so the
    // expensive Circle-of-Fifths wheel isn't repainted every camera frame.
    struct CamOverlay final : juce::Component
    {
        explicit CamOverlay (ChordSelectorComponent& o) : owner (o) { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override { owner.paintCameraOverlay (g); }
        ChordSelectorComponent& owner;
    };
    void paintCameraOverlay (juce::Graphics&);
    void resized() override;
    std::unique_ptr<CamOverlay> camOverlay;

    juce::Rectangle<int> panelBounds() const;
    juce::Rectangle<int> headerBounds() const;
    juce::Rectangle<int> wheelBounds() const;
    juce::Rectangle<int> qualityRowBounds() const;
    juce::Rectangle<int> extensionGridBounds() const;
    juce::Rectangle<int> diatonicRowBounds() const;
    juce::Rectangle<int> keyboardBounds() const;
    juce::Rectangle<int> closeButtonBounds() const;
    juce::Rectangle<int> keyLabelBounds() const;   // clickable "Key: …" area in the header

    std::array<juce::Rectangle<int>, 8>  qualityRects() const;
    std::array<juce::Rectangle<int>, 12> extensionRects() const;
    std::array<juce::Rectangle<int>, 7>  diatonicRects() const;

    void commitChange (bool audition);

    // Circle of Fifths hit result: outer ring = major chords by fifths, inner ring = relative minors.
    struct WheelChord { int rootPc { -1 }; bool minor { false }; bool isValid() const { return rootPc >= 0; } };
    WheelChord wheelChordAtPoint (juce::Point<int> p) const;   // invalid if outside the rings
    static int outerRootAt (int pos) noexcept { return (7 * pos) % 12; }        // major, fifths order (C,G,D…)
    static int innerRootAt (int pos) noexcept { return (7 * pos + 9) % 12; }    // relative minor of the outer

    orion::chords::ChordSpec spec;
    int keyRootPc { 0 };
    std::array<int, 7> keyPattern { { 0, 2, 3, 5, 7, 8, 10 } };
    juce::String keyName { "C Minor" };

    int  hoverPc { -1 };         // hovered chord root, -1 = none
    bool hoverMinor { false };   // which ring the hover is on
    bool dragStarted { false };
    bool movingPanel { false };  // dragging the header moves the whole panel
    juce::ComponentDragger panelDragger;

    // Camera mode state.
    bool cameraOn { false };
    bool cameraOpening { false };
    std::unique_ptr<juce::CameraDevice> camera;
    std::unique_ptr<FrameWorker> worker;
    juce::CriticalSection frameLock;
    juce::Image latestFrame;
    int  pointHoverPc { -1 };        // chord under the fingertip, -1 = none
    bool pointHoverMinor { false };
    double lastPointAuditionMs { 0.0 };
    juce::Point<float> smoothTip { 0.5f, 0.5f };  // gained+smoothed fingertip → drives the wheel cursor
    juce::Point<float> previewTip { 0.5f, 0.5f };  // raw smoothed fingertip → the dot on the self-view
    bool  haveTip { false };
    int   missedFrames { 0 };        // brief hold-last when detection drops a frame
    juce::Point<float> cursorPos;    // on-wheel cursor in local coords
    bool  cursorValid { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordSelectorComponent)
};
