#include "MainComponent.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include "../Audio/AudioInputRecorder.h"
#include "../Audio/OrionStretchEngine.h"
#include "../Audio/PlaybackSources.h"
#include "../Audio/WarpEngine.h"
#include "../Sampler/SamplerEngine.h"
#include "OrionTheme.h"

namespace
{
namespace th = orion::theme;
const auto backgroundColour      = th::core::canvas;
const auto panelColour           = th::core::studio;
const auto accentColour          = th::warm::red;
const auto panelStroke           = th::line::subtle;
const auto mutedText             = th::text::muted;
const auto transportShelfColour  = th::core::voidBlack;
const auto transportShelfStroke  = th::line::subtle;
const auto transportButtonColour = th::core::studio;
const auto transportButtonText   = th::text::secondary;
const auto transportDarkPanel    = th::core::voidBlack;
const auto transportSectionFill  = th::core::canvas;
const auto transportSectionStroke = th::line::subtle.withAlpha(0.45f);
const auto recordAccent          = th::status::error;
constexpr double previewMaxLengthSeconds = 12.0;
constexpr int minBrowserPanelWidth = 220;
constexpr int maxBrowserPanelWidth = 520;
constexpr int browserResizeHandleWidth = 10;
constexpr int transportShelfHeight = orion::TransportBarComponent::preferredHeight;
// Browser preview plays this much below unity so dropping a sample into the playlist
// (which plays at true level) gives the Ableton-style "louder when dropped" jump.
constexpr double browserPreviewHeadroomDb = -6.0;
constexpr int transportBrandWidth = 210;
constexpr int transportClusterWidth = 264;
constexpr int transportTempoWidth = 178; // BPM + KEY combined card
constexpr int transportModeWidth = 152;
constexpr int transportUtilityWidth = 302;
constexpr int transportSectionGap = 12;
constexpr int transportControlHeight = 46;
constexpr int transportSectionHeight = 54;
constexpr int transportContentVerticalNudge = 0;
constexpr int samplerPanelHeight = 350;   // shared by the sampler and clip editor (compact lower panel)
constexpr const char* sidebarFoldersSettingsKey = "sidebar.customFolders";

enum MenuItemId
{
    menuProjectOpen = 1001,
    menuProjectSave,
    menuProjectExport,
    menuProjectSettings,
    menuEditUndo,
    menuEditRedo,
    menuMixMixer,
    menuWindowPlaylist
};

juce::String compactInspectorFileName(const juce::File& file, const juce::String& fallbackName)
{
    auto name = file.existsAsFile() ? file.getFileNameWithoutExtension() : fallbackName;
    if (name.length() <= 22)
        return name;

    return name.substring(0, 19) + "...";
}

juce::String formatTransportPosition(double playheadBeat, int numerator)
{
    const auto clampedBeat = juce::jmax(0.0, playheadBeat);
    const auto beatsPerBar = juce::jmax(1, numerator);
    const auto totalBeatIndex = static_cast<int>(std::floor(clampedBeat));
    const auto bar = (totalBeatIndex / beatsPerBar) + 1;
    const auto beat = (totalBeatIndex % beatsPerBar) + 1;
    const auto tick = static_cast<int>(std::floor((clampedBeat - std::floor(clampedBeat)) * 100.0)) + 1;
    return juce::String(bar).paddedLeft('0', 3) + "."
        + juce::String(beat).paddedLeft('0', 2) + "."
        + juce::String(tick).paddedLeft('0', 2);
}

// Playhead position as real elapsed time "M:SS.t" (minutes : seconds . tenths).
juce::String formatTransportTime(double playheadBeat, double bpm)
{
    const auto seconds = bpm > 0.0 ? juce::jmax(0.0, playheadBeat) * 60.0 / bpm : 0.0;
    const auto mins = static_cast<int>(seconds) / 60;
    const auto secs = seconds - mins * 60.0;
    return juce::String(mins) + ":" + juce::String(secs, 1).paddedLeft('0', 4);
}

std::unique_ptr<juce::PropertiesFile> makeUserSettingsFile()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "Orion";
    options.filenameSuffix = "settings";
    options.osxLibrarySubFolder = "Application Support";
    return std::make_unique<juce::PropertiesFile>(options);
}

// A modern, rounded popup menu look (dark panel, accent hover) — for the app's own menus
// so they feel native to the UI instead of the default system-grey list.
class OrionPopupMenuLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    OrionPopupMenuLookAndFeel()
    {
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff1b2027));
        setColour(juce::PopupMenu::textColourId, juce::Colours::white.withAlpha(0.92f));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, th::warm::red);
        setColour(juce::PopupMenu::highlightedTextColourId, th::text::inverse);
    }

    void drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                            const juce::PopupMenu::Options&) override
    {
        auto r = juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height).reduced(1.0f);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(r.translated(0.0f, 2.0f), 10.0f);
        g.setColour(findColour(juce::PopupMenu::backgroundColourId));
        g.fillRoundedRectangle(r, 10.0f);
        g.setColour(juce::Colour(0xff2b3640));
        g.drawRoundedRectangle(r, 10.0f, 1.0f);
    }

    void getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool isSeparator,
                                              int standardMenuItemHeight, int& idealWidth, int& idealHeight,
                                              const juce::PopupMenu::Options& o) override
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSizeWithOptions(text, isSeparator, standardMenuItemHeight,
                                                                   idealWidth, idealHeight, o);
        if (! isSeparator)
        {
            idealHeight = juce::jmax(idealHeight, 32);
            idealWidth += 28;
        }
    }

    void drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                      bool isHighlighted, const juce::PopupMenu::Item& item,
                                      const juce::PopupMenu::Options& o) override
    {
        if (item.isSeparator)
        {
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.fillRect(area.reduced(10, 0).removeFromTop(1).translated(0, area.getHeight() / 2));
            return;
        }
        auto r = area.reduced(5, 2);
        if (isHighlighted && item.isEnabled)
        {
            g.setColour(findColour(juce::PopupMenu::highlightedBackgroundColourId).withAlpha(0.92f));
            g.fillRoundedRectangle(r.toFloat(), 6.0f);
        }
        g.setColour(item.isEnabled ? (isHighlighted ? findColour(juce::PopupMenu::highlightedTextColourId)
                                                     : findColour(juce::PopupMenu::textColourId))
                                   : juce::Colours::white.withAlpha(0.4f));
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(item.text, r.reduced(12, 0), juce::Justification::centredLeft, true);
        juce::ignoreUnused(o);
    }
};

juce::LookAndFeel& orionPopupMenuLookAndFeel()
{
    static OrionPopupMenuLookAndFeel lnf;
    return lnf;
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
        if (button.getComponentID() == "play" && button.getToggleState())
            fill = accentColour;
        else if (button.getComponentID() == "play")
            fill = accentColour.withAlpha(0.88f);
        else if (shouldDrawButtonAsDown)
            fill = fill.darker(0.18f);
        else if (shouldDrawButtonAsHighlighted)
            fill = fill.brighter(0.05f);

        g.setColour(juce::Colours::black.withAlpha(0.28f));
        g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f), 8.0f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 8.0f);
        g.setColour(button.getToggleState() || button.getComponentID() == "play"
                        ? accentColour.brighter(0.22f).withAlpha(0.75f)
                        : juce::Colours::white.withAlpha(0.10f));
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
        auto iconBounds = bounds.removeFromTop(static_cast<int>(bounds.getHeight() * 0.62f)).reduced(8, 3);
        auto labelBounds = bounds.withTrimmedTop(2);
        drawTransportIcon(g, button.getComponentID(), iconBounds, textColour);

        g.setFont(juce::FontOptions(9.2f, juce::Font::bold));
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
                    bool initialOrionWarp,
                    std::function<void(int)> onBrowserWidthChanged,
                    std::function<void(int)> onExportSampleRateChanged,
                    std::function<void(bool)> onOrionWarpChanged,
                    std::function<void()> onMidiDevicesChanged,
                    std::function<void()> onSave)
        : deviceManager(manager),
          audioSelector(manager, 0, 2, 0, 2, false, false, false, false),
          browserWidthChanged(std::move(onBrowserWidthChanged)),
          exportSampleRateChanged(std::move(onExportSampleRateChanged)),
          orionWarpChanged(std::move(onOrionWarpChanged)),
          midiDevicesChanged(std::move(onMidiDevicesChanged)),
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

        orionWarpToggle.setButtonText("Orion warp engine (experimental)");
        orionWarpToggle.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
        orionWarpToggle.setToggleState(initialOrionWarp, juce::dontSendNotification);
        orionWarpToggle.onClick = [this]
        {
            if (orionWarpChanged)
                orionWarpChanged(orionWarpToggle.getToggleState());
        };
        addAndMakeVisible(orionWarpToggle);

        midiLabel.setText("MIDI Input", juce::dontSendNotification);
        midiLabel.setColour(juce::Label::textColourId, mutedText);
        addAndMakeVisible(midiLabel);

        midiRescanButton.setButtonText("Rescan");
        midiRescanButton.setColour(juce::TextButton::buttonColourId, transportDarkPanel);
        midiRescanButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        midiRescanButton.onClick = [this] { rebuildMidiDeviceList(); };
        addAndMakeVisible(midiRescanButton);

        midiEmptyLabel.setText("No MIDI devices found", juce::dontSendNotification);
        midiEmptyLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        midiEmptyLabel.setFont(juce::FontOptions(13.0f, juce::Font::italic));
        addChildComponent(midiEmptyLabel);

        rebuildMidiDeviceList();

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

    // Rebuilds the per-device enable toggles from the current list of MIDI inputs.
    // Called on construction and from the Rescan button so newly plugged-in
    // keyboards show up without reopening Settings.
    void rebuildMidiDeviceList()
    {
        midiDeviceToggles.clear();

        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& device : devices)
        {
            auto* toggle = new juce::ToggleButton(device.name);
            toggle->setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.9f));
            toggle->setToggleState(deviceManager.isMidiInputDeviceEnabled(device.identifier),
                                   juce::dontSendNotification);
            const auto identifier = device.identifier;
            toggle->onClick = [this, identifier, toggle]
            {
                deviceManager.setMidiInputDeviceEnabled(identifier, toggle->getToggleState());
                if (midiDevicesChanged)
                    midiDevicesChanged();   // (re)attach/detach the note callback immediately
            };
            addAndMakeVisible(toggle);
            midiDeviceToggles.add(toggle);
        }

        midiEmptyLabel.setVisible(devices.isEmpty());
        resized();
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

        orionWarpToggle.setBounds(area.removeFromTop(26));
        area.removeFromTop(14);

        // MIDI Input section: header row (label + Rescan), then one toggle per device.
        {
            auto headerRow = area.removeFromTop(24);
            midiRescanButton.setBounds(headerRow.removeFromRight(80));
            midiLabel.setBounds(headerRow.withTrimmedTop(2));
            area.removeFromTop(6);

            if (midiDeviceToggles.isEmpty())
            {
                midiEmptyLabel.setBounds(area.removeFromTop(22));
            }
            else
            {
                for (auto* toggle : midiDeviceToggles)
                    toggle->setBounds(area.removeFromTop(24));
            }
            area.removeFromTop(14);
        }

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
    juce::ToggleButton orionWarpToggle;
    juce::Label midiLabel;
    juce::TextButton midiRescanButton;
    juce::Label midiEmptyLabel;
    juce::OwnedArray<juce::ToggleButton> midiDeviceToggles;
    juce::Label audioLabel;
    juce::AudioDeviceManager& deviceManager;
    juce::AudioDeviceSelectorComponent audioSelector;
    juce::TextButton saveButton;
    std::function<void(int)> browserWidthChanged;
    std::function<void(int)> exportSampleRateChanged;
    std::function<void(bool)> orionWarpChanged;
    std::function<void()> midiDevicesChanged;
    std::function<void()> saveCallback;
};

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "Project", "Edit", "Mix", "Window" };
}

juce::PopupMenu MainComponent::getMenuForIndex(int, const juce::String& menuName)
{
    juce::PopupMenu menu;

    if (menuName == "Project")
    {
        menu.addItem(menuProjectOpen, "Open...");
        menu.addItem(menuProjectSave, "Save");
        menu.addItem(menuProjectExport, "Export...");
        menu.addSeparator();
        menu.addItem(menuProjectSettings, "Settings...");
    }
    else if (menuName == "Edit")
    {
        menu.addItem(menuEditUndo, "Undo", arrangementTimeline.canUndo());
        menu.addItem(menuEditRedo, "Redo", arrangementTimeline.canRedo());
    }
    else if (menuName == "Mix")
    {
        menu.addItem(menuMixMixer, mixerPanel.isVisible() ? "Close Mixer" : "Open Mixer");
    }
    else if (menuName == "Window")
    {
        menu.addItem(menuWindowPlaylist, "Playlist");
    }

    return menu;
}

void MainComponent::menuItemSelected(int menuItemID, int)
{
    switch (menuItemID)
    {
        case menuProjectOpen:     openProjectInteractively(); break;
        case menuProjectSave:     saveProjectInteractively(); break;
        case menuProjectExport:   exportProjectInteractively(); break;
        case menuProjectSettings: openSettingsDialog(); break;
        case menuEditUndo:
            arrangementTimeline.undo();
            updateTransportLabels();
            break;
        case menuEditRedo:
            arrangementTimeline.redo();
            updateTransportLabels();
            break;
        case menuMixMixer:        toggleMixerFromUi(); break;
        case menuWindowPlaylist:  resetToPlaylistView(); break;
        default: break;
    }
}

MainComponent::MainComponent()
    : transportEngine(projectState),
      transportController(projectState, transportEngine),
      arrangementTimeline(projectState, transportEngine),
      mixerPanel(projectState)
{
    setWantsKeyboardFocus(true);

    headerLabel.setText("ORION", juce::dontSendNotification);
    headerLabel.setFont(juce::FontOptions(27.0f, juce::Font::bold));
    headerLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.96f));
    addAndMakeVisible(headerLabel);
    headerLabel.setVisible(false);

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
    // Match the transport readout's value font so the number doesn't change size
    // when you click to edit it.
    const juce::Font bpmEditorFont(juce::FontOptions(18.0f, juce::Font::bold));
    bpmEditor.setFont(bpmEditorFont);
    bpmEditor.applyFontToAllText(bpmEditorFont);
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
    statusLabel.setVisible(false);

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

    selectionInspector.onGainChanged = [this](double gainDb)
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->gainDb = gainDb;
            clipGainSlider.setValue(gainDb, juce::dontSendNotification);
            clipGainValueLabel.setText(juce::String(gainDb, 1) + " dB", juce::dontSendNotification);
            arrangementTimeline.repaint();
            return;
        }

        if (const auto trackIndex = arrangementTimeline.getSelectedTrackIndex(); trackIndex.has_value())
        {
            auto& tracks = projectState.getTracks();
            if (*trackIndex >= 0 && *trackIndex < static_cast<int>(tracks.size()))
            {
                tracks[static_cast<std::size_t>(*trackIndex)].volumeDb = gainDb;
                arrangementTimeline.repaint();
            }
        }
    };

    selectionInspector.onMuteChanged = [this](bool shouldMute)
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->muted = shouldMute;
            clipMuteToggle.setToggleState(shouldMute, juce::dontSendNotification);
            arrangementTimeline.repaint();
            return;
        }

        if (const auto trackIndex = arrangementTimeline.getSelectedTrackIndex(); trackIndex.has_value())
        {
            auto& tracks = projectState.getTracks();
            if (*trackIndex >= 0 && *trackIndex < static_cast<int>(tracks.size()))
            {
                tracks[static_cast<std::size_t>(*trackIndex)].muted = shouldMute;
                arrangementTimeline.repaint();
            }
        }
    };

    selectionInspector.onSoloChanged = [this](bool shouldSolo)
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->solo = shouldSolo;
            clipSoloToggle.setToggleState(shouldSolo, juce::dontSendNotification);
            arrangementTimeline.repaint();
            return;
        }

        if (const auto trackIndex = arrangementTimeline.getSelectedTrackIndex(); trackIndex.has_value())
        {
            auto& tracks = projectState.getTracks();
            if (*trackIndex >= 0 && *trackIndex < static_cast<int>(tracks.size()))
            {
                tracks[static_cast<std::size_t>(*trackIndex)].solo = shouldSolo;
                arrangementTimeline.repaint();
            }
        }
    };

    selectionInspector.onWarpChanged = [this](bool shouldWarp)
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

            clip->warpEnabled = shouldWarp;
            clipWarpToggle.setToggleState(shouldWarp, juce::dontSendNotification);
            refreshAudioClipWarpLengths();
            refreshClipInspector();
            arrangementTimeline.repaint();
        }
    };
    selectionInspector.onRequestLiveLevel = [this]() -> float
    {
        int trackIndex = -1;
        if (selectedArrangementClip.has_value())
            trackIndex = selectedArrangementClip->first;
        else if (const auto selectedTrack = arrangementTimeline.getSelectedTrackIndex(); selectedTrack.has_value())
            trackIndex = *selectedTrack;

        return (trackIndex >= 0 && trackIndex < static_cast<int>(trackPeakHoldDb.size()))
            ? juce::jlimit(0.0f, 1.0f, juce::Decibels::decibelsToGain(trackPeakHoldDb[static_cast<std::size_t>(trackIndex)]))
            : 0.0f;
    };
    selectionInspector.onRequestLiveLevelDb = [this]() -> float
    {
        int trackIndex = -1;
        if (selectedArrangementClip.has_value())
            trackIndex = selectedArrangementClip->first;
        else if (const auto selectedTrack = arrangementTimeline.getSelectedTrackIndex(); selectedTrack.has_value())
            trackIndex = *selectedTrack;

        return (trackIndex >= 0 && trackIndex < static_cast<int>(trackPeakHoldDb.size()))
            ? trackPeakHoldDb[static_cast<std::size_t>(trackIndex)]
            : -100.0f;
    };
    addAndMakeVisible(selectionInspector);
    selectionInspector.setVisible(false);

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
    mixerButton.setButtonText("MIXER");
    openButton.setButtonText("OPEN");
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
    mixerButton.setComponentID("mixer");
    openButton.setComponentID("open");
    saveButton.setComponentID("save");
    exportButton.setComponentID("export");
    settingsButton.setComponentID("settings");

    for (auto* button : { &playButton, &stopButton, &recordButton, &rewindButton, &undoButton, &redoButton,
                          &metronomeButton, &loopButton, &countInButton, &browserButton, &scanPluginsButton,
                          &mixerButton, &openButton, &saveButton, &exportButton, &settingsButton })
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

    // Collapse arrow removed — browser show/hide now lives on the FILES sidebar button.
    browserCollapseArrow.setToggleState(browserPanelVisible, juce::dontSendNotification);
    browserCollapseArrow.addListener(this);
    addChildComponent(browserCollapseArrow);   // kept as a hidden listener target, never shown
    recordButton.setClickingTogglesState(true);
    metronomeButton.setToggleState(false, juce::dontSendNotification);
    loopButton.setToggleState(false, juce::dontSendNotification);
    countInButton.setToggleState(projectState.isRecordWithCountIn(), juce::dontSendNotification);

    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xfff7e5e5));
    recordButton.setColour(juce::TextButton::textColourOffId, recordAccent.darker(0.2f));
    rewindButton.setVisible(false);
    scanPluginsButton.setVisible(false);

    for (auto* legacyTransportControl : { static_cast<juce::Component*>(&playButton),
                                          static_cast<juce::Component*>(&stopButton),
                                          static_cast<juce::Component*>(&recordButton),
                                          static_cast<juce::Component*>(&rewindButton),
                                          static_cast<juce::Component*>(&undoButton),
                                          static_cast<juce::Component*>(&redoButton),
                                          static_cast<juce::Component*>(&metronomeButton),
                                          static_cast<juce::Component*>(&loopButton),
                                          static_cast<juce::Component*>(&countInButton),
                                          static_cast<juce::Component*>(&mixerButton),
                                          static_cast<juce::Component*>(&openButton),
                                          static_cast<juce::Component*>(&saveButton),
                                          static_cast<juce::Component*>(&exportButton),
                                          static_cast<juce::Component*>(&settingsButton),
                                          static_cast<juce::Component*>(&bpmValueLabel),
                                          static_cast<juce::Component*>(&bpmCaptionLabel),
                                          static_cast<juce::Component*>(&meterValueLabel),
                                          static_cast<juce::Component*>(&meterCaptionLabel) })
        legacyTransportControl->setVisible(false);

    transportBar.onPlay = [this]() { toggleTransportFromUi(); };
    transportBar.onStop = [this]() { stopTransportFromUi(); };
    transportBar.onSkipToStart = [this]()
    {
        transportEngine.setPlayheadBeat(0.0);
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->syncToTransportPosition();
        updateTransportLabels();
        arrangementTimeline.repaint();
    };
    transportBar.onRecordChanged = [this](bool shouldRecord)
    {
        recordButton.setToggleState(shouldRecord, juce::dontSendNotification);
        transportController.setRecordArmed(shouldRecord);
        if (! transportEngine.isRecordArmed())
        {
            finalizeRecordingClip();
            finalizeAudioRecordingClip();
        }
        startMidiRecordingFromRecordButtonIfNeeded();
        recordButton.setToggleState(transportEngine.isRecordArmed(), juce::dontSendNotification);
        updateTransportLabels();
    };
    transportBar.onRecordOptions = [this]()
    {
        juce::PopupMenu menu;
        const auto withMetro = projectState.isRecordWithMetronome();
        const auto withPrecount = projectState.isRecordWithCountIn();
        menu.addItem(1, "Record with metronome", true, withMetro && ! withPrecount);
        menu.addItem(2, "Record without metronome", true, ! withMetro && ! withPrecount);
        menu.addItem(3, "4-count before recording", true, withPrecount);
        menu.showMenuAsync(juce::PopupMenu::Options{}
                                .withTargetComponent(&transportBar)
                                .withTargetScreenArea(transportBar.localAreaToGlobal(transportBar.getRecordOptionsBounds())),
            [this](int result)
            {
                if (result == 1)
                {
                    projectState.setRecordWithMetronome(true);
                    projectState.setRecordWithCountIn(false);
                    metronomeButton.setToggleState(true, juce::dontSendNotification);
                    countInButton.setToggleState(false, juce::dontSendNotification);
                }
                else if (result == 2)
                {
                    projectState.setRecordWithMetronome(false);
                    projectState.setRecordWithCountIn(false);
                    metronomeButton.setToggleState(false, juce::dontSendNotification);
                    countInButton.setToggleState(false, juce::dontSendNotification);
                }
                else if (result == 3)
                {
                    projectState.setRecordWithMetronome(false);
                    projectState.setRecordWithCountIn(true);
                    metronomeButton.setToggleState(false, juce::dontSendNotification);
                    countInButton.setToggleState(true, juce::dontSendNotification);
                }
                updateTransportLabels();
            });
    };
    transportBar.onMetronomeChanged = [this](bool enabled)
    {
        metronomeButton.setToggleState(enabled, juce::dontSendNotification);
        if (enabled)
        {
            projectState.setRecordWithMetronome(true);
            projectState.setRecordWithCountIn(false);
            countInButton.setToggleState(false, juce::dontSendNotification);
        }
        else
        {
            projectState.setRecordWithMetronome(false);
        }
        updateTransportLabels();
    };
    transportBar.onLoopChanged = [this](bool enabled)
    {
        loopButton.setToggleState(enabled, juce::dontSendNotification);
        toggleLoopFromUi();
    };
    transportBar.onTempoEdit = [this]() { beginTempoEditing(); };
    transportBar.onKeySelect = [this]() { showKeySelectionMenu(); };
    transportBar.onMixer = [this]() { toggleMixerFromUi(); };
    transportBar.onClipEditor = [this]() { toggleClipEditorFromUi(); };
    transportBar.onStepSequencer = [this]() { toggleStepSequencerFromUi(); };
    addAndMakeVisible(transportBar);

    stepSequencer.onOpenPianoRoll = [this](int trackIndex, int clipIndex)
    {
        auto& tracks = projectState.getTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
            return;
        auto& track = tracks[static_cast<std::size_t>(trackIndex)];
        if (clipIndex < 0 || clipIndex >= static_cast<int>(track.clips.size()))
            return;
        midiEditorOverlay.openClip(track, track.clips[static_cast<std::size_t>(clipIndex)],
                                   projectState.getKeyRoot(),
                                   projectState.isKeyMinor(),
                                   projectState.isKeyEnabled() && projectState.isScaleLockEnabled());
        midiEditorOverlay.setChordModeExternally(projectState.isChordModeEnabled(), projectState.getChordSizeNotes());
    };
    stepSequencer.onPatternEdited = [this]() { arrangementTimeline.repaint(); };
    stepSequencer.onOpenSampler = [this](int trackIndex)
    {
        // Click a channel/sample in the rack → open its manipulators (the sampler panel).
        // Remember we came from the step rack so closing the sampler returns there.
        if (openSamplerForTrackIfAvailable(trackIndex))
            samplerOpenedFromStep = true;
    };
    // Helper: load a sample into a track as a step-sequencer channel (one-shot, no clip / no
    // sampler panel — we stay in the rack).
    auto loadSamplerChannel = [this](int trackIndex, const juce::File& file)
    {
        auto& tracks = projectState.getTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()) || ! file.existsAsFile())
            return;
        auto& track = tracks[static_cast<std::size_t>(trackIndex)];
        const auto analysis = analyzeAudioWarpMetadata(file, projectState.getTempoBpm(), projectState.getNumerator());
        track.samplerSourcePath = file.getFullPathName();
        track.samplerSourceDurationSeconds = analysis.durationSeconds;
        track.samplerSourceBpm = analysis.sourceBpm;
        track.samplerDetectedBars = analysis.detectedBars;
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->prewarmSamplerWarp(track.samplerSourcePath, track.samplerSourceBpm);
    };
    stepSequencer.onAddChannelFromSample = [this, loadSamplerChannel](juce::File file)
    {
        if (! file.existsAsFile())
            return;
        auto& tracks = projectState.getTracks();
        TrackState t;
        t.name = file.getFileNameWithoutExtension();
        t.isMidiTrack = true;
        t.samplerMode = SamplerPlaybackMode::oneShot;   // drum-style one-shot per step
        t.colour = theme::tracks::colourForIndex(static_cast<int>(tracks.size()));   // distinct per channel
        tracks.push_back(std::move(t));
        loadSamplerChannel(static_cast<int>(tracks.size()) - 1, file);
        arrangementTimeline.repaint();
        stepSequencer.repaint();
        resized();
    };
    stepSequencer.onAssignSampleToChannel = [this, loadSamplerChannel](int trackIndex, juce::File file)
    {
        loadSamplerChannel(trackIndex, file);
        arrangementTimeline.repaint();
        stepSequencer.repaint();
    };
    // NB: selecting a channel in the rack does NOT touch the playlist selection — the rack has
    // its own highlight, and Enter-replace reads the rack's selected channel (see below).
    addChildComponent(stepSequencer);

    juce::MenuBarModel::setMacMainMenu(this);

    addAndMakeVisible(arrangementTimeline);
    addAndMakeVisible(sidebarNav);
    loadSidebarBrowserFolders();
    addAndMakeVisible(browserPanel);
    addAndMakeVisible(midiEditorOverlay);
    addChildComponent(clipEditorPanel);
    addAndMakeVisible(samplerPanel);
    addChildComponent(mixerPanel);
    audioFormatManager.registerBasicFormats();
    arrangementPlaybackSource = std::make_unique<ArrangementPlaybackSource>(projectState, transportEngine, audioFormatManager);
    clickTrackSource = std::make_unique<ClickTrackSource>(projectState, transportEngine,
                                                          [this]() { return metronomeButton.getToggleState(); });
    audioInputRecorder = std::make_unique<AudioInputRecorder>();
    // Open output only at launch. Audio input is enabled lazily when an audio track is
    // armed/recorded. We request mic permission separately below, without opening input,
    // so macOS shows one first-run prompt instead of a second CoreAudio prompt.
    audioDeviceManager.initialise(0, 2, nullptr, true);
    refreshMidiInputDevices();
    requestMicrophonePermissionAtLaunch();
    audioDeviceManager.addAudioCallback(&previewSourcePlayer);
    masterMixerSource.addInputSource(&previewTransportSource, false);
    masterMixerSource.addInputSource(&clipEditorPreviewTransportSource, false);
    masterMixerSource.addInputSource(arrangementPlaybackSource.get(), false);
    // NOTE: the click is NOT mixed here — it's a monitor-only signal attached to the
    // master stage below, added after metering so it never inflates the master level.
    // Master stage (gain + level metering) sits between the mixer and the device.
    masterStripSource = std::make_unique<MasterStripSource>(masterMixerSource);
    masterStripSource->setMonitorSource(clickTrackSource.get());
    masterStripSource->setGainDb(masterGainDb);
    previewSourcePlayer.setSource(masterStripSource.get());

    mixerPanel.onClose = [this]() { arrangementTimeline.grabKeyboardFocus(); };
    mixerPanel.onTrackChanged = [this]()
    {
        arrangementTimeline.repaint();
        refreshClipInspector();
    };
    mixerPanel.onSetMasterGainDb = [this](double db)
    {
        masterGainDb = db;
        if (masterStripSource != nullptr)
            masterStripSource->setGainDb(db);
    };
    mixerPanel.onRequestMasterGainDb = [this]() { return masterGainDb; };
    // The peaks are fetched/reset once per tick in updateTrackMeterLevels() (single
    // consumer); these callbacks just return the stored values so the mixer and the
    // bottom bar never steal peaks from each other.
    mixerPanel.onRequestMasterPeak = [this]() -> float
    {
        return juce::jmax(masterRawPeakL, masterRawPeakR);
    };
    mixerPanel.onRequestMasterPeakStereo = [this]() -> std::pair<float, float>
    {
        return { masterRawPeakL, masterRawPeakR };
    };
    // Unified (host-owned) master readouts so the mixer master matches the track meters.
    mixerPanel.onRequestMasterLevelStereo = [this]() -> std::pair<float, float>
    {
        return { masterMeterLevelL, masterMeterLevelR };
    };
    mixerPanel.onRequestMasterLevelDb = [this]() -> float { return masterMeterDb; };
    mixerPanel.onInsertClicked = [this](int trackIndex, int insertIndex)
    {
        if (trackIndex >= 0)
            showInsertMenuForTrack(trackIndex, insertIndex);
    };
    mixerPanel.onInsertMoved = [this](int fromTrack, int fromIndex, int toTrack, int toIndex)
    {
        if (fromTrack == toTrack)
            moveInsert(fromTrack, fromIndex, toTrack, toIndex);   // reorder within the track
        else
            copyInsertToTrack(fromTrack, fromIndex, toTrack);     // copy onto another track
    };
    mixerPanel.onAddBus = [this]() { addBus(); };
    mixerPanel.onSendClicked = [this](int trackIndex, int sendIndex)
    {
        if (trackIndex >= 0)
            showSendMenuForTrack(trackIndex, sendIndex);
    };
    mixerPanel.onSendLevelChanged = [this](int trackIndex, int sendIndex, float level)
    {
        auto& tracks = projectState.getTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) return;
        auto& sends = tracks[static_cast<std::size_t>(trackIndex)].sends;
        if (sendIndex < 0 || sendIndex >= static_cast<int>(sends.size())) return;
        sends[static_cast<std::size_t>(sendIndex)].level = juce::jlimit(0.0, 1.0, static_cast<double>(level));
        if (mixerPanel.isVisible()) mixerPanel.repaint();
    };
    mixerPanel.onSelectTrack = [this](int trackIndex)
    {
        arrangementTimeline.selectTrack(trackIndex);
    };
    mixerPanel.onOutputRouteClicked = [this](int trackIndex)
    {
        showOutputRouteMenuForTrack(trackIndex);
    };
    mixerPanel.onRequestBusLevelStereo = [this](int b) -> std::pair<float, float>
    {
        const auto toBar = [](float lin)
        {
            const auto db = lin > 0.0f ? juce::Decibels::gainToDecibels(lin, -60.0f) : -60.0f;
            return juce::jlimit(0.0f, 1.0f, juce::jmap(db, -60.0f, 0.0f, 0.0f, 1.0f));
        };
        if (b < 0 || b >= static_cast<int>(busMeterLevelsL.size())) return { 0.0f, 0.0f };
        return { toBar(busMeterLevelsL[static_cast<std::size_t>(b)]), toBar(busMeterLevelsR[static_cast<std::size_t>(b)]) };
    };
    mixerPanel.onRequestBusLevelDb = [this](int b) -> float
    {
        return (b >= 0 && b < static_cast<int>(busPeakHoldDb.size())) ? busPeakHoldDb[static_cast<std::size_t>(b)] : -100.0f;
    };

    clipEditorPanel.onGainChanged = [this](double gainDb)
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            // The audio render reads clip.gainDb live per block, so this is instant. Keep the handler
            // LIGHT — no full clip-editor rebuild / waveform redraw per drag tick (that made the slider
            // feel laggy). The GAIN readouts refresh on the next timer tick anyway.
            clip->gainDb = juce::jlimit(-24.0, 12.0, gainDb);
            arrangementTimeline.repaint();
        }
    };
    clipEditorPanel.onWarpChanged = [this](bool shouldWarp)
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

            clip->warpEnabled = shouldWarp;
            refreshAudioClipWarpLengths();
            rebuildArrangementWarpNonBlocking();
            refreshClipInspector();
            refreshClipEditor();
            arrangementTimeline.repaint();
        }
    };
    clipEditorPanel.onKeyShiftChanged = [this](bool enabled)
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->keyShiftEnabled = enabled;
            rebuildArrangementWarpNonBlocking();
            refreshClipEditor();
            arrangementTimeline.repaint();
        }
    };
    clipEditorPanel.onTransposeChanged = [this](int semitones)
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            clip->transposeSemitones = juce::jlimit(-24, 24, semitones);
            rebuildArrangementWarpNonBlocking();

            // If the clip-editor preview is playing, re-pitch it live and continue
            // from the same spot — no stop/replay needed. The resume position is
            // carried through to the (possibly background) rebuild.
            if (clipEditorPreviewTransportSource.isPlaying())
            {
                clipEditorPreviewResumeSeconds = clipEditorPreviewTransportSource.getCurrentPosition();
                startClipEditorPreview();
            }

            refreshClipEditor();
            arrangementTimeline.repaint();
        }
    };
    clipEditorPanel.onSampleRangeChanged = [this](double startRatio, double endRatio)
    {
        if (selectedArrangementClip.has_value())
        {
            const auto start = juce::jlimit(0.0, 0.999, startRatio);
            const auto end = juce::jlimit(start + 0.001, 1.0, endRatio);
            clipEditorPreviewClip = selectedArrangementClip;
            clipEditorPreviewStartRatio = start;
            clipEditorPreviewEndRatio = end;
            // During the drag we only move the marker visually; the playing loop is left
            // alone (no live re-loop, no flicker, no crackle). The loop is repositioned and
            // playback jumps to the new start on mouse-release (onSampleRangeFinalized).
            if (! clipEditorPreviewTransportSource.isPlaying())
                clipEditorPreviewPlayheadRatio = juce::jlimit(start, end, clipEditorPreviewPlayheadRatio);
            clipEditorSelectionRanges[*selectedArrangementClip] = { start, end };

            // Move the PLAYLIST playhead in sync with the START marker (purely visual — the
            // clip itself isn't touched). The marker's position along the source maps to a
            // point inside the clip on the timeline.
            if (! transportEngine.isPlaying())
            {
                if (auto* clip = getSelectedTimelineClip(); clip != nullptr)
                {
                    const auto beat = clip->startBeat + start * clip->lengthInBeats;
                    transportEngine.setPlayheadBeat(beat);
                    if (arrangementPlaybackSource != nullptr)
                        arrangementPlaybackSource->syncToTransportPosition();
                    arrangementTimeline.repaint();
                }
            }

            refreshClipEditor();
        }
    };
    clipEditorPanel.onSampleRangeFinalized = [this](double startRatio, double endRatio, bool startMoved)
    {
        if (! selectedArrangementClip.has_value())
            return;
        const auto start = juce::jlimit(0.0, 0.999, startRatio);
        const auto end = juce::jlimit(start + 0.001, 1.0, endRatio);
        clipEditorPreviewStartRatio = start;
        clipEditorPreviewEndRatio = end;
        clipEditorSelectionRanges[*selectedArrangementClip] = { start, end };
        // Don't modify the clip in the playlist (no trim / no length / no tempo change) —
        // just move the clip-editor's playback line to the new START so it follows the marker.

        // AKAI MPC-style: on release, reposition the loop to the final selection. If the
        // START marker was moved, jump playback to the new start so it plays from there.
        if (clipEditorPreviewTransportSource.isPlaying() && clipEditorPreviewStreamSource != nullptr)
        {
            clipEditorPreviewStreamSource->setLoopBounds(start, end);
            if (startMoved)
                clipEditorPreviewTransportSource.setPosition(start * clipEditorLocalPreviewDurationSeconds);
        }
        else
        {
            clipEditorPreviewPlayheadRatio = juce::jlimit(start, end, clipEditorPreviewPlayheadRatio);
        }
        refreshClipEditor();
    };
    clipEditorPanel.onPreviewSeek = [this](double ratio)
    {
        setClipEditorLocalPreviewPosition(ratio);
        refreshClipEditor();
    };
    clipEditorPanel.onNormalize = [this]() { normalizeSelectedAudioClip(); };

    // Warp markers. Mutate the clip, keep them sorted by source position, refresh the editor, and — if
    // the loop preview is running — rebuild it so the piecewise warp is heard immediately.
    auto applyWarpMarkerChange = [this]()
    {
        refreshClipEditor();
        if (clipEditorPreviewTransportSource.isPlaying())
            startClipEditorPreview();
    };
    clipEditorPanel.onWarpMarkerAdded = [this, applyWarpMarkerChange](double sourceRatio, double beat)
    {
        if (auto* clip = getSelectedTimelineClip())
        {
            arrangementTimeline.captureUndoSnapshot();   // so Ctrl+Z removes the marker, not the track
            clip->warpMarkers.push_back({ sourceRatio, beat });
            std::sort(clip->warpMarkers.begin(), clip->warpMarkers.end(),
                      [](const auto& a, const auto& b) { return a.sourceRatio < b.sourceRatio; });
            applyWarpMarkerChange();
        }
    };
    clipEditorPanel.onWarpMarkerMoved = [this, applyWarpMarkerChange](int index, double beat)
    {
        if (auto* clip = getSelectedTimelineClip(); clip != nullptr
            && index >= 0 && index < static_cast<int>(clip->warpMarkers.size()))
        {
            arrangementTimeline.captureUndoSnapshot();
            clip->warpMarkers[static_cast<std::size_t>(index)].beat = beat;
            applyWarpMarkerChange();
        }
    };
    clipEditorPanel.onWarpMarkerRemoved = [this, applyWarpMarkerChange](int index)
    {
        if (auto* clip = getSelectedTimelineClip(); clip != nullptr
            && index >= 0 && index < static_cast<int>(clip->warpMarkers.size()))
        {
            arrangementTimeline.captureUndoSnapshot();
            clip->warpMarkers.erase(clip->warpMarkers.begin() + index);
            applyWarpMarkerChange();
        }
    };

    // Both the mixer strips and the timeline track headers read the decayed levels
    // that this component maintains (single owner — see updateTrackMeterLevels()).
    const auto linearLevelForTrack = [this](int trackIndex) -> float
    {
        return (trackIndex >= 0 && trackIndex < static_cast<int>(trackMeterLevels.size()))
                   ? trackMeterLevels[static_cast<std::size_t>(trackIndex)]
                   : 0.0f;
    };
    // 0..1 bar height (mapped over -60..0 dB).
    const auto requestTrackLevel = [linearLevelForTrack](int trackIndex) -> float
    {
        const auto lin = linearLevelForTrack(trackIndex);
        const auto db = lin > 0.0f ? juce::Decibels::gainToDecibels(lin, -60.0f) : -60.0f;
        return juce::jlimit(0.0f, 1.0f, juce::jmap(db, -60.0f, 0.0f, 0.0f, 1.0f));
    };
    // Held peak level in dB (Logic-style hold; returns -100 when silent → "-inf").
    const auto requestTrackLevelDb = [this](int trackIndex) -> float
    {
        return (trackIndex >= 0 && trackIndex < static_cast<int>(trackPeakHoldDb.size()))
                   ? trackPeakHoldDb[static_cast<std::size_t>(trackIndex)]
                   : -100.0f;
    };
    // Stereo levels for the timeline header meters (each channel mapped over -60..0 dB).
    const auto requestTrackLevelStereo = [this](int trackIndex) -> std::pair<float, float>
    {
        const auto toBar = [](float lin)
        {
            const auto db = lin > 0.0f ? juce::Decibels::gainToDecibels(lin, -60.0f) : -60.0f;
            return juce::jlimit(0.0f, 1.0f, juce::jmap(db, -60.0f, 0.0f, 0.0f, 1.0f));
        };
        if (trackIndex < 0 || trackIndex >= static_cast<int>(trackMeterLevelsL.size()))
            return { 0.0f, 0.0f };
        return { toBar(trackMeterLevelsL[static_cast<std::size_t>(trackIndex)]),
                 toBar(trackMeterLevelsR[static_cast<std::size_t>(trackIndex)]) };
    };
    mixerPanel.onRequestTrackLevel = requestTrackLevel;
    mixerPanel.onRequestTrackLevelStereo = requestTrackLevelStereo;
    mixerPanel.onRequestTrackLevelDb = requestTrackLevelDb;
    arrangementTimeline.onRequestTrackLevel = requestTrackLevel;
    arrangementTimeline.onRequestTrackLevelDb = requestTrackLevelDb;
    arrangementTimeline.onRequestTrackLevelStereo = requestTrackLevelStereo;
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
        // Double-click a sound → create a sampler track and open its UI only. Do NOT drop an audio
        // clip into the playlist (that's what dragging the sample onto a lane is for).
        loadBrowserItemIntoSampler(item, /*addClipToPlaylist*/ false);
    };
    browserPanel.onOpenInSampler = [this](const BrowserItem& item)
    {
        loadBrowserItemIntoSampler(item, /*addClipToPlaylist*/ false);   // sampler track only
    };
    browserPanel.onAddItemToPlaylist = [this](const BrowserItem& item)
    {
        // Enter in the browser: add the sound to the playlist as a sampler track + clip.
        loadBrowserItemIntoSampler(item, /*addClipToPlaylist*/ true);
    };
    browserPanel.onReplaceSelectedTrackSample = [this](const BrowserItem& item)
    {
        if (item.isDirectory || ! item.file.existsAsFile())
            return;

        // If audio clips are selected in the playlist, replace just those clips' sample.
        if (const int replaced = arrangementTimeline.replaceSelectedAudioClipsSource(item.file); replaced > 0)
        {
            statusLabel.setText("Replaced sample in " + juce::String(replaced)
                                + (replaced == 1 ? " clip" : " clips"), juce::dontSendNotification);
            return;
        }

        auto& tracks = projectState.getTracks();
        // Prefer the channel selected in the step sequencer (when it's open); otherwise the
        // track selected in the playlist.
        int target = -1;
        if (stepSequencer.isVisible() && stepSequencer.selectedChannelTrackIndex() >= 0)
            target = stepSequencer.selectedChannelTrackIndex();
        else if (const auto sel = arrangementTimeline.getSelectedTrackIndex(); sel.has_value())
            target = *sel;

        if (target >= 0 && target < static_cast<int>(tracks.size())
            && tracks[static_cast<std::size_t>(target)].isMidiTrack)
        {
            // Replace the selected sampler/instrument track's sound in place.
            auto& track = tracks[static_cast<std::size_t>(target)];
            const auto analysis = analyzeAudioWarpMetadata(item.file, projectState.getTempoBpm(), projectState.getNumerator());
            track.samplerSourcePath = item.file.getFullPathName();
            track.samplerSourceDurationSeconds = analysis.durationSeconds;
            track.samplerSourceBpm = analysis.sourceBpm;
            track.samplerDetectedBars = analysis.detectedBars;
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->prewarmSamplerWarp(track.samplerSourcePath, track.samplerSourceBpm);
            statusLabel.setText("Replaced sample: " + track.name, juce::dontSendNotification);
            stepSequencer.repaint();
            arrangementTimeline.repaint();
        }
        else
        {
            loadBrowserItemIntoSampler(item);   // no sampler track selected → load as a new one
        }
    };
    browserPanel.onCloseRequested = [this]
    {
        // Wired to the × button inside the browser header — collapses the panel and
        // lets the playlist expand into the freed horizontal space.
        browserPanelVisible = false;       // timer slides it closed smoothly
        startTimerHz(60);
        resized();
        repaint();
    };
    browserPanel.onTogglePreviewPlayback = [this]
    {
        if (previewTransportSource.isPlaying() || pendingBrowserPreviewStart)
        {
            pendingBrowserPreviewStart = false;
            browserPanel.setPreviewArmed(false);
            previewTransportSource.stop();
        }
        else if (previewBufferSource != nullptr)
        {
            armOrStartBrowserPreview();
        }
    };
    browserPanel.onDragStarted = [this]
    {
        // Silence the browser preview as soon as the sample is dragged toward the playlist.
        stopBrowserPreview(true);
    };
    browserPanel.onPreviewBpmSyncToggled = [this]
    {
        // Reload preview with the new sync mode if a sample is already loaded.
        auto selected = browserPanel.getSelectedItem();
        if (selected.has_value() && previewBufferSource != nullptr)
        {
            currentPreviewTempoBpm = 0.0;  // invalidate cache
            playBrowserPreview(*selected);
        }
    };
    browserPanel.onRootFolderChosen = [this](const juce::File& folder)
    {
        if (! folder.isDirectory())
            return;

        const auto folderPath = folder.getFullPathName();
        const auto alreadyAdded = std::any_of(sidebarBrowserFolders.begin(), sidebarBrowserFolders.end(),
                                              [&folderPath](const juce::File& existing)
                                              {
                                                  return existing.getFullPathName() == folderPath;
                                              });
        if (! alreadyAdded)
        {
            sidebarBrowserFolders.push_back(folder);
            sidebarNav.setCustomFolders(sidebarBrowserFolders);
            saveSidebarBrowserFolders();
        }

        browserPanelVisible = true;
        browserButton.setToggleState(true, juce::dontSendNotification);
        browserPanel.setVisible(true);
        browserPanel.openFolder(folder);
        sidebarNav.setActiveFolder(folder);
        startTimerHz(60);
        resized();
        repaint();
    };
    sidebarNav.onFolderSelected = [this](const juce::File& folder)
    {
        if (! folder.isDirectory())
            return;

        browserPanelVisible = true;
        browserButton.setToggleState(true, juce::dontSendNotification);
        browserPanel.setVisible(true);
        browserPanel.openFolder(folder);
        startTimerHz(60);
        resized();
        repaint();
    };
    sidebarNav.onItemSelected = [this](SidebarNavItem item)
    {
        if (item == SidebarNavItem::files || item == SidebarNavItem::samples)
        {
            const auto shouldCollapse = browserPanelVisible && browserPanel.isShowingRootLocations();
            browserPanelVisible = ! shouldCollapse;
            browserButton.setToggleState(browserPanelVisible, juce::dontSendNotification);
            if (browserPanelVisible)
            {
                browserPanel.setVisible(true);
                browserPanel.showRootLocations();
            }
            startTimerHz(60);
            resized();
            repaint();
            return;
        }

        if (item == SidebarNavItem::addFolder)
        {
            browserPanelVisible = true;
            browserButton.setToggleState(true, juce::dontSendNotification);
            browserPanel.setVisible(true);
            startTimerHz(60);
            resized();
            repaint();
            browserPanel.chooseRootFolder();
            return;
        }

        if (item == SidebarNavItem::vst)
        {
            // Open the instrument picker for the selected MIDI track. Do NOT create a track
            // up-front — a new MIDI (instrument) track is only materialised if the user
            // actually picks a plugin (handled in pluginPicker.onPick). Cancelling the picker
            // leaves the project untouched.
            auto& tracks = projectState.getTracks();
            const auto sel = arrangementTimeline.getSelectedTrackIndex();
            int target = -1;
            if (sel.has_value() && *sel >= 0 && *sel < static_cast<int>(tracks.size())
                && tracks[static_cast<std::size_t>(*sel)].isMidiTrack)
                target = *sel;

            showInstrumentPicker(target);   // target == -1 → create on pick
            return;
        }

        if (item == SidebarNavItem::add)
        {
            juce::StringArray busNames;
            for (const auto& b : projectState.getBuses())
                busNames.add(b.name);
            addTrackDialog.setBounds(getLocalBounds());
            addTrackDialog.show(static_cast<int>(projectState.getTracks().size()),
                                busNames,
                                pluginManager.getInstrumentDescriptions());
            addTrackDialog.toFront(true);
        }
    };

    // Modern instrument picker overlay (replaces the native instrument popup menu).
    pluginPicker.onPick = [this](const juce::PluginDescription& desc)
    {
        int target = pluginPickerTargetTrack;
        if (target < 0)
        {
            // No suitable MIDI track existed when the picker opened — create one now, only
            // because the user actually chose a plugin.
            arrangementTimeline.addMidiTrack();
            target = static_cast<int>(projectState.getTracks().size()) - 1;
            refreshClipInspector();
            resized();
            repaint();
        }
        loadInstrumentOnTrack(target, desc);
    };
    pluginPicker.onRescan = [this]()
    {
        scanPluginsInteractively([this]()
        {
            if (pluginPicker.isVisible())
                pluginPicker.show("Load Instrument", pluginManager.getInstrumentDescriptions(), pluginManager.isScanning());
        });
    };
    pluginPicker.onClose = [this]() { arrangementTimeline.grabKeyboardFocus(); };
    addChildComponent(pluginPicker);

    // Modern "Add Track" dialog (replaces the small +/menu).
    addTrackDialog.onCreate = [this](const AddTrackDialogComponent::Result& r)
    {
        using TT = AddTrackDialogComponent::TrackType;
        auto& tracks = projectState.getTracks();

        if (r.type == TT::folder)
        {
            // A folder/group track owns a dedicated aux bus; its children route into that bus,
            // so the existing bus engine handles group volume / mute / inserts for free.
            auto& buses = projectState.getBuses();
            const auto folderName = r.name.isNotEmpty() ? r.name : juce::String("Group");
            const auto folderColour = r.autoColour ? juce::Colour(0xff3a7bd5) : r.colour;

            BusState bus;
            bus.name = folderName;
            bus.colour = folderColour;
            buses.push_back(bus);
            const int busIndex = static_cast<int>(buses.size()) - 1;
            const int gid = projectState.allocateGroupId();

            TrackState folder;
            folder.name = folderName;
            folder.isFolder = true;
            folder.groupId = gid;
            folder.folderBusIndex = busIndex;
            folder.colour = folderColour;
            tracks.push_back(std::move(folder));
            arrangementTimeline.selectTrack(static_cast<int>(tracks.size()) - 1);

            syncFoldersToBuses();
            refreshClipInspector();
            resized();
            repaint();
            return;
        }

        const bool midiLike = (r.type == TT::midi || r.type == TT::sampler);
        auto baseName = r.name.isNotEmpty() ? r.name : juce::String(midiLike ? "MIDI" : "Audio");
        std::vector<int> createdTrackIndices;

        // If a folder (or one of its children) is currently selected, new tracks join that
        // folder: they're inserted at the end of its child block and routed to its bus.
        const auto sel = arrangementTimeline.getSelectedTrackIndex();
        int targetFolder = sel.has_value() ? arrangementTimeline.owningFolderIndex(*sel) : -1;

        for (int i = 0; i < r.count; ++i)
        {
            const auto trackName = r.count > 1 ? (baseName + " " + juce::String(i + 1)) : baseName;

            if (targetFolder >= 0)
            {
                const auto at = arrangementTimeline.folderChildInsertIndex(targetFolder);
                const auto newIdx = arrangementTimeline.insertTrackAt(at, midiLike, trackName, r.colour, r.autoColour);
                auto& nt = tracks[static_cast<std::size_t>(newIdx)];
                nt.parentGroup = tracks[static_cast<std::size_t>(targetFolder)].groupId;
                nt.outputBus   = tracks[static_cast<std::size_t>(targetFolder)].folderBusIndex;
                createdTrackIndices.push_back(newIdx);
            }
            else
            {
                if (midiLike) arrangementTimeline.addMidiTrack();
                else          arrangementTimeline.addAudioTrack();
                if (tracks.empty()) break;
                auto& t = tracks.back();
                t.name = trackName;
                if (! r.autoColour) t.colour = r.colour;
                t.outputBus = r.outputBus;
                createdTrackIndices.push_back(static_cast<int>(tracks.size()) - 1);
            }
        }
        if (midiLike && r.instrumentPluginId.isNotEmpty())
        {
            if (const auto desc = pluginManager.findDescription(r.instrumentPluginId); desc.has_value())
                for (const auto trackIndex : createdTrackIndices)
                    loadInstrumentOnTrack(trackIndex, *desc);
        }
        syncFoldersToBuses();
        refreshClipInspector();
        resized();
        repaint();
    };
    addTrackDialog.onClose = [this]() { arrangementTimeline.grabKeyboardFocus(); };
    addChildComponent(addTrackDialog);

    // The timeline "+" button opens the full Add Track dialog (same as the old sidebar +).
    arrangementTimeline.onAddTrackRequested = [this]()
    {
        juce::StringArray busNames;
        for (const auto& b : projectState.getBuses())
            busNames.add(b.name);
        addTrackDialog.setBounds(getLocalBounds());
        addTrackDialog.show(static_cast<int>(projectState.getTracks().size()),
                            busNames,
                            pluginManager.getInstrumentDescriptions());
        addTrackDialog.toFront(true);
    };

    samplerPanel.onClose = [this]()
    {
        // If we opened the sampler from the step rack, closing it returns there.
        if (samplerOpenedFromStep)
        {
            samplerOpenedFromStep = false;
            stepSequencer.setVisible(true);
        }
        resized();
        arrangementTimeline.grabKeyboardFocus();
    };
    samplerPanel.onRequestProjectTempoBpm = [this]() { return projectState.getTempoBpm(); };
    samplerPanel.onRequestProjectKeyRoot    = [this]() { return projectState.getKeyRoot(); };
    samplerPanel.onRequestProjectKeyIsMinor = [this]() { return projectState.isKeyMinor(); };
    samplerPanel.onRequestScaleLockEnabled  = [this]() { return projectState.isKeyEnabled() && projectState.isScaleLockEnabled(); };
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
        // Chord mode expands one key into a diatonic chord — but not for SLICE mode, where each key
        // is a distinct drum slice, not a pitch. Expansion happens here so the sampler component
        // itself stays single-note (no regression to its keyboard/slice logic).
        const auto pitches = (playbackMode == SamplerPlaybackMode::slice)
                                 ? std::vector<int>{ midiNote }
                                 : chordPitchesForNote(midiNote);
        samplerChordVoicing[midiNote] = pitches;

        if (arrangementPlaybackSource != nullptr)
        {
            const auto activeTrack = samplerPanel.getActiveTrackIndex();
            const bool onInstrument = activeTrack >= 0 && arrangementPlaybackSource->hasTrackInstrument(activeTrack);
            const bool fullSampleTrigger = activeTrack >= 0
                && activeTrack < static_cast<int>(projectState.getTracks().size())
                && projectState.getTracks()[static_cast<std::size_t>(activeTrack)].samplerFullSampleTrigger;
            for (const auto p : pitches)
            {
                if (onInstrument)
                    arrangementPlaybackSource->instrumentLiveNoteOn(activeTrack, p, velocity);
                else
                    arrangementPlaybackSource->samplerNoteOn(sourcePath, p, velocity, rootMidiNote,
                                                             gainDb, playbackMode, sliceIndex,
                                                             sliceCount, warpEnabled, sourceBpm,
                                                             fullSampleTrigger);
            }
        }
        for (const auto p : pitches)
            recordNoteOn(p, velocity);
    };
    samplerPanel.onNoteOff = [this](int midiNote, SamplerPlaybackMode playbackMode)
    {
        std::vector<int> pitches { midiNote };
        if (const auto it = samplerChordVoicing.find(midiNote); it != samplerChordVoicing.end())
        {
            pitches = it->second;
            samplerChordVoicing.erase(it);
        }

        if (arrangementPlaybackSource != nullptr)
        {
            const auto activeTrack = samplerPanel.getActiveTrackIndex();
            const bool onInstrument = activeTrack >= 0 && arrangementPlaybackSource->hasTrackInstrument(activeTrack);
            // Classic (без Full Sample) = Ableton: releasing the key stops the note live too.
            const bool gateByNoteLength = samplerTrackGatesByNoteLength(activeTrack);
            for (const auto p : pitches)
            {
                if (onInstrument)
                    arrangementPlaybackSource->instrumentLiveNoteOff(activeTrack, p);
                else
                    arrangementPlaybackSource->samplerNoteOff(p, playbackMode, gateByNoteLength);
            }
        }
        for (const auto p : pitches)
            recordNoteOff(p);
    };
    samplerPanel.onAllNotesOff = [this]()
    {
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allSamplerNotesOff();
            arrangementPlaybackSource->allInstrumentNotesOff();
        }
    };
    samplerPanel.onSlicePointsChanged = [this](const std::vector<double>& points)
    {
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->setSamplerLiveSlicePoints(points);
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
    midiEditorOverlay.onPreviewNoteOn = [this](int midiNote, int velocity)
    {
        if (selectedArrangementClip.has_value())
            liveMidiNoteOn(selectedArrangementClip->first, midiNote, velocity);
    };
    midiEditorOverlay.onPreviewNoteOff = [this](int midiNote)
    {
        if (selectedArrangementClip.has_value())
            liveMidiNoteOff(selectedArrangementClip->first, midiNote);
    };
    midiEditorOverlay.onPreviewChordRetrigger = [this]()
    {
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allSamplerNotesOff();
            arrangementPlaybackSource->allInstrumentNotesOff();
        }
    };
    midiEditorOverlay.onGlideChanged = [this](bool on)
    {
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->setSamplerGlide(on, 0.08);
    };
    midiEditorOverlay.onRequestPlayheadBeat = [this]() { return transportEngine.getPlayheadBeat(); };
    midiEditorOverlay.onRequestPlayingState = [this]() { return transportEngine.isPlaying(); };
    midiEditorOverlay.onStartGlobalSpacePreview = [this](double startBeat) { startGlobalSpacePreview(startBeat); };
    midiEditorOverlay.onStopGlobalSpacePreview = [this]() { stopGlobalSpacePreview(); };
    midiEditorOverlay.onCommitGlobalSpacePreview = [this]() { commitGlobalSpacePreview(); };
    arrangementTimeline.onMidiClipDoubleClick = [this](int trackIndex, int clipIndex)
    {
        auto& track = projectState.getTracks()[static_cast<std::size_t>(trackIndex)];
        auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
        midiEditorOverlay.openClip(track, clip,
                                   projectState.getKeyRoot(),
                                   projectState.isKeyMinor(),
                                   projectState.isKeyEnabled() && projectState.isScaleLockEnabled());
        midiEditorOverlay.setChordModeExternally(projectState.isChordModeEnabled(), projectState.getChordSizeNotes());
    };
    arrangementTimeline.onAudioClipDoubleClick = [this](int trackIndex, int clipIndex)
    {
        selectedArrangementClip = std::pair { trackIndex, clipIndex };
        samplerPanel.setVisible(false);
        stepSequencer.setVisible(false);
        clipEditorPanel.setVisible(true);
        refreshClipInspector();
        refreshClipEditor();
        resized();
        updateTransportLabels();
    };
    arrangementTimeline.onTogglePlayback = [this]()
    {
        if (transportEngine.isPlaying() || transportEngine.isCountInActive())
            stopTransportFromUi();
        else
            toggleTransportFromUi();
    };
    arrangementTimeline.onTransportSeek = [this]()
    {
        // Jump the audio engine to the new playhead position (works while playing too).
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->syncToTransportPosition();
        updateTransportLabels();
    };
    midiEditorOverlay.onScaleLockChanged = [this](bool enabled)
    {
        projectState.setScaleLockEnabled(enabled);
    };
    midiEditorOverlay.onChordModeChanged = [this](bool enabled)
    {
        projectState.setChordModeEnabled(enabled);
        syncChordModeToSurfaces();
    };
    arrangementTimeline.onClipWarpEdited = [this]()
    {
        if (arrangementPlaybackSource == nullptr)
            return;
        // Live time-stretch: re-prep warp so the new speed is heard immediately. While playing,
        // use the non-blocking path (background producer builds the new-length stream and the
        // render swaps to it) so nothing stalls — no stop/replay needed.
        if (transportEngine.isPlaying() && arrangementPlaybackSource->isRealtimeWarpEnabled())
            arrangementPlaybackSource->prepareWarpStreams(false);
        else
            rebuildArrangementWarpNonBlocking();
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
                    // Arm the (sampler or instrument) keyboard for this track WITHOUT
                    // popping the sampler panel. A single click only selects/arms; the
                    // piano roll opens on double-click and the sampler via its button or a
                    // header double-click. (Popping the panel here also re-laid out the
                    // lanes mid-gesture, which teleported the clip being dragged onto
                    // whatever track ended up under the cursor in the new layout.)
                    const bool wasVisible = samplerPanel.isVisible();
                    samplerPanel.openTrackIndex(trackIndex);
                    if (! wasVisible)
                        samplerPanel.setVisible(false);
                    else if (hasSampler)
                        resized();   // keep an already-open sampler following the selection
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
            // Clicking empty playlist space clears the selection — also close any open lower
            // panel (step sequencer / sampler) so it gets out of the way.
            bool closedPanel = false;
            if (stepSequencer.isVisible())
            {
                stepSequencer.setVisible(false);
                closedPanel = true;
            }
            if (samplerPanel.isVisible())
            {
                samplerOpenedFromStep = false;   // close straight to the playlist, don't bounce to step
                samplerPanel.setVisible(false);
                closedPanel = true;
            }
            if (closedPanel)
                resized();
        }

        if (! selectedArrangementClip.has_value() && clipEditorPanel.isVisible())
        {
            clipEditorPanel.setVisible(false);
            resized();
        }

        // Close the sampler panel if its bound track no longer exists or has no sample (e.g.
        // the clip/track it was opened for was just deleted) — otherwise it lingers showing
        // "No sample loaded".
        if (samplerPanel.isVisible())
        {
            const auto activeIdx = samplerPanel.getActiveTrackIndex();
            const auto& currentTracks = projectState.getTracks();
            const bool stillValid = activeIdx >= 0
                                    && activeIdx < static_cast<int>(currentTracks.size())
                                    && currentTracks[static_cast<std::size_t>(activeIdx)].samplerSourcePath.isNotEmpty();
            if (! stillValid)
            {
                samplerPanel.disarmKeyboard();
                samplerPanel.setVisible(false);
                resized();
            }
        }

        // NOTE: do NOT re-bake the warp cache here. This handler fires on every clip
        // selection (including the auto-select on drop), and prepareWarpCacheForCurrentTempo
        // synchronously RubberBand-stretches every warp clip — for a 1-minute clip that's
        // a multi-second UI freeze on drop. The warp cache is built on Play (togglePlayback)
        // where it belongs.
        refreshClipInspector();
        refreshClipEditor();
        updateTransportLabels();
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
    arrangementTimeline.onTrackDeleted = [this](int trackIndex)
    {
        // Realign the engine's per-track instrument/insert slots with the project's new
        // track order so a deleted track can't leave a hosted instrument keyed wrong (which
        // left a held note ringing). Also drop any now-stale per-track UI references.
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->removeTrackAndReindex(trackIndex);

        if (samplerPanel.getActiveTrackIndex() == trackIndex)
        {
            samplerPanel.disarmKeyboard();
            samplerPanel.setVisible(false);
        }
        if (recordingSession.has_value() && recordingSession->trackIndex == trackIndex)
            recordingSession.reset();
        resized();
    };
    arrangementTimeline.onInstrumentLayoutChangedByHistory = [this]()
    {
        // Undo/redo restored a different track layout (e.g. a deleted track came back). Re-home
        // the LIVE instrument instances onto the restored layout (instant, lossless) so each
        // plugin is keyed to the right track again — a VST can't stay bound to what is now a
        // sampler track, and nothing is reinstantiated (no freeze).
        resyncInstrumentsAfterHistory();
        resized();
    };
    arrangementTimeline.onTrackHeaderRightClick = [this](int trackIndex)
    {
        showTrackInstrumentMenu(trackIndex);
    };
    arrangementTimeline.onTrackInstrumentClicked = [this](int trackIndex)
    {
        auto& tracks = projectState.getTracks();
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
            return;

        // Instrument loaded → open its editor window. Otherwise open the picker to load one.
        if (tracks[static_cast<std::size_t>(trackIndex)].instrumentPluginId.isNotEmpty())
            openInstrumentEditor(trackIndex);
        else
            showInstrumentPicker(trackIndex);
    };

    resetToPlaylistView();
    setClipInspectorVisible(false);
    updateTransportLabels();
    grabKeyboardFocus();
    startTimerHz(60);
}

MainComponent::~MainComponent()
{
    if (juce::MenuBarModel::getMacMainMenu() == this)
        juce::MenuBarModel::setMacMainMenu(nullptr);

    for (auto* button : { &playButton, &stopButton, &recordButton, &rewindButton, &undoButton, &redoButton,
                          &metronomeButton, &loopButton, &countInButton, &browserButton, &scanPluginsButton,
                          &mixerButton, &openButton, &saveButton, &exportButton, &settingsButton })
    {
        button->setLookAndFeel(nullptr);
    }

    // Editor windows borrow plugin instances owned by the playback source — close
    // them before that source (and its instruments) is destroyed.
    closeAllInstrumentEditors();
    insertEditorWindows.clear();

    finalizeRecordingClip();
    finalizeAudioRecordingClip();
    stopBrowserPreview(true);
    stopClipEditorPreview(true);
    masterMixerSource.removeAllInputs();
    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();
    clipEditorPreviewTransportSource.stop();
    clipEditorPreviewTransportSource.setSource(nullptr);
    clipEditorPreviewBufferSource.reset();
    clipEditorPreviewStreamSource.reset();
    // Stop the device pulling the master stage, then detach the click monitor BEFORE
    // destroying the click (the master stage references it as its monitor source).
    previewSourcePlayer.setSource(nullptr);
    if (masterStripSource != nullptr)
        masterStripSource->setMonitorSource(nullptr);
    clickTrackSource.reset();
    arrangementPlaybackSource.reset();
    currentPreviewFile = juce::File();
    currentPreviewTempoBpm = 0.0;
    masterStripSource.reset();
    if (audioInputRecorder != nullptr && audioRecorderCallbackAttached)
    {
        audioDeviceManager.removeAudioCallback(audioInputRecorder.get());
        audioRecorderCallbackAttached = false;
    }
    for (const auto& midiInputId : activeMidiInputDeviceIds)
        audioDeviceManager.removeMidiInputDeviceCallback(midiInputId, this);
    activeMidiInputDeviceIds.clear();
    audioDeviceManager.removeAudioCallback(&previewSourcePlayer);
    audioInputRecorder.reset();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);

    auto bounds = getLocalBounds();
    auto topStrip = bounds.removeFromTop(transportShelfHeight);

    g.setColour(transportShelfColour);
    g.fillRect(topStrip);

    {
        const auto strip = topStrip.toFloat();
        juce::ColourGradient tealGlow(juce::Colour(0xff53f0ba).withAlpha(0.30f),
                                      strip.getX(), strip.getY(),
                                      juce::Colours::transparentBlack,
                                      strip.getCentreX(), strip.getBottom(),
                                      false);
        tealGlow.addColour(0.45, juce::Colour(0xff12b6b2).withAlpha(0.09f));
        g.setGradientFill(tealGlow);
        g.fillRect(strip);

        juce::ColourGradient magentaGlow(juce::Colours::transparentBlack,
                                         strip.getX() + strip.getWidth() * 0.18f, strip.getY(),
                                         juce::Colour(0xffff2d91).withAlpha(0.20f),
                                         strip.getX() + strip.getWidth() * 0.72f, strip.getY() + strip.getHeight() * 0.22f,
                                         false);
        magentaGlow.addColour(0.52, juce::Colour(0xff4e63ff).withAlpha(0.10f));
        g.setGradientFill(magentaGlow);
        g.fillRect(strip);

        juce::ColourGradient emberGlow(juce::Colours::transparentBlack,
                                       strip.getCentreX(), strip.getY(),
                                       juce::Colour(0xffff5a35).withAlpha(0.18f),
                                       strip.getRight(), strip.getY() + strip.getHeight() * 0.12f,
                                       false);
        emberGlow.addColour(0.72, juce::Colour(0xffffb347).withAlpha(0.08f));
        g.setGradientFill(emberGlow);
        g.fillRect(strip);

        juce::ColourGradient vignette(juce::Colours::black.withAlpha(0.10f),
                                      strip.getCentreX(), strip.getY(),
                                      juce::Colours::black.withAlpha(0.84f),
                                      strip.getCentreX(), strip.getBottom(),
                                      false);
        g.setGradientFill(vignette);
        g.fillRect(strip);
    }

    const auto shell = topStrip.reduced(8, 8).toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.30f));
    g.fillRoundedRectangle(shell.translated(0.0f, 2.0f), 12.0f);
    g.setColour(juce::Colour(0xff090d12).withAlpha(0.68f));
    g.fillRoundedRectangle(shell, 12.0f);
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawRoundedRectangle(shell.reduced(0.5f), 12.0f, 1.0f);

    auto transportVisual = topStrip.reduced(18, 8);
    auto contentRow = transportVisual;
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
        g.setColour(isTempoCard ? transportDarkPanel.withAlpha(0.86f) : transportSectionFill.withAlpha(0.78f));
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
        scanArea = scanArea.withY(topStrip.getBottom() - 12).withHeight(9);
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
        g.setFont(juce::FontOptions(21.0f, juce::Font::bold));
        g.drawText(juce::String(projectState.getTempoBpm(), 2), bpmTextBounds, juce::Justification::centred);
    }

    // KEY half: large key name on top, "KEY" caption below.
    {
        auto keyValueBounds = keyHalf.withSizeKeepingCentre(keyHalf.getWidth(), transportControlHeight)
                                  .translated(0, transportContentVerticalNudge);
        auto keyCaptionBounds = keyValueBounds;
        keyValueBounds = keyValueBounds.removeFromTop(38);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
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
    // Horizontal extents MUST match resized() (which works off getLocalBounds().reduced(8)),
    // otherwise the painted panel card sits 8px off from where the component is positioned —
    // which made the browser's internal padding asymmetric (24px left vs 8px right).
    auto workArea = bounds.reduced(8, 0).withTrimmedTop(2);
    workArea.removeFromLeft(SidebarNavComponent::preferredWidth + 2);
    juce::Rectangle<int> browserPanelBounds;
    if (browserPanelShown())
        browserPanelBounds = workArea.removeFromLeft(currentBrowserWidth());
    auto arrangementPanel = workArea;

    g.setColour(panelColour);
    // Round only the OUTER corners of each panel; the corners that face the neighbouring
    // panel stay square so the two cards abut cleanly along the seam. Rounding both sides
    // left little dark triangular wedges at the top/bottom of the browser↔playlist seam.
    auto paintPanel = [&](juce::Rectangle<int> panel, bool roundLeft, bool roundRight, bool border)
    {
        if (panel.isEmpty())
            return;
        const auto r = 14.0f;
        const auto b = panel.toFloat();
        juce::Path p;
        p.addRoundedRectangle(b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                              r, r, roundLeft, roundRight, roundLeft, roundRight);
        g.setColour(panelColour);
        g.fillPath(p);
        if (border)
        {
            g.setColour(panelStroke);
            g.strokePath(p, juce::PathStrokeType(1.0f));
        }
    };
    // Panels abut their neighbours flush with no borders. The only rounded corner is the
    // playlist's outer (right) edge — the far right of the window. Everything to its left
    // (sidebar seam, browser seam) is square so the panels meet perfectly.
    if (browserPanelShown())
        paintPanel(browserPanelBounds, false, false, false);   // browser: square, flush, no border
    paintPanel(arrangementPanel, false, true, false);          // playlist: square left, round right only

    // Keep the browser resize hit area invisible; a visible handle reads as a stray divider.
}

void MainComponent::syncFoldersToBuses()
{
    auto& tracks = projectState.getTracks();
    auto& buses  = projectState.getBuses();
    for (auto& t : tracks)
    {
        if (! t.isFolder)
            continue;
        if (t.folderBusIndex >= 0 && t.folderBusIndex < static_cast<int>(buses.size()))
        {
            auto& bus = buses[static_cast<std::size_t>(t.folderBusIndex)];
            // Only write when something actually changed — avoids churning juce::String /
            // double values that the audio thread reads every single frame.
            if (bus.name != t.name)           bus.name     = t.name;
            if (bus.colour != t.colour)       bus.colour   = t.colour;
            if (bus.volumeDb != t.volumeDb)   bus.volumeDb = t.volumeDb;
            if (bus.muted != t.muted)         bus.muted    = t.muted;
        }
        // Solo on the folder solos all its children (so the group plays in isolation).
        for (auto& c : tracks)
            if (c.parentGroup == t.groupId && c.solo != t.solo)
                c.solo = t.solo;
    }
}

int MainComponent::currentBrowserWidth() const noexcept
{
    // Cubic ease-out on the linear progress for a smooth slide.
    const auto t = juce::jlimit(0.0f, 1.0f, browserAnim);
    const auto eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    return static_cast<int>(std::round(browserPanelWidth * eased));
}

bool MainComponent::browserPanelShown() const noexcept
{
    return browserAnim > 0.001f;
}

void MainComponent::resized()
{
    transportBar.setBounds(getLocalBounds().removeFromTop(transportShelfHeight));
    transportBar.toFront(false);
    cachedKeyCardBounds = transportBar.getKeyBounds().translated(transportBar.getX(), transportBar.getY());
    // Reduced padding for a sleeker edge-to-edge floating layout
    auto bounds = getLocalBounds().reduced(8);
    auto topStrip = bounds.removeFromTop(transportShelfHeight).reduced(18, 10);
    bounds.setBottom(getLocalBounds().getBottom());
    const auto contentWidth = transportBrandWidth + transportClusterWidth + transportTempoWidth + transportModeWidth
        + transportUtilityWidth + transportSectionGap * 4;
    auto contentRow = topStrip.withSizeKeepingCentre(juce::jmin(contentWidth, topStrip.getWidth()), topStrip.getHeight());
    contentRow.removeFromLeft(transportBrandWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto transportCluster = contentRow.removeFromLeft(transportClusterWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto bpmCard = contentRow.removeFromLeft(transportTempoWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto modeCluster = contentRow.removeFromLeft(transportModeWidth);
    contentRow.removeFromLeft(transportSectionGap);
    auto rightUtility = contentRow.removeFromLeft(transportUtilityWidth);

    headerLabel.setBounds({});
    statusLabel.setBounds({});

    auto scanTopArea = getLocalBounds().reduced(8).removeFromTop(transportShelfHeight).reduced(18, 0);
    auto scanLabelArea = scanTopArea.withY(scanTopArea.getBottom() - 22).withHeight(12);
    pluginScanNameLabel.setBounds(scanLabelArea);
    pluginScanNameLabel.setVisible(pluginScanVisible);

    auto centeredTransportCluster = transportCluster.withSizeKeepingCentre(transportCluster.getWidth(), transportSectionHeight);
    auto centeredBpmCard = bpmCard.withSizeKeepingCentre(bpmCard.getWidth(), transportSectionHeight);
    auto centeredModeCluster = modeCluster.withSizeKeepingCentre(modeCluster.getWidth(), transportSectionHeight);
    auto centeredRightUtility = rightUtility.withSizeKeepingCentre(rightUtility.getWidth(), transportSectionHeight);

    auto transportButtons = centeredTransportCluster.withSizeKeepingCentre(centeredTransportCluster.getWidth(), transportControlHeight)
                                .translated(0, transportContentVerticalNudge)
                                .reduced(8, 0);
    playButton.setBounds(transportButtons.removeFromLeft(50));
    transportButtons.removeFromLeft(6);
    stopButton.setBounds(transportButtons.removeFromLeft(46));
    transportButtons.removeFromLeft(6);
    recordButton.setBounds(transportButtons.removeFromLeft(46));
    transportButtons.removeFromLeft(6);
    undoButton.setBounds(transportButtons.removeFromLeft(46));
    transportButtons.removeFromLeft(6);
    redoButton.setBounds(transportButtons.removeFromLeft(46));

    // Combined BPM + KEY card: BPM occupies the left half, KEY the right half.
    auto bpmHalf = centeredBpmCard.withTrimmedRight(centeredBpmCard.getWidth() / 2);
    auto bpmBounds = bpmHalf.withSizeKeepingCentre(bpmHalf.getWidth(), transportControlHeight)
                         .translated(0, transportContentVerticalNudge);
    auto bpmTop = bpmBounds.removeFromTop(30);
    bpmValueLabel.setBounds(bpmTop);
    const auto editorBoxHeight = static_cast<int>(std::ceil(bpmEditor.getFont().getHeight())) + 4;
    auto transportTempoEditorBounds = transportBar.getTempoEditorBounds().translated(transportBar.getX(), transportBar.getY());
    bpmEditor.setBounds(transportTempoEditorBounds.withSizeKeepingCentre(transportTempoEditorBounds.getWidth(), editorBoxHeight));
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
    metronomeButton.setBounds(modeButtons.removeFromLeft(44));
    modeButtons.removeFromLeft(6);
    loopButton.setBounds(modeButtons.removeFromLeft(44));
    modeButtons.removeFromLeft(6);
    countInButton.setBounds(modeButtons.removeFromLeft(46));
    // BROWSER button removed from the toolbar — the small triangle in the top-left
    // corner is now the only browser toggle.
    browserButton.setBounds({});

    auto utilityButtons = centeredRightUtility.withSizeKeepingCentre(centeredRightUtility.getWidth(), transportControlHeight)
                              .translated(0, transportContentVerticalNudge)
                              .reduced(8, 0);
    mixerButton.setBounds(utilityButtons.removeFromLeft(52));
    utilityButtons.removeFromLeft(6);
    openButton.setBounds(utilityButtons.removeFromLeft(48));
    utilityButtons.removeFromLeft(6);
    saveButton.setBounds(utilityButtons.removeFromLeft(48));
    utilityButtons.removeFromLeft(6);
    exportButton.setBounds(utilityButtons.removeFromLeft(58));
    utilityButtons.removeFromLeft(6);
    settingsButton.setBounds(utilityButtons.removeFromLeft(62));
    scanPluginsButton.setBounds({});

    // Use the full window width so the timeline reaches the right edge (no dead strip).
    // Only the left/top insets come from `bounds`; right/bottom go flush to the window.
    auto workArea = bounds.withTrimmedTop(2);
    workArea.setTop(juce::jmax(0, workArea.getY() - 8));
    workArea.setRight(getLocalBounds().getRight());
    auto sidebarBounds = workArea.removeFromLeft(SidebarNavComponent::preferredWidth);
    sidebarNav.setBounds(sidebarBounds);
    sidebarNav.toFront(false);
    workArea.removeFromLeft(2);   // tight gap between the sidebar and the browser/playlist

    // Small collapse arrow at the top-left of the work area, just below the transport
    // bar. Always visible, ~14×14 px, sits next to the browser/playlist boundary so it's
    // obvious what it controls.
    browserCollapseArrow.setBounds(workArea.getX() + 14,
                                   workArea.getY() + 12,
                                   14, 14);
    browserCollapseArrow.toFront(false);

    juce::Rectangle<int> browserPanelBounds;
    if (browserPanelShown())
        browserPanelBounds = workArea.removeFromLeft(currentBrowserWidth());
    auto arrangementPanel = workArea;

    auto playlistArea = arrangementPanel;
    playlistArea.removeFromTop(2);

    const auto samplerOpen = samplerPanel.isVisible();
    const auto clipEditorOpen = clipEditorPanel.isVisible();
    const auto stepSequencerOpen = stepSequencer.isVisible();
    const auto bottomPanelOpen = samplerOpen || clipEditorOpen || stepSequencerOpen;
    const auto closedArrangementArea = playlistArea;
    auto openArrangementArea = playlistArea;
    // Sampler and clip editor share one compact lower-panel height.
    auto lowerPanelArea = openArrangementArea.removeFromBottom(juce::jmin(samplerPanelHeight, openArrangementArea.getHeight()));
    const auto arrangementArea = bottomPanelOpen ? openArrangementArea : closedArrangementArea;

    arrangementTimeline.setBounds(arrangementArea);
    // Any open lower panel (sampler / clip editor / step sequencer) compresses the track lanes
    // so every track stays visible instead of being covered by the panel.
    arrangementTimeline.setFitTrackLanesToVisibleArea(bottomPanelOpen);

    if (browserPanelShown())
    {
        auto browserInner = browserPanelBounds.withTrimmedTop(16).withTrimmedBottom(16);
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
    exportButton.setVisible(false);
    saveButton.setVisible(false);
    settingsButton.setVisible(false);
    browserButton.setVisible(false);
    scanPluginsButton.setVisible(false);

    // The selected-track controls now live inside ArrangementTimelineComponent.
    // Keeping the floating inspector over the playlist blocked volume, warp and
    // lane resize hit-testing.
    selectionInspector.setBounds({});

    midiEditorOverlay.setBounds(getLocalBounds());
    mixerPanel.setBounds(getLocalBounds());
    clipEditorPanel.setBounds(clipEditorOpen ? lowerPanelArea : juce::Rectangle<int>());
    clipEditorPanel.setVisible(clipEditorOpen);
    samplerPanel.setBounds(samplerOpen ? lowerPanelArea : juce::Rectangle<int>());
    samplerPanel.setVisible(samplerOpen);
    stepSequencer.setBounds(stepSequencerOpen ? lowerPanelArea : juce::Rectangle<int>());
    stepSequencer.setVisible(stepSequencerOpen);

    // The sidebar / transport were just raised with toFront above; if the mixer overlay
    // is open it must stay on top of them, otherwise they overlap and clip it.
    if (mixerPanel.isVisible())
        mixerPanel.toFront(false);

    pluginPicker.setBounds(getLocalBounds());
    if (pluginPicker.isVisible())
        pluginPicker.toFront(false);

    addTrackDialog.setBounds(getLocalBounds());
    if (addTrackDialog.isVisible())
        addTrackDialog.toFront(false);
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
        const auto withPrecount = projectState.isRecordWithCountIn();
        menu.addItem(1, "Record with metronome", true, withMetro && ! withPrecount);
        menu.addItem(2, "Record without metronome", true, ! withMetro && ! withPrecount);
        menu.addItem(3, "4-count before recording", true, withPrecount);
        menu.showMenuAsync(juce::PopupMenu::Options{}
                                .withTargetComponent(&recordButton)
                                .withTargetScreenArea(recordButton.localAreaToGlobal(recordButton.getLocalBounds())),
            [this](int result)
            {
                if (result == 1)
                {
                    projectState.setRecordWithMetronome(true);
                    projectState.setRecordWithCountIn(false);
                    metronomeButton.setToggleState(true, juce::dontSendNotification);
                    countInButton.setToggleState(false, juce::dontSendNotification);
                }
                else if (result == 2)
                {
                    projectState.setRecordWithMetronome(false);
                    projectState.setRecordWithCountIn(false);
                    metronomeButton.setToggleState(false, juce::dontSendNotification);
                    countInButton.setToggleState(false, juce::dontSendNotification);
                }
                else if (result == 3)
                {
                    projectState.setRecordWithMetronome(false);
                    projectState.setRecordWithCountIn(true);
                    metronomeButton.setToggleState(false, juce::dontSendNotification);
                    countInButton.setToggleState(true, juce::dontSendNotification);
                }
                updateTransportLabels();
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
    if (midiEditorOverlay.isVisible() && midiEditorOverlay.hasKeyboardFocus(true) && midiEditorOverlay.keyPressed(key))
        return true;

    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false;

    if (mixerPanel.isVisible() && key == juce::KeyPress::escapeKey)
    {
        mixerPanel.closePanel();
        return true;
    }

    if (samplerPanel.isVisible() && (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey))
    {
        samplerPanel.closePanel();
        return true;
    }

    if (key == juce::KeyPress('r', juce::ModifierKeys::commandModifier, 0))
    {
        const auto selectedTrackIndex = arrangementTimeline.getSelectedTrackIndex();
        bool shouldRecord = ! transportEngine.isRecordArmed();

        if (selectedTrackIndex.has_value())
        {
            auto& tracks = projectState.getTracks();
            if (*selectedTrackIndex >= 0 && *selectedTrackIndex < static_cast<int>(tracks.size()))
            {
                auto& track = tracks[static_cast<std::size_t>(*selectedTrackIndex)];
                shouldRecord = ! (track.recordArmed && transportEngine.isRecordArmed());
                track.recordArmed = shouldRecord;
                arrangementTimeline.repaint();
                mixerPanel.repaint();
            }
        }

        transportController.setRecordArmed(shouldRecord);
        if (! transportEngine.isRecordArmed())
        {
            finalizeRecordingClip();
            finalizeAudioRecordingClip();
        }
        startMidiRecordingFromRecordButtonIfNeeded();
        recordButton.setToggleState(transportEngine.isRecordArmed(), juce::dontSendNotification);
        updateTransportLabels();
        return true;
    }

    if (key == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0))
        return arrangementTimeline.duplicateSelectedClip();

    if (key == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0))
        return arrangementTimeline.selectAllClips();

    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0) && arrangementTimeline.undo())
        return true;

    if ((key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
         || key == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0)) && arrangementTimeline.redo())
        return true;

    // Delete/Backspace: forward to the timeline so deleting the selected clip(s) or track(s) works even
    // when keyboard focus landed on MainComponent (or elsewhere) rather than the timeline itself.
    if ((key == juce::KeyPress::backspaceKey || key == juce::KeyPress::deleteKey)
        && arrangementTimeline.keyPressed(key))
        return true;

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

    // Escape cancels an in-progress recording (discards the current take).
    if (key == juce::KeyPress::escapeKey
        && (audioRecordingSession.has_value() || recordingSession.has_value()))
    {
        cancelRecording();
        return true;
    }

    // Catch-all: when the sampler/piano-roll can play typing-piano notes, note playback runs via
    // keyStateChanged independent of keyboard focus. If the matching keyPressed goes unconsumed,
    // macOS emits its system beep alongside the note. Consume any plain (no-modifier) musical key
    // here so the beep never mixes in. Placed after all shortcuts so it steals nothing from them.
    const bool notesCanPlay = samplerPanel.isVisible() || samplerPanel.isArmed() || midiEditorOverlay.isVisible();
    if (notesCanPlay
        && ! key.getModifiers().isAnyModifierKeyDown()
        && SamplerPanelComponent::isTypingMusicalKey(key.getKeyCode()))
        return true;

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
    if (midiEditorOverlay.isVisible() && midiEditorOverlay.keyStateChanged(isKeyDown))
        return true;

    if ((samplerPanel.isVisible() || samplerPanel.isArmed()) && samplerPanel.keyStateChanged(isKeyDown))
        return true;

    return false;
}

void MainComponent::resetToPlaylistView()
{
    stopGlobalSpacePreview();
    midiEditorOverlay.setVisible(false);
    samplerPanel.setVisible(false);
    resized();
    arrangementTimeline.grabKeyboardFocus();
    repaint();
}

void MainComponent::updateTrackMeterLevels()
{
    const auto trackCount = static_cast<int>(projectState.getTracks().size());
    const auto sized = static_cast<std::size_t>(juce::jmax(0, trackCount));
    if (static_cast<int>(trackMeterLevels.size()) != trackCount)
    {
        trackMeterLevels.assign(sized, 0.0f);
        trackMeterLevelsL.assign(sized, 0.0f);
        trackMeterLevelsR.assign(sized, 0.0f);
        trackPeakHoldDb.assign(sized, -100.0f);
        trackPeakRecentDb.assign(sized, -100.0f);
        trackPeakHoldFrames.assign(sized, 0);
    }

    // Keep input monitoring in sync with the armed/recording state, then work out which
    // track should display the live input level this tick.
    updateInputMonitoring();
    int monitoredAudioTrack = -1;
    if (audioRecordingSession.has_value())
    {
        monitoredAudioTrack = audioRecordingSession->trackIndex;
    }
    else
    {
        const auto& tracks = projectState.getTracks();
        for (int t = 0; t < static_cast<int>(tracks.size()); ++t)
            if (! tracks[static_cast<std::size_t>(t)].isMidiTrack && tracks[static_cast<std::size_t>(t)].recordArmed)
            {
                monitoredAudioTrack = t;
                break;
            }
    }

    // Numeric dB readout = a Logic/Studio One-style peak hold: it jumps up instantly on
    // a louder peak, HOLDS for a while, then snaps in ONE step to the loudest level seen
    // recently. It never rolls digit-by-digit, so it stays readable (bars move smoothly).
    constexpr int peakHoldFrames = 90;             // ~1.5 s hold at 60 Hz

    for (int i = 0; i < trackCount; ++i)
    {
        float peakL = 0.0f, peakR = 0.0f;
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->fetchAndResetTrackPeakStereo(i, peakL, peakR);

        // Show the LIVE INPUT level on the monitored track (the armed audio track, or
        // the one currently recording) so the user can confirm the mic works — both
        // before and during recording.
        if (i == monitoredAudioTrack && audioRecorderCallbackAttached
            && audioInputRecorder != nullptr)
        {
            float inL = 0.0f, inR = 0.0f;
            audioInputRecorder->fetchAndResetInputPeak(inL, inR);
            peakL = juce::jmax(peakL, inL);
            peakR = juce::jmax(peakR, inR);
        }

        const auto peak = juce::jmax(peakL, peakR);


        // Fast bar level (linear): instant rise, smooth fall. Kept per-channel for the
        // stereo header meter, plus a combined value for the mixer/legacy consumers.
        auto& level  = trackMeterLevels[static_cast<std::size_t>(i)];
        auto& levelL = trackMeterLevelsL[static_cast<std::size_t>(i)];
        auto& levelR = trackMeterLevelsR[static_cast<std::size_t>(i)];
        level  = juce::jmax(peak,  level  * 0.85f);
        levelL = juce::jmax(peakL, levelL * 0.85f);
        levelR = juce::jmax(peakR, levelR * 0.85f);

        // Peak-hold numeric readout (dB): jump up instantly, hold, then snap once.
        const auto peakDb = peak > 0.0001f ? juce::Decibels::gainToDecibels(peak) : -100.0f;
        auto& holdDb     = trackPeakHoldDb[static_cast<std::size_t>(i)];
        auto& recentDb   = trackPeakRecentDb[static_cast<std::size_t>(i)];
        auto& holdFrames = trackPeakHoldFrames[static_cast<std::size_t>(i)];
        recentDb = juce::jmax(recentDb, peakDb);
        if (peakDb >= holdDb)
        {
            holdDb = peakDb;          // louder than shown → jump up now
            recentDb = peakDb;
            holdFrames = peakHoldFrames;
        }
        else if (--holdFrames <= 0)
        {
            holdDb = recentDb;        // hold elapsed → snap once to the recent max
            recentDb = -100.0f;
            holdFrames = peakHoldFrames;
        }
    }

    // Aux-bus meters (same ballistics + peak-hold as the track meters).
    const auto busCount = static_cast<int>(projectState.getBuses().size());
    const auto busSized = static_cast<std::size_t>(juce::jmax(0, busCount));
    if (static_cast<int>(busMeterLevelsL.size()) != busCount)
    {
        busMeterLevelsL.assign(busSized, 0.0f);
        busMeterLevelsR.assign(busSized, 0.0f);
        busPeakHoldDb.assign(busSized, -100.0f);
        busPeakRecentDb.assign(busSized, -100.0f);
        busPeakHoldFrames.assign(busSized, 0);
    }
    for (int b = 0; b < busCount; ++b)
    {
        float pL = 0.0f, pR = 0.0f;
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->fetchAndResetBusPeakStereo(b, pL, pR);
        const auto bi = static_cast<std::size_t>(b);
        busMeterLevelsL[bi] = juce::jmax(pL, busMeterLevelsL[bi] * 0.85f);
        busMeterLevelsR[bi] = juce::jmax(pR, busMeterLevelsR[bi] * 0.85f);
        const auto peak = juce::jmax(pL, pR);
        const auto peakDb = peak > 0.0001f ? juce::Decibels::gainToDecibels(peak) : -100.0f;
        busPeakRecentDb[bi] = juce::jmax(busPeakRecentDb[bi], peakDb);
        if (peakDb >= busPeakHoldDb[bi]) { busPeakHoldDb[bi] = peakDb; busPeakRecentDb[bi] = peakDb; busPeakHoldFrames[bi] = peakHoldFrames; }
        else if (--busPeakHoldFrames[bi] <= 0) { busPeakHoldDb[bi] = busPeakRecentDb[bi]; busPeakRecentDb[bi] = -100.0f; busPeakHoldFrames[bi] = peakHoldFrames; }
    }

    // Master output level — fetched once here (single consumer) so the mixer and the
    // bottom MASTER OUT bar both read consistent values without stealing peaks.
    masterRawPeakL = masterRawPeakR = 0.0f;
    if (masterStripSource != nullptr)
        masterStripSource->fetchAndResetPeakStereo(masterRawPeakL, masterRawPeakR);
    const auto masterPeak = juce::jmax(masterRawPeakL, masterRawPeakR);
    const auto masterDb = masterPeak > 0.0f ? juce::Decibels::gainToDecibels(masterPeak, -60.0f) : -60.0f;
    const auto masterTarget = juce::jlimit(0.0f, 1.0f, juce::jmap(masterDb, -60.0f, 0.0f, 0.0f, 1.0f));
    masterMeterLevel = juce::jmax(masterTarget, masterMeterLevel * 0.85f);
    // Per-channel decayed levels (same ballistics as the tracks) for the mixer master bar.
    masterMeterLevelL = juce::jmax(masterRawPeakL, masterMeterLevelL * 0.85f);
    masterMeterLevelR = juce::jmax(masterRawPeakR, masterMeterLevelR * 0.85f);

    const auto masterDisplayDb = masterPeak > 0.0001f ? juce::Decibels::gainToDecibels(masterPeak) : -100.0f;
    masterMeterRecentDb = juce::jmax(masterMeterRecentDb, masterDisplayDb);
    if (masterDisplayDb >= masterMeterDb)
    {
        masterMeterDb = masterDisplayDb;
        masterMeterRecentDb = masterDisplayDb;
        masterMeterDbHoldFrames = peakHoldFrames;
    }
    else if (--masterMeterDbHoldFrames <= 0)
    {
        masterMeterDb = masterMeterRecentDb;
        masterMeterRecentDb = -100.0f;
        masterMeterDbHoldFrames = peakHoldFrames;
    }

    // The timeline repaints itself continuously (its own 120 Hz timer), so the
    // header meters animate without an extra repaint here.
}

void MainComponent::timerCallback()
{
    // Smoothly slide the browser panel open/closed.
    {
        const float target = browserPanelVisible ? 1.0f : 0.0f;
        if (std::abs(browserAnim - target) > 0.001f)
        {
            const float step = 0.08f;   // matches the Add Track dialog's open speed
            browserAnim += (target - browserAnim > 0.0f ? 1.0f : -1.0f) * step;
            browserAnim = juce::jlimit(0.0f, 1.0f, browserAnim);
            if (std::abs(browserAnim - target) <= step)
                browserAnim = target;
            resized();
            repaint();
        }
    }

    updateClipEditorPreviewPlayhead();

    // Poll for freshly plugged-in MIDI keyboards. Enumerating CoreMIDI touches
    // AudioDeviceManager locks, so we NEVER do it during playback/recording (that
    // caused periodic audio dips) and only every couple of seconds when idle.
    if (! transportEngine.isPlaying() && ! transportEngine.isCountInActive() && ! transportEngine.isRecordArmed())
    {
        if (++midiDeviceRescanCounter >= 120)
        {
            midiDeviceRescanCounter = 0;
            refreshMidiInputDevices();
        }
    }

    syncFoldersToBuses();
    if (pendingBrowserPreviewStart)
    {
        if (! transportEngine.isPlaying() || pendingBrowserPreviewGeneration != previewRequestGeneration.load())
        {
            // Transport stopped or a newer preview was requested → cancel the armed start.
            pendingBrowserPreviewStart = false;
            browserPanel.setPreviewArmed(false);
        }
        else
        {
            const auto beat = transportEngine.getPlayheadBeat();
            // Fire when the playhead reaches the target beat, OR when it wraps backwards
            // (loop restart / rewind) — a musically sensible launch point either way.
            const auto wrappedBackwards = beat < pendingBrowserPreviewLastBeat - 1.0e-3;
            pendingBrowserPreviewLastBeat = beat;
            if (beat >= pendingBrowserPreviewStartBeat || wrappedBackwards)
            {
                pendingBrowserPreviewStart = false;
                browserPanel.setPreviewArmed(false);
                previewTransportSource.setPosition(0.0);
                previewTransportSource.start();
            }
            else
            {
                // Keep the pulse animating while we wait.
                browserPanel.setPreviewArmed(true);
            }
        }
    }

    // Drive the browser preview bar's playhead + play/stop state.
    if (browserPanelShown())
    {
        const auto len = previewTransportSource.getLengthInSeconds();
        const auto ratio = len > 0.0 ? static_cast<float>(previewTransportSource.getCurrentPosition() / len) : 0.0f;
        browserPanel.setPreviewPlayback(previewTransportSource.isPlaying(), ratio);
    }
    updateTransportLabels();
    updateTrackMeterLevels();
    maybeStartBackgroundAnalysis();

    // Keep the sampler's live audition gain in sync with the armed track's volume so the
    // Gain knob (and track fader) are heard during playback, not only after a re-trigger.
    if (arrangementPlaybackSource != nullptr)
    {
        double samplerGainDb = 0.0;
        if (samplerPanel.getActiveTrackGainDb(samplerGainDb))
            arrangementPlaybackSource->setSamplerLiveGainDb(samplerGainDb);
    }
    // While stopped, pre-configure warp streamers + warm the background producer so the
    // next Play is instant and the stretched audio is already filled ahead. Cheap once the
    // streams exist (skips configured ones); the one-time original decode is invisible
    // while stopped.
    if (arrangementPlaybackSource != nullptr && arrangementPlaybackSource->isRealtimeWarpEnabled()
        && ! transportEngine.isPlaying() && ! transportEngine.isCountInActive())
        arrangementPlaybackSource->prepareWarpStreams();
    if (clipEditorPanel.isVisible())
        refreshClipEditor();

    // While recording, drive the live UI feedback:
    //  (a) auto-start a recording clip the moment count-in ends and real playback begins
    //  (b) grow the recording clip's length in real time so the user sees a visual bar
    //      sweeping across the track as audio/MIDI is captured.
    const auto playing      = transportEngine.isPlaying();
    const auto countingIn   = transportEngine.isCountInActive();
    const auto recordArmed  = transportEngine.isRecordArmed();
    const auto canRecordNow = playing && ! countingIn && recordArmed;

    // Find a record-armed MIDI track. If none was explicitly R-armed, fall back to
    // the selected clip's track / selected MIDI track header — that way the user
    // just presses REC + PLAY without having to also tap R on the track. The same
    // resolution feeds live MIDI-keyboard playthrough so you hear what you record.
    auto& tracks = projectState.getTracks();
    const int armedTrack = resolveArmedMidiTrack();

    int armedAudioTrack = -1;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (! tracks[i].isMidiTrack && tracks[i].recordArmed) { armedAudioTrack = static_cast<int>(i); break; }

    if (armedAudioTrack < 0)
    {
        if (selectedArrangementClip.has_value())
        {
            const auto idx = selectedArrangementClip->first;
            if (idx >= 0 && idx < static_cast<int>(tracks.size()) && ! tracks[static_cast<std::size_t>(idx)].isMidiTrack)
                armedAudioTrack = idx;
        }

        if (armedAudioTrack < 0)
        {
            const auto sel = arrangementTimeline.getSelectedTrackIndex();
            if (sel.has_value() && *sel >= 0 && *sel < static_cast<int>(tracks.size())
                && ! tracks[static_cast<std::size_t>(*sel)].isMidiTrack)
                armedAudioTrack = *sel;
        }
    }

    if (canRecordNow && armedTrack >= 0)
    {
        // (a) Open a recording clip the instant real playback starts (after count-in),
        // even before any note is played, so the user immediately sees "recording" feedback.
        ensureMidiRecordingSession(armedTrack);

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
        finishRecordingAndDisarm();
    }

    if (canRecordNow && armedAudioTrack >= 0)
    {
        if (! audioRecordingSession.has_value() || audioRecordingSession->trackIndex != armedAudioTrack)
        {
            finalizeAudioRecordingClip();
            startAudioRecordingClip(armedAudioTrack);
        }

        if (audioRecordingSession.has_value() && audioRecordingSession->trackIndex < static_cast<int>(tracks.size()))
        {
            auto& clips = tracks[static_cast<std::size_t>(audioRecordingSession->trackIndex)].clips;
            if (audioRecordingSession->clipIndex < static_cast<int>(clips.size()))
            {
                auto& clip = clips[static_cast<std::size_t>(audioRecordingSession->clipIndex)];
                const auto rawLen = juce::jmax(0.25, transportEngine.getPlayheadBeat() - audioRecordingSession->clipStartBeat);
                clip.lengthInBeats = rawLen;

                // Feed the live capture waveform to the timeline so the clip shows its
                // waveform as it records (the file isn't readable yet).
                if (audioInputRecorder != nullptr)
                {
                    std::vector<float> mins, maxs;
                    audioInputRecorder->copyLiveWaveform(mins, maxs);
                    arrangementTimeline.setLiveRecordingWaveform(audioRecordingSession->trackIndex,
                                                                 audioRecordingSession->clipIndex,
                                                                 std::move(mins), std::move(maxs));
                }
                arrangementTimeline.repaint();
            }
        }
    }
    else if ((! playing || ! recordArmed || armedAudioTrack < 0) && audioRecordingSession.has_value())
    {
        if (! playing)
            finishRecordingAndDisarm();
        else
            finalizeAudioRecordingClip();
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
        if (! transportEngine.isRecordArmed())
        {
            finalizeRecordingClip();
            finalizeAudioRecordingClip();
        }
        startMidiRecordingFromRecordButtonIfNeeded();
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
        if (button == &metronomeButton)
        {
            const auto enabled = metronomeButton.getToggleState();
            projectState.setRecordWithMetronome(enabled);
            if (enabled)
            {
                projectState.setRecordWithCountIn(false);
                countInButton.setToggleState(false, juce::dontSendNotification);
            }
        }
        else
        {
            const auto enabled = countInButton.getToggleState();
            projectState.setRecordWithCountIn(enabled);
            if (enabled)
            {
                projectState.setRecordWithMetronome(false);
                metronomeButton.setToggleState(false, juce::dontSendNotification);
            }
        }
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
        if (browserPanelVisible)
            browserPanel.setVisible(true);
        startTimerHz(60);
        resized();
        repaint();
    }
    else if (button == &mixerButton)
    {
        toggleMixerFromUi();
    }
    else if (button == &openButton)
    {
        openProjectInteractively();
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

void MainComponent::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message)
{
    // Called on the MIDI thread. Copy the message and hand it to the message
    // thread, where it's safe to touch the project state and UI.
    juce::Component::SafePointer<MainComponent> safeThis(this);
    const juce::MidiMessage msg(message);
    juce::MessageManager::callAsync([safeThis, msg]
    {
        if (safeThis != nullptr)
            safeThis->routeLiveMidiMessage(msg);
    });
}

void MainComponent::routeLiveMidiMessage(const juce::MidiMessage& message)
{
    const auto targetTrack = resolveLiveMidiTargetTrack();

    // Note on (a note-on with velocity 0 is a note-off by MIDI convention, which
    // isNoteOn() correctly reports as false / isNoteOff() as true).
    if (message.isNoteOn())
    {
        const auto note     = message.getNoteNumber();
        const auto velocity = juce::jlimit(1, 127, static_cast<int>(message.getVelocity()));

        // Step-write into the MIDI editor when it's armed for it. It returns true
        // only when it consumed the note, in which case it has already previewed
        // the (possibly scale-snapped) pitch — so don't also play it directly.
        const bool consumedByEditor = midiEditorOverlay.isVisible()
                                      && midiEditorOverlay.stepWriteMidiNoteOn(note, velocity);

        // Chord mode expands one key into a diatonic chord for live play + recording. Step-write
        // keeps its own single-note behaviour (the editor already handled the note).
        const auto pitches = consumedByEditor ? std::vector<int>{ note } : chordPitchesForNote(note);
        if (! consumedByEditor)
        {
            liveChordVoicing[note] = pitches;
            if (targetTrack >= 0)
                for (const auto p : pitches)
                    liveMidiNoteOn(targetTrack, p, velocity);
        }

        for (const auto p : pitches)
            recordNoteOn(p, velocity);
        return;
    }

    if (message.isNoteOff())
    {
        const auto note = message.getNoteNumber();

        const bool consumedByEditor = midiEditorOverlay.isVisible()
                                      && midiEditorOverlay.stepWriteMidiNoteOff(note);

        // Release exactly the pitches this key sounded (remembered at note-on), so a chord is
        // fully released even if chord mode was toggled off while the key was held.
        std::vector<int> pitches { note };
        if (const auto it = liveChordVoicing.find(note); it != liveChordVoicing.end())
        {
            pitches = it->second;
            liveChordVoicing.erase(it);
        }

        if (! consumedByEditor && targetTrack >= 0)
            for (const auto p : pitches)
                liveMidiNoteOff(targetTrack, p);

        for (const auto p : pitches)
            recordNoteOff(p);
        return;
    }

    // Controllers / pitch bend / aftertouch only mean something to a hosted VST
    // instrument; forward them through verbatim. (The sampler has no modulation
    // inputs, so there's nothing to route them to there.)
    if (targetTrack >= 0
        && (message.isController() || message.isPitchWheel()
            || message.isAftertouch() || message.isChannelPressure()
            || message.isProgramChange()))
    {
        if (arrangementPlaybackSource != nullptr
            && arrangementPlaybackSource->hasTrackInstrument(targetTrack))
            arrangementPlaybackSource->instrumentLiveMidiMessage(targetTrack, message);
    }
}

// Resolves which track a hardware MIDI keyboard should play/record into. Priority
// follows where the user's attention is: an open recording take, then an open
// sampler/MIDI-editor panel, then the armed/selected MIDI track.
int MainComponent::resolveLiveMidiTargetTrack()
{
    auto& tracks = projectState.getTracks();
    const auto valid = [&](int idx) { return idx >= 0 && idx < static_cast<int>(tracks.size()); };

    // While a take is rolling, always sound the track we're recording into so the
    // player hears exactly what's being captured.
    if (recordingSession.has_value() && valid(recordingSession->trackIndex))
        return recordingSession->trackIndex;

    if (samplerPanel.isVisible())
    {
        const auto idx = samplerPanel.getActiveTrackIndex();
        if (valid(idx))
            return idx;
    }

    if (midiEditorOverlay.isVisible() && selectedArrangementClip.has_value()
        && valid(selectedArrangementClip->first))
        return selectedArrangementClip->first;

    return resolveArmedMidiTrack();
}

// The record-target MIDI track: an explicitly R-armed MIDI track, else the
// selected clip's track, else a selected MIDI track header. -1 if none.
int MainComponent::resolveArmedMidiTrack()
{
    auto& tracks = projectState.getTracks();

    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].isMidiTrack && tracks[i].recordArmed)
            return static_cast<int>(i);

    if (selectedArrangementClip.has_value())
    {
        const auto idx = selectedArrangementClip->first;
        if (idx >= 0 && idx < static_cast<int>(tracks.size()) && tracks[static_cast<std::size_t>(idx)].isMidiTrack)
            return idx;
    }

    const auto sel = arrangementTimeline.getSelectedTrackIndex();
    if (sel.has_value() && *sel >= 0 && *sel < static_cast<int>(tracks.size())
        && tracks[static_cast<std::size_t>(*sel)].isMidiTrack)
        return *sel;

    return -1;
}

std::vector<int> MainComponent::chordPitchesForNote(int midiNote) const
{
    // Chord mode needs a project key to be diatonic. Off → the note plays as-is.
    if (! projectState.isChordModeEnabled() || ! projectState.isKeyEnabled())
        return { midiNote };

    // Diatonic scales as semitone offsets from the tonic. Minor = natural minor.
    static constexpr int majorScale[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int minorScale[7] = { 0, 2, 3, 5, 7, 8, 10 };
    const int* scale = projectState.isKeyMinor() ? minorScale : majorScale;
    const int root = ((projectState.getKeyRoot() % 12) + 12) % 12;

    // Snap the played note to the nearest scale tone, then find its scale degree + octave.
    const int pc = (((midiNote - root) % 12) + 12) % 12;
    int degree = 0, best = 128;
    for (int i = 0; i < 7; ++i)
    {
        const int dist = std::abs(scale[i] - pc);
        if (dist < best) { best = dist; degree = i; }
    }
    const int snapped = midiNote + (scale[degree] - pc);
    const int octave = (snapped - root - scale[degree]) / 12;

    // Stack diatonic thirds (degree, +2, +4, …) so each chord's quality is correct for the key.
    const int size = juce::jlimit(3, 7, projectState.getChordSizeNotes());
    std::vector<int> pitches;
    pitches.reserve(static_cast<std::size_t>(size));
    for (int k = 0; k < size; ++k)
    {
        const int d = degree + 2 * k;
        const int o = octave + d / 7;
        const int midi = root + 12 * o + scale[d % 7];
        pitches.push_back(juce::jlimit(0, 127, midi));
    }
    return pitches;
}

bool MainComponent::samplerTrackGatesByNoteLength(int trackIndex) const
{
    const auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return false;
    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    return track.samplerMode == SamplerPlaybackMode::classic && ! track.samplerFullSampleTrigger;
}

void MainComponent::syncChordModeToSurfaces()
{
    const bool on   = projectState.isChordModeEnabled();
    const int  size = projectState.getChordSizeNotes();
    midiEditorOverlay.setChordModeExternally(on, size);
    updateTransportLabels();
    arrangementTimeline.repaint();   // track headers reflect the shared state
}

void MainComponent::liveMidiNoteOn(int trackIndex, int midiNote, int velocity)
{
    if (arrangementPlaybackSource == nullptr)
        return;

    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (arrangementPlaybackSource->hasTrackInstrument(trackIndex))
    {
        arrangementPlaybackSource->instrumentLiveNoteOn(trackIndex, midiNote, velocity);
        return;
    }

    if (track.samplerSourcePath.isNotEmpty())
        arrangementPlaybackSource->samplerNoteOn(track.samplerSourcePath,
                                                 midiNote,
                                                 velocity,
                                                 track.samplerRootMidiNote,
                                                 track.volumeDb,
                                                 track.samplerMode,
                                                 0,
                                                 track.samplerSliceCount,
                                                 track.samplerWarpEnabled,
                                                 track.samplerSourceBpm,
                                                 track.samplerFullSampleTrigger);
}

void MainComponent::liveMidiNoteOff(int trackIndex, int midiNote)
{
    if (arrangementPlaybackSource == nullptr)
        return;

    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (arrangementPlaybackSource->hasTrackInstrument(trackIndex))
        arrangementPlaybackSource->instrumentLiveNoteOff(trackIndex, midiNote);
    else
        arrangementPlaybackSource->samplerNoteOff(midiNote, track.samplerMode,
                                                  samplerTrackGatesByNoteLength(trackIndex));
}

void MainComponent::refreshMidiInputDevices()
{
    const auto devices = juce::MidiInput::getAvailableDevices();

    // Plug-and-play: enable a device the first time we ever see it, but only once.
    // After that its on/off state belongs to the user (the Settings toggles), so we
    // never re-enable something they switched off, and never re-disable on a poll.
    for (const auto& d : devices)
    {
        if (! seenMidiInputDeviceIds.contains(d.identifier))
        {
            seenMidiInputDeviceIds.add(d.identifier);
            audioDeviceManager.setMidiInputDeviceEnabled(d.identifier, true);
        }
    }

    // Attach our note callback to every enabled device, and drop it from any that
    // became disabled or were unplugged.
    for (const auto& d : devices)
    {
        const bool enabled  = audioDeviceManager.isMidiInputDeviceEnabled(d.identifier);
        const bool attached = activeMidiInputDeviceIds.contains(d.identifier);

        if (enabled && ! attached)
        {
            audioDeviceManager.addMidiInputDeviceCallback(d.identifier, this);
            activeMidiInputDeviceIds.add(d.identifier);
        }
        else if (! enabled && attached)
        {
            audioDeviceManager.removeMidiInputDeviceCallback(d.identifier, this);
            activeMidiInputDeviceIds.removeString(d.identifier);
        }
    }

    // Detach from devices that have disappeared entirely (unplugged).
    for (int i = activeMidiInputDeviceIds.size(); --i >= 0;)
    {
        const auto& id = activeMidiInputDeviceIds[i];
        const bool stillPresent = std::any_of(devices.begin(), devices.end(),
                                              [&](const auto& d) { return d.identifier == id; });
        if (! stillPresent)
        {
            audioDeviceManager.removeMidiInputDeviceCallback(id, this);
            activeMidiInputDeviceIds.remove(i);
        }
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

    const auto clipPreviewPlaying = clipEditorPreviewTransportSource.isPlaying();
    playButton.setToggleState(transportEngine.isPlaying() || transportEngine.isCountInActive() || clipPreviewPlaying,
                              juce::dontSendNotification);
    recordButton.setToggleState(transportEngine.isRecordArmed(), juce::dontSendNotification);
    loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
    metronomeButton.setToggleState(projectState.isRecordWithMetronome(), juce::dontSendNotification);
    countInButton.setToggleState(projectState.isRecordWithCountIn(), juce::dontSendNotification);
    undoButton.setEnabled(arrangementTimeline.canUndo());
    redoButton.setEnabled(arrangementTimeline.canRedo());
    tempoLabel.setText("Tempo: " + juce::String(projectState.getTempoBpm(), 0) + " BPM", juce::dontSendNotification);
    meterLabel.setText("Meter: " + juce::String(projectState.getNumerator()) + "/" + juce::String(projectState.getDenominator()), juce::dontSendNotification);
    playheadLabel.setText("Playhead: beat " + juce::String(transportEngine.getPlayheadBeat(), 2), juce::dontSendNotification);

    TransportBarState transportState;
    transportState.tempoBpm = projectState.getTempoBpm();
    transportState.projectName = currentProjectFile.existsAsFile()
        ? currentProjectFile.getFileNameWithoutExtension()
        : juce::String("Untitled");
    transportState.keyText = projectState.isKeyEnabled()
        ? formatKeyName(projectState.getKeyRoot(), projectState.isKeyMinor())
          + (projectState.isChordModeEnabled()
                 ? " ·" + juce::String(std::array<const char*, 5>{ "3","7","9","11","13" }
                                           [static_cast<std::size_t>(juce::jlimit(3, 7, projectState.getChordSizeNotes()) - 3)])
                 : juce::String())
        : juce::String("Off");
    transportState.timeSignature = juce::String(projectState.getNumerator()) + "/" + juce::String(projectState.getDenominator());
    transportState.positionText = formatTransportTime(transportEngine.getPlayheadBeat(), projectState.getTempoBpm());
    transportState.playing = transportEngine.isPlaying() || transportEngine.isCountInActive();
    transportState.recording = transportEngine.isRecordArmed();
    transportState.loop = transportEngine.isLoopEnabled();
    transportState.metronome = projectState.isRecordWithMetronome();
    transportState.countIn = projectState.isRecordWithCountIn();
    transportState.scanVisible = pluginScanVisible;
    transportState.scanProgress = pluginScanProgress;
    transportState.scanName = pluginScanNameLabel.getText();
    transportState.engineLoad = static_cast<float>(juce::jlimit(0.0, 1.0, audioDeviceManager.getCpuUsage()));
    transportState.masterGainDb = masterGainDb;
    transportState.masterLevel = juce::jlimit(0.0f, 1.0f, masterMeterLevel);
    transportState.masterLevelDb = masterMeterDb;
    transportState.mixerOpen = mixerPanel.isVisible();
    transportState.clipEditorOpen = clipEditorPanel.isVisible();
    transportState.stepSequencerOpen = stepSequencer.isVisible();
    transportBar.setState(transportState);

    menuItemsChanged();
}

void MainComponent::playBrowserPreview(const BrowserItem& item)
{
    if (! item.file.existsAsFile())
    {
        statusLabel.setText("Preview failed: file missing", juce::dontSendNotification);
        return;
    }

    // Same file already loaded at the current tempo and sync mode → just restart instantly.
    if (previewBufferSource != nullptr
        && item.file == currentPreviewFile
        && std::abs(currentPreviewTempoBpm - projectState.getTempoBpm()) < 0.001
        && currentPreviewBpmSync == browserPanel.isPreviewBpmSyncEnabled())
    {
        pendingBrowserPreviewStart = false;
        previewTransportSource.stop();
        armOrStartBrowserPreview();
        return;
    }

    statusLabel.setText("Previewing: " + item.file.getFileName(), juce::dontSendNotification);

    // Read + decode on a background thread so flipping through samples on a slow / external
    // drive never freezes the UI. A generation token discards stale loads when the user has
    // already moved on to another sample.
    const auto generation = ++previewRequestGeneration;
    pendingBrowserPreviewStart = false;
    const auto file = item.file;
    const auto displayName = item.file.getFileNameWithoutExtension();
    const auto tempoNow = projectState.getTempoBpm();
    const auto numerator = projectState.getNumerator();
    const auto fitToTempo = browserPanel.isPreviewBpmSyncEnabled();
    juce::Component::SafePointer<MainComponent> safeThis(this);

    previewLoadPool.addJob([this, safeThis, generation, file, displayName, tempoNow, numerator, fitToTempo]
    {
        std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return;

        const auto sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
        const auto maxPreviewSamples = static_cast<juce::int64>(previewMaxLengthSeconds * sampleRate);
        const auto samplesToRead = static_cast<int>(juce::jmin(reader->lengthInSamples, maxPreviewSamples));
        if (samplesToRead <= 0)
            return;

        auto buffer = std::make_shared<juce::AudioBuffer<float>>(static_cast<int>(reader->numChannels), samplesToRead);
        reader->read(buffer.get(), 0, samplesToRead, 0, true, true);

        if (fitToTempo)
        {
            const auto analysis = analyzeAudioWarpMetadata(file, tempoNow, numerator);
            *buffer = makeTempoFittedPreviewBuffer(*buffer, analysis.sourceBpm, tempoNow, sampleRate, file.getFullPathName());
        }

        // If a newer preview was requested while we were reading, drop this one.
        if (generation != previewRequestGeneration.load())
            return;

        juce::MessageManager::callAsync([safeThis, generation, buffer, sampleRate, file, displayName]
        {
            if (safeThis == nullptr || generation != safeThis->previewRequestGeneration.load())
                return;
            safeThis->startPreviewPlayback(std::move(*buffer), sampleRate, file, displayName);
        });
    });
}

void MainComponent::armOrStartBrowserPreview()
{
    previewTransportSource.setPosition(0.0);

    // Launch quantize (Ableton-style): only when synced to project tempo AND the transport
    // is actually running — without a moving playhead there's no beat to lock onto.
    if (browserPanel.isPreviewBpmSyncEnabled() && transportEngine.isPlaying())
    {
        const auto currentBeat = transportEngine.getPlayheadBeat();
        pendingBrowserPreviewStart = true;
        pendingBrowserPreviewGeneration = previewRequestGeneration.load();
        // Next whole beat. The tiny epsilon avoids "already past it" when we're a hair early.
        pendingBrowserPreviewStartBeat = std::floor(currentBeat + 1.0e-4) + 1.0;
        pendingBrowserPreviewLastBeat = currentBeat;
        browserPanel.setPreviewArmed(true);
    }
    else
    {
        pendingBrowserPreviewStart = false;
        browserPanel.setPreviewArmed(false);
        previewTransportSource.start();
    }
}

void MainComponent::startPreviewPlayback(juce::AudioBuffer<float> previewBuffer, double sampleRate,
                                         const juce::File& file, const juce::String& displayName)
{
    previewTransportSource.stop();
    previewTransportSource.setSource(nullptr);
    previewBufferSource.reset();

    // Compact waveform (normalised abs peaks) for the browser's preview bar.
    {
        constexpr int columns = 480;
        std::vector<float> peaks(columns, 0.0f);
        const auto total = previewBuffer.getNumSamples();
        const auto chans = previewBuffer.getNumChannels();
        if (total > 0 && chans > 0)
        {
            float globalMax = 1.0e-6f;
            for (int c = 0; c < columns; ++c)
            {
                const auto s0 = static_cast<juce::int64>(c) * total / columns;
                const auto s1 = juce::jmax(s0 + 1, static_cast<juce::int64>(c + 1) * total / columns);
                float m = 0.0f;
                for (int ch = 0; ch < chans; ++ch)
                {
                    const auto* d = previewBuffer.getReadPointer(ch);
                    for (auto s = s0; s < s1 && s < total; ++s)
                        m = juce::jmax(m, std::abs(d[s]));
                }
                peaks[static_cast<std::size_t>(c)] = m;
                globalMax = juce::jmax(globalMax, m);
            }
            for (auto& p : peaks) p /= globalMax;
        }
        browserPanel.setPreviewWaveform(displayName, std::move(peaks));
    }

    previewBufferSource = std::make_unique<BufferPreviewSource>(std::move(previewBuffer), sampleRate);
    previewTransportSource.setSource(previewBufferSource.get(), 0, nullptr, sampleRate);
    // Browser preview plays with headroom (quieter than unity), like Ableton — so a
    // sample audibly "opens up" / gets louder the moment you drop it into the playlist,
    // where it plays at its true level.
    previewTransportSource.setGain(juce::Decibels::decibelsToGain(browserPreviewHeadroomDb));
    currentPreviewFile = file;
    currentPreviewTempoBpm = projectState.getTempoBpm();
    currentPreviewBpmSync = browserPanel.isPreviewBpmSyncEnabled();
    armOrStartBrowserPreview();
}

void MainComponent::loadBrowserItemIntoSampler(const BrowserItem& item, bool addClipToPlaylist)
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

    arrangementTimeline.captureUndoSnapshot();   // one checkpoint for the whole load

    const auto analysis = analyzeAudioWarpMetadata(item.file, projectState.getTempoBpm(), projectState.getNumerator());
    track.samplerSourcePath = item.file.getFullPathName();
    track.samplerSourceDurationSeconds = analysis.durationSeconds;
    track.samplerSourceBpm = analysis.sourceBpm;
    track.samplerDetectedBars = analysis.detectedBars;

    // Warp ON by default for tempo'd loops, so they sync to the project tempo just like a
    // dropped playlist clip (the Warp button then toggles it OFF). One-shots stay unwarped.
    const bool isOneShot = analysis.durationSeconds > 0.0 && analysis.durationSeconds < 1.2
                           && analysis.detectedBars == 0;
    track.samplerWarpEnabled = analysis.durationSeconds > 0.0 && analysis.durationSeconds <= 90.0
                               && ! isOneShot
                               && (analysis.detectedBars > 0 || analysis.sourceBpm > 0.0);

    // Auto-transpose into the project key (shortest direction, max 6 semitones), mirroring
    // the playlist clip's key-shift default so the sampler plays in the project's key too.
    if (projectState.isKeyEnabled() && analysis.sourceKeyRoot >= 0)
    {
        int keyDiff = projectState.getKeyRoot() - analysis.sourceKeyRoot;
        while (keyDiff > 6)  keyDiff -= 12;
        while (keyDiff < -6) keyDiff += 12;
        track.samplerTransposeSemitones = keyDiff;
    }
    else
    {
        track.samplerTransposeSemitones = 0;
    }

    // Optionally drop the sample into the playlist on the same (hybrid) track at the start.
    // Double-click and "Open in sampler" both load the sampler only; dragging a sample onto a
    // lane is the gesture that places an audio clip.
    if (addClipToPlaylist)
        arrangementTimeline.addAudioClipToTrack(item.file, trackIndex, 0.0);

    // Pre-warp in the background now, so the warped buffer is cached before the first note.
    if (track.samplerWarpEnabled && arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->prewarmSamplerWarp(track.samplerSourcePath, track.samplerSourceBpm);

    stepSequencer.setVisible(false);   // only one lower panel at a time
    samplerPanel.openTrackIndex(trackIndex);
    resized();
    statusLabel.setText("Sampler loaded: " + item.file.getFileName(), juce::dontSendNotification);
    arrangementTimeline.repaint();
}

int MainComponent::findOrCreateSamplerTargetTrack()
{
    auto& tracks = projectState.getTracks();

    // Only reuse the selected track if it's a BLANK instrument track (a MIDI track with no
    // VST instrument and no sampler sample yet) — so we don't clobber a loaded VST/sampler
    // track and don't leave an empty track behind. Otherwise always create a fresh sampler
    // track, so double-clicking a sample reliably produces its own track in the playlist.
    const auto isBlankInstrumentTrack = [&tracks](int idx)
    {
        if (idx < 0 || idx >= static_cast<int>(tracks.size()))
            return false;
        const auto& t = tracks[static_cast<std::size_t>(idx)];
        return t.isMidiTrack && t.instrumentPluginId.isEmpty() && t.samplerSourcePath.isEmpty();
    };

    if (const auto sel = arrangementTimeline.getSelectedTrackIndex(); sel.has_value() && isBlankInstrumentTrack(*sel))
        return *sel;

    TrackState samplerTrack;
    samplerTrack.name = "Sampler Track";
    samplerTrack.isMidiTrack = true;
    samplerTrack.colour = theme::tracks::colourForIndex(static_cast<int>(tracks.size()));
    tracks.push_back(std::move(samplerTrack));

    const auto newIndex = static_cast<int>(tracks.size()) - 1;
    arrangementTimeline.selectTrack(newIndex);   // select + reveal the new track
    arrangementTimeline.repaint();
    return newIndex;
}

bool MainComponent::openSamplerForTrackIfAvailable(int trackIndex)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return false;

    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (! track.isMidiTrack || track.samplerSourcePath.isEmpty())
        return false;

    stepSequencer.setVisible(false);   // only one lower panel at a time
    samplerOpenedFromStep = false;     // default; the step-rack path re-sets this to true
    samplerPanel.openTrackIndex(trackIndex);
    resized();
    return true;
}

double MainComponent::getCurrentPluginSampleRate() const noexcept
{
    if (auto* device = audioDeviceManager.getCurrentAudioDevice())
        if (device->getCurrentSampleRate() > 0.0)
            return device->getCurrentSampleRate();

    return 44100.0;
}

int MainComponent::getCurrentPluginBlockSize() const noexcept
{
    if (auto* device = audioDeviceManager.getCurrentAudioDevice())
        if (device->getCurrentBufferSizeSamples() > 0)
            return device->getCurrentBufferSizeSamples();

    return 512;
}

void MainComponent::showInstrumentPicker(int trackIndex)
{
    pluginPickerTargetTrack = trackIndex;
    pluginPicker.setBounds(getLocalBounds());
    pluginPicker.show("Load Instrument", pluginManager.getInstrumentDescriptions(), pluginManager.isScanning());
    pluginPicker.toFront(true);
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
    const auto loadablePlugins = pluginManager.getInstrumentDescriptions();

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
            loadMenu.addItem(3, "Scan VST3 instruments...");
    }
    else
    {
        int id = 1000;
        for (const auto& desc : loadablePlugins)
        {
            auto label = desc.name;
            if (desc.manufacturerName.isNotEmpty())
                label += " - " + desc.manufacturerName;
            if (desc.pluginFormatName.isNotEmpty())
                label += "  (" + desc.pluginFormatName + ")";
            loadMenu.addItem(id++, label);
        }
    }
    menu.addSubMenu(hasInstrument ? "Replace instrument" : "Load instrument", loadMenu);
    menu.addSeparator();
    menu.addItem(3, pluginManager.isScanning() ? "Scanning..." : "Rescan VST3 plugins...",
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
    updateTransportLabels();
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

            updateTransportLabels();
            repaint();
        },
        [this, onFinished]()
        {
            const auto count = pluginManager.getAllDescriptions().size();
            statusLabel.setText("Found " + juce::String(count) + " plugin(s)", juce::dontSendNotification);
            pluginScanProgress = 1.0;
            pluginScanNameLabel.setText("Scan complete", juce::dontSendNotification);
            updateTransportLabels();
            repaint();
            juce::Timer::callAfterDelay(1200, [safeThis = juce::Component::SafePointer<MainComponent>(this)]
            {
                if (safeThis == nullptr)
                    return;

                safeThis->pluginScanVisible = false;
                safeThis->pluginScanNameLabel.setVisible(false);
                safeThis->updateTransportLabels();
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

    if (! description.isInstrument)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "This plugin is not an instrument",
                                               description.name + " is a VST3 effect. Effects need an insert chain; "
                                                                  "MIDI tracks can only load instruments here.");
        return;
    }

    juce::String error;
    auto instance = pluginManager.createInstance(description,
                                                getCurrentPluginSampleRate(),
                                                getCurrentPluginBlockSize(),
                                                error);
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

    stepSequencer.setVisible(false);   // only one lower panel at a time
    samplerPanel.openTrackIndex(trackIndex);
    samplerPanel.setVisible(false);
    statusLabel.setText("Instrument loaded: " + description.name, juce::dontSendNotification);
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

std::vector<TrackState::InsertFx>* MainComponent::insertChainForId(int id)
{
    if (id == ArrangementPlaybackSource::kMasterInsertKey)
        return &projectState.getMasterInserts();
    if (id >= ArrangementPlaybackSource::kBusInsertKeyBase)
    {
        const auto b = id - ArrangementPlaybackSource::kBusInsertKeyBase;
        auto& buses = projectState.getBuses();
        return (b >= 0 && b < static_cast<int>(buses.size())) ? &buses[static_cast<std::size_t>(b)].inserts : nullptr;
    }
    auto& tracks = projectState.getTracks();
    return (id >= 0 && id < static_cast<int>(tracks.size())) ? &tracks[static_cast<std::size_t>(id)].inserts : nullptr;
}

juce::String MainComponent::insertOwnerName(int id) const
{
    if (id == ArrangementPlaybackSource::kMasterInsertKey)
        return "Master";
    if (id >= ArrangementPlaybackSource::kBusInsertKeyBase)
    {
        const auto b = id - ArrangementPlaybackSource::kBusInsertKeyBase;
        const auto& buses = projectState.getBuses();
        return (b >= 0 && b < static_cast<int>(buses.size())) ? buses[static_cast<std::size_t>(b)].name : juce::String("Bus");
    }
    const auto& tracks = projectState.getTracks();
    return (id >= 0 && id < static_cast<int>(tracks.size())) ? tracks[static_cast<std::size_t>(id)].name : juce::String("Track");
}

void MainComponent::addBus()
{
    auto& buses = projectState.getBuses();
    orion::BusState bus;
    bus.name = "Bus " + juce::String(static_cast<int>(buses.size()) + 1);
    buses.push_back(std::move(bus));
    if (mixerPanel.isVisible()) mixerPanel.refreshStrips();
}

void MainComponent::showSendMenuForTrack(int trackIndex, int sendIndex)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    auto& buses = projectState.getBuses();

    juce::PopupMenu menu;

    // Existing send row: change level or remove.
    if (sendIndex >= 0 && sendIndex < static_cast<int>(track.sends.size()))
    {
        const std::array<int, 5> pcts { 10, 25, 50, 75, 100 };
        const auto& send = track.sends[static_cast<std::size_t>(sendIndex)];
        const auto cur = juce::roundToInt(send.level * 100.0);
        for (int i = 0; i < 5; ++i)
            menu.addItem(10 + i, juce::String(pcts[static_cast<std::size_t>(i)]) + "%", true, cur == pcts[static_cast<std::size_t>(i)]);
        menu.addSeparator();
        menu.addItem(8, send.prefader ? "Pre-fader (on)" : "Pre-fader", true, send.prefader);
        menu.addItem(9, "Remove send");
        menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex, sendIndex, pcts](int r)
        {
            auto& tr = projectState.getTracks();
            if (trackIndex >= static_cast<int>(tr.size())) return;
            auto& sends = tr[static_cast<std::size_t>(trackIndex)].sends;
            if (sendIndex >= static_cast<int>(sends.size())) return;
            if (r == 8) sends[static_cast<std::size_t>(sendIndex)].prefader = ! sends[static_cast<std::size_t>(sendIndex)].prefader;
            else if (r == 9) sends.erase(sends.begin() + sendIndex);
            else if (r >= 10 && r < 15) sends[static_cast<std::size_t>(sendIndex)].level = pcts[static_cast<std::size_t>(r - 10)] / 100.0;
            if (mixerPanel.isVisible()) mixerPanel.refreshStrips();
        });
        return;
    }

    // Add slot: pick a bus to send to (or create one).
    if (buses.empty())
    {
        menu.addItem(200, "New FX Bus (send here)");
    }
    else
    {
        for (int b = 0; b < static_cast<int>(buses.size()); ++b)
            menu.addItem(100 + b, juce::String::fromUTF8("\xe2\x86\x92 ") + buses[static_cast<std::size_t>(b)].name);
        menu.addSeparator();
        menu.addItem(200, "New FX Bus");
    }
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex](int r)
    {
        auto& tr = projectState.getTracks();
        if (trackIndex >= static_cast<int>(tr.size())) return;
        int busIndex = -1;
        if (r == 200) { addBus(); busIndex = static_cast<int>(projectState.getBuses().size()) - 1; }
        else if (r >= 100) busIndex = r - 100;
        if (busIndex < 0 || busIndex >= static_cast<int>(projectState.getBuses().size())) return;
        TrackState::SendFx s; s.busIndex = busIndex; s.level = 0.25;
        tr[static_cast<std::size_t>(trackIndex)].sends.push_back(s);
        if (mixerPanel.isVisible()) mixerPanel.refreshStrips();
    });
}

void MainComponent::showOutputRouteMenuForTrack(int trackIndex)
{
    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;
    const auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    const auto& buses = projectState.getBuses();

    juce::PopupMenu menu;
    menu.addItem(1, "Master", true, track.outputBus < 0);
    if (! buses.empty())
    {
        menu.addSeparator();
        for (int b = 0; b < static_cast<int>(buses.size()); ++b)
            menu.addItem(100 + b, buses[static_cast<std::size_t>(b)].name, true, track.outputBus == b);
    }
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex](int r)
    {
        auto& tr = projectState.getTracks();
        if (trackIndex >= static_cast<int>(tr.size())) return;
        if (r == 1)            tr[static_cast<std::size_t>(trackIndex)].outputBus = -1;
        else if (r >= 100)     tr[static_cast<std::size_t>(trackIndex)].outputBus = r - 100;
        if (mixerPanel.isVisible()) mixerPanel.repaint();
    });
}

void MainComponent::showInsertMenuForTrack(int trackIndex, int insertIndex)
{
    auto* chainPtr = insertChainForId(trackIndex);
    if (chainPtr == nullptr)
        return;
    auto& chain = *chainPtr;

    juce::PopupMenu menu;

    // VST3 effects list (shared by add + replace).
    juce::Array<juce::PluginDescription> effects;
    for (const auto& d : pluginManager.getAllDescriptions())
        if (! d.isInstrument)
            effects.add(d);

    // Clicking an existing insert chip: open / bypass / replace / remove.
    if (insertIndex >= 0 && insertIndex < static_cast<int>(chain.size()))
    {
        const auto& fx = chain[static_cast<std::size_t>(insertIndex)];
        menu.addItem(1, "Open " + fx.pluginName);
        menu.addItem(2, fx.bypassed ? "Un-bypass" : "Bypass", true, fx.bypassed);

        juce::PopupMenu replaceMenu;
        {
            int id = 2000;
            for (const auto& d : effects)
            {
                auto label = d.name;
                if (d.manufacturerName.isNotEmpty()) label += " - " + d.manufacturerName;
                replaceMenu.addItem(id++, label);
            }
        }
        menu.addSubMenu("Replace with", replaceMenu, ! effects.isEmpty());
        menu.addItem(3, "Remove");

        const auto list = effects;
        menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex, insertIndex, list](int r)
        {
            if (r == 1) openInsertEditor(trackIndex, insertIndex);
            else if (r == 2) toggleInsertBypass(trackIndex, insertIndex);
            else if (r == 3) removeInsertFromTrack(trackIndex, insertIndex);
            else if (r >= 2000)
            {
                const auto i = r - 2000;
                if (i >= 0 && i < list.size())
                    replaceInsertOnTrack(trackIndex, insertIndex, list.getReference(i));
            }
        });
        return;
    }

    // The "+" add slot: pick a VST3 effect to append.
    juce::PopupMenu addMenu;
    if (effects.isEmpty())
        addMenu.addItem(900, pluginManager.isScanning() ? "Scanning..." : "No VST3 effects found — Rescan...", ! pluginManager.isScanning());
    else
    {
        int id = 1000;
        for (const auto& d : effects)
        {
            auto label = d.name;
            if (d.manufacturerName.isNotEmpty()) label += " - " + d.manufacturerName;
            addMenu.addItem(id++, label);
        }
    }
    menu.addSubMenu("Add effect", addMenu);
    menu.addSeparator();
    menu.addItem(901, pluginManager.isScanning() ? "Scanning..." : "Rescan VST3 plugins...", ! pluginManager.isScanning());

    const auto list = effects;
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, trackIndex, list](int r)
    {
        if (r == 900 || r == 901) { scanPluginsInteractively(); return; }
        if (r >= 1000)
        {
            const auto i = r - 1000;
            if (i >= 0 && i < list.size())
                addInsertOnTrack(trackIndex, list.getReference(i));
        }
    });
}

void MainComponent::addInsertOnTrack(int trackIndex, const juce::PluginDescription& description)
{
    auto* chainPtr = insertChainForId(trackIndex);
    if (chainPtr == nullptr || arrangementPlaybackSource == nullptr)
        return;

    juce::String error;
    auto instance = pluginManager.createInstance(description, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
    if (instance == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not load effect",
                                               error.isNotEmpty() ? error : juce::String("Unknown error."));
        return;
    }

    const auto idx = arrangementPlaybackSource->addInsert(trackIndex, std::move(instance), false);
    if (idx < 0)
        return;

    TrackState::InsertFx fx;
    fx.pluginId = description.createIdentifierString();
    fx.pluginName = description.name;
    chainPtr->push_back(std::move(fx));

    statusLabel.setText("Insert added: " + description.name, juce::dontSendNotification);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
    arrangementTimeline.repaint();
    openInsertEditor(trackIndex, idx);
}

void MainComponent::replaceInsertOnTrack(int trackIndex, int insertIndex, const juce::PluginDescription& description)
{
    auto* chainPtr = insertChainForId(trackIndex);
    if (chainPtr == nullptr || arrangementPlaybackSource == nullptr)
        return;
    auto& chain = *chainPtr;
    if (insertIndex < 0 || insertIndex >= static_cast<int>(chain.size()))
        return;

    juce::String error;
    auto instance = pluginManager.createInstance(description, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
    if (instance == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not load effect",
                                               error.isNotEmpty() ? error : juce::String("Unknown error."));
        return;
    }

    const auto bypassed = chain[static_cast<std::size_t>(insertIndex)].bypassed;
    insertEditorWindows.erase({ trackIndex, insertIndex });

    // Engine: drop the old instance, add the new one and move it to the same slot.
    arrangementPlaybackSource->removeInsert(trackIndex, insertIndex);
    const auto appended = arrangementPlaybackSource->addInsert(trackIndex, std::move(instance), bypassed);
    if (appended != insertIndex)
        arrangementPlaybackSource->moveInsert(trackIndex, appended, trackIndex, insertIndex);

    // Model: replace the entry in place.
    TrackState::InsertFx fx;
    fx.pluginId = description.createIdentifierString();
    fx.pluginName = description.name;
    fx.bypassed = bypassed;
    chain[static_cast<std::size_t>(insertIndex)] = std::move(fx);

    statusLabel.setText("Replaced with " + description.name, juce::dontSendNotification);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
    openInsertEditor(trackIndex, insertIndex);
}

void MainComponent::removeInsertFromTrack(int trackIndex, int insertIndex)
{
    insertEditorWindows.erase({ trackIndex, insertIndex });
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->removeInsert(trackIndex, insertIndex);

    if (auto* chainPtr = insertChainForId(trackIndex))
        if (insertIndex >= 0 && insertIndex < static_cast<int>(chainPtr->size()))
            chainPtr->erase(chainPtr->begin() + insertIndex);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
}

void MainComponent::toggleInsertBypass(int trackIndex, int insertIndex)
{
    auto* chainPtr = insertChainForId(trackIndex);
    if (chainPtr == nullptr)
        return;
    auto& chain = *chainPtr;
    if (insertIndex < 0 || insertIndex >= static_cast<int>(chain.size()))
        return;
    chain[static_cast<std::size_t>(insertIndex)].bypassed = ! chain[static_cast<std::size_t>(insertIndex)].bypassed;
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->setInsertBypass(trackIndex, insertIndex, chain[static_cast<std::size_t>(insertIndex)].bypassed);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
}

void MainComponent::openInsertEditor(int trackIndex, int insertIndex)
{
    if (arrangementPlaybackSource == nullptr)
        return;
    auto* instance = arrangementPlaybackSource->getInsertInstance(trackIndex, insertIndex);
    if (instance == nullptr)
        return;

    const std::pair<int, int> key { trackIndex, insertIndex };
    if (auto it = insertEditorWindows.find(key); it != insertEditorWindows.end() && it->second != nullptr)
    {
        it->second->toFront(true);
        return;
    }

    juce::String title = insertOwnerName(trackIndex);
    if (auto* chain = insertChainForId(trackIndex); chain != nullptr
        && insertIndex >= 0 && insertIndex < static_cast<int>(chain->size()))
        title += " - " + (*chain)[static_cast<std::size_t>(insertIndex)].pluginName;

    insertEditorWindows[key] = std::make_unique<PluginEditorWindow>(
        *instance, title,
        [this, key]() { insertEditorWindows.erase(key); },
        [this](const juce::KeyPress& k) { return keyPressed(k); },
        [this](bool down) { return keyStateChanged(down); });
}

void MainComponent::moveInsert(int fromTrack, int fromIndex, int toTrack, int toIndex)
{
    auto& tracks = projectState.getTracks();
    if (fromTrack < 0 || fromTrack >= static_cast<int>(tracks.size())
        || toTrack < 0 || toTrack >= static_cast<int>(tracks.size()))
        return;
    auto& fromChain = tracks[static_cast<std::size_t>(fromTrack)].inserts;
    if (fromIndex < 0 || fromIndex >= static_cast<int>(fromChain.size()))
        return;
    if (fromTrack == toTrack && (toIndex == fromIndex || toIndex == fromIndex + 1))
        return;   // no-op move

    // Editor windows are keyed by (track,index); indices shift, so just close them.
    insertEditorWindows.clear();

    // Move in the engine (the live instance).
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->moveInsert(fromTrack, fromIndex, toTrack, toIndex);

    // Mirror in the model.
    auto fx = fromChain[static_cast<std::size_t>(fromIndex)];
    fromChain.erase(fromChain.begin() + fromIndex);
    auto& destChain = tracks[static_cast<std::size_t>(toTrack)].inserts;
    auto clamped = juce::jlimit(0, static_cast<int>(destChain.size()), toIndex);
    destChain.insert(destChain.begin() + clamped, std::move(fx));

    if (mixerPanel.isVisible()) mixerPanel.repaint();
}

void MainComponent::copyInsertToTrack(int fromTrack, int fromIndex, int toTrack)
{
    auto& tracks = projectState.getTracks();
    if (fromTrack < 0 || fromTrack >= static_cast<int>(tracks.size())
        || toTrack < 0 || toTrack >= static_cast<int>(tracks.size())
        || arrangementPlaybackSource == nullptr)
        return;
    auto& fromChain = tracks[static_cast<std::size_t>(fromTrack)].inserts;
    if (fromIndex < 0 || fromIndex >= static_cast<int>(fromChain.size()))
        return;

    const auto src = fromChain[static_cast<std::size_t>(fromIndex)];   // copy of the model entry
    const auto desc = pluginManager.findDescription(src.pluginId);
    if (! desc.has_value())
        return;

    juce::String error;
    auto instance = pluginManager.createInstance(*desc, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
    if (instance == nullptr)
        return;

    // Copy the source plugin's current settings onto the new instance.
    if (auto* srcInst = arrangementPlaybackSource->getInsertInstance(fromTrack, fromIndex))
    {
        juce::MemoryBlock state;
        srcInst->getStateInformation(state);
        if (state.getSize() > 0)
            instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }

    arrangementPlaybackSource->addInsert(toTrack, std::move(instance), src.bypassed);
    tracks[static_cast<std::size_t>(toTrack)].inserts.push_back(src);   // append copy to target

    statusLabel.setText("Copied " + src.pluginName + " to another track", juce::dontSendNotification);
    if (mixerPanel.isVisible()) mixerPanel.repaint();
}

void MainComponent::restoreInsertsFromProject()
{
    if (arrangementPlaybackSource == nullptr)
        return;
    insertEditorWindows.clear();   // old windows reference instances we're about to replace
    auto& tracks = projectState.getTracks();
    for (int t = 0; t < static_cast<int>(tracks.size()); ++t)
    {
        arrangementPlaybackSource->clearTrackInserts(t);
        for (auto& fx : tracks[static_cast<std::size_t>(t)].inserts)
        {
            const auto desc = pluginManager.findDescription(fx.pluginId);
            if (! desc.has_value())
                continue;
            juce::String error;
            auto inst = pluginManager.createInstance(*desc, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
            if (inst != nullptr)
                arrangementPlaybackSource->addInsert(t, std::move(inst), fx.bypassed);
        }
    }

    // Bus insert chains (stored under the bus key offset).
    auto& buses = projectState.getBuses();
    for (int b = 0; b < static_cast<int>(buses.size()); ++b)
    {
        const auto key = ArrangementPlaybackSource::busInsertKey(b);
        arrangementPlaybackSource->clearTrackInserts(key);
        for (auto& fx : buses[static_cast<std::size_t>(b)].inserts)
        {
            const auto desc = pluginManager.findDescription(fx.pluginId);
            if (! desc.has_value())
                continue;
            juce::String error;
            auto inst = pluginManager.createInstance(*desc, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
            if (inst != nullptr)
                arrangementPlaybackSource->addInsert(key, std::move(inst), fx.bypassed);
        }
    }

    // Master insert chain.
    arrangementPlaybackSource->clearTrackInserts(ArrangementPlaybackSource::kMasterInsertKey);
    for (auto& fx : projectState.getMasterInserts())
    {
        const auto desc = pluginManager.findDescription(fx.pluginId);
        if (! desc.has_value())
            continue;
        juce::String error;
        auto inst = pluginManager.createInstance(*desc, getCurrentPluginSampleRate(), getCurrentPluginBlockSize(), error);
        if (inst != nullptr)
            arrangementPlaybackSource->addInsert(ArrangementPlaybackSource::kMasterInsertKey, std::move(inst), fx.bypassed);
    }
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
        if (! desc->isInstrument)
            continue;  // old projects may contain an effect id in the instrument slot

        juce::String error;
        auto instance = pluginManager.createInstance(*desc,
                                                    getCurrentPluginSampleRate(),
                                                    getCurrentPluginBlockSize(),
                                                    error);
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

void MainComponent::resyncInstrumentsAfterHistory()
{
    if (arrangementPlaybackSource == nullptr)
        return;

    // Editor windows are keyed by the old track index — they'd point at the wrong slot now.
    closeAllInstrumentEditors();

    auto& tracks = projectState.getTracks();

    // Re-home every live instrument onto the restored layout by plugin id, in one atomic step.
    // Reused instances keep their exact state and need no reinstantiation (instant).
    std::vector<std::pair<int, juce::String>> wanted;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<std::size_t>(i)].instrumentPluginId.isNotEmpty())
            wanted.push_back({ i, tracks[static_cast<std::size_t>(i)].instrumentPluginId });

    const auto unsatisfied = arrangementPlaybackSource->rehomeInstrumentsFromStash(wanted);

    // Rare fallback: a wanted plugin had no live instance available (stash overflowed) —
    // reinstantiate just those from their saved state.
    for (const auto& [i, id] : unsatisfied)
    {
        const auto desc = pluginManager.findDescription(id);
        if (! desc.has_value() || ! desc->isInstrument)
            continue;

        juce::String error;
        auto instance = pluginManager.createInstance(*desc,
                                                     getCurrentPluginSampleRate(),
                                                     getCurrentPluginBlockSize(),
                                                     error);
        if (instance == nullptr)
            continue;

        const auto& base64 = tracks[static_cast<std::size_t>(i)].instrumentStateBase64;
        if (base64.isNotEmpty())
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding(base64) && block.getSize() > 0)
                instance->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        }

        arrangementPlaybackSource->setTrackInstrument(i, std::move(instance));
    }

    arrangementPlaybackSource->trimInstrumentStash(ArrangementPlaybackSource::kInstrumentStashLimit);
    arrangementTimeline.repaint();
}

void MainComponent::stopBrowserPreview(bool resetPosition)
{
    previewTransportSource.stop();
    if (resetPosition)
        previewTransportSource.setPosition(0.0);
}

void MainComponent::rebuildArrangementWarpNonBlocking()
{
    if (arrangementPlaybackSource == nullptr)
        return;
    // Kick a background decode for every audio clip so the heavy file read (a 90 s clip is ~1.5 s)
    // never blocks Play or the audio thread — that block was why a long warped clip started late.
    for (const auto& track : projectState.getTracks())
        for (const auto& clip : track.clips)
            if (clip.type == ClipType::audio && clip.sourcePath.isNotEmpty())
                arrangementPlaybackSource->prewarmAudioFile(clip.sourcePath);
    // Realtime warp configures streamers (cheap) and kicks the background producer, so the
    // heavy RubberBand stretch never runs on the message thread — pitch/warp toggles stay
    // instant instead of freezing the UI for seconds.
    if (arrangementPlaybackSource->isRealtimeWarpEnabled())
        arrangementPlaybackSource->prepareWarpStreams();
    else
        arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
}

bool MainComponent::startClipEditorPreview()
{
    auto* clip = getSelectedTimelineClip();
    if (clip == nullptr || clip->type != ClipType::audio || clip->sourcePath.isEmpty())
        return false;

    juce::File sourceFile(clip->sourcePath);
    if (! sourceFile.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(sourceFile));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
        return false;

    const auto startRatio = juce::jlimit(0.0, 0.999, clipEditorPreviewStartRatio);
    const auto endRatio = juce::jlimit(startRatio + 0.001, 1.0, clipEditorPreviewEndRatio);
    const auto totalSamples64 = reader->lengthInSamples;
    if (totalSamples64 > static_cast<juce::int64>(std::numeric_limits<int>::max()))
        return false;
    const int fullSourceSamples = static_cast<int>(totalSamples64);

    // Total pitch shift to match the arrangement (manual transpose + auto key-match).
    // Auto key-match is skipped entirely when the project has no key.
    int semitones = clip->transposeSemitones;
    if (clip->keyShiftEnabled && clip->sourceKeyRoot >= 0 && projectState.isKeyEnabled())
    {
        int keyDiff = projectState.getKeyRoot() - clip->sourceKeyRoot;
        while (keyDiff > 6)  keyDiff -= 12;
        while (keyDiff < -6) keyDiff += 12;
        semitones += keyDiff;
    }
    const auto pitchScale = std::pow(2.0, static_cast<double>(semitones) / 12.0);

    // AKAI MPC-style: warp the WHOLE source to project tempo (not just the selected
    // region), then loop the [startRatio, endRatio] selection. Because the whole source is
    // rendered, the START/END markers can be dragged anywhere during playback and the loop
    // follows live (see setLoopBounds in onSampleRangeChanged).
    const double projectBps = projectState.getTempoBpm() / 60.0;
    int fullTargetSamples = fullSourceSamples;
    double fullWarpBeats = 0.0;
    if (clip->warpEnabled && projectBps > 0.0)
    {
        const auto clipTrimStart = juce::jlimit(0.0, 0.999, clip->sampleStartRatio);
        const auto clipTrimEnd   = juce::jlimit(clipTrimStart + 0.001, 1.0, clip->sampleEndRatio);
        const auto clipTrimSpan  = juce::jmax(0.001, clipTrimEnd - clipTrimStart);
        fullWarpBeats = clip->warpTargetLengthInBeats > 0.0
            ? clip->warpTargetLengthInBeats
            : clip->lengthInBeats / clipTrimSpan;
        const double tgt = (fullWarpBeats / projectBps) * reader->sampleRate;
        fullTargetSamples = juce::jlimit(1, std::numeric_limits<int>::max(), static_cast<int>(std::llround(tgt)));
    }

    // Piecewise warp control points for the streamer: (outputRatio, sourceRatio) across the whole
    // warped source. Only when warp is on and the user has placed markers; otherwise empty = linear.
    std::vector<std::pair<double, double>> warpPts;
    juce::String warpSig;
    if (clip->warpEnabled && ! clip->warpMarkers.empty() && fullWarpBeats > 0.0)
    {
        for (const auto& p : warpControlPoints(clip->warpMarkers, fullWarpBeats))
        {
            warpPts.push_back({ p.beat / fullWarpBeats, p.sourceRatio });
            warpSig << juce::String(p.sourceRatio, 4) << ":" << juce::String(p.beat, 4) << ";";
        }
    }

    clipEditorPreviewResumeSeconds = -1.0;

    const double fullDurationSeconds = static_cast<double>(fullTargetSamples) / reader->sampleRate;
    const auto streamKey = clip->sourcePath.toStdString()
                         + "|" + std::to_string(semitones)
                         + "|t" + std::to_string(fullTargetSamples)
                         + "|w" + warpSig.toStdString();

    // Reuse the existing rendered source when nothing that affects the buffer changed
    // (same file/pitch/tempo). Its producer fills the whole source even while stopped, so
    // re-pressing Play is instant from ANY loop start — no re-render, no fill wait.
    const bool canReuse = clipEditorPreviewStreamSource != nullptr
                          && clipEditorPreviewStreamKey == streamKey;

    if (transportEngine.isPlaying() || transportEngine.isCountInActive())
    {
        transportEngine.pause();
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allInstrumentNotesOff();
            arrangementPlaybackSource->clearClipEditorPreviewTrim();
            arrangementPlaybackSource->syncToTransportPosition();
        }
    }
    stopBrowserPreview(true);
    clipEditorPreviewTransportSource.stop();

    if (! canReuse)
    {
        // Read the full source and build a fresh looping streamer.
        juce::AudioBuffer<float> region(juce::jmax(1, static_cast<int>(reader->numChannels)),
                                        juce::jmax(1, fullSourceSamples));
        region.clear();
        reader->read(&region, 0, fullSourceSamples, 0, true, true);

        clipEditorPreviewTransportSource.setSource(nullptr);
        clipEditorPreviewBufferSource.reset();
        clipEditorPreviewStreamSource.reset();
        clipEditorPreviewStreamSource = std::make_unique<StreamingWarpPreviewSource>(
            std::move(region), reader->sampleRate, juce::jmax(1, fullTargetSamples), pitchScale, std::move(warpPts));
        clipEditorPreviewStreamKey = streamKey;
        clipEditorPreviewTransportSource.setSource(clipEditorPreviewStreamSource.get(), 0, nullptr, reader->sampleRate);
    }

    clipEditorPreviewStreamSource->setLoopBounds(startRatio, endRatio);

    // The playhead maps over the FULL warped source (0..1); the loop keeps it inside the
    // selection. Start the transport AT the loop start so the playhead doesn't flash at 0.
    clipEditorLocalPreviewStartRatio = 0.0;
    clipEditorLocalPreviewEndRatio = 1.0;
    clipEditorLocalPreviewDurationSeconds = fullDurationSeconds;
    clipEditorPreviewTransportSource.setPosition(startRatio * fullDurationSeconds);
    setClipEditorLocalPreviewPosition(startRatio);
    clipEditorPreviewTransportSource.start();
    return true;
}

void MainComponent::playClipEditorPreviewBuffer(juce::AudioBuffer<float> buffer, double sampleRate,
                                                double startRatio, double endRatio,
                                                double rawDurationSeconds, double resumeSeconds)
{
    if (transportEngine.isPlaying() || transportEngine.isCountInActive())
    {
        transportEngine.pause();
        if (arrangementPlaybackSource != nullptr)
        {
            arrangementPlaybackSource->allInstrumentNotesOff();
            arrangementPlaybackSource->clearClipEditorPreviewTrim();
            arrangementPlaybackSource->syncToTransportPosition();
        }
    }

    stopBrowserPreview(true);
    clipEditorPreviewTransportSource.stop();
    clipEditorPreviewTransportSource.setSource(nullptr);
    clipEditorPreviewStreamSource.reset();   // drop the streaming stand-in, if any
    clipEditorPreviewBufferSource = std::make_unique<BufferPreviewSource>(std::move(buffer), sampleRate);
    clipEditorPreviewTransportSource.setSource(clipEditorPreviewBufferSource.get(), 0, nullptr, sampleRate);
    clipEditorPreviewTransportSource.setPosition(0.0);
    clipEditorLocalPreviewStartRatio = startRatio;
    clipEditorLocalPreviewEndRatio = endRatio;
    clipEditorLocalPreviewDurationSeconds = rawDurationSeconds;
    setClipEditorLocalPreviewPosition(startRatio);

    if (resumeSeconds >= 0.0)
        clipEditorPreviewTransportSource.setPosition(juce::jlimit(0.0, clipEditorLocalPreviewDurationSeconds, resumeSeconds));

    clipEditorPreviewTransportSource.start();
}

void MainComponent::stopClipEditorPreview(bool resetToStart)
{
    // Keep the streaming source alive and attached so re-pressing Play reuses its already-
    // rendered buffer (instant). Its producer keeps filling even while stopped. The source
    // is only torn down when the clip/pitch/tempo changes (key mismatch) or on shutdown.
    clipEditorPreviewTransportSource.stop();
    if (clipEditorPreviewStreamSource != nullptr)
        clipEditorPreviewTransportSource.setPosition(clipEditorPreviewStartRatio * clipEditorLocalPreviewDurationSeconds);
    if (resetToStart)
        setClipEditorLocalPreviewPosition(clipEditorPreviewStartRatio);
}

void MainComponent::updateClipEditorPreviewPlayhead()
{
    if (! clipEditorPreviewTransportSource.isPlaying())
        return;

    if (clipEditorLocalPreviewDurationSeconds <= 0.0)
    {
        stopClipEditorPreview(true);
        return;
    }

    // The preview loops the selection (MPC-style), so it doesn't auto-stop. The streaming
    // source's read position maps directly onto the full warped source: ratio = position /
    // full-output-duration, which lands inside the live loop region.
    const auto position = clipEditorPreviewTransportSource.getCurrentPosition();
    clipEditorPreviewPlayheadRatio = juce::jlimit(0.0, 1.0, position / clipEditorLocalPreviewDurationSeconds);
}

void MainComponent::startGlobalSpacePreview(double startBeat)
{
    if (! globalSpacePreviewRestoreBeat.has_value())
    {
        globalSpacePreviewRestoreBeat = transportEngine.getPlayheadBeat();
        globalSpacePreviewWasRecordArmed = transportEngine.isRecordArmed();
    }

    stopBrowserPreview(true);
    stopClipEditorPreview(true);

    // NB: do NOT panic the instruments here. allInstrumentNotesOff() starts a 3-block panic
    // that skips clip note-ons, which dropped the very first note when the preview started
    // right before it. (Stop still panics to silence the tail.)

    transportEngine.pause();
    transportController.setRecordArmed(false);
    transportEngine.setPlayheadBeat(startBeat);

    if (arrangementPlaybackSource != nullptr)
    {
        if (arrangementPlaybackSource->isRealtimeWarpEnabled())
            arrangementPlaybackSource->prepareWarpStreams(/*blockForLead*/ true, /*allowSyncDecode*/ true);
        else
            arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();

        arrangementPlaybackSource->syncToTransportPosition();
    }

    transportEngine.play(false);
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::commitGlobalSpacePreview()
{
    // A space TAP turns the momentary preview into normal playback: keep playing and drop
    // the rewind anchor so it doesn't snap back.
    globalSpacePreviewRestoreBeat.reset();
    updateTransportLabels();
}

void MainComponent::stopGlobalSpacePreview()
{
    if (! globalSpacePreviewRestoreBeat.has_value())
        return;

    transportEngine.pause();

    if (arrangementPlaybackSource != nullptr)
    {
        arrangementPlaybackSource->allSamplerNotesOff();
        arrangementPlaybackSource->allInstrumentNotesOff();
    }

    const auto restoreBeat = *globalSpacePreviewRestoreBeat;
    globalSpacePreviewRestoreBeat.reset();
    transportController.setRecordArmed(globalSpacePreviewWasRecordArmed);
    transportEngine.setPlayheadBeat(restoreBeat);

    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->syncToTransportPosition();

    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::toggleTransportFromUi()
{
    // NB: previously, with the clip editor open, Play hijacked to an isolated local loop of
    // the selected clip and never started the transport — so the playlist didn't play. Now
    // Play always drives the arrangement, so the clip plays in context with the rest of the
    // playlist. (The local preview still re-auditions automatically on transpose changes.)

    // The main transport ALWAYS drives the arrangement now, even with the clip editor open, so you can
    // hear the whole mix while tweaking a clip. The clip-editor loop preview lives on its own ▶ button
    // in the clip editor (onLoopPreviewToggle), so it never hijacks the global Play again.

    // Count-in belongs to recording only. Keep normal playback instant even if the
    // user previously selected "4-count before recording" in the REC options.
    const auto useCountIn = transportEngine.isRecordArmed() && projectState.isRecordWithCountIn();

    transportController.togglePlayback(
        useCountIn,
        [this]()
        {
            // Kill BOTH previews so they don't double up with the arrangement on the
            // master bus (a lingering clip-editor preview otherwise adds +6 dB).
            stopBrowserPreview(true);
            stopClipEditorPreview(true);
        },
        [this]()
        {
            if (arrangementPlaybackSource == nullptr)
                return;
            // Real-time warp: configure streamers (cheap) + start the background producer.
            // The heavy stretching happens off-thread, so Play is instant and glitch-free.
            // allowSyncDecode: if a source somehow isn't decoded yet (played the instant after a
            // drop), decode it here so Play always has audio — never regress "plays right away".
            if (arrangementPlaybackSource->isRealtimeWarpEnabled())
                arrangementPlaybackSource->prepareWarpStreams(/*blockForLead*/ true, /*allowSyncDecode*/ true);
            else
                arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
        },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    updateTransportLabels();
}

void MainComponent::startMidiRecordingFromRecordButtonIfNeeded()
{
    if (! transportEngine.isRecordArmed()
        || transportEngine.isPlaying()
        || transportEngine.isCountInActive())
        return;

    auto& tracks = projectState.getTracks();
    auto targetTrack = resolveArmedMidiTrack();
    if (targetTrack < 0)
    {
        const auto audioArmed = std::any_of(tracks.begin(), tracks.end(), [](const TrackState& track)
        {
            return ! track.isMidiTrack && track.recordArmed;
        });
        if (audioArmed)
            return;

        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        {
            if (tracks[static_cast<std::size_t>(i)].isMidiTrack)
            {
                targetTrack = i;
                break;
            }
        }
    }

    if (targetTrack < 0)
        return;

    if (targetTrack < static_cast<int>(tracks.size()))
    {
        tracks[static_cast<std::size_t>(targetTrack)].recordArmed = true;
        arrangementTimeline.repaint();
        mixerPanel.repaint();
    }

    toggleTransportFromUi();
}

void MainComponent::stopTransportFromUi()
{
    finishRecordingAndDisarm();
    stopClipEditorPreview(true);
    samplerPanel.stopPreviewPlayback(); // halt the simpler audition + waveform playhead
    if (arrangementPlaybackSource != nullptr)
    {
        arrangementPlaybackSource->allInstrumentNotesOff(); // flush any hung instrument notes
        arrangementPlaybackSource->allSamplerNotesOff();    // and any sampler audition voices
    }
    transportController.stop(
        [this]() { stopBrowserPreview(true); },
        [this]()
        {
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->syncToTransportPosition();
        });
    if (clipEditorPanel.isVisible())
    {
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->clearClipEditorPreviewTrim();
        setClipEditorLocalPreviewPosition(clipEditorPreviewStartRatio);
        refreshClipEditor();
    }
    updateTransportLabels();
    arrangementTimeline.repaint();
}

void MainComponent::finishRecordingAndDisarm()
{
    const auto hadRecording = recordingSession.has_value() || audioRecordingSession.has_value();

    finalizeRecordingClip(); // close any in-flight MIDI recording session
    finalizeAudioRecordingClip();

    if (hadRecording || transportEngine.isRecordArmed())
    {
        transportController.setRecordArmed(false);
        recordButton.setToggleState(false, juce::dontSendNotification);
    }

    updateTransportLabels();
}

void MainComponent::ensureMidiRecordingSession(int armedTrack)
{
    auto& tracks = projectState.getTracks();
    if (armedTrack < 0 || armedTrack >= static_cast<int>(tracks.size()))
        return;
    if (recordingSession.has_value() && recordingSession->trackIndex == armedTrack)
        return;   // already recording this track

    finalizeRecordingClip();
    const auto playheadBeat = transportEngine.getPlayheadBeat();
    auto& track = tracks[static_cast<std::size_t>(armedTrack)];
    arrangementTimeline.captureUndoSnapshot();

    // Overdub: if the playhead is inside an existing MIDI clip on this track, record INTO it
    // (merge the new notes) instead of stacking a separate overlapping clip.
    int targetClip = -1;
    for (int i = 0; i < static_cast<int>(track.clips.size()); ++i)
    {
        const auto& c = track.clips[i];
        if (c.type == ClipType::midi
            && playheadBeat >= c.startBeat - 1.0e-6
            && playheadBeat < c.startBeat + c.lengthInBeats - 1.0e-6)
        {
            targetClip = i;
            break;
        }
    }

    RecordingSession session;
    session.trackIndex = armedTrack;
    if (targetClip >= 0)
    {
        track.clips[static_cast<std::size_t>(targetClip)].recording = true;
        session.clipIndex     = targetClip;
        session.clipStartBeat = track.clips[static_cast<std::size_t>(targetClip)].startBeat;
    }
    else
    {
        const auto clipStart = std::floor(playheadBeat);
        track.clips.push_back(TimelineClip {
            "Recording",
            ClipType::midi,
            clipStart,
            juce::jmax(0.25, playheadBeat - clipStart),
            track.colour.brighter(0.1f),
            {}, {}, "", 0.0, false, false,
            0.0, 0.0, 0,
            false, false, 0.0,
            -1, false, true
        });
        track.clips.back().recording = true;
        session.clipIndex     = static_cast<int>(track.clips.size()) - 1;
        session.clipStartBeat = clipStart;
    }
    recordingSession = std::move(session);
}

void MainComponent::recordNoteOn(int pitch, int velocity)
{
    if (! transportEngine.isRecordArmed())
        return;
    if (! transportEngine.isPlaying())
    {
        // Not playing yet. Capture ONLY the anticipated first chord struck in the LAST BEAT of the
        // count-in (it lands at the clip start, beat 0) — players hit the downbeat a hair early. Notes
        // played earlier in the count-in aren't part of the take, so they're ignored: that avoids a
        // whole rushed progression piling up at beat 0 (which read as "only one chord recorded").
        if (! transportEngine.isCountInActive())
            return;
        const double countInBeats = static_cast<double>(juce::jmax(1, projectState.getNumerator()));
        if (transportEngine.getClickBeat() < countInBeats - 1.0)
            return;
    }
    // Open the session on the spot if the timer hasn't yet — otherwise the very first note
    // (played in the gap between count-in ending and the next timer tick) was dropped.
    if (! recordingSession.has_value())
        ensureMidiRecordingSession(resolveArmedMidiTrack());
    if (! recordingSession.has_value())
        return;

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

juce::File MainComponent::getAudioRecordingDirectory() const
{
    if (currentProjectFile.existsAsFile())
        return currentProjectFile.getParentDirectory().getChildFile("Recordings");

    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Orion Recordings");
}

void MainComponent::updateInputMonitoring()
{
    if (audioInputRecorder == nullptr)
        return;

    bool anyAudioArmed = false;
    for (const auto& t : projectState.getTracks())
        if (! t.isMidiTrack && t.recordArmed) { anyAudioArmed = true; break; }

    const bool wantInput = anyAudioArmed || audioInputRecorder->isRecording();
    if (wantInput && ! audioRecorderCallbackAttached)
    {
        if (! ensureAudioInputReady(true))
            return;

        audioDeviceManager.addAudioCallback(audioInputRecorder.get());
        audioRecorderCallbackAttached = true;
    }
    else if (! wantInput && audioRecorderCallbackAttached)
    {
        audioDeviceManager.removeAudioCallback(audioInputRecorder.get());
        audioRecorderCallbackAttached = false;
    }
}

void MainComponent::requestMicrophonePermissionAtLaunch()
{
    if (! juce::RuntimePermissions::isRequired(juce::RuntimePermissions::recordAudio)
        || juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio)
        || audioInputPermissionRequestInFlight)
    {
        return;
    }

    audioInputPermissionRequestInFlight = true;
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
                                      [safeThis](bool granted)
                                      {
                                          if (safeThis == nullptr)
                                              return;

                                          safeThis->audioInputPermissionRequestInFlight = false;
                                          if (granted)
                                              safeThis->updateInputMonitoring();
                                      });
}

bool MainComponent::ensureAudioInputReady(bool requestPermission)
{
    if (juce::RuntimePermissions::isRequired(juce::RuntimePermissions::recordAudio)
        && ! juce::RuntimePermissions::isGranted(juce::RuntimePermissions::recordAudio))
    {
        if (requestPermission && ! audioInputPermissionRequestInFlight)
        {
            audioInputPermissionRequestInFlight = true;
            juce::Component::SafePointer<MainComponent> safeThis(this);
            juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
                                              [safeThis](bool granted)
                                              {
                                                  if (safeThis == nullptr)
                                                      return;

                                                  safeThis->audioInputPermissionRequestInFlight = false;
                                                  if (granted)
                                                      safeThis->updateInputMonitoring();
                                                  else
                                                      safeThis->statusLabel.setText("Microphone permission denied. Enable it in macOS Privacy settings.",
                                                                                    juce::dontSendNotification);
                                              });
        }
        return false;
    }

    auto* currentDevice = audioDeviceManager.getCurrentAudioDevice();
    if (currentDevice == nullptr)
        return false;

    if (currentDevice->getActiveInputChannels().countNumberOfSetBits() > 0)
        return true;

    auto setup = audioDeviceManager.getAudioDeviceSetup();
    setup.inputChannels.clear();
    const auto inputCount = currentDevice->getInputChannelNames().size();
    setup.inputChannels.setRange(0, juce::jmin(2, inputCount), true);
    if (setup.inputChannels.countNumberOfSetBits() <= 0)
        return false;

    const auto error = audioDeviceManager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
    {
        statusLabel.setText("Audio input failed: " + error, juce::dontSendNotification);
        return false;
    }

    return audioDeviceManager.getCurrentAudioDevice() != nullptr
        && audioDeviceManager.getCurrentAudioDevice()->getActiveInputChannels().countNumberOfSetBits() > 0;
}

void MainComponent::startAudioRecordingClip(int trackIndex)
{
    if (audioInputRecorder == nullptr)
        return;

    auto& tracks = projectState.getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        return;

    auto& track = tracks[static_cast<std::size_t>(trackIndex)];
    if (track.isMidiTrack)
        return;

    if (! ensureAudioInputReady(true))
        return;

    auto* currentDevice = audioDeviceManager.getCurrentAudioDevice();
    const auto inputChannels = currentDevice != nullptr
        ? juce::jmin(2, currentDevice->getActiveInputChannels().countNumberOfSetBits())
        : 0;
    if (inputChannels <= 0)
    {
        statusLabel.setText("No audio input selected. Open Settings and enable a microphone/input.",
                            juce::dontSendNotification);
        return;
    }

    const auto playheadBeat = transportEngine.getPlayheadBeat();
    const auto clipStart = std::floor(playheadBeat);
    const auto sampleRate = currentDevice != nullptr && currentDevice->getCurrentSampleRate() > 0.0
        ? currentDevice->getCurrentSampleRate()
        : 44100.0;
    const auto directory = getAudioRecordingDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    const auto file = directory.getNonexistentChildFile("Orion_Take_" + stamp, ".wav", false);

    juce::String error;
    if (! audioInputRecorder->start(file, sampleRate, inputChannels, error))
    {
        statusLabel.setText("Audio recording failed: " + error, juce::dontSendNotification);
        return;
    }

    if (! audioRecorderCallbackAttached)
    {
        audioDeviceManager.addAudioCallback(audioInputRecorder.get());
        audioRecorderCallbackAttached = true;
    }

    TimelineClip clip;
    clip.name = "Audio Recording";
    clip.type = ClipType::audio;
    clip.startBeat = clipStart;
    clip.lengthInBeats = juce::jmax(0.25, playheadBeat - clipStart);
    clip.colour = track.colour.brighter(0.1f);
    clip.sourcePath = file.getFullPathName();
    clip.sourceDurationSeconds = 0.0;
    clip.sourceBpm = projectState.getTempoBpm();
    clip.detectedBars = 0;
    clip.warpEnabled = false;
    clip.bpmGuessed = false;
    clip.warpTargetLengthInBeats = 0.0;
    clip.sourceKeyRoot = -1;
    clip.sourceKeyIsMinor = false;
    clip.keyShiftEnabled = false;
    clip.recording = true;

    // Checkpoint so the finished take can be removed with Cmd+Z (and cleaned up on cancel).
    arrangementTimeline.captureUndoSnapshot();
    track.clips.push_back(std::move(clip));

    AudioRecordingSession session;
    session.trackIndex = trackIndex;
    session.clipIndex = static_cast<int>(track.clips.size()) - 1;
    session.clipStartBeat = clipStart;
    session.sampleRate = sampleRate;
    session.file = file;
    audioRecordingSession = std::move(session);

    statusLabel.setText("Recording audio: " + file.getFileName(), juce::dontSendNotification);
    arrangementTimeline.repaint();
}

void MainComponent::finalizeAudioRecordingClip()
{
    // Note: the input callback stays attached for monitoring while a track is armed;
    // updateInputMonitoring() (and shutdown) own detaching it. Here we only stop the
    // writer so the WAV file is flushed/closed.
    if (! audioRecordingSession.has_value())
    {
        if (audioInputRecorder != nullptr)
            audioInputRecorder->stop();
        return;
    }

    const auto session = *audioRecordingSession;
    audioRecordingSession.reset();
    arrangementTimeline.clearLiveRecordingWaveform();

    const auto samplesWritten = audioInputRecorder != nullptr ? audioInputRecorder->stop() : 0;
    const auto durationSeconds = session.sampleRate > 0.0
        ? static_cast<double>(samplesWritten) / session.sampleRate
        : 0.0;
    const auto lengthBeats = durationSeconds * projectState.getTempoBpm() / 60.0;

    auto& tracks = projectState.getTracks();
    if (session.trackIndex >= 0 && session.trackIndex < static_cast<int>(tracks.size()))
    {
        auto& clips = tracks[static_cast<std::size_t>(session.trackIndex)].clips;
        if (session.clipIndex >= 0 && session.clipIndex < static_cast<int>(clips.size()))
        {
            auto& clip = clips[static_cast<std::size_t>(session.clipIndex)];
            if (samplesWritten <= static_cast<juce::int64>(session.sampleRate * 0.05))
            {
                clips.erase(clips.begin() + session.clipIndex);
                session.file.deleteFile();
                arrangementTimeline.dropLastUndoSnapshot();   // nothing kept — no undo entry
                statusLabel.setText("Audio recording discarded: no input captured", juce::dontSendNotification);
            }
            else
            {
                clip.recording = false;
                clip.sourceDurationSeconds = durationSeconds;
                clip.lengthInBeats = juce::jmax(0.25, lengthBeats);
                clip.warpTargetLengthInBeats = clip.lengthInBeats;
                statusLabel.setText("Recorded audio: " + session.file.getFileName(), juce::dontSendNotification);
            }
        }
    }

    arrangementTimeline.repaint();
}

void MainComponent::cancelRecording()
{
    auto& tracks = projectState.getTracks();

    // Discard the in-progress audio take: stop the recorder, remove its clip + file.
    if (audioRecordingSession.has_value())
    {
        const auto session = *audioRecordingSession;
        audioRecordingSession.reset();
        arrangementTimeline.clearLiveRecordingWaveform();
        if (audioInputRecorder != nullptr)
            audioInputRecorder->stop();
        if (session.trackIndex >= 0 && session.trackIndex < static_cast<int>(tracks.size()))
        {
            auto& clips = tracks[static_cast<std::size_t>(session.trackIndex)].clips;
            if (session.clipIndex >= 0 && session.clipIndex < static_cast<int>(clips.size()))
                clips.erase(clips.begin() + session.clipIndex);
        }
        session.file.deleteFile();
        arrangementTimeline.dropLastUndoSnapshot();
    }

    // Discard the in-progress MIDI take.
    if (recordingSession.has_value())
    {
        const auto session = *recordingSession;
        recordingSession.reset();
        if (session.trackIndex >= 0 && session.trackIndex < static_cast<int>(tracks.size()))
        {
            auto& clips = tracks[static_cast<std::size_t>(session.trackIndex)].clips;
            if (session.clipIndex >= 0 && session.clipIndex < static_cast<int>(clips.size()))
                clips.erase(clips.begin() + session.clipIndex);
        }
        arrangementTimeline.dropLastUndoSnapshot();
    }

    stopTransportFromUi();
    statusLabel.setText("Recording cancelled", juce::dontSendNotification);
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
    if (loopButton.getToggleState() && arrangementTimeline.loopToSelectedClip())
    {
        loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
        updateTransportLabels();
        arrangementTimeline.repaint();
        return;
    }

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

void MainComponent::toggleMixerFromUi()
{
    if (mixerPanel.isVisible())
    {
        mixerPanel.closePanel();
    }
    else
    {
        mixerPanel.setBounds(getLocalBounds());
        mixerPanel.open();
    }
}

void MainComponent::toggleClipEditorFromUi()
{
    const auto shouldOpen = ! clipEditorPanel.isVisible();
    if (shouldOpen)
    {
        // The clip editor edits the SELECTED audio clip — don't open an empty editor when
        // there's nothing to edit (e.g. no clips in the playlist / no clip selected).
        if (auto* clip = getSelectedTimelineClip(); clip == nullptr || clip->type != ClipType::audio)
        {
            statusLabel.setText("Select an audio clip to open the Clip Editor", juce::dontSendNotification);
            updateTransportLabels();   // keep the navbar button reflecting "closed"
            return;
        }

        samplerPanel.setVisible(false);
        stepSequencer.setVisible(false);
        refreshClipEditor();
    }
    else
    {
        stopClipEditorPreview(true);
    }

    clipEditorPanel.setVisible(shouldOpen);
    resized();
    updateTransportLabels();
}

void MainComponent::toggleStepSequencerFromUi()
{
    const auto shouldOpen = ! stepSequencer.isVisible();
    if (shouldOpen)
    {
        // Only one lower panel at a time.
        samplerPanel.setVisible(false);
        clipEditorPanel.setVisible(false);
        stopClipEditorPreview(true);

        // Give any still-default (grey) sampler channels distinct colours.
        auto& tracks = projectState.getTracks();
        for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
            if (tracks[static_cast<std::size_t>(i)].colour == juce::Colour(0xff9db0c4))
                tracks[static_cast<std::size_t>(i)].colour = theme::tracks::colourForIndex(i);
    }

    stepSequencer.setVisible(shouldOpen);
    resized();
    updateTransportLabels();
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
    auto defaultTarget = defaultDirectory.getChildFile("Untitled.orion");
    saveFileChooser = std::make_unique<juce::FileChooser>("Save Orion Project",
                                                          defaultTarget,
                                                          "*.orion");

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

                                     if (! selectedFile.hasFileExtension("orion"))
                                         selectedFile = selectedFile.withFileExtension(".orion");

                                     saveToTarget(selectedFile);
                                 });
}

void MainComponent::loadSidebarBrowserFolders()
{
    sidebarBrowserFolders.clear();

    auto settings = makeUserSettingsFile();
    if (settings == nullptr)
        return;

    juce::StringArray paths;
    paths.addLines(settings->getValue(sidebarFoldersSettingsKey));

    for (const auto& path : paths)
    {
        const auto folder = juce::File(path);
        if (! folder.isDirectory())
            continue;

        const auto folderPath = folder.getFullPathName();
        const auto alreadyAdded = std::any_of(sidebarBrowserFolders.begin(), sidebarBrowserFolders.end(),
                                              [&folderPath](const juce::File& existing)
                                              {
                                                  return existing.getFullPathName() == folderPath;
                                              });
        if (! alreadyAdded)
            sidebarBrowserFolders.push_back(folder);
    }

    sidebarNav.setCustomFolders(sidebarBrowserFolders);
}

void MainComponent::saveSidebarBrowserFolders() const
{
    auto settings = makeUserSettingsFile();
    if (settings == nullptr)
        return;

    juce::StringArray paths;
    for (const auto& folder : sidebarBrowserFolders)
        if (folder.isDirectory())
            paths.addIfNotAlreadyThere(folder.getFullPathName());

    settings->setValue(sidebarFoldersSettingsKey, paths.joinIntoString("\n"));
    settings->saveIfNeeded();
}

void MainComponent::openProjectInteractively()
{
    auto defaultDirectory = currentProjectFile.existsAsFile()
                                ? currentProjectFile.getParentDirectory()
                                : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

    // Accept the new ".orion" extension and the legacy ".orion.json" so older
    // projects still open.
    openFileChooser = std::make_unique<juce::FileChooser>("Open Orion Project",
                                                          defaultDirectory,
                                                          "*.orion;*.orion.json");

    auto chooserFlags = juce::FileBrowserComponent::openMode
                      | juce::FileBrowserComponent::canSelectFiles;

    openFileChooser->launchAsync(chooserFlags,
                                 [this](const juce::FileChooser& chooser)
                                 {
                                     auto selectedFile = chooser.getResult();
                                     openFileChooser.reset();

                                     if (selectedFile == juce::File())
                                     {
                                         statusLabel.setText("Open cancelled", juce::dontSendNotification);
                                         return;
                                     }

                                     loadProjectFromFile(selectedFile);
                                 });
}

void MainComponent::loadProjectFromFile(const juce::File& file)
{
    // Make sure nothing is playing/recording while we swap the project out from
    // under the audio engine.
    stopTransportFromUi();

    juce::String errorMessage;
    if (! ProjectSerializer::loadFromFile(projectState, file, &errorMessage))
    {
        statusLabel.setText("Open failed: " + errorMessage, juce::dontSendNotification);
        return;
    }

    currentProjectFile = file;

    // Older builds created sampler/MIDI tracks with this fixed grey, which made every MIDI
    // clip on those tracks grey too. Restore palette colours on load without touching any
    // user-chosen colours.
    auto& loadedTracks = projectState.getTracks();
    constexpr auto legacySamplerGrey = juce::uint32 { 0xff9db0c4 };
    for (int i = 0; i < static_cast<int>(loadedTracks.size()); ++i)
    {
        auto& track = loadedTracks[static_cast<std::size_t>(i)];
        if (track.isMidiTrack
            && track.samplerSourcePath.isNotEmpty()
            && track.colour == juce::Colour(legacySamplerGrey))
        {
            const auto paletteColour = theme::tracks::colourForIndex(i);
            track.colour = paletteColour;
            for (auto& clip : track.clips)
                if (clip.colour == juce::Colour(legacySamplerGrey))
                    clip.colour = paletteColour.brighter(0.1f);
        }
    }

    // Re-instantiate hosted VST instruments from the loaded track state, and drop
    // any selection/history that referred to the previous project.
    restoreInstrumentsFromProject();
    restoreInsertsFromProject();
    arrangementTimeline.resetForNewProject();
    selectedArrangementClip.reset();

    // Recompute derived audio-clip lengths, refresh transport + inspector UI.
    refreshAudioClipWarpLengths();
    refreshClipInspector();
    refreshClipEditor();
    loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
    updateTransportLabels();

    if (arrangementPlaybackSource != nullptr)
    {
        arrangementPlaybackSource->syncToTransportPosition();

        // Warm the warp engine right after loading so the FIRST Play is instant — previously
        // warp streams / sampler buffers were only prepared on the first press, so opening a
        // project and hitting play had an audible delay before sound.
        if (arrangementPlaybackSource->isRealtimeWarpEnabled())
            arrangementPlaybackSource->prepareWarpStreams();
        else
            arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();

        for (const auto& track : projectState.getTracks())
            if (track.isMidiTrack && track.samplerSourcePath.isNotEmpty() && track.samplerWarpEnabled)
                arrangementPlaybackSource->prewarmSamplerWarp(track.samplerSourcePath, track.samplerSourceBpm);
    }

    rewindTransportFromUi();
    resetToPlaylistView();
    statusLabel.setText("Opened: " + file.getFileName(), juce::dontSendNotification);
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
        isOrionWarpEnabled(),
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
        [this](bool orionOn)
        {
            setOrionWarpEnabled(orionOn);
            // Re-render warped clips with the chosen backend on next playback.
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
            statusLabel.setText(orionOn ? "Warp engine: Orion (experimental)" : "Warp engine: default",
                                juce::dontSendNotification);
        },
        [this]()
        {
            // A MIDI device was toggled in Settings — attach/detach its callback now.
            refreshMidiInputDevices();
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
    options.content->setSize(560, 640);
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
                // Fast (no audio decode) — this runs in bulk over every clip and on every
                // tempo change, so it must not block. Deep key/tempo is filled by the
                // background worker (maybeStartBackgroundAnalysis).
                const auto analysis = analyzeAudioWarpMetadata(sourceFile, projectState.getTempoBpm(), projectState.getNumerator(), false);
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
                const auto trimStart = juce::jlimit(0.0, 0.999, clip.sampleStartRatio);
                const auto trimEnd = juce::jlimit(trimStart + 0.001, 1.0, clip.sampleEndRatio);
                const auto trimSpan = juce::jmax(0.001, trimEnd - trimStart);
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

                if (trimSpan < 0.999 && clip.warpTargetLengthInBeats > 0.0
                    && std::abs(clip.warpTargetLengthInBeats - clip.lengthInBeats) <= 0.001)
                {
                    clip.warpTargetLengthInBeats = juce::jmax(1.0, clip.lengthInBeats / trimSpan);
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

void MainComponent::maybeStartBackgroundAnalysis()
{
    if (analysisJobActive.load(std::memory_order_acquire))
        return;

    // Distinct files of clips awaiting signal analysis (key/tempo) OR gain normalization.
    juce::StringArray paths;
    for (const auto& track : projectState.getTracks())
        for (const auto& clip : track.clips)
            if ((clip.signalAnalysisPending || clip.gainNormalizationPending)
                && clip.type == ClipType::audio && clip.sourcePath.isNotEmpty())
                paths.addIfNotAlreadyThere(clip.sourcePath);

    if (paths.isEmpty())
        return;

    const auto tempo = projectState.getTempoBpm();
    const auto numerator = projectState.getNumerator();
    analysisJobActive.store(true, std::memory_order_release);
    juce::Component::SafePointer<MainComponent> safe(this);

    analysisThreadPool.addJob([safe, paths, tempo, numerator]
    {
        // Heavy work off the message thread: decode + chroma key + autocorrelation tempo.
        auto results = std::make_shared<std::map<juce::String, orion::AudioWarpAnalysis>>();

        // Own format manager — the component's may be gone by the time this runs.
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        for (const auto& p : paths)
        {
            auto analysis = orion::analyzeAudioWarpMetadata(juce::File(p), tempo, numerator, true);

            // Measure the source peak (dBFS) so the clip can be auto-normalised to 0.
            std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(juce::File(p)));
            if (reader != nullptr && reader->lengthInSamples > 0)
            {
                constexpr int chunk = 16384;
                juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), chunk);
                float peak = 0.0f;
                juce::int64 pos = 0;
                while (pos < reader->lengthInSamples)
                {
                    const auto n = static_cast<int>(juce::jmin<juce::int64>(chunk, reader->lengthInSamples - pos));
                    buffer.clear();
                    reader->read(&buffer, 0, n, pos, true, true);
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, n));
                    pos += n;
                }
                if (peak > 0.000001f)
                    analysis.peakDb = static_cast<double>(juce::Decibels::gainToDecibels(peak, -60.0f));
            }

            (*results)[p] = analysis;
        }

        juce::MessageManager::callAsync([safe, results]
        {
            if (auto* self = safe.getComponent())
                self->applyBackgroundAnalysis(*results);
        });
    });
}

void MainComponent::applyBackgroundAnalysis(const std::map<juce::String, orion::AudioWarpAnalysis>& results)
{
    bool changed = false;
    for (auto& track : projectState.getTracks())
        for (auto& clip : track.clips)
        {
            if (! clip.signalAnalysisPending && ! clip.gainNormalizationPending)
                continue;
            const auto it = results.find(clip.sourcePath);
            if (it == results.end())
                continue;
            const auto& a = it->second;

            if (clip.signalAnalysisPending)
            {
                if (clip.sourceKeyRoot < 0 && a.sourceKeyRoot >= 0)
                {
                    clip.sourceKeyRoot    = a.sourceKeyRoot;
                    clip.sourceKeyIsMinor = a.sourceKeyIsMinor;
                }
                if (clip.sourceBpm <= 0.0 && a.sourceBpm > 0.0)
                {
                    clip.sourceBpm   = a.sourceBpm;
                    clip.bpmGuessed  = a.bpmGuessed;
                }
                if (clip.detectedBars <= 0 && a.detectedBars > 0)
                    clip.detectedBars = a.detectedBars;
                if (clip.sourceDurationSeconds <= 0.0 && a.durationSeconds > 0.0)
                    clip.sourceDurationSeconds = a.durationSeconds;
                clip.signalAnalysisPending = false;
            }

            if (clip.gainNormalizationPending)
            {
                // Keep imported audio at unity. Auto gain changes made dropped loops
                // unpredictably louder and could clip when several tracks summed.
                clip.gainNormalizationPending = false;
            }

            changed = true;
        }

    analysisJobActive.store(false, std::memory_order_release);

    if (changed)
    {
        refreshAudioClipWarpLengths();
        refreshClipInspector();
        arrangementTimeline.repaint();
    }
}

void MainComponent::refreshClipEditor()
{
    ClipEditorState editorState;
    const auto* clip = getSelectedTimelineClip();
    editorState.hasSelection = clip != nullptr;

    // Catch-all: never leave the clip editor open with nothing to edit (e.g. the last clip
    // was deleted). It only makes sense for a selected audio clip.
    if (clipEditorPanel.isVisible() && (clip == nullptr || clip->type != ClipType::audio))
    {
        stopClipEditorPreview(true);
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->clearClipEditorPreviewTrim();
        clipEditorPanel.setVisible(false);
        resized();
        updateTransportLabels();
        return;
    }

    if (clip != nullptr)
    {
        editorState.isAudioClip = clip->type == ClipType::audio;
        editorState.title = clip->name;
        editorState.fileName = clip->sourcePath.isNotEmpty()
                                   ? juce::File(clip->sourcePath).getFileName()
                                   : juce::String();
        editorState.sourcePath = clip->sourcePath;
        editorState.accent = clip->colour;
        editorState.startBeat = clip->startBeat;
        editorState.lengthInBeats = clip->lengthInBeats;
        // Full source length in beats (the clip's length is only the trimmed portion).
        // Needed so dragging a region out of an already-trimmed clip keeps its speed.
        {
            const auto clipTrimStart = juce::jlimit(0.0, 0.999, clip->sampleStartRatio);
            const auto clipTrimEnd   = juce::jlimit(clipTrimStart + 0.001, 1.0, clip->sampleEndRatio);
            const auto clipTrimSpan  = juce::jmax(0.001, clipTrimEnd - clipTrimStart);
            editorState.sourceLengthBeats = clip->warpTargetLengthInBeats > 0.0
                ? clip->warpTargetLengthInBeats
                : clip->lengthInBeats / clipTrimSpan;
        }
        editorState.gainDb = clip->gainDb;
        editorState.sourceBpm = clip->sourceBpm;
        editorState.detectedBars = clip->detectedBars;
        editorState.transposeSemitones = clip->transposeSemitones;
        // Auto key-shift into the project key (mirrors computeKeyShiftSemitones), shown in CLIP INFO.
        editorState.autoKeyShiftActive = clip->keyShiftEnabled && clip->sourceKeyRoot >= 0 && projectState.isKeyEnabled();
        if (editorState.autoKeyShiftActive)
        {
            int keyDiff = projectState.getKeyRoot() - clip->sourceKeyRoot;
            while (keyDiff > 6)  keyDiff -= 12;
            while (keyDiff < -6) keyDiff += 12;
            editorState.autoKeyShiftSemitones = keyDiff;
        }
        else
            editorState.autoKeyShiftSemitones = 0;
        if (! selectedArrangementClip.has_value() || clipEditorPreviewClip != selectedArrangementClip)
        {
            stopClipEditorPreview(false);
            if (arrangementPlaybackSource != nullptr)
                arrangementPlaybackSource->clearClipEditorPreviewTrim();
            clipEditorPreviewClip = selectedArrangementClip;
            auto previewStart = clip->sampleStartRatio;
            auto previewEnd = clip->sampleEndRatio;
            if (selectedArrangementClip.has_value())
            {
                if (const auto it = clipEditorSelectionRanges.find(*selectedArrangementClip);
                    it != clipEditorSelectionRanges.end())
                {
                    previewStart = it->second.first;
                    previewEnd = it->second.second;
                }
            }

            clipEditorPreviewStartRatio = juce::jlimit(0.0, 0.999, previewStart);
            clipEditorPreviewEndRatio = juce::jlimit(clipEditorPreviewStartRatio + 0.001, 1.0, previewEnd);
            clipEditorPreviewPlayheadRatio = clipEditorPreviewStartRatio;
        }
        editorState.sampleStartRatio = juce::jlimit(0.0, 0.999, clipEditorPreviewStartRatio);
        editorState.sampleEndRatio = juce::jlimit(editorState.sampleStartRatio + 0.001, 1.0, clipEditorPreviewEndRatio);
        // While the preview is playing it sweeps the range that was locked in at Play time
        // (clipEditorLocalPreview*). Clamp the displayed playhead to THAT range, not the
        // editor's live selection — otherwise dragging the start/end markers pins the line
        // to the moving marker and it flickers against the real playback position.
        // Interpolate the preview playhead with wall-clock time so it sweeps smoothly instead of
        // stepping with the audio-block-quantised transport position (that was the "low FPS" jerk).
        if (clipEditorPreviewTransportSource.isPlaying() && clipEditorLocalPreviewDurationSeconds > 0.0)
        {
            const double duration = clipEditorLocalPreviewDurationSeconds;
            const double startSec = clipEditorLocalPreviewStartRatio * duration;
            const double endSec   = clipEditorLocalPreviewEndRatio * duration;
            const double actualSec = clipEditorPreviewTransportSource.getCurrentPosition();
            const double now = juce::Time::getMillisecondCounterHiRes();
            const double dt = juce::jlimit(0.0, 0.1, (now - clipEditorPlayheadWallMs) / 1000.0);
            clipEditorPlayheadWallMs = now;

            // Loop wrap or seek → the reported position jumps; snap the smoothed value to it.
            const bool discontinuity = actualSec < clipEditorLastActualPlayheadSec - 0.001
                                    || std::abs(actualSec - clipEditorSmoothPlayheadSec) > 0.15;
            clipEditorLastActualPlayheadSec = actualSec;

            if (discontinuity)
                clipEditorSmoothPlayheadSec = actualSec;
            else
                // advance by real time (1x playback) then gently correct drift toward the true position
                clipEditorSmoothPlayheadSec += dt + (actualSec - clipEditorSmoothPlayheadSec) * 0.10;

            clipEditorSmoothPlayheadSec = juce::jlimit(startSec, endSec, clipEditorSmoothPlayheadSec);
            clipEditorPreviewPlayheadRatio = juce::jlimit(0.0, 1.0, clipEditorSmoothPlayheadSec / duration);
        }
        else
        {
            // Keep the wall clock fresh so the first frame of the next playback has a small dt.
            clipEditorPlayheadWallMs = juce::Time::getMillisecondCounterHiRes();
        }

        auto previewSourceRatio = clipEditorPreviewTransportSource.isPlaying()
            ? juce::jlimit(clipEditorLocalPreviewStartRatio, clipEditorLocalPreviewEndRatio, clipEditorPreviewPlayheadRatio)
            : juce::jlimit(editorState.sampleStartRatio, editorState.sampleEndRatio, clipEditorPreviewPlayheadRatio);
        if (! clipEditorPreviewTransportSource.isPlaying()
            && transportEngine.isPlaying()
            && editorState.isAudioClip
            && clip->lengthInBeats > 0.0)
        {
            const auto trimStart = juce::jlimit(0.0, 0.999, clip->sampleStartRatio);
            const auto trimEnd = juce::jlimit(trimStart + 0.001, 1.0, clip->sampleEndRatio);
            const auto trimSpan = juce::jmax(0.001, trimEnd - trimStart);
            const auto clipProgress = juce::jlimit(0.0,
                                                   1.0,
                                                   (transportEngine.getPlayheadBeat() - clip->startBeat) / clip->lengthInBeats);
            previewSourceRatio = juce::jlimit(editorState.sampleStartRatio,
                                              editorState.sampleEndRatio,
                                              trimStart + clipProgress * trimSpan);
            clipEditorPreviewPlayheadRatio = previewSourceRatio;
        }
        editorState.previewSourceRatio = previewSourceRatio;
        editorState.playheadIsBeatTime = clipEditorPreviewTransportSource.isPlaying();
        editorState.playheadBeat = transportEngine.getPlayheadBeat();
        editorState.playing = transportEngine.isPlaying() || clipEditorPreviewTransportSource.isPlaying();
        editorState.warpEnabled = clip->warpEnabled;
        editorState.keyShiftEnabled = clip->keyShiftEnabled;
        editorState.warpMarkers = clip->warpMarkers;
        if (editorState.isAudioClip && clip->sourcePath.isNotEmpty())
        {
            rebuildClipEditorWaveform(clip->sourcePath);
            const auto key = clip->sourcePath.toStdString();
            if (const auto it = clipEditorWaveformCache.find(key); it != clipEditorWaveformCache.end())
            {
                editorState.waveformMin = it->second.first;
                editorState.waveformMax = it->second.second;
            }
        }

        if (selectedArrangementClip.has_value())
        {
            const auto trackIndex = selectedArrangementClip->first;
            const auto& tracks = projectState.getTracks();
            if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size()))
                editorState.trackName = tracks[static_cast<std::size_t>(trackIndex)].name;
        }
    }
    else if (arrangementPlaybackSource != nullptr)
    {
        stopClipEditorPreview(false);
        arrangementPlaybackSource->clearClipEditorPreviewTrim();
    }

    clipEditorPanel.setState(editorState);
}

void MainComponent::setClipEditorLocalPreviewPosition(double sourceRatio)
{
    const auto previewStart = juce::jlimit(0.0, 0.999, clipEditorPreviewStartRatio);
    const auto previewEnd = juce::jlimit(previewStart + 0.001, 1.0, clipEditorPreviewEndRatio);
    clipEditorPreviewPlayheadRatio = juce::jlimit(previewStart, previewEnd, sourceRatio);
}

bool MainComponent::setClipEditorPreviewPlaybackPosition(double sourceRatio)
{
    if (! selectedArrangementClip.has_value())
        return false;

    auto* clip = getSelectedTimelineClip();
    if (clip == nullptr || clip->type != ClipType::audio)
        return false;

    const auto previewStart = juce::jlimit(0.0, 0.999, clipEditorPreviewStartRatio);
    const auto previewEnd = juce::jlimit(previewStart + 0.001, 1.0, clipEditorPreviewEndRatio);
    setClipEditorLocalPreviewPosition(juce::jlimit(previewStart, previewEnd, sourceRatio));

    return true;
}

void MainComponent::rebuildClipEditorWaveform(const juce::String& sourcePath)
{
    if (sourcePath.isEmpty())
        return;

    const auto key = sourcePath.toStdString();
    if (clipEditorWaveformCache.find(key) != clipEditorWaveformCache.end())
        return;

    juce::File file(sourcePath);
    if (! file.existsAsFile())
        return;

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
        return;

    constexpr int targetBuckets = 1400;
    const auto totalSamples = static_cast<juce::int64>(reader->lengthInSamples);
    const auto samplesPerBucket = juce::jmax<juce::int64>(1, totalSamples / targetBuckets);
    const auto bucketCount = static_cast<int>((totalSamples + samplesPerBucket - 1) / samplesPerBucket);
    const auto channelCount = static_cast<int>(reader->numChannels);

    std::vector<float> minVals(static_cast<std::size_t>(bucketCount), 0.0f);
    std::vector<float> maxVals(static_cast<std::size_t>(bucketCount), 0.0f);

    constexpr int chunkSize = 8192;
    juce::AudioBuffer<float> chunk(channelCount, chunkSize);
    juce::int64 processed = 0;
    while (processed < totalSamples)
    {
        const auto toRead = static_cast<int>(juce::jmin<juce::int64>(chunkSize, totalSamples - processed));
        chunk.clear();
        if (! reader->read(&chunk, 0, toRead, processed, true, true))
            break;

        for (int sample = 0; sample < toRead; ++sample)
        {
            const auto bucket = static_cast<int>((processed + sample) / samplesPerBucket);
            if (bucket < 0 || bucket >= bucketCount)
                continue;

            float value = 0.0f;
            for (int channel = 0; channel < channelCount; ++channel)
                value += chunk.getSample(channel, sample);
            value /= static_cast<float>(channelCount);

            auto& minValue = minVals[static_cast<std::size_t>(bucket)];
            auto& maxValue = maxVals[static_cast<std::size_t>(bucket)];
            minValue = juce::jmin(minValue, value);
            maxValue = juce::jmax(maxValue, value);
        }
        processed += toRead;
    }

    clipEditorWaveformCache.emplace(key, std::make_pair(std::move(minVals), std::move(maxVals)));
}

void MainComponent::normalizeSelectedAudioClip()
{
    auto* clip = getSelectedTimelineClip();
    if (clip == nullptr || clip->type != ClipType::audio || clip->sourcePath.isEmpty())
        return;

    const auto sourceFile = juce::File(clip->sourcePath);
    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(sourceFile));
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        statusLabel.setText("Normalize failed: can't read clip", juce::dontSendNotification);
        return;
    }

    constexpr int chunkSize = 16384;
    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), chunkSize);
    float peak = 0.0f;
    juce::int64 position = 0;

    while (position < reader->lengthInSamples)
    {
        const auto samplesToRead = static_cast<int>(juce::jmin<juce::int64>(chunkSize, reader->lengthInSamples - position));
        buffer.clear();
        reader->read(&buffer, 0, samplesToRead, position, true, true);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            peak = juce::jmax(peak, buffer.getMagnitude(channel, 0, samplesToRead));
        position += samplesToRead;
    }

    if (peak <= 0.000001f)
    {
        statusLabel.setText("Normalize skipped: silent clip", juce::dontSendNotification);
        return;
    }

    constexpr double normalizeTargetDb = -3.0;
    const auto peakDb = static_cast<double>(juce::Decibels::gainToDecibels(peak, -60.0f));
    const auto gainDb = juce::jlimit(-24.0, 12.0, normalizeTargetDb - peakDb);
    clip->gainDb = gainDb;
    statusLabel.setText("Normalized clip: " + juce::String(gainDb, 1) + " dB", juce::dontSendNotification);
    refreshClipInspector();
    refreshClipEditor();
    arrangementTimeline.repaint();
}

void MainComponent::refreshClipInspector()
{
    const auto* clip = getSelectedTimelineClip();
    if (clip == nullptr || clip->type != ClipType::audio)
    {
        if (const auto selectedTrack = arrangementTimeline.getSelectedTrackIndex(); selectedTrack.has_value())
        {
            const auto& tracks = projectState.getTracks();
            if (*selectedTrack >= 0 && *selectedTrack < static_cast<int>(tracks.size()))
            {
                const auto& track = tracks[static_cast<std::size_t>(*selectedTrack)];

                setClipInspectorVisible(false);

                SelectionInspectorModel inspectorModel;
                inspectorModel.title = track.name;
                inspectorModel.subtitle = track.isMidiTrack ? "MIDI Track" : "Audio Track";
                inspectorModel.detail = track.isMidiTrack ? "Instrument / MIDI" : "Audio lane";
                inspectorModel.accent = track.colour;
                inspectorModel.gainDb = track.volumeDb;
                inspectorModel.muted = track.muted;
                inspectorModel.solo = track.solo;
                inspectorModel.showWarp = false;
                inspectorModel.hasSelection = true;
                selectionInspector.setModel(inspectorModel);
                selectionInspector.setVisible(false);
                resized();
                repaint();
                return;
            }
        }

        setClipInspectorVisible(false);
        selectionInspector.setModel({});
        selectionInspector.setVisible(false);
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
            // Fast (no audio decode) — the inspector must not stall on selection. Deep
            // key/tempo is filled by the background worker.
            const auto analysis = analyzeAudioWarpMetadata(sourceFile, projectState.getTempoBpm(), projectState.getNumerator(), false);
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
        // NOTE: no prepareWarpCacheForCurrentTempo() here — it RubberBand-stretches every
        // warp clip synchronously, which froze the UI on drop/selection (a 1-minute clip
        // took ~4s). The warp cache is built on Play.
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

    SelectionInspectorModel inspectorModel;
    inspectorModel.title = clip->name;
    inspectorModel.subtitle = projectState.getTracks()[static_cast<std::size_t>(trackIndex)].name;
    inspectorModel.detail = compactInspectorFileName(sourceFile, clip->name);
    inspectorModel.accent = clip->colour;
    inspectorModel.gainDb = clip->gainDb;
    inspectorModel.muted = clip->muted;
    inspectorModel.solo = clip->solo;
    inspectorModel.warpEnabled = clip->warpEnabled;
    inspectorModel.showWarp = true;
    inspectorModel.hasSelection = true;
    selectionInspector.setModel(inspectorModel);
    selectionInspector.setVisible(false);

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
    // Toggle the whole tonality feature on/off (checkmark shows current state).
    menu.addItem(1, "Project key", true, projectState.isKeyEnabled());
    menu.addSeparator();
    menu.addSubMenu("Major", majorSub);
    menu.addSubMenu("Minor", minorSub);

    // Chord mode: one key → diatonic chord in the project key.
    menu.addSeparator();
    menu.addItem(2, "Chord mode (one key = chord)", projectState.isKeyEnabled(), projectState.isChordModeEnabled());
    juce::PopupMenu chordSub;
    static const std::array<std::pair<const char*, int>, 5> chordTypes {{
        { "Triad", 3 }, { "7th", 4 }, { "9th", 5 }, { "11th", 6 }, { "13th", 7 } }};
    for (const auto& [name, size] : chordTypes)
        chordSub.addItem(300 + size, name, true, projectState.getChordSizeNotes() == size);
    menu.addSubMenu("Chord type", chordSub, projectState.isChordModeEnabled());

    const auto keyBounds = transportBar.getKeyBounds().translated(transportBar.getX(), transportBar.getY());
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withTargetScreenArea(
        localAreaToGlobal(keyBounds.isEmpty() ? cachedKeyCardBounds : keyBounds)),
        [this](int result)
        {
            if (result <= 0) return;

            if (result == 2)
            {
                projectState.setChordModeEnabled(! projectState.isChordModeEnabled());
                syncChordModeToSurfaces();
                return;
            }
            if (result >= 303 && result <= 307)
            {
                projectState.setChordSizeNotes(result - 300);
                projectState.setChordModeEnabled(true);   // picking a type turns the mode on
                syncChordModeToSurfaces();
                return;
            }

            if (result == 1)
            {
                // Flip tonality on/off. Rebuild warped buffers so already-placed
                // samples drop their key-match pitch shift (or pick it back up).
                projectState.setKeyEnabled(! projectState.isKeyEnabled());
                if (arrangementPlaybackSource != nullptr)
                    arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
                midiEditorOverlay.setScaleLockExternally(projectState.isKeyEnabled() && projectState.isScaleLockEnabled());
                updateTransportLabels();
                refreshClipInspector();
                arrangementTimeline.repaint();
                repaint();
                return;
            }

            const auto isMinor = result >= 200;
            const auto root    = (isMinor ? result - 200 : result - 100) % 12;
            projectState.setKeyEnabled(true);   // choosing a key re-enables tonality
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
