#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace orion
{
struct ClipEditorState
{
    bool hasSelection { false };
    bool isAudioClip { false };
    juce::String title;
    juce::String trackName;
    juce::String fileName;
    juce::String sourcePath;
    juce::Colour accent { 0xffff5a4d };
    double startBeat { 0.0 };
    double lengthInBeats { 0.0 };
    double gainDb { 0.0 };
    double sourceBpm { 0.0 };
    int detectedBars { 0 };
    int transposeSemitones { 0 };
    double sampleStartRatio { 0.0 };
    double sampleEndRatio { 1.0 };
    double previewSourceRatio { 0.0 };
    double playheadBeat { 0.0 };
    bool playing { false };
    std::vector<float> waveformMin;
    std::vector<float> waveformMax;
    bool warpEnabled { false };
    bool keyShiftEnabled { false };
};

class ClipEditorComponent final : public juce::Component
{
public:
    ClipEditorComponent();

    std::function<void(bool)> onWarpChanged;
    std::function<void(bool)> onKeyShiftChanged;
    std::function<void(int)> onTransposeChanged;
    std::function<void(double)> onGainChanged;
    std::function<void(double, double)> onSampleRangeChanged;
    std::function<void(double)> onPreviewSeek;
    std::function<void()> onNormalize;

    void setState(const ClipEditorState& newState);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    enum class WaveDragMode
    {
        none,
        start,
        end,
        playhead
    };

    static juce::String formatBeat(double beat);
    static juce::String formatDb(double db);

    void drawWaveformPreview(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawInfoCard(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void drawControlCard(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void updateControlsFromState();
    double visibleWaveSpan() const noexcept;
    double clampWaveViewStart(double start) const noexcept;
    double xToWaveRatio(int x) const noexcept;
    int waveRatioToX(double ratio) const noexcept;
    void zoomWaveformAt(int x, double zoomFactor);
    void updateSampleMarker(WaveDragMode mode, int x, bool shouldSeek);
    void setTransposeSemitones(int semitones);
    void beginTrimmedClipDrag(const juce::MouseEvent& event);
    void setTransportButtonStyle(juce::Button& button);

    ClipEditorState state;
    juce::TextButton warpButton { "WARP" };
    juce::TextButton keyShiftButton { "KEY SHIFT" };
    juce::TextButton normalizeButton { "NORMALIZE" };
    juce::TextButton transposeDownButton { "-" };
    juce::TextButton transposeUpButton { "+" };
    juce::Slider gainSlider;
    juce::Rectangle<int> lastWaveformBounds;
    juce::Rectangle<int> pitchValueBounds;
    WaveDragMode waveDragMode { WaveDragMode::none };
    bool trimmedClipDragCandidate { false };
    bool trimmedClipDragStarted { false };
    juce::String lastSourcePath;
    double waveformZoom { 1.0 };
    double waveformViewStart { 0.0 };
};
}  // namespace orion
