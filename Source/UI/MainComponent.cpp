#include "MainComponent.h"

#include <cmath>
#include <limits>
#include <map>
#include <vector>

#include "../Audio/PlaybackSources.h"
#include "../Audio/WarpEngine.h"
#include "../Sampler/SamplerEngine.h"

namespace
{
const auto backgroundColour = juce::Colour(0xff0b0f12);
const auto panelColour = juce::Colour(0xff131a20);
const auto accentColour = juce::Colour(0xffeb6f3a);
const auto panelStroke = juce::Colour(0xff25313c);
const auto mutedText = juce::Colours::white.withAlpha(0.64f);
const auto transportShelfColour = juce::Colour(0xff171d23);
const auto transportShelfStroke = juce::Colour(0xff2b3640);
const auto transportButtonColour = juce::Colour(0xfff0e8dc);
const auto transportButtonText = juce::Colour(0xff222222);
const auto transportDarkPanel = juce::Colour(0xff20252c);
const auto transportSectionFill = juce::Colour(0xff11171d);
const auto transportSectionStroke = juce::Colour(0xff303c47);
const auto recordAccent = juce::Colour(0xffd95050);
constexpr double previewMaxLengthSeconds = 12.0;
constexpr int minBrowserPanelWidth = 220;
constexpr int maxBrowserPanelWidth = 520;
constexpr int browserResizeHandleWidth = 10;
constexpr int transportBrandWidth = 92;
constexpr int transportClusterWidth = 314;
constexpr int transportTempoWidth = 178; // BPM + KEY combined card
constexpr int transportModeWidth = 200; // METRONOME (56) + 8 + LOOP (56) + 8 + COUNT IN (66) + slack
constexpr int transportUtilityWidth = 236;
constexpr int transportSectionGap = 12;
constexpr int transportControlHeight = 62;
constexpr int transportSectionHeight = 76;
constexpr int transportContentVerticalNudge = -3;
constexpr int samplerBottomPanelHeight = 320;

juce::String compactInspectorFileName(const juce::File& file, const juce::String& fallbackName)
{
    auto name = file.existsAsFile() ? file.getFileNameWithoutExtension() : fallbackName;
    if (name.length() <= 22)
        return name;

    return name.substring(0, 19) + "...";
}

class TransportButtonLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& buttonBackgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat();
        auto fill = buttonBackgroundColour;

        if (button.getToggleState())
            fill = fill.interpolatedWith(accentColour, 0.78f);
        else if (shouldDrawButtonAsDown)
            fill = fill.darker(0.18f);
        else if (shouldDrawButtonAsHighlighted)
            fill = fill.brighter(0.05f);

        g.setColour(juce::Colours::black.withAlpha(0.14f));
        g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f), 8.0f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 8.0f);
        g.setColour(button.getToggleState() ? accentColour.brighter(0.2f) : juce::Colours::black.withAlpha(0.16f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);
    }

    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool,
                        bool) override
    {
        const auto textColour = button.findColour(button.getToggleState()
            ? juce::TextButton::textColourOnId
            : juce::TextButton::textColourOffId);

        auto bounds = button.getLocalBounds().reduced(4, 4);
        g.setColour(textColour);
        auto iconBounds = bounds.removeFromTop(static_cast<int>(bounds.getHeight() * 0.56f)).reduced(8, 4);
        auto labelBounds = bounds.withTrimmedTop(2);
        drawTransportIcon(g, button.getComponentID(), iconBounds, textColour);

        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        g.drawText(button.getButtonText(), labelBounds, juce::Justification::centredTop);
    }

private:
    static void drawTransportIcon(juce::Graphics& g,
                                  const juce::String& role,
                                  juce::Rectangle<int> bounds,
                                  juce::Colour colour)
    {
        auto area = bounds.toFloat();
        g.setColour(colour);

        if (role == "play")
        {
            juce::Path triangle;
            triangle.addTriangle(area.getX() + area.getWidth() * 0.28f, area.getY() + area.getHeight() * 0.18f,
                                 area.getRight() - area.getWidth() * 0.22f, area.getCentreY(),
                                 area.getX() + area.getWidth() * 0.28f, area.getBottom() - area.getHeight() * 0.18f);
            g.fillPath(triangle);
        }
        else if (role == "stop")
        {
            g.fillRoundedRectangle(area.reduced(area.getWidth() * 0.28f, area.getHeight() * 0.2f), 2.0f);
        }
        else if (role == "record")
        {
            g.setColour(recordAccent);
            g.fillEllipse(area.reduced(area.getWidth() * 0.28f, area.getHeight() * 0.22f));
        }
        else if (role == "undo" || role == "redo")
        {
            juce::Path path;
            const auto left = area.getX() + area.getWidth() * 0.2f;
            const auto right = area.getRight() - area.getWidth() * 0.2f;
            const auto midY = area.getCentreY();
            if (role == "undo")
            {
                path.startNewSubPath(right, midY - area.getHeight() * 0.18f);
                path.quadraticTo(left + area.getWidth() * 0.24f, midY - area.getHeight() * 0.18f, left + area.getWidth() * 0.24f, midY);
                path.quadraticTo(left + area.getWidth() * 0.24f, midY + area.getHeight() * 0.18f, right - area.getWidth() * 0.08f, midY + area.getHeight() * 0.18f);
                g.strokePath(path, juce::PathStrokeType(2.2f));

                juce::Path arrow;
                arrow.addTriangle(left, midY, left + area.getWidth() * 0.22f, midY - area.getHeight() * 0.2f, left + area.getWidth() * 0.22f, midY + area.getHeight() * 0.2f);
                g.fillPath(arrow);
            }
            else
            {
                path.startNewSubPath(left, midY - area.getHeight() * 0.18f);
                path.quadraticTo(right - area.getWidth() * 0.24f, midY - area.getHeight() * 0.18f, right - area.getWidth() * 0.24f, midY);
                path.quadraticTo(right - area.getWidth() * 0.24f, midY + area.getHeight() * 0.18f, left + area.getWidth() * 0.08f, midY + area.getHeight() * 0.18f);
                g.strokePath(path, juce::PathStrokeType(2.2f));

                juce::Path arrow;
                arrow.addTriangle(right, midY, right - area.getWidth() * 0.22f, midY - area.getHeight() * 0.2f, right - area.getWidth() * 0.22f, midY + area.getHeight() * 0.2f);
                g.fillPath(arrow);
            }
        }
        else if (role == "metronome")
        {
            juce::Path metro;
            metro.startNewSubPath(area.getCentreX(), area.getY() + area.getHeight() * 0.12f);
            metro.lineTo(area.getX() + area.getWidth() * 0.3f, area.getBottom() - area.getHeight() * 0.12f);
            metro.lineTo(area.getRight() - area.getWidth() * 0.3f, area.getBottom() - area.getHeight() * 0.12f);
            metro.closeSubPath();
            g.strokePath(metro, juce::PathStrokeType(2.0f));
            g.drawLine(area.getCentreX(), area.getY() + area.getHeight() * 0.24f,
                       area.getCentreX() + area.getWidth() * 0.12f, area.getCentreY(), 2.0f);
        }
        else if (role == "loop")
        {
            juce::Path loop;
            loop.addCentredArc(area.getCentreX(), area.getCentreY(), area.getWidth() * 0.22f, area.getHeight() * 0.22f, 0.0f,
                               juce::MathConstants<float>::pi * 0.25f, juce::MathConstants<float>::pi * 1.7f, true);
            g.strokePath(loop, juce::PathStrokeType(2.2f));
            juce::Path arrow;
            arrow.addTriangle(area.getRight() - area.getWidth() * 0.26f, area.getCentreY() - area.getHeight() * 0.12f,
                              area.getRight() - area.getWidth() * 0.16f, area.getCentreY(),
                              area.getRight() - area.getWidth() * 0.28f, area.getCentreY() + area.getHeight() * 0.1f);
            g.fillPath(arrow);
        }
        else if (role == "countin")
        {
            const auto barWidth = area.getWidth() * 0.08f;
            const auto gap = area.getWidth() * 0.11f;
            for (int i = 0; i < 3; ++i)
            {
                const auto x = area.getCentreX() - gap + i * gap;
                g.fillRoundedRectangle(x, area.getY() + area.getHeight() * 0.2f, barWidth, area.getHeight() * (0.44f + 0.12f * i), 1.0f);
            }
        }
        else if (role == "browser")
        {
            // Ableton-style folder icon: a body with a tab on the top-left. Filled when
            // toggled on (browser open), outlined when off (browser collapsed).
            auto icon = area.reduced(area.getWidth() * 0.18f, area.getHeight() * 0.20f);
            const auto tabHeight  = icon.getHeight() * 0.22f;
            const auto tabWidth   = icon.getWidth()  * 0.40f;
            const auto tabSlope   = icon.getWidth()  * 0.08f;
            const auto corner     = 1.5f;

            juce::Path folder;
            // Tab (top-left)
            folder.startNewSubPath(icon.getX(), icon.getY() + tabHeight);
            folder.lineTo(icon.getX(), icon.getY() + corner);
            folder.quadraticTo(icon.getX(), icon.getY(), icon.getX() + corner, icon.getY());
            folder.lineTo(icon.getX() + tabWidth, icon.getY());
            folder.lineTo(icon.getX() + tabWidth + tabSlope, icon.getY() + tabHeight);
            // Top edge of the body
            folder.lineTo(icon.getRight() - corner, icon.getY() + tabHeight);
            folder.quadraticTo(icon.getRight(), icon.getY() + tabHeight,
                               icon.getRight(), icon.getY() + tabHeight + corner);
            // Right edge
            folder.lineTo(icon.getRight(), icon.getBottom() - corner);
            folder.quadraticTo(icon.getRight(), icon.getBottom(),
                               icon.getRight() - corner, icon.getBottom());
            // Bottom edge
            folder.lineTo(icon.getX() + corner, icon.getBottom());
            folder.quadraticTo(icon.getX(), icon.getBottom(),
                               icon.getX(), icon.getBottom() - corner);
            folder.closeSubPath();

            g.fillPath(folder);
        }
        else if (role == "save")
        {
            auto icon = area.reduced(area.getWidth() * 0.24f, area.getHeight() * 0.16f);
            g.drawRoundedRectangle(icon.reduced(0.5f), 2.0f, 1.8f);
            g.fillRect(icon.withHeight(static_cast<int>(icon.getHeight() * 0.26f)).reduced(2, 0));
            g.drawRect(icon.reduced(static_cast<int>(icon.getWidth() * 0.24f), static_cast<int>(icon.getHeight() * 0.38f)), 1);
        }
        else if (role == "export")
        {
            g.drawLine(area.getCentreX(), area.getBottom() - area.getHeight() * 0.18f, area.getCentreX(), area.getY() + area.getHeight() * 0.24f, 2.0f);
            juce::Path arrow;
            arrow.addTriangle(area.getCentreX(), area.getY() + area.getHeight() * 0.14f,
                              area.getCentreX() - area.getWidth() * 0.14f, area.getY() + area.getHeight() * 0.34f,
                              area.getCentreX() + area.getWidth() * 0.14f, area.getY() + area.getHeight() * 0.34f);
            g.fillPath(arrow);
            g.drawLine(area.getX() + area.getWidth() * 0.26f, area.getBottom() - area.getHeight() * 0.22f,
                       area.getRight() - area.getWidth() * 0.26f, area.getBottom() - area.getHeight() * 0.22f, 2.0f);
        }
        else if (role == "settings")
        {
            g.drawEllipse(area.reduced(area.getWidth() * 0.3f, area.getHeight() * 0.22f), 2.0f);
            g.fillEllipse(area.reduced(area.getWidth() * 0.42f, area.getHeight() * 0.34f));
        }
    }
};

TransportButtonLookAndFeel transportButtonLookAndFeel;
}  // namespace

namespace orion
{

class SettingsContent final : public juce::Component
{
public:
    SettingsContent(juce::AudioDeviceManager& manager,
                    int initialBrowserWidth,
                    int initialExportSampleRate,
                    std::function<void(int)> onBrowserWidthChanged,
                    std::function<void(int)> onExportSampleRateChanged,
                    std::function<void()> onSave)
        : audioSelector(manager, 0, 2, 0, 2, true, false, false, false),
          browserWidthChanged(std::move(onBrowserWidthChanged)),
          exportSampleRateChanged(std::move(onExportSampleRateChanged)),
          saveCallback(std::move(onSave))
    {
        titleLabel.setText("Settings", juce::dontSendNotification);
        titleLabel.setFont(juce::FontOptions(22.0f, juce::Font::bold));
        titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(titleLabel);

        browserWidthLabel.setText("Browser Width", juce::dontSendNotification);
        browserWidthLabel.setColour(juce::Label::textColourId, mutedText);
        addAndMakeVisible(browserWidthLabel);

        browserWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        browserWidthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 24);
        browserWidthSlider.setRange(minBrowserPanelWidth, maxBrowserPanelWidth, 1.0);
        browserWidthSlider.setValue(initialBrowserWidth, juce::dontSendNotification);
        browserWidthSlider.setColour(juce::Slider::trackColourId, accentColour);
        browserWidthSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
        browserWidthSlider.onValueChange = [this]
        {
            browserWidthChanged(static_cast<int>(std::round(browserWidthSlider.getValue())));
        };
        addAndMakeVisible(browserWidthSlider);

        exportSampleRateLabel.setText("Export Sample Rate", juce::dontSendNotification);
        exportSampleRateLabel.setColour(juce::Label::textColourId, mutedText);
        addAndMakeVisible(exportSampleRateLabel);

        exportSampleRateBox.addItem("44.1 kHz", 44100);
        exportSampleRateBox.addItem("48 kHz", 48000);
        exportSampleRateBox.addItem("96 kHz", 96000);
        exportSampleRateBox.setSelectedId(initialExportSampleRate, juce::dontSendNotification);
        exportSampleRateBox.onChange = [this]
        {
            const auto selected = exportSampleRateBox.getSelectedId();
            if (selected > 0)
                exportSampleRateChanged(selected);
        };
        addAndMakeVisible(exportSampleRateBox);

        audioLabel.setText("Audio Device", juce::dontSendNotification);
        audioLabel.setColour(juce::Label::textColourId, mutedText);
        addAndMakeVisible(audioLabel);

        addAndMakeVisible(audioSelector);

        saveButton.setButtonText("Save");
        saveButton.setColour(juce::TextButton::buttonColourId, accentColour);
        saveButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        saveButton.onClick = [this]
        {
            if (saveCallback)
                saveCallback();

            if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                dialog->setVisible(false);
        };
        addAndMakeVisible(saveButton);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(panelColour);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(18);
        titleLabel.setBounds(area.removeFromTop(28));
        area.removeFromTop(12);

        browserWidthLabel.setBounds(area.removeFromTop(20));
        browserWidthSlider.setBounds(area.removeFromTop(32));
        area.removeFromTop(10);

        exportSampleRateLabel.setBounds(area.removeFromTop(20));
        exportSampleRateBox.setBounds(area.removeFromTop(28).removeFromLeft(140));
        area.removeFromTop(14);

        audioLabel.setBounds(area.removeFromTop(20));
        area.removeFromTop(6);
        auto footerArea = area.removeFromBottom(48);
        audioSelector.setBounds(area);
        saveButton.setBounds(footerArea.removeFromRight(120).reduced(0, 6));
    }

private:
    juce::Label titleLabel;
    juce::Label browserWidthLabel;
    juce::Slider browserWidthSlider;
    juce::Label exportSampleRateLabel;
    juce::ComboBox exportSampleRateBox;
    juce::Label audioLabel;
    juce::AudioDeviceSelectorComponent audioSelector;
    juce::TextButton saveButton;
    std::function<void(int)> browserWidthChanged;
    std::function<void(int)> exportSampleRateChanged;
    std::function<void()> saveCallback;
};

MainComponent::MainComponent()
    : transportEngine(projectState),
      transportController(projectState, transportEngine),
      arrangementTimeline(projectState, transportEngine)
{
    setWantsKeyboardFocus(true);

    headerLabel.setText("ORION", juce::dontSendNotification);
    headerLabel.setFont(juce::FontOptions(27.0f, juce::Font::bold));
    headerLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.96f));
    addAndMakeVisible(headerLabel);

    bpmCaptionLabel.setText("TEMPO", juce::dontSendNotification);
    bpmCaptionLabel.setColour(juce::Label::textColourId, mutedText);
    bpmCaptionLabel.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    addAndMakeVisible(bpmCaptionLabel);

    bpmValueLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    bpmValueLabel.setFont(juce::FontOptions(26.0f, juce::Font::bold));
    bpmValueLabel.setJustificationType(juce::Justification::centred);
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmValueLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(bpmValueLabel);
    bpmValueLabel.setVisible(false);

    bpmEditor.setColour(juce::TextEditor::backgroundColourId, transportDarkPanel);
    bpmEditor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    bpmEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    bpmEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    bpmEditor.setColour(juce::TextEditor::highlightColourId, juce::Colours::white.withAlpha(0.18f));
    bpmEditor.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
    bpmEditor.setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
    bpmEditor.setJustification(juce::Justification::centred);
    bpmEditor.setFont(bpmValueLabel.getFont());
    bpmEditor.applyFontToAllText(bpmValueLabel.getFont());
    bpmEditor.setBorder(juce::BorderSize<int>(0));
    bpmEditor.setIndents(0, 0);
    bpmEditor.setInputRestrictions(6, "0123456789.");
    bpmEditor.setAlwaysOnTop(true);
    bpmEditor.onReturnKey = [this]() { endTempoEditing(true); };
    bpmEditor.onEscapeKey = [this]() { endTempoEditing(false); };
    bpmEditor.onFocusLost = [this]() { endTempoEditing(true); };
    // addChildComponent keeps the editor hidden by default so paint()'s g.drawText
    // renders the BPM number at startup. addAndMakeVisible would auto-show it empty.
    addChildComponent(bpmEditor);

    meterCaptionLabel.setText("BPM", juce::dontSendNotification);
    meterCaptionLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.68f));
    meterCaptionLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    addAndMakeVisible(meterCaptionLabel);

    meterValueLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.76f));
    meterValueLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    meterValueLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(meterValueLabel);

    statusLabel.setText("DAW", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.74f));
    statusLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    addAndMakeVisible(statusLabel);

    pluginScanNameLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.72f));
    pluginScanNameLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    pluginScanNameLabel.setJustificationType(juce::Justification::centred);
    pluginScanNameLabel.setInterceptsMouseClicks(false, false);
    pluginScanNameLabel.setVisible(false);
    addAndMakeVisible(pluginScanNameLabel);

    tempoLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(tempoLabel);

    meterLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(meterLabel);

    playheadLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    addAndMakeVisible(playheadLabel);

    playlistLabel.setText("Playlist: arrangement-first timeline for clips and loops", juce::dontSendNotification);
    playlistLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(playlistLabel);

    pianoRollLabel.setText("Piano Roll: double-click a MIDI clip to open full focus editor", juce::dontSendNotification);
    pianoRollLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    addAndMakeVisible(pianoRollLabel);

    clipInspectorEmptyLabel.setText("Select audio clip", juce::dontSendNotification);
    clipInspectorEmptyLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.58f));
    clipInspectorEmptyLabel.setJustificationType(juce::Justification::centred);
    clipInspectorEmptyLabel.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorEmptyLabel);

    clipInspectorTitleLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.96f));
    clipInspectorTitleLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    addAndMakeVisible(clipInspectorTitleLabel);

    clipInspectorTrackLabel.setColour(juce::Label::textColourId, mutedText);
    clipInspectorTrackLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorTrackLabel);

    clipInspectorFileLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.82f));
    clipInspectorFileLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipInspectorFileLabel);

    clipWarpLabel.setText("Warp", juce::dontSendNotification);
    clipWarpLabel.setColour(juce::Label::textColourId, mutedText);
    addAndMakeVisible(clipWarpLabel);

    clipWarpInfoLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
    clipWarpInfoLabel.setFont(juce::FontOptions(12.0f, juce::Font::plain));
    addAndMakeVisible(clipWarpInfoLabel);

    clipSourceBpmLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    clipSourceBpmLabel.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    addAndMakeVisible(clipSourceBpmLabel);

    clipBarsLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.78f));
    clipBarsLabel.setFont(juce::FontOptions(11.0f, juce::Font::plain));
    addAndMakeVisible(clipBarsLabel);

    clipWarpToggle.setButtonText("");
    clipWarpToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipWarpToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            if ((clip->sourceDurationSeconds <= 0.0 || (clip->sourceBpm <= 0.0 && clip->detectedBars == 0)) && clip->sourcePath.isNotEmpty())
            {
                const auto analysis = analyzeAudioWarpMetadata(juce::File(clip->sourcePath), projectState.getTempoBpm(), projectState.getNumerator());
                if (clip->sourceDurationSeconds <= 0.0)
                    clip->sourceDurationSeconds = analysis.durationSeconds;
                if (clip->sourceBpm <= 0.0 && analysis.sourceBpm > 0.0)
                {
                    clip->sourceBpm = analysis.sourceBpm;
                    clip->bpmGuessed = analysis.bpmGuessed;
                }
                if (clip->detectedBars == 0 && analysis.detectedBars > 0)
                    clip->detectedBars = analysis.detectedBars;
                if (clip->sourceKeyRoot < 0 && analysis.sourceKeyRoot >= 0)
                {
                    clip->sourceKeyRoot    = analysis.sourceKeyRoot;
                    clip->sourceKeyIsMinor = analysis.sourceKeyIsMinor;
                }
            }

            clip->warpEnabled = clipWarpToggle.getToggleState();
            refreshAudioClipWarpLengths();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipWarpToggle);

    clipGainLabel.setText("Gain", juce::dontSendNotification);
    clipGainLabel.setColour(juce::Label::textColourId, mutedText);
    addAndMakeVisible(clipGainLabel);

    clipGainValueLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipGainValueLabel.setJustificationType(juce::Justification::centredRight);
    clipGainValueLabel.setEditable(false, true, false);
    clipGainValueLabel.onTextChange = [this]() { applyGainFromInspectorText(); };
    addAndMakeVisible(clipGainValueLabel);

    clipGainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    clipGainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    clipGainSlider.setRange(-24.0, 12.0, 0.1);
    clipGainSlider.setColour(juce::Slider::trackColourId, accentColour);
    clipGainSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    clipGainSlider.onValueChange = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->gainDb = clipGainSlider.getValue();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipGainSlider);

    clipMuteToggle.setButtonText("M");
    clipMuteToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipMuteToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->muted = clipMuteToggle.getToggleState();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipMuteToggle);

    clipSoloToggle.setButtonText("S");
    clipSoloToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
    clipSoloToggle.onClick = [this]
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->solo = clipSoloToggle.getToggleState();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    addAndMakeVisible(clipSoloToggle);

    playButton.setButtonText("PLAY");
    stopButton.setButtonText("STOP");
    recordButton.setButtonText("REC");
    rewindButton.setButtonText("UNDO");
    undoButton.setButtonText("UNDO");
    redoButton.setButtonText("REDO");
    metronomeButton.setButtonText("METRONOME");
    loopButton.setButtonText("LOOP");
    countInButton.setButtonText("COUNT IN");
    browserButton.setButtonText("BROWSER");
    scanPluginsButton.setButtonText("Scan VST3");
    saveButton.setButtonText("SAVE");
    exportButton.setButtonText("EXPORT");
    settingsButton.setButtonText("SETTINGS");

    playButton.setComponentID("play");
    stopButton.setComponentID("stop");
    recordButton.setComponentID("record");
    recordButton.addMouseListener(this, false);
    rewindButton.setComponentID("undo");
    undoButton.setComponentID("undo");
    redoButton.setComponentID("redo");
    metronomeButton.setComponentID("metronome");
    loopButton.setComponentID("loop");
    countInButton.setComponentID("countin");
    browserButton.setComponentID("browser");
    saveButton.setComponentID("save");
    exportButton.setComponentID("export");
    settingsButton.setComponentID("settings");

    for (auto* button : { &playButton, &stopButton, &recordButton, &rewindButton, &undoButton, &redoButton,
                          &metronomeButton, &loopButton, &countInButton, &browserButton, &scanPluginsButton,
                          &saveButton, &exportButton, &settingsButton })
    {
        button->setLookAndFeel(&transportButtonLookAndFeel);
        button->setColour(juce::TextButton::buttonColourId, transportButtonColour);
        button->setColour(juce::TextButton::textColourOffId, transportButtonText);
        button->setColour(juce::TextButton::buttonOnColourId, accentColour);
        button->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        button->addListener(this);
        addAndMakeVisible(*button);
    }

    for (auto* toggleButton : { &metronomeButton, &loopButton, &countInButton, &browserButton })
        toggleButton->setClickingTogglesState(true);
    // Replaced by the small triangle arrow in the top-left corner.
    browserButton.setVisible(false);

    browserCollapseArrow.setToggleState(browserPanelVisible, juce::dontSendNotification);
    browserCollapseArrow.addListener(this);
    addAndMakeVisible(browserCollapseArrow);
    recordButton.setClickingTogglesState(true);
    metronomeButton.setToggleState(false, juce::dontSendNotification);
    loopButton.setToggleState(false, juce::dontSendNotification);
    countInButton.setToggleState(false, juce::dontSendNotification);

    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xfff7e5e5));
    recordButton.setColour(juce::TextButton::textColourOffId, recordAccent.darker(0.2f));
    rewindButton.setVisible(false);
    scanPluginsButton.setVisible(false);

    addAndMakeVisible(arrangementTimeline);
    addAndMakeVisible(browserPanel);
    addAndMakeVisible(midiEditorOverlay);
    addAndMakeVisible(samplerPanel);
    audioFormatManager.registerBasicFormats();
    arrangementPlaybackSource = std::make_unique<ArrangementPlaybackSource>(projectState, transportEngine, audioFormatManager);
    clickTrackSource = std::make_unique<ClickTrackSource>(projectState, transportEngine,
                                                          [this]() { return metronomeButton.getToggleState(); });
    audioDeviceManager.initialise(0, 2, nullptr, true);
    audioDeviceManager.addAudioCallback(&previewSourcePlayer);
    masterMixerSource.addInputSource(&previewTransportSource, false);
    masterMixerSource.addInputSource(arrangementPlaybackSource.get(), false);
    masterMixerSource.addInputSource(clickTrackSource.get(), false);
    previewSourcePlayer.setSource(&masterMixerSource);
    browserPanel.onPreviewItem = [this](const BrowserItem& item)
    {
        if (item.isDirectory)
        {
            statusLabel.setText("Folder: " + item.file.getFullPathName(), juce::dontSendNotification);
            return;
        }

        playBrowserPreview(item);
    };
    browserPanel.onActivateItem = [this](const BrowserItem& item)
    {
        loadBrowserItemIntoSampler(item);
    };
    browserPanel.onCloseRequested = [this]
    {
        // Wired to the × button inside the browser header — collapses the panel and
        // lets the playlist expand into the freed horizontal space.
        browserPanelVisible = false;
        browserPanel.setVisible(false);
        resized();
        repaint();
    };
    samplerPanel.onClose = [this]()
    {
        resized();
        arrangementTimeline.grabKeyboardFocus();
    };
    samplerPanel.onRequestProjectTempoBpm = [this]() { return projectState.getTempoBpm(); };
    samplerPanel.onRequestProjectKeyRoot    = [this]() { return projectState.getKeyRoot(); };
    samplerPanel.onRequestProjectKeyIsMinor = [this]() { return projectState.isKeyMinor(); };
    samplerPanel.onRequestScaleLockEnabled  = [this]() { return projectState.isScaleLockEnabled(); };
    samplerPanel.onResolveTrack = [this](int trackIndex) -> TrackState*
    {
        auto& tracks = projectState.getTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
            return nullptr;

        return &tracks[static_cast<std::size_t>(trackIndex)];
    };
    samplerPanel.onNoteOn = [this](const juce::String& sourcePath,
                                   int midiNote,
                                   int velocity,
                                   int rootMidiNote,
                                   double gainDb,
                                   SamplerPlaybackMode playbackMode,
                                   int sliceIndex,
                                   int sliceCount,
                                   bool warpEnabled,
                                   double sourceBpm)
    {
        if (arrangementPlaybackSource != nullptr)
        {
            const auto activeTrack = samplerPanel.getActiveTrackIndex();
            if (activeTrack >= 0 && arrangementPlaybackSource->hasTrackInstrument(activeTrack))
                arrangementPlaybackSource->instrumentLiveNoteOn(activeTrack, midiNote, velocity);
            else
                arrangementPlaybackSource->samplerNoteOn(sourcePath,
                                                         midiNote,
                                                         velocity,
                                                         rootMidiNote,
                                                         gainDb,
                                                         playbackMode,
                                                         sliceIndex,
                                                         sliceCount,
                                                         warpEnabled,
                                                         sourceBpm);
        }
        recordNoteOn(midiNote, velocity);
    };
    samplerPanel.onNoteOff = [this](int midiNote, SamplerPlaybackMode playbackMode)
    {
        if (arrangementPlaybackSource != nullptr)
        {
            const auto activeTrack = samplerPanel.getActiveTrackIndex();
            if (activeTrack >= 0 && arrangementPlaybackSource->hasTrackInstrument(activeTrack))
                arrangementPlaybackSource->instrumentLiveNoteOff(activeTrack, midiNote);
            else
                arrangementPlaybackSource->samplerNoteOff(midiNote, playbackMode);
        }
        recordNoteOff(midiNote);
    };
    samplerPanel.onAllNotesOff = [this]()
    {
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allSamplerNotesOff();
            arrangementPlaybackSource->allInstrumentNotesOff();
        }
    };
    midiEditorOverlay.onClose = [this]() { arrangementTimeline.grabKeyboardFocus(); };
    midiEditorOverlay.onTogglePlayback = [this]() { toggleTransportFromUi(); };
    midiEditorOverlay.onStopAndRewindToClipStart = [this]()
    {
        // Stop playback and set the playhead to the open clip's start beat.
        finalizeRecordingClip();
        if (const auto* clip = getSelectedTimelineClip())
        {
            if (arrangementPlaybackSource != nullptr)
            {
                arrangementPlaybackSource->allSamplerNotesOff();
                arrangementPlaybackSource->allInstrumentNotesOff();
            }
            transportController.stop(
                [this]() { stopBrowserPreview(true); },
                [this]() { if (arrangementPlaybackSource != nullptr) arrangementPlaybackSource->syncToTransportPosition(); });
            transportEngine.setPlayheadBeat(clip->startBeat);
            if (arrangementPlaybackSource != nullptr)
            {
                arrangementPlaybackSource->allSamplerNotesOff();
                arrangementPlaybackSource->allInstrumentNotesOff();
                arrangementPlaybackSource->syncToTransportPosition();
            }
            updateTransportLabels();
            arrangementTimeline.repaint();
        }
        else
        {
            stopTransportFromUi();
        }
    };
    midiEditorOverlay.onRequestPlayheadBeat = [this]() { return transportEngine.getPlayheadBeat(); };
    midiEditorOverlay.onRequestPlayingState = [this]() { return transportEngine.isPlaying(); };
    arrangementTimeline.onMidiClipDoubleClick = [this](int trackIndex, int clipIndex)
    {
        auto& track = projectState.getTracks()[static_cast<std::size_t>(trackIndex)];
        auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
        midiEditorOverlay.openClip(track, clip,
                                   projectState.getKeyRoot(),
                                   projectState.isKeyMinor(),
                                   projectState.isScaleLockEnabled());
    };
    midiEditorOverlay.onScaleLockChanged = [this](bool enabled)
    {
        projectState.setScaleLockEnabled(enabled);
    };
    arrangementTimeline.onClipSelectionChanged = [this](int trackIndex, int clipIndex)
    {
        if (trackIndex >= 0)
        {
            if (clipIndex >= 0)
                selectedArrangementClip = std::pair { trackIndex, clipIndex };
            else
                selectedArrangementClip.reset();

            const auto& tracks = projectState.getTracks();
            if (trackIndex < static_cast<int>(tracks.size()))
            {
                const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
                // FL-style sampler arm: any selection of a MIDI track with a sampler
                // OR a hosted VST instrument arms it for keyboard play, whether the
                // user clicked a clip OR just the track header.
                const bool hasSampler    = track.samplerSourcePath.isNotEmpty();
                const bool hasInstrument = track.instrumentPluginId.isNotEmpty();
                if (track.isMidiTrack && (hasSampler || hasInstrument))
                {
                    // Only pop the visible sampler panel for sampler tracks when a clip
                    // was clicked. Instrument-only tracks arm silently (no sampler UI).
                    if (clipIndex >= 0 && hasSampler)
                    {
                        samplerPanel.openTrackIndex(trackIndex);
                        resized();
                    }
                    else
                    {
                        // Header click — arm silently (don't pop the panel).
                        if (! samplerPanel.isArmed() || ! samplerPanel.isVisible())
                        {
                            // Need to bind the active track for keyboard polling without
                            // forcing the visible panel; openTrackIndex does the binding,
                            // then we hide the panel again if it wasn't open before.
                            const bool wasVisible = samplerPanel.isVisible();
                            samplerPanel.openTrackIndex(trackIndex);
                            if (! wasVisible)
                                samplerPanel.setVisible(false);
                        }
                        else
                        {
                            samplerPanel.openTrackIndex(trackIndex);
                        }
                    }
                }
                else if (samplerPanel.isArmed())
                {
                    // Switched to an audio (or non-sampled) track — disarm.
                    samplerPanel.disarmKeyboard();
                }

                if (clipIndex >= 0 && clipIndex < static_cast<int>(track.clips.size()))
                {
                    const auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
                    if (clip.type == ClipType::audio && clip.sourceKeyRoot >= 0)
                        DBG("[KeyDetect] clip='" + clip.name + "' sourceKey="
                            + formatKeyName(clip.sourceKeyRoot, clip.sourceKeyIsMinor)
                            + " projectKey="
                            + formatKeyName(projectState.getKeyRoot(), projectState.isKeyMinor()));
                }
            }
        }
        else
        {
            selectedArrangementClip.reset();
        }

        // Re-bake the warp cache so freshly-dropped clips pick up auto-detected pitch.
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();

        refreshClipInspector();
    };
    arrangementTimeline.onTrackHeaderDoubleClick = [this](int trackIndex)
    {
        // Double-clicking an instrument track opens its plugin editor; otherwise
        // fall back to the sampler panel for sampled tracks.
        if (arrangementPlaybackSource != nullptr && arrangementPlaybackSource->hasTrackInstrument(trackIndex))
            openInstrumentEditor(trackIndex);
        else
            openSamplerForTrackIfAvailable(trackIndex);
    };
    arrangementTimeline.onTrackHeaderRightClick = [this](int trackIndex)
    {
        showTrackInstrumentMenu(trackIndex);
    };

    resetToPlaylistView();
    setClipInspectorVisible(false);
    updateTransportLabels();
    grabKeyboardFocus();
    startTimerHz(60);
}

MainComponent::~MainComponent()
{
    for (auto* button : { &playButton, &stopButton, &recordButton, &rewindButton, &undoButton, &redoButton,
                          &metronomeButton, &loopButton, &countInButton, &browserButton, &scanPluginsButton,
                          &saveButton, &exportButton, &settingsButton })
    {
        button->setLookAndFeel(nullptr);
    }

    // Editor windows borrow plugin instances owned by the playback source — close
    // them before that source (and its instruments) is destroyed.
    closeAllInstrumentEditors();

    stopBrowserPreview(true);
    masterMixerSource.removeAllInputs();
    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();
    clickTrackSource.reset();
    arrangementPlaybackSource.reset();
    currentPreviewFile = juce::File();
    currentPreviewTempoBpm = 0.0;
    previewSourcePlayer.setSource(nullptr);
    audioDeviceManager.removeAudioCallback(&previewSourcePlayer);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto bounds = getLocalBounds();
    auto topStrip = bounds.removeFromTop(118);

    g.setColour(transportShelfColour);
    g.fillRect(topStrip);
    g.setColour(transportShelfStroke);
    g.drawLine(static_cast<float>(topStrip.getX()), static_cast<float>(topStrip.getBottom() - 1),
               static_cast<float>(topStrip.getRight()), static_cast<float>(topStrip.getBottom() - 1), 1.0f);

    auto transportVisual = topStrip.reduced(18, 14);
    const auto contentWidth = transportBrandWidth + transportClusterWidth + transportTempoWidth
        + transportModeWidth + transportUtilityWidth + transportSectionGap * 4;
    auto contentRow = transportVisual.withSizeKeepingCentre(juce::jmin(contentWidth, transportVisual.getWidth()), transportVisual.getHeight());
    contentRow.removeFromLeft(transportBrandWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto transportCluster = contentRow.removeFromLeft(transportClusterWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto bpmCard = contentRow.removeFromLeft(transportTempoWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto modeCluster = contentRow.removeFromLeft(transportModeWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto rightUtility = contentRow.removeFromLeft(transportUtilityWidth);

    const auto centeredTransportCluster = transportCluster.withSizeKeepingCentre(transportCluster.getWidth(), transportSectionHeight);
    const auto centeredBpmCard = bpmCard.withSizeKeepingCentre(bpmCard.getWidth(), transportSectionHeight);
    const auto centeredModeCluster = modeCluster.withSizeKeepingCentre(modeCluster.getWidth(), transportSectionHeight);
    const auto centeredRightUtility = rightUtility.withSizeKeepingCentre(rightUtility.getWidth(), transportSectionHeight);

    // KEY occupies the right half of the BPM card.
    cachedKeyCardBounds = centeredBpmCard.withTrimmedLeft(centeredBpmCard.getWidth() / 2);

    for (const auto& section : { centeredTransportCluster, centeredBpmCard, centeredModeCluster, centeredRightUtility })
    {
        const auto isTempoCard = section == centeredBpmCard;
        g.setColour(isTempoCard ? transportDarkPanel : transportSectionFill);
        g.fillRoundedRectangle(section.toFloat(), 12.0f);
        g.setColour(isTempoCard ? transportShelfStroke.brighter(0.25f) : transportSectionStroke);
        g.drawRoundedRectangle(section.toFloat(), 12.0f, 1.0f);
    }

    // Vertical divider between BPM and KEY halves of the combined card.
    {
        const auto dividerX = centeredBpmCard.getCentreX();
        g.setColour(juce::Colours::white.withAlpha(0.10f));
        g.drawLine(static_cast<float>(dividerX), static_cast<float>(centeredBpmCard.getY() + 10),
                   static_cast<float>(dividerX), static_cast<float>(centeredBpmCard.getBottom() - 10), 1.0f);
    }

    for (const auto x : { centeredTransportCluster.getRight() + (transportSectionGap / 2),
                          centeredBpmCard.getRight() + (transportSectionGap / 2),
                          centeredModeCluster.getRight() + (transportSectionGap / 2) })
    {
        g.setColour(juce::Colours::white.withAlpha(0.09f));
        g.drawLine(static_cast<float>(x), static_cast<float>(centeredTransportCluster.getY() + 6),
                   static_cast<float>(x), static_cast<float>(centeredTransportCluster.getBottom() - 6), 1.0f);
    }

    if (pluginScanVisible)
    {
        auto scanArea = getLocalBounds().reduced(26);
        scanArea = scanArea.withY(topStrip.getBottom() - 15).withHeight(11);
        const auto barArea = scanArea.removeFromBottom(4).toFloat();

        g.setColour(juce::Colours::black.withAlpha(0.22f));
        g.fillRoundedRectangle(barArea, 2.0f);
        g.setColour(accentColour.withAlpha(0.95f));
        g.fillRoundedRectangle(barArea.withWidth(barArea.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, pluginScanProgress))),
                               2.0f);
    }

    const auto bpmHalf = centeredBpmCard.withTrimmedRight(centeredBpmCard.getWidth() / 2);
    const auto keyHalf = cachedKeyCardBounds;

    if (! bpmEditor.isVisible())
    {
        auto bpmTextBounds = bpmHalf.withSizeKeepingCentre(bpmHalf.getWidth(), transportControlHeight)
                                .translated(0, transportContentVerticalNudge);
        bpmTextBounds = bpmTextBounds.removeFromTop(38);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        g.drawText(juce::String(projectState.getTempoBpm(), 2), bpmTextBounds, juce::Justification::centred);
    }

    // KEY half: large key name on top, "KEY" caption below.
    {
        auto keyValueBounds = keyHalf.withSizeKeepingCentre(keyHalf.getWidth(), transportControlHeight)
                                  .translated(0, transportContentVerticalNudge);
        auto keyCaptionBounds = keyValueBounds;
        keyValueBounds = keyValueBounds.removeFromTop(38);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(24.0f, juce::Font::bold));
        g.drawText(formatKeyName(projectState.getKeyRoot(), projectState.isKeyMinor()),
                   keyValueBounds, juce::Justification::centred);

        keyCaptionBounds.removeFromTop(40);
        g.setColour(mutedText);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText("KEY", keyCaptionBounds, juce::Justification::centredTop);
    }

    // Reclaim as much vertical space as possible for the playlist (Studio One-style).
    // Old layout reserved 20px above the cards, plus a 66px "Playlist" header inside,
    // plus 18px paddings — all gone now. The cards hug the work area edges tightly.
    auto workArea = bounds.withTrimmedTop(8);
    juce::Rectangle<int> browserPanelBounds;
    if (browserPanelVisible)
        browserPanelBounds = workArea.removeFromLeft(browserPanelWidth);
    auto arrangementPanel = workArea;

    g.setColour(panelColour);
    auto paintPanel = [&](juce::Rectangle<int> panel)
    {
        if (panel.isEmpty())
            return;
        g.setColour(panelColour);
        g.fillRoundedRectangle(panel.toFloat(), 14.0f);
        g.setColour(panelStroke);
        g.drawRoundedRectangle(panel.toFloat(), 14.0f, 1.0f);
    };
    if (browserPanelVisible)
        paintPanel(browserPanelBounds);
    paintPanel(arrangementPanel);

    if (browserPanelVisible)
    {
        const auto resizeHandleBounds = getBrowserResizeHandleBounds();
        g.setColour(juce::Colours::white.withAlpha(isResizingBrowserPanel ? 0.18f : 0.08f));
        g.fillRoundedRectangle(resizeHandleBounds.toFloat(), 4.0f);
        g.setColour(juce::Colours::white.withAlpha(isResizingBrowserPanel ? 0.42f : 0.18f));
        g.drawRoundedRectangle(resizeHandleBounds.toFloat(), 4.0f, 1.0f);
    }
}

void MainComponent::resized()
{
    // Reduced padding for a sleeker edge-to-edge floating layout
    auto bounds = getLocalBounds().reduced(8);
    auto topStrip = bounds.removeFromTop(118).reduced(18, 14);
    const auto contentWidth = transportBrandWidth + transportClusterWidth + transportTempoWidth + transportModeWidth
        + transportUtilityWidth + transportSectionGap * 4;
    auto contentRow = topStrip.withSizeKeepingCentre(juce::jmin(contentWidth, topStrip.getWidth()), topStrip.getHeight());
    auto leftBrand = contentRow.removeFromLeft(transportBrandWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto transportCluster = contentRow.removeFromLeft(transportClusterWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto bpmCard = contentRow.removeFromLeft(transportTempoWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto modeCluster = contentRow.removeFromLeft(transportModeWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto rightUtility = contentRow.removeFromLeft(transportUtilityWidth);

    auto leftBrandContent = leftBrand.withSizeKeepingCentre(leftBrand.getWidth(), 56);
    headerLabel.setBounds(leftBrandContent.removeFromTop(34));
    statusLabel.setBounds(leftBrandContent.removeFromTop(20));

    auto scanTopArea = getLocalBounds().reduced(8).removeFromTop(118).reduced(18, 0);
    auto scanLabelArea = scanTopArea.withY(scanTopArea.getBottom() - 27).withHeight(12);
    pluginScanNameLabel.setBounds(scanLabelArea);
    pluginScanNameLabel.setVisible(pluginScanVisible);

    auto centeredTransportCluster = transportCluster.withSizeKeepingCentre(transportCluster.getWidth(), transportSectionHeight);
    auto centeredBpmCard = bpmCard.withSizeKeepingCentre(bpmCard.getWidth(), transportSectionHeight);
    auto centeredModeCluster = modeCluster.withSizeKeepingCentre(modeCluster.getWidth(), transportSectionHeight);
    auto centeredRightUtility = rightUtility.withSizeKeepingCentre(rightUtility.getWidth(), transportSectionHeight);

    auto transportButtons = centeredTransportCluster.withSizeKeepingCentre(centeredTransportCluster.getWidth(), transportControlHeight)
                                .translated(0, transportContentVerticalNudge)
                                .reduced(8, 0);
    playButton.setBounds(transportButtons.removeFromLeft(54));
    transportButtons.removeFromLeft(8);
    stopButton.setBounds(transportButtons.removeFromLeft(54));
    transportButtons.removeFromLeft(8);
    recordButton.setBounds(transportButtons.removeFromLeft(54));
    transportButtons.removeFromLeft(8);
    undoButton.setBounds(transportButtons.removeFromLeft(54));
    transportButtons.removeFromLeft(8);
    redoButton.setBounds(transportButtons.removeFromLeft(54));

    // Combined BPM + KEY card: BPM occupies the left half, KEY the right half.
    auto bpmHalf = centeredBpmCard.withTrimmedRight(centeredBpmCard.getWidth() / 2);
    auto keyHalf = centeredBpmCard.withTrimmedLeft(centeredBpmCard.getWidth() / 2);

    auto bpmBounds = bpmHalf.withSizeKeepingCentre(bpmHalf.getWidth(), transportControlHeight)
                         .translated(0, transportContentVerticalNudge);
    auto bpmTop = bpmBounds.removeFromTop(38);
    bpmValueLabel.setBounds(bpmTop);
    const auto editorBoxHeight = static_cast<int>(std::ceil(bpmValueLabel.getFont().getHeight()));
    bpmEditor.setBounds(bpmTop.withSizeKeepingCentre(bpmTop.getWidth(), editorBoxHeight));
    if (bpmEditor.isVisible())
        bpmEditor.toFront(false);
    else
        bpmValueLabel.setVisible(false);

    bpmCaptionLabel.setBounds(bpmBounds); // "TEMPO" caption fills the bottom of the BPM half.

    // KEY half: value and "KEY" caption are drawn directly in paint().
    // Hide the legacy meter labels so they don't overlap our custom drawing.
    meterValueLabel.setBounds({});
    meterCaptionLabel.setBounds({});

    auto modeButtons = centeredModeCluster.withSizeKeepingCentre(centeredModeCluster.getWidth(), transportControlHeight)
                           .translated(0, transportContentVerticalNudge)
                           .reduced(8, 0);
    metronomeButton.setBounds(modeButtons.removeFromLeft(56));
    modeButtons.removeFromLeft(8);
    loopButton.setBounds(modeButtons.removeFromLeft(56));
    modeButtons.removeFromLeft(8);
    countInButton.setBounds(modeButtons.removeFromLeft(66));
    // BROWSER button removed from the toolbar — the small triangle in the top-left
    // corner is now the only browser toggle.
    browserButton.setBounds({});

    auto utilityButtons = centeredRightUtility.withSizeKeepingCentre(centeredRightUtility.getWidth(), transportControlHeight)
                              .translated(0, transportContentVerticalNudge)
                              .reduced(8, 0);
    saveButton.setBounds(utilityButtons.removeFromLeft(56));
    utilityButtons.removeFromLeft(8);
    exportButton.setBounds(utilityButtons.removeFromLeft(66));
    utilityButtons.removeFromLeft(8);
    settingsButton.setBounds(utilityButtons.removeFromLeft(74));
    scanPluginsButton.setBounds({});

    // Matches paint() — minimal margins so the playlist fills the work area.
    auto workArea = bounds.withTrimmedTop(8);

    // Small collapse arrow at the top-left of the work area, just below the transport
    // bar. Always visible, ~14×14 px, sits next to the browser/playlist boundary so it's
    // obvious what it controls.
    browserCollapseArrow.setBounds(workArea.getX() + 14,
                                   workArea.getY() + 12,
                                   14, 14);
    browserCollapseArrow.toFront(false);

    juce::Rectangle<int> browserPanelBounds;
    if (browserPanelVisible)
        browserPanelBounds = workArea.removeFromLeft(browserPanelWidth);
    auto arrangementPanel = workArea;

    auto playlistArea = arrangementPanel;
    playlistArea.removeFromLeft(8);
    playlistArea.removeFromRight(8);
    playlistArea.removeFromTop(8);
    playlistArea.removeFromBottom(8);

    const auto samplerOpen = samplerPanel.isVisible();
    const auto closedArrangementArea = playlistArea;
    auto openArrangementArea = playlistArea;
    auto samplerArea = openArrangementArea.removeFromBottom(juce::jmin(samplerBottomPanelHeight, openArrangementArea.getHeight()));
    const auto arrangementArea = samplerOpen ? openArrangementArea : closedArrangementArea;

    arrangementTimeline.setBounds(arrangementArea);

    if (browserPanelVisible)
    {
        auto browserInner = browserPanelBounds.reduced(16);
        browserPanel.setBounds(browserInner);
        browserPanel.setVisible(true);
    }
    else
    {
        browserPanel.setVisible(false);
    }

    tempoLabel.setBounds({});
    meterLabel.setBounds({});
    playheadLabel.setBounds({});
    playlistLabel.setBounds({});
    pianoRollLabel.setBounds({});
    exportButton.setVisible(true);
    saveButton.setVisible(true);
    settingsButton.setVisible(true);
    browserButton.setVisible(true);
    scanPluginsButton.setVisible(false);

    if (const auto trackInspectorBounds = arrangementTimeline.getSelectedTrackInspectorBounds(); trackInspectorBounds.has_value())
    {
        const auto timelineBounds = arrangementTimeline.getBounds();
        auto clipSection = trackInspectorBounds->translated(arrangementTimeline.getX(), arrangementTimeline.getY())
                               .getIntersection(timelineBounds)
                               .reduced(10, 8)
                               .withHeight(juce::jmin(78, juce::jmax(0, trackInspectorBounds->getHeight() - 16)));
        clipInspectorEmptyLabel.setBounds(clipSection);

        clipSection.removeFromTop(30);
        clipInspectorTitleLabel.setBounds({});
        clipInspectorTrackLabel.setBounds({});

        clipInspectorFileLabel.setBounds(clipSection.removeFromTop(14));
        clipSection.removeFromTop(3);

        auto warpRow = clipSection.removeFromTop(14);
        clipWarpLabel.setBounds(warpRow.removeFromLeft(30));
        clipWarpToggle.setBounds(warpRow.removeFromLeft(26));
        clipGainValueLabel.setBounds(warpRow.removeFromRight(48));
        clipWarpInfoLabel.setBounds(warpRow);

        clipSourceBpmLabel.setBounds({});
        clipBarsLabel.setBounds({});
        clipSection.removeFromTop(4);

        clipGainLabel.setBounds({});
        auto controlRow = clipSection.removeFromTop(16);
        clipGainSlider.setBounds(controlRow.removeFromLeft(86));
        controlRow.removeFromLeft(6);

        clipMuteToggle.setBounds(controlRow.removeFromLeft(46));
        controlRow.removeFromLeft(4);
        clipSoloToggle.setBounds(controlRow.removeFromLeft(46));
    }

    midiEditorOverlay.setBounds(getLocalBounds());
    samplerPanel.setBounds(samplerOpen ? samplerArea : juce::Rectangle<int>());
    samplerPanel.setVisible(samplerOpen);
}

void MainComponent::mouseMove(const juce::MouseEvent& event)
{
    if (getBrowserResizeHandleBounds().expanded(2, 0).contains(event.getPosition()))
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    else if (! isResizingBrowserPanel)
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void MainComponent::mouseExit(const juce::MouseEvent&)
{
    if (! isResizingBrowserPanel)
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void MainComponent::mouseDown(const juce::MouseEvent& event)
{
    // Right-click on the REC transport button opens a context menu for the
    // "record with metronome" option (one-bar count-in + click during recording).
    if (event.eventComponent == &recordButton && event.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        const auto withMetro = projectState.isRecordWithMetronome();
        menu.addItem(1, "Record with metronome", true, withMetro);
        menu.addItem(2, "Record without metronome", true, ! withMetro);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&recordButton),
            [this](int result)
            {
                if (result == 1) projectState.setRecordWithMetronome(true);
                else if (result == 2) projectState.setRecordWithMetronome(false);
            });
        return;
    }

    if (event.getNumberOfClicks() >= 2 && bpmValueLabel.getBounds().contains(event.getPosition()))
    {
        beginTempoEditing();
        return;
    }

    if (! cachedKeyCardBounds.isEmpty() && cachedKeyCardBounds.contains(event.getPosition()))
    {
        showKeySelectionMenu();
        return;
    }

    if (! getBrowserResizeHandleBounds().expanded(2, 0).contains(event.getPosition()))
        return;

    isResizingBrowserPanel = true;
    browserResizeStartX = event.getPosition().x;
    browserResizeStartWidth = browserPanelWidth;
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
}

void MainComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (bpmValueLabel.getBounds().contains(event.getPosition()))
    {
        beginTempoEditing();
        return;
    }
}

void MainComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (! isResizingBrowserPanel)
        return;

    const auto deltaX = event.getPosition().x - browserResizeStartX;
    browserPanelWidth = juce::jlimit(minBrowserPanelWidth, maxBrowserPanelWidth, browserResizeStartWidth + deltaX);
    resized();
    repaint();
}

void MainComponent::mouseUp(const juce::MouseEvent&)
{
    if (! isResizingBrowserPanel)
        return;

    isResizingBrowserPanel = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (midiEditorOverlay.isVisible() && midiEditorOverlay.keyPressed(key))
        return true;

    if (samplerPanel.isVisible() && (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey))
    {
        samplerPanel.closePanel();
        return true;
    }

    if ((samplerPanel.isVisible() || samplerPanel.isArmed()) && samplerPanel.keyPressed(key))
        return true;

    if (key == juce::KeyPress::returnKey)
    {
        const auto selectedTrackIndex = arrangementTimeline.getSelectedTrackIndex();
        if (selectedTrackIndex.has_value() && openSamplerForTrackIfAvailable(*selectedTrackIndex))
            return true;
    }

    if (key == juce::KeyPress('l', 0, 0) || key == juce::KeyPress('l', juce::ModifierKeys::commandModifier, 0))
    {
        const auto shouldEnable = getSelectedTimelineClip() != nullptr ? true : ! transportEngine.isLoopEnabled();
        loopButton.setToggleState(shouldEnable, juce::sendNotification);
        return true;
    }

    if (key != juce::KeyPress::spaceKey)
        return false;

    if (transportEngine.isPlaying() || transportEngine.isCountInActive())
        stopTransportFromUi();
    else
        toggleTransportFromUi();

    return true;
}

bool MainComponent::keyStateChanged(bool isKeyDown)
{
    if (midiEditorOverlay.isVisible())
        midiEditorOverlay.keyStateChanged(isKeyDown);

    if ((samplerPanel.isVisible() || samplerPanel.isArmed()) && samplerPanel.keyStateChanged(isKeyDown))
        return true;

    return false;
}

void MainComponent::resetToPlaylistView()
{
    midiEditorOverlay.setVisible(false);
    samplerPanel.setVisible(false);
    resized();
    arrangementTimeline.grabKeyboardFocus();
    repaint();
}

void MainComponent::timerCallback()
{
    updateTransportLabels();

    // While recording, drive the live UI feedback:
    //  (a) auto-start a recording clip the moment count-in ends and real playback begins
    //  (b) grow the recording clip's length in real time so the user sees a visual bar
    //      sweeping across the track as audio/MIDI is captured.
    const auto playing      = transportEngine.isPlaying();
    const auto countingIn   = transportEngine.isCountInActive();
    const auto recordArmed  = transportEngine.isRecordArmed();
    const auto canRecordNow = playing && ! countingIn && recordArmed;

    // Find a record-armed MIDI track. If none was explicitly R-armed, fall back to
    // the currently selected MIDI track (or the sampler-armed one) — that way the
    // user just presses REC + PLAY without having to also tap R on the track.
    auto& tracks = projectState.getTracks();
    int armedTrack = -1;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].isMidiTrack && tracks[i].recordArmed) { armedTrack = static_cast<int>(i); break; }

    if (armedTrack < 0)
    {
        // Fallback 1: selected clip's track if it's MIDI.
        if (selectedArrangementClip.has_value())
        {
            const auto idx = selectedArrangementClip->first;
            if (idx >= 0 && idx < static_cast<int>(tracks.size()) && tracks[static_cast<std::size_t>(idx)].isMidiTrack)
                armedTrack = idx;
        }
        // Fallback 2: any selected MIDI track header.
        if (armedTrack < 0)
        {
            const auto sel = arrangementTimeline.getSelectedTrackIndex();
            if (sel.has_value() && *sel >= 0 && *sel < static_cast<int>(tracks.size())
                && tracks[static_cast<std::size_t>(*sel)].isMidiTrack)
                armedTrack = *sel;
        }
    }

    if (canRecordNow && armedTrack >= 0)
    {
        // (a) Open a recording clip the instant real playback starts (after count-in),
        // even before any note is played, so the user immediately sees "recording" feedback.
        if (! recordingSession.has_value() || recordingSession->trackIndex != armedTrack)
        {
            finalizeRecordingClip();
            const auto playheadBeat = transportEngine.getPlayheadBeat();
            const auto clipStart    = std::floor(playheadBeat);
            auto& track = tracks[static_cast<std::size_t>(armedTrack)];
            track.clips.push_back(TimelineClip {
                "Recording",
                ClipType::midi,
                clipStart,
                juce::jmax(0.25, playheadBeat - clipStart),
                track.colour.brighter(0.1f),
                {}, "", 0.0, false, false,
                0.0, 0.0, 0,
                false, false, 0.0,
                -1, false, true
            });
            track.clips.back().recording = true;
            RecordingSession session;
            session.trackIndex    = armedTrack;
            session.clipIndex     = static_cast<int>(track.clips.size()) - 1;
            session.clipStartBeat = clipStart;
            recordingSession      = std::move(session);
        }

        // (b) Live-grow the clip so it sweeps with the playhead — but snap the visible
        // length UP to the next bar boundary instead of crawling beat-by-beat. The clip
        // then renders as a whole-bar block (FL-style) and never ends mid-bar.
        if (recordingSession->trackIndex < static_cast<int>(tracks.size()))
        {
            auto& clips = tracks[static_cast<std::size_t>(recordingSession->trackIndex)].clips;
            if (recordingSession->clipIndex < static_cast<int>(clips.size()))
            {
                auto& clip       = clips[static_cast<std::size_t>(recordingSession->clipIndex)];
                const auto rawLen      = juce::jmax(0.25, transportEngine.getPlayheadBeat() - recordingSession->clipStartBeat);
                const auto beatsPerBar = static_cast<double>(juce::jmax(1, projectState.getNumerator()));
                const auto bars        = std::ceil(rawLen / beatsPerBar);
                const auto snappedLen  = juce::jmax(beatsPerBar, bars * beatsPerBar);
                if (snappedLen > clip.lengthInBeats)
                {
                    clip.lengthInBeats = snappedLen;
                    arrangementTimeline.repaint();
                }
            }
        }
    }
    else if (! playing && recordingSession.has_value())
    {
        // Playback stopped externally (loop end / user stop / count-in cancel) — finalise.
        finalizeRecordingClip();
    }
}

void MainComponent::buttonClicked(juce::Button* button)
{
    if (button == &playButton)
        toggleTransportFromUi();
    else if (button == &stopButton)
        stopTransportFromUi();
    else if (button == &recordButton)
    {
        transportController.setRecordArmed(recordButton.getToggleState());
        recordButton.setToggleState(transportEngine.isRecordArmed(), juce::dontSendNotification);
        updateTransportLabels();
    }
    else if (button == &rewindButton)
        rewindTransportFromUi();
    else if (button == &undoButton)
    {
        arrangementTimeline.undo();
        updateTransportLabels();
    }
    else if (button == &redoButton)
    {
        arrangementTimeline.redo();
        updateTransportLabels();
    }
    else if (button == &loopButton)
        toggleLoopFromUi();
    else if (button == &metronomeButton || button == &countInButton)
    {
        updateTransportLabels();
    }
    else if (button == &browserButton || button == &browserCollapseArrow)
    {
        // Both controls are sticky toggles; the state we just received IS the new desired
        // visibility. Mirror it onto whichever toggle the user didn't click so they stay
        // in sync.
        browserPanelVisible = button->getToggleState();
        browserButton.setToggleState(browserPanelVisible, juce::dontSendNotification);
        browserCollapseArrow.setToggleState(browserPanelVisible, juce::dontSendNotification);
        browserPanel.setVisible(browserPanelVisible);
        resized();
        repaint();
    }
    else if (button == &saveButton)
    {
        saveProjectInteractively();
    }
    else if (button == &exportButton)
    {
        exportProjectInteractively();
    }
    else if (button == &settingsButton)
    {
        openSettingsDialog();
    }
}

void MainComponent::updateTransportLabels()
{
    if (! bpmEditor.isVisible())
    {
        bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
        bpmValueLabel.setVisible(false);
        repaint();
    }

    if (transportEngine.isCountInActive())
        meterValueLabel.setText("COUNT", juce::dontSendNotification);
    else
        meterValueLabel.setText(juce::String(projectState.getNumerator()) + "/" + juce::String(projectState.getDenominator()), juce::dontSendNotification);

    if (transportEngine.isCountInActive())
        meterCaptionLabel.setText("COUNT-IN", juce::dontSendNotification);
    else if (transportEngine.isPlaying())
        meterCaptionLabel.setText("RUNNING", juce::dontSendNotification);
    else if (transportEngine.isPaused())
        meterCaptionLabel.setText("PAUSED", juce::dontSendNotification);
    else
        meterCaptionLabel.setText("STOPPED", juce::dontSendNotification);

    playButton.setToggleState(transportEngine.isPlaying() || transportEngine.isCountInActive(), juce::dontSendNotification);
    recordButton.setToggleState(transportEngine.isRecordArmed(), juce::dontSendNotification);
    loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
    undoButton.setEnabled(arrangementTimeline.canUndo());
    redoButton.setEnabled(arrangementTimeline.canRedo());
    tempoLabel.setText("Tempo: " + juce::String(projectState.getTempoBpm(), 0) + " BPM", juce::dontSendNotification);
    meterLabel.setText("Meter: " + juce::String(projectState.getNumerator()) + "/" + juce::String(projectState.getDenominator()), juce::dontSendNotification);
    playheadLabel.setText("Playhead: beat " + juce::String(transportEngine.getPlayheadBeat(), 2), juce::dontSendNotification);
}

void MainComponent::playBrowserPreview(const BrowserItem& item)
{
    statusLabel.setText("Previewing: " + item.file.getFileName(), juce::dontSendNotification);

    if (! item.file.existsAsFile())
    {
        statusLabel.setText("Preview failed: file missing", juce::dontSendNotification);
        return;
    }

    if (previewBufferSource != nullptr
        && item.file == currentPreviewFile
        && std::abs(currentPreviewTempoBpm - projectState.getTempoBpm()) < 0.001)
    {
        previewTransportSource.setPosition(0.0);
        previewTransportSource.start();
        return;
    }

    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();
    currentPreviewFile = juce::File();
    currentPreviewTempoBpm = 0.0;

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(item.file));
    if (reader == nullptr)
    {
        statusLabel.setText("Preview failed: unsupported file", juce::dontSendNotification);
        return;
    }

    const auto sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    const auto maxPreviewSamples = static_cast<juce::int64>(previewMaxLengthSeconds * sampleRate);
    const auto samplesToRead = static_cast<int>(juce::jmin(reader->lengthInSamples, maxPreviewSamples));
    if (samplesToRead <= 0)
    {
        statusLabel.setText("Preview failed: empty file", juce::dontSendNotification);
        return;
    }

    juce::AudioBuffer<float> previewBuffer(static_cast<int>(reader->numChannels), samplesToRead);
    reader->read(&previewBuffer, 0, samplesToRead, 0, true, true);

    if (transportEngine.isPlaying())
    {
        const auto analysis = analyzeAudioWarpMetadata(item.file, projectState.getTempoBpm(), projectState.getNumerator());
        previewBuffer = makeTempoFittedPreviewBuffer(previewBuffer, analysis.sourceBpm, projectState.getTempoBpm(), sampleRate, item.file.getFullPathName());
    }

    previewBufferSource = std::make_unique<BufferPreviewSource>(std::move(previewBuffer), sampleRate);
    previewTransportSource.setSource(previewBufferSource.get(), 0, nullptr, sampleRate);
    currentPreviewFile = item.file;
    currentPreviewTempoBpm = projectState.getTempoBpm();
    previewTransportSource.setPosition(0.0);
    previewTransportSource.start();
}

void MainComponent::loadBrowserItemIntoSampler(const BrowserItem& item)
{
    if (item.isDirectory || ! item.file.existsAsFile())
        return;

    const auto trackIndex = findOrCreateSamplerTargetTrack();
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (! track.isMidiTrack)
        return;

    const auto analysis = analyzeAudioWarpMetadata(item.file, projectState.getTempoBpm(), projectState.getNumerator());
    track.samplerSourcePath = item.file.getFullPathName();
    track.samplerSourceDurationSeconds = analysis.durationSeconds;
    track.samplerSourceBpm = analysis.sourceBpm;
    track.samplerDetectedBars = analysis.detectedBars;

    samplerPanel.openTrackIndex(trackIndex);
    resized();
    statusLabel.setText("Sampler loaded: " + item.file.getFileName(), juce::dontSendNotification);
    arrangementTimeline.repaint();
}

int MainComponent::findOrCreateSamplerTargetTrack()
{
    auto& tracks = projectState.getTracks();

    if (selectedArrangementClip.has_value())
    {
        const auto trackIndex = selectedArrangementClip->first;
        if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
        {
            const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
            if (track.isMidiTrack)
                return trackIndex;
        }
    }

    const auto selectedTrackIndex = arrangementTimeline.getSelectedTrackIndex();
    if (selectedTrackIndex.has_value() && *selectedTrackIndex >= 0 && *selectedTrackIndex < static_cast<int>(tracks.size()))
    {
        const auto& track = tracks[static_cast<std::size_t>(*selectedTrackIndex)];
        if (track.isMidiTrack)
            return *selectedTrackIndex;
    }

    TrackState samplerTrack;
    samplerTrack.name = "Sampler Track";
    samplerTrack.isMidiTrack = true;
    samplerTrack.colour = juce::Colour(0xff9db0c4);
    tracks.push_back(std::move(samplerTrack));

    arrangementTimeline.repaint();
    return static_cast<int>(tracks.size()) - 1;
}

bool MainComponent::openSamplerForTrackIfAvailable(int trackIndex)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return false;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (! track.isMidiTrack || track.samplerSourcePath.isEmpty())
        return false;

    samplerPanel.openTrackIndex(trackIndex);
    resized();
    return true;
}

void MainComponent::showTrackInstrumentMenu(int trackIndex)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (! track.isMidiTrack)
        return;  // only MIDI tracks host instruments

    const bool hasInstrument = track.instrumentPluginId.isNotEmpty();
    const auto loadablePlugins = pluginManager.getAllDescriptions();

    juce::PopupMenu menu;
    if (hasInstrument)
    {
        menu.addSectionHeader(track.instrumentPluginName.isNotEmpty() ? track.instrumentPluginName
                                                                      : juce::String("Instrument"));
        menu.addItem(1, "Open instrument editor");
        menu.addItem(2, "Remove instrument");
        menu.addSeparator();
    }

    juce::PopupMenu loadMenu;
    if (loadablePlugins.isEmpty())
    {
        if (pluginManager.isScanning())
            loadMenu.addItem(9999, "Scanning...", false, false);
        else
            loadMenu.addItem(3, "Scan VST plugins...");
    }
    else
    {
        int id = 1000;
        for (const auto& desc : loadablePlugins)
        {
            auto label = desc.name + "  (" + desc.pluginFormatName + ")";
            if (! desc.isInstrument)
                label += " - effect";
            loadMenu.addItem(id++, label);
        }
    }
    menu.addSubMenu(hasInstrument ? "Replace instrument" : "Load instrument", loadMenu);
    menu.addSeparator();
    menu.addItem(3, pluginManager.isScanning() ? "Scanning..." : "Rescan VST plugins...",
                 ! pluginManager.isScanning());

    const auto pluginList = loadablePlugins;
    menu.showMenuAsync(juce::PopupMenu::Options(),
        [this, trackIndex, pluginList](int result)
        {
            if (result == 0)
                return;
            if (result == 1) { openInstrumentEditor(trackIndex); return; }
            if (result == 2) { removeInstrumentFromTrack(trackIndex); return; }
            if (result == 3) { scanPluginsInteractively(); return; }
            if (result >= 1000 && result < 9999)
            {
                const auto index = result - 1000;
                if (index >= 0 && index < pluginList.size())
                    loadInstrumentOnTrack(trackIndex, pluginList.getReference(index));
            }
        });
}

void MainComponent::scanPluginsInteractively(std::function<void()> onFinished)
{
    if (pluginManager.isScanning())
        return;

    pluginScanVisible = true;
    pluginScanProgress = 0.0;
    pluginScanNameLabel.setText("Starting VST scan...", juce::dontSendNotification);
    pluginScanNameLabel.setVisible(true);
    statusLabel.setText("Scanning for VST plugins...", juce::dontSendNotification);
    resized();
    repaint();

    pluginManager.scanForPlugins(
        [this](const juce::String& name, double progress)
        {
            pluginScanProgress = progress;

            if (name.isNotEmpty())
            {
                const auto displayName = juce::File(name).getFileName().isNotEmpty()
                    ? juce::File(name).getFileName()
                    : name;
                pluginScanNameLabel.setText(displayName, juce::dontSendNotification);
                statusLabel.setText("Scanning: " + displayName, juce::dontSendNotification);
            }

            repaint();
        },
        [this, onFinished]()
        {
            const auto count = pluginManager.getAllDescriptions().size();
            statusLabel.setText("Found " + juce::String(count) + " plugin(s)", juce::dontSendNotification);
            pluginScanProgress = 1.0;
            pluginScanNameLabel.setText("Scan complete", juce::dontSendNotification);
            repaint();
            juce::Timer::callAfterDelay(1200, [safeThis = juce::Component::SafePointer<MainComponent>(this)]
            {
                if (safeThis == nullptr)
                    return;

                safeThis->pluginScanVisible = false;
                safeThis->pluginScanNameLabel.setVisible(false);
                safeThis->resized();
                safeThis->repaint();
            });
            if (onFinished)
                onFinished();
        });
}

void MainComponent::loadInstrumentOnTrack(int trackIndex, const juce::PluginDescription& description)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()) || arrangementPlaybackSource == nullptr)
        return;

    juce::String error;
    auto instance = pluginManager.createInstance(description, 44100.0, 512, error);
    if (instance == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Could not load plugin",
                                               error.isNotEmpty() ? error : juce::String("Unknown error."));
        return;
    }

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    track.instrumentPluginId    = description.createIdentifierString();
    track.instrumentPluginName  = description.name;
    track.instrumentStateBase64 = {};

    closeInstrumentEditor(trackIndex);
    arrangementPlaybackSource->setTrackInstrument(trackIndex, std::move(instance));

    samplerPanel.openTrackIndex(trackIndex);
    samplerPanel.setVisible(false);
    statusLabel.setText(description.isInstrument
                            ? "Instrument loaded: " + description.name
                            : "Effect loaded: " + description.name + " (keyboard notes need an instrument)",
                        juce::dontSendNotification);
    arrangementTimeline.repaint();
    openInstrumentEditor(trackIndex);
}

void MainComponent::removeInstrumentFromTrack(int trackIndex)
{
    closeInstrumentEditor(trackIndex);
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->clearTrackInstrument(trackIndex);

    auto& tracks = projectState.getTracks();
    if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
    {
        auto& track = tracks[static_cast<std::size_t>(trackIndex)];
        track.instrumentPluginId    = {};
        track.instrumentPluginName  = {};
        track.instrumentStateBase64 = {};
    }
    arrangementTimeline.repaint();
}

void MainComponent::openInstrumentEditor(int trackIndex)
{
    if (arrangementPlaybackSource == nullptr)
        return;

    auto* instance = arrangementPlaybackSource->getTrackInstrument(trackIndex);
    if (instance == nullptr)
        return;

    if (auto it = instrumentEditorWindows.find(trackIndex);
        it != instrumentEditorWindows.end() && it->second != nullptr)
    {
        it->second->toFront(true);
        return;
    }

    auto& tracks = projectState.getTracks();
    juce::String title = "Instrument";
    if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
    {
        const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
        title = track.name + " - " + track.instrumentPluginName;
    }

    instrumentEditorWindows[trackIndex] = std::make_unique<PluginEditorWindow>(
        *instance,
        title,
        [this, trackIndex]() { closeInstrumentEditor(trackIndex); },
        [this](const juce::KeyPress& key) { return keyPressed(key); },
        [this](bool isKeyDown) { return keyStateChanged(isKeyDown); });
}

void MainComponent::closeInstrumentEditor(int trackIndex)
{
    instrumentEditorWindows.erase(trackIndex);
}

void MainComponent::closeAllInstrumentEditors()
{
    instrumentEditorWindows.clear();
}

void MainComponent::captureAllInstrumentStates()
{
    if (arrangementPlaybackSource == nullptr)
        return;

    auto& tracks = projectState.getTracks();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        auto* instance = arrangementPlaybackSource->getTrackInstrument(i);
        if (instance == nullptr)
            continue;

        juce::MemoryBlock block;
        instance->getStateInformation(block);
        tracks[static_cast<std::size_t>(i)].instrumentStateBase64 = block.toBase64Encoding();
    }
}

void MainComponent::restoreInstrumentsFromProject()
{
    if (arrangementPlaybackSource == nullptr)
        return;

    closeAllInstrumentEditors();
    auto& tracks = projectState.getTracks();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
    {
        arrangementPlaybackSource->clearTrackInstrument(i);

        auto& track = tracks[static_cast<std::size_t>(i)];
        if (track.instrumentPluginId.isEmpty())
            continue;

        const auto desc = pluginManager.findDescription(track.instrumentPluginId);
        if (! desc.has_value())
            continue;  // plugin not installed/scanned right now — keep id for a later rescan

        juce::String error;
        auto instance = pluginManager.createInstance(*desc, 44100.0, 512, error);
        if (instance == nullptr)
            continue;

        if (track.instrumentStateBase64.isNotEmpty())
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding(track.instrumentStateBase64) && block.getSize() > 0)
                instance->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        }

        arrangementPlaybackSource->setTrackInstrument(i, std::move(instance));
    }
    arrangementTimeline.repaint();
}

void MainComponent::stopBrowserPreview(bool resetPosition)
{
    previewTransportSource.stop();
    if (resetPosition)
        previewTransportSource.setPosition(0.0);
}

void MainComponent::toggleTransportFromUi()
{
    // Always do a one-bar count-in before recording starts so the user has a
    // tempo reference before the first note. Count-in is also honoured when the
    // user enabled it manually for non-recording playback. The user can opt out
    // of the metronome on record via right-click on the REC button.
    const auto useCountIn = countInButton.getToggleState()
        || (transportEngine.isRecordArmed() && projectState.isRecordWithMetronome());
    transportController.togglePlayback(
        useCountIn,
        [this]() { stopBrowserPreview(true); },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
        },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    updateTransportLabels();
}

void MainComponent::stopTransportFromUi()
{
    finalizeRecordingClip(); // close any in-flight MIDI recording session
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->allInstrumentNotesOff(); // flush any hung instrument notes
    transportController.stop(
        [this]() { stopBrowserPreview(true); },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::recordNoteOn(int pitch, int velocity)
{
    // Don't capture notes while count-in is still ticking — only after real playback starts.
    if (! transportEngine.isPlaying() || transportEngine.isCountInActive() || ! transportEngine.isRecordArmed())
        return;
    if (! recordingSession.has_value())
        return; // timerCallback opens the session on the first tick after count-in

    const auto playheadBeat = transportEngine.getPlayheadBeat();
    const auto beatInClip   = juce::jmax(0.0, playheadBeat - recordingSession->clipStartBeat);
    recordingSession->pendingNotes[pitch] = { velocity, beatInClip };
}

void MainComponent::recordNoteOff(int pitch)
{
    if (! recordingSession.has_value()) return;

    auto it = recordingSession->pendingNotes.find(pitch);
    if (it == recordingSession->pendingNotes.end()) return;

    const auto playheadBeat = transportEngine.getPlayheadBeat();
    const auto endBeatInClip = juce::jmax(it->second.startBeatInClip + 0.05,
                                          playheadBeat - recordingSession->clipStartBeat);
    const auto lengthBeats = endBeatInClip - it->second.startBeatInClip;

    auto& tracks = projectState.getTracks();
    if (recordingSession->trackIndex < static_cast<int>(tracks.size()))
    {
        auto& clips = tracks[static_cast<std::size_t>(recordingSession->trackIndex)].clips;
        if (recordingSession->clipIndex < static_cast<int>(clips.size()))
        {
            auto& clip = clips[static_cast<std::size_t>(recordingSession->clipIndex)];
            clip.midiNotes.push_back(MidiNote { pitch, it->second.startBeatInClip, lengthBeats, it->second.velocity });
            if (endBeatInClip + 0.5 > clip.lengthInBeats)
                clip.lengthInBeats = std::ceil(endBeatInClip + 0.25); // expand to next quarter
        }
    }

    recordingSession->pendingNotes.erase(it);
    arrangementTimeline.repaint();
}

void MainComponent::finalizeRecordingClip()
{
    if (! recordingSession.has_value()) return;

    // Close any notes that are still held when recording ends.
    const auto playheadBeat = transportEngine.getPlayheadBeat();
    auto& tracks = projectState.getTracks();

    if (recordingSession->trackIndex < static_cast<int>(tracks.size()))
    {
        auto& clips = tracks[static_cast<std::size_t>(recordingSession->trackIndex)].clips;
        if (recordingSession->clipIndex < static_cast<int>(clips.size()))
        {
            auto& clip = clips[static_cast<std::size_t>(recordingSession->clipIndex)];
            for (const auto& [pitch, pending] : recordingSession->pendingNotes)
            {
                const auto endBeatInClip = juce::jmax(pending.startBeatInClip + 0.05,
                                                      playheadBeat - recordingSession->clipStartBeat);
                clip.midiNotes.push_back(MidiNote {
                    pitch,
                    pending.startBeatInClip,
                    endBeatInClip - pending.startBeatInClip,
                    pending.velocity
                });
                if (endBeatInClip + 0.5 > clip.lengthInBeats)
                    clip.lengthInBeats = std::ceil(endBeatInClip + 0.25);
            }
            // If the recording captured nothing, drop the empty clip so we don't pollute the timeline.
            if (clip.midiNotes.empty())
            {
                clips.erase(clips.begin() + recordingSession->clipIndex);
            }
            else
            {
                clip.recording = false;
                // Round the clip length UP to the next bar boundary so playback always
                // covers a full bar — matches FL's behaviour where the pattern length
                // snaps to the bar grid even if you stopped recording mid-bar.
                const auto beatsPerBar = static_cast<double>(juce::jmax(1, projectState.getNumerator()));
                const auto bars        = std::ceil(clip.lengthInBeats / beatsPerBar);
                clip.lengthInBeats     = juce::jmax(beatsPerBar, bars * beatsPerBar);
            }
        }
    }

    recordingSession.reset();
    arrangementTimeline.repaint();
}

void MainComponent::rewindTransportFromUi()
{
    transportController.rewind(
        [this]() { stopBrowserPreview(true); },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::toggleLoopFromUi()
{
    transportController.setLoopEnabled(
        loopButton.getToggleState(),
        getSelectedTimelineClip(),
        [this](const juce::String& statusText)
        {
            statusLabel.setText(statusText, juce::dontSendNotification);
        });
    loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::saveProjectInteractively()
{
    // Pull the latest plugin state into the project before it is serialized.
    captureAllInstrumentStates();

    auto saveToTarget = [this](const juce::File& targetFile)
    {
        juce::String errorMessage;
        if (ProjectSerializer::saveToFile(projectState, targetFile, &errorMessage))
        {
            currentProjectFile = targetFile;
            statusLabel.setText("Saved: " + targetFile.getFileName(), juce::dontSendNotification);
        }
        else
        {
            statusLabel.setText("Save failed: " + errorMessage, juce::dontSendNotification);
        }
    };

    if (currentProjectFile.existsAsFile())
    {
        saveToTarget(currentProjectFile);
        return;
    }

    auto defaultDirectory = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto defaultTarget = defaultDirectory.getChildFile("Untitled.orion.json");
    saveFileChooser = std::make_unique<juce::FileChooser>("Save Orion Project",
                                                          defaultTarget,
                                                          "*.orion.json");

    auto chooserFlags = juce::FileBrowserComponent::saveMode
                      | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    saveFileChooser->launchAsync(chooserFlags,
                                 [this, saveToTarget](const juce::FileChooser& chooser)
                                 {
                                     auto selectedFile = chooser.getResult();
                                     saveFileChooser.reset();

                                     if (selectedFile == juce::File())
                                     {
                                         statusLabel.setText("Save cancelled", juce::dontSendNotification);
                                         return;
                                     }

                                     if (! selectedFile.hasFileExtension("orion.json"))
                                         selectedFile = selectedFile.withFileExtension(".orion.json");

                                     saveToTarget(selectedFile);
                                 });
}

void MainComponent::exportProjectInteractively()
{
    if (arrangementPlaybackSource == nullptr)
    {
        statusLabel.setText("Export failed: playback source unavailable", juce::dontSendNotification);
        return;
    }

    auto defaultDirectory = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto defaultTarget = defaultDirectory.getChildFile("Orion Export.wav");
    exportFileChooser = std::make_unique<juce::FileChooser>("Export Orion Mix",
                                                            defaultTarget,
                                                            "*.wav");

    auto chooserFlags = juce::FileBrowserComponent::saveMode
                      | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;

    exportFileChooser->launchAsync(chooserFlags,
                                   [this](const juce::FileChooser& chooser)
                                   {
                                       auto selectedFile = chooser.getResult();
                                       exportFileChooser.reset();

                                       if (selectedFile == juce::File())
                                       {
                                           statusLabel.setText("Export cancelled", juce::dontSendNotification);
                                           return;
                                       }

                                       if (! selectedFile.hasFileExtension("wav"))
                                           selectedFile = selectedFile.withFileExtension(".wav");

                                       juce::String errorMessage;
                                       const auto exported = ExportService::exportToWav(
                                           projectState,
                                           transportEngine.isLoopEnabled(),
                                           exportSampleRate,
                                           selectedFile,
                                           [this]()
                                           {
                                               if (arrangementPlaybackSource != nullptr)
                                                   arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
                                           },
                                           [this](juce::AudioBuffer<float>& buffer,
                                                  int startSample,
                                                  int numSamples,
                                                  double blockStartBeat,
                                                  double renderSampleRate)
                                           {
                                               if (arrangementPlaybackSource != nullptr)
                                                   arrangementPlaybackSource->renderOfflineBlock(buffer,
                                                                                                 startSample,
                                                                                                 numSamples,
                                                                                                 blockStartBeat,
                                                                                                 renderSampleRate);
                                           },
                                           &errorMessage);

                                       statusLabel.setText(exported
                                                               ? "Exported: " + selectedFile.getFileName()
                                                               : "Export failed: " + errorMessage,
                                                           juce::dontSendNotification);
                                   });
}

void MainComponent::openSettingsDialog()
{
    auto settingsComponent = std::make_unique<SettingsContent>(
        audioDeviceManager,
        browserPanelWidth,
        exportSampleRate,
        [this](int newWidth)
        {
            browserPanelWidth = juce::jlimit(minBrowserPanelWidth, maxBrowserPanelWidth, newWidth);
            resized();
            repaint();
        },
        [this](int newRate)
        {
            exportSampleRate = newRate;
            statusLabel.setText("Export sample rate: " + juce::String(exportSampleRate / 1000.0, 1) + " kHz",
                                juce::dontSendNotification);
        },
        [this]()
        {
            statusLabel.setText("Settings saved", juce::dontSendNotification);
        });

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(settingsComponent.release());
    options.dialogTitle = "Orion Settings";
    options.dialogBackgroundColour = panelColour;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = true;
    options.componentToCentreAround = this;
    options.content->setSize(560, 520);
    options.launchAsync();
}

void MainComponent::refreshAudioClipWarpLengths()
{
    for (auto& track : projectState.getTracks())
    {
        for (auto& clip : track.clips)
        {
            if (clip.type != ClipType::audio || clip.sourcePath.isEmpty())
                continue;

            const auto sourceFile = juce::File(clip.sourcePath);
            const auto namedBpm = parseBpmFromFileName(sourceFile);
            const auto needsAnalysis = clip.sourceDurationSeconds <= 0.0
                || (clip.sourceBpm <= 0.0 && clip.detectedBars == 0)
                || (namedBpm > 0.0 && std::abs(clip.sourceBpm - namedBpm) > 0.01);

            if (needsAnalysis)
            {
                const auto analysis = analyzeAudioWarpMetadata(sourceFile, projectState.getTempoBpm(), projectState.getNumerator());
                if (clip.sourceDurationSeconds <= 0.0 && analysis.durationSeconds > 0.0)
                    clip.sourceDurationSeconds = analysis.durationSeconds;
                if ((clip.sourceBpm <= 0.0 || namedBpm > 0.0) && analysis.sourceBpm > 0.0)
                {
                    clip.sourceBpm = analysis.sourceBpm;
                    clip.bpmGuessed = analysis.bpmGuessed;
                }
                if ((clip.detectedBars == 0 || namedBpm > 0.0) && analysis.detectedBars > 0)
                    clip.detectedBars = analysis.detectedBars;
            }

            if (clip.warpEnabled)
            {
                double detectedLengthInBeats = 0.0;
                if (clip.detectedBars > 0)
                    detectedLengthInBeats = juce::jmax(1.0, static_cast<double>(clip.detectedBars * juce::jmax(1, projectState.getNumerator())));
                else if (clip.sourceDurationSeconds > 0.0 && clip.sourceBpm > 0.0)
                    detectedLengthInBeats = juce::jmax(1.0, clip.sourceDurationSeconds * (clip.sourceBpm / 60.0));

                if (detectedLengthInBeats > 0.0 && clip.warpTargetLengthInBeats <= 0.0)
                {
                    clip.lengthInBeats = detectedLengthInBeats;
                    clip.warpTargetLengthInBeats = detectedLengthInBeats;
                }
            }
            else if (clip.sourceDurationSeconds > 0.0)
            {
                clip.lengthInBeats = juce::jmax(1.0, clip.sourceDurationSeconds * (projectState.getTempoBpm() / 60.0));
                clip.warpTargetLengthInBeats = 0.0;
            }
        }
    }
}

void MainComponent::refreshClipInspector()
{
    const auto* clip = getSelectedTimelineClip();
    if (clip == nullptr || clip->type != ClipType::audio)
    {
        setClipInspectorVisible(false);
        clipInspectorEmptyLabel.setVisible(false);
        clipInspectorEmptyLabel.setText("Select audio clip", juce::dontSendNotification);
        resized();
        repaint();
        return;
    }

    setClipInspectorVisible(true);
    clipInspectorEmptyLabel.setVisible(false);
    clipInspectorTitleLabel.setText(clip->name, juce::dontSendNotification);

    const auto [trackIndex, clipIndex] = *selectedArrangementClip;
    juce::ignoreUnused(clipIndex);
    clipInspectorTrackLabel.setText("Track: " + projectState.getTracks()[static_cast<std::size_t>(trackIndex)].name, juce::dontSendNotification);

    auto* mutableClip = getSelectedTimelineClip();
    if (mutableClip != nullptr && mutableClip->sourcePath.isNotEmpty())
    {
        const auto sourceFile = juce::File(mutableClip->sourcePath);
        const auto namedBpm = parseBpmFromFileName(sourceFile);
        const auto shouldEnableDefaultWarp = ! mutableClip->warpEnabled
            && mutableClip->sourceDurationSeconds <= 0.0
            && mutableClip->sourceBpm <= 0.0
            && mutableClip->detectedBars == 0;

        if (mutableClip->sourceDurationSeconds <= 0.0
            || (mutableClip->sourceBpm <= 0.0 && mutableClip->detectedBars == 0)
            || (namedBpm > 0.0 && std::abs(mutableClip->sourceBpm - namedBpm) > 0.01))
        {
            const auto analysis = analyzeAudioWarpMetadata(sourceFile, projectState.getTempoBpm(), projectState.getNumerator());
            if (mutableClip->sourceDurationSeconds <= 0.0 && analysis.durationSeconds > 0.0)
                mutableClip->sourceDurationSeconds = analysis.durationSeconds;
            if ((mutableClip->sourceBpm <= 0.0 || namedBpm > 0.0) && analysis.sourceBpm > 0.0)
            {
                mutableClip->sourceBpm = analysis.sourceBpm;
                mutableClip->bpmGuessed = analysis.bpmGuessed;
            }
            if ((mutableClip->detectedBars == 0 || namedBpm > 0.0) && analysis.detectedBars > 0)
                mutableClip->detectedBars = analysis.detectedBars;
        }

        if (shouldEnableDefaultWarp && mutableClip->sourceBpm > 0.0)
            mutableClip->warpEnabled = true;
        if (mutableClip->warpEnabled && mutableClip->detectedBars > 0 && mutableClip->warpTargetLengthInBeats <= 0.0)
        {
            mutableClip->lengthInBeats = static_cast<double>(mutableClip->detectedBars * juce::jmax(1, projectState.getNumerator()));
            mutableClip->warpTargetLengthInBeats = mutableClip->lengthInBeats;
        }
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
    }

    auto sourceFile = juce::File(clip->sourcePath);
    clipInspectorFileLabel.setText(compactInspectorFileName(sourceFile, clip->name), juce::dontSendNotification);
    clipWarpToggle.setToggleState(clip->warpEnabled, juce::dontSendNotification);
    clipWarpInfoLabel.setText(clip->warpEnabled ? "Warp" : "Raw", juce::dontSendNotification);
    clipSourceBpmLabel.setText(
        clip->sourceBpm > 0.0 ? "Source BPM: " + juce::String(clip->sourceBpm, 1) : "Source BPM: not detected",
        juce::dontSendNotification);
    clipBarsLabel.setText(
        clip->detectedBars > 0
            ? "Detected loop: " + juce::String(clip->detectedBars) + " bar" + (clip->detectedBars == 1 ? "" : "s")
            : "Detected loop: free length",
        juce::dontSendNotification);

    clipGainSlider.setValue(clip->gainDb, juce::dontSendNotification);
    clipGainValueLabel.setText(juce::String(clip->gainDb, 1) + " dB", juce::dontSendNotification);

    clipMuteToggle.setToggleState(clip->muted, juce::dontSendNotification);
    clipSoloToggle.setToggleState(clip->solo, juce::dontSendNotification);
    resized();
    repaint();
}

void MainComponent::setClipInspectorVisible(bool shouldShow)
{
    // The floating clip inspector is disabled until it is rebuilt inside the
    // track header; drawing it as a MainComponent overlay made it drift away
    // from the selected track when the playlist scrolled or resized.
    shouldShow = false;

    clipInspectorEmptyLabel.setVisible(false);
    clipInspectorTitleLabel.setVisible(false);
    clipInspectorTrackLabel.setVisible(false);
    clipGainLabel.setVisible(false);
    clipSourceBpmLabel.setVisible(false);
    clipBarsLabel.setVisible(false);
    for (auto* component : { static_cast<juce::Component*>(&clipInspectorFileLabel),
                             static_cast<juce::Component*>(&clipWarpLabel),
                             static_cast<juce::Component*>(&clipWarpInfoLabel),
                             static_cast<juce::Component*>(&clipWarpToggle),
                             static_cast<juce::Component*>(&clipGainValueLabel),
                             static_cast<juce::Component*>(&clipGainSlider),
                             static_cast<juce::Component*>(&clipMuteToggle),
                             static_cast<juce::Component*>(&clipSoloToggle) })
    {
        component->setVisible(shouldShow);
        if (shouldShow)
            component->toFront(false);
    }

    if (shouldShow)
    {
        clipInspectorEmptyLabel.toFront(false);
        bpmEditor.toFront(false);
    }
}

void MainComponent::applyGainFromInspectorText()
{
    auto* clip = getSelectedTimelineClip();
    if (clip == nullptr)
        return;

    auto text = clipGainValueLabel.getText().upToFirstOccurrenceOf("dB", false, false).trim();
    if (text.isEmpty())
        return;

    const auto parsedValue = text.getDoubleValue();
    const auto clampedValue = juce::jlimit(-24.0, 12.0, parsedValue);
    clip->gainDb = clampedValue;
    clipGainSlider.setValue(clampedValue, juce::dontSendNotification);
    clipGainValueLabel.setText(juce::String(clampedValue, 1) + " dB", juce::dontSendNotification);
    arrangementTimeline.repaint();
}

void MainComponent::applyTempoFromTransportText()
{
    auto text = bpmEditor.getText().trim();
    if (text.isEmpty())
        return;

    const auto parsedValue = text.getDoubleValue();
    if (parsedValue <= 0.0)
    {
        updateTransportLabels();
        return;
    }

    transportController.setTempoBpm(parsedValue);
    refreshAudioClipWarpLengths();
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    updateTransportLabels();
    refreshClipInspector();
    arrangementTimeline.repaint();
}

void MainComponent::beginTempoEditing()
{
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmValueLabel.setVisible(false);
    bpmEditor.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmEditor.setVisible(true);
    bpmEditor.toFront(false);
    bpmEditor.repaint();
    bpmEditor.selectAll();
    bpmEditor.grabKeyboardFocus();
}

void MainComponent::endTempoEditing(bool applyChanges)
{
    if (! bpmEditor.isVisible())
        return;

    if (applyChanges)
        applyTempoFromTransportText();

    bpmEditor.setVisible(false);
    bpmValueLabel.setText(juce::String(projectState.getTempoBpm(), 2), juce::dontSendNotification);
    bpmValueLabel.setVisible(false);
    repaint();
}

void MainComponent::showKeySelectionMenu()
{
    juce::PopupMenu menu;
    static const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    const auto currentRoot  = projectState.getKeyRoot();
    const auto currentMinor = projectState.isKeyMinor();

    juce::PopupMenu majorSub;
    juce::PopupMenu minorSub;
    for (int root = 0; root < 12; ++root)
    {
        const auto majorId = 100 + root;
        const auto minorId = 200 + root;
        majorSub.addItem(majorId, juce::String(noteNames[root]) + " major",
                          true, root == currentRoot && ! currentMinor);
        minorSub.addItem(minorId, juce::String(noteNames[root]) + " minor",
                          true, root == currentRoot && currentMinor);
    }
    menu.addSubMenu("Major", majorSub);
    menu.addSubMenu("Minor", minorSub);

    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withTargetScreenArea(
        localAreaToGlobal(cachedKeyCardBounds)),
        [this](int result)
        {
            if (result <= 0) return;
            const auto isMinor = result >= 200;
            const auto root    = (isMinor ? result - 200 : result - 100) % 12;
            projectState.setKey(root, isMinor);
            // Force-rebuild every clip's warped buffer with the new pitch shift —
            // the cache key includes the semitone shift, so this populates the new entries.
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
            // Sync the piano-roll scale to the new project key if it's open.
            midiEditorOverlay.setProjectKey(root, isMinor);
            updateTransportLabels();
            refreshClipInspector();
            arrangementTimeline.repaint();
            repaint();
        });
}

TimelineClip* MainComponent::getSelectedTimelineClip() noexcept
{
    if (! selectedArrangementClip.has_value())
        return nullptr;

    const auto [trackIndex, clipIndex] = *selectedArrangementClip;
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || clipIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return nullptr;

    auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
    if (clipIndex >= static_cast<int>(clips.size()))
        return nullptr;

    return &clips[static_cast<std::size_t>(clipIndex)];
}

const TimelineClip* MainComponent::getSelectedTimelineClip() const noexcept
{
    if (! selectedArrangementClip.has_value())
        return nullptr;

    const auto [trackIndex, clipIndex] = *selectedArrangementClip;
    const auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || clipIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return nullptr;

    const auto& clips = tracks[static_cast<std::size_t>(trackIndex)].clips;
    if (clipIndex >= static_cast<int>(clips.size()))
        return nullptr;

    return &clips[static_cast<std::size_t>(clipIndex)];
}

juce::Rectangle<int> MainComponent::getBrowserResizeHandleBounds() const noexcept
{
    if (! browserPanelVisible)
        return {}; // no draggable handle when the browser is collapsed

    auto bounds = getLocalBounds().reduced(8);
    bounds.removeFromTop(112);
    auto workArea = bounds;
    auto browserPanelBounds = workArea.removeFromLeft(browserPanelWidth);
    return browserPanelBounds.withTrimmedLeft(browserPanelBounds.getWidth() - (browserResizeHandleWidth / 2))
        .withWidth(browserResizeHandleWidth)
        .reduced(0, 16);
}

}  // namespace orion
