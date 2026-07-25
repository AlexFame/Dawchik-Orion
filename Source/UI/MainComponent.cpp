#include "MainComponent.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include "../Audio/AudioInputRecorder.h"
#include "../Audio/LoudnessMeter.h"
#include "../Audio/OrionStretchEngine.h"
#include "../Audio/PlaybackSources.h"
#include "../Audio/WarpEngine.h"
#include "../Sampler/SamplerEngine.h"
#include "OrionTheme.h"
#include "MainComponentInternal.h"

namespace
{
namespace th = orion::theme;
using orion::backgroundColour, orion::panelColour, orion::accentColour, orion::panelStroke,
    orion::mutedText, orion::transportShelfColour, orion::transportShelfStroke,
    orion::transportButtonColour, orion::transportButtonText, orion::transportDarkPanel,
    orion::transportSectionFill, orion::transportSectionStroke, orion::recordAccent,
    orion::minBrowserPanelWidth, orion::maxBrowserPanelWidth, orion::browserResizeHandleWidth,
    orion::transportShelfHeight, orion::workspaceTopGap, orion::transportBrandWidth,
    orion::transportClusterWidth, orion::transportTempoWidth, orion::transportModeWidth,
    orion::transportUtilityWidth, orion::transportSectionGap, orion::transportControlHeight,
    orion::transportSectionHeight, orion::transportContentVerticalNudge, orion::samplerPanelHeight,
    orion::sidebarFoldersSettingsKey;

enum MenuItemId
{
    menuProjectNew = 1001,
    menuProjectOpen,
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
        setColour(juce::PopupMenu::highlightedBackgroundColourId, th::accent::activeCoral);
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
        browserWidthLabel.setColour(juce::Label::textColourId, th::text::secondary);
        addAndMakeVisible(browserWidthLabel);

        browserWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        browserWidthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 24);
        browserWidthSlider.setRange(minBrowserPanelWidth, maxBrowserPanelWidth, 1.0);
        browserWidthSlider.setValue(initialBrowserWidth, juce::dontSendNotification);
        browserWidthSlider.setColour(juce::Slider::trackColourId, accentColour);
        browserWidthSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
        browserWidthSlider.setColour(juce::Slider::backgroundColourId, th::line::subtle);
        browserWidthSlider.setColour(juce::Slider::textBoxTextColourId, th::text::primary);
        browserWidthSlider.setColour(juce::Slider::textBoxBackgroundColourId, th::core::canvas);
        browserWidthSlider.setColour(juce::Slider::textBoxOutlineColourId, th::line::normal);
        browserWidthSlider.onValueChange = [this]
        {
            browserWidthChanged(static_cast<int>(std::round(browserWidthSlider.getValue())));
        };
        addAndMakeVisible(browserWidthSlider);

        exportSampleRateLabel.setText("Export Sample Rate", juce::dontSendNotification);
        exportSampleRateLabel.setColour(juce::Label::textColourId, th::text::secondary);
        addAndMakeVisible(exportSampleRateLabel);

        exportSampleRateBox.addItem("44.1 kHz", 44100);
        exportSampleRateBox.addItem("48 kHz", 48000);
        exportSampleRateBox.addItem("96 kHz", 96000);
        exportSampleRateBox.setSelectedId(initialExportSampleRate, juce::dontSendNotification);
        exportSampleRateBox.setColour(juce::ComboBox::backgroundColourId, th::core::canvas);
        exportSampleRateBox.setColour(juce::ComboBox::textColourId, th::text::primary);
        exportSampleRateBox.setColour(juce::ComboBox::outlineColourId, th::line::normal);
        exportSampleRateBox.setColour(juce::ComboBox::arrowColourId, th::text::primary);
        exportSampleRateBox.onChange = [this]
        {
            const auto selected = exportSampleRateBox.getSelectedId();
            if (selected > 0)
                exportSampleRateChanged(selected);
        };
        addAndMakeVisible(exportSampleRateBox);

        orionWarpToggle.setButtonText("Orion warp engine (experimental)");
        orionWarpToggle.setColour(juce::ToggleButton::textColourId, th::text::primary);
        orionWarpToggle.setColour(juce::ToggleButton::tickColourId, accentColour);
        orionWarpToggle.setToggleState(initialOrionWarp, juce::dontSendNotification);
        orionWarpToggle.onClick = [this]
        {
            if (orionWarpChanged)
                orionWarpChanged(orionWarpToggle.getToggleState());
        };
        addAndMakeVisible(orionWarpToggle);

        midiLabel.setText("MIDI Input", juce::dontSendNotification);
        midiLabel.setColour(juce::Label::textColourId, th::text::secondary);
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
        audioLabel.setColour(juce::Label::textColourId, th::text::secondary);
        addAndMakeVisible(audioLabel);

        addAndMakeVisible(audioSelector);
        limitAudioBufferChoices();

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
        g.fillAll(th::core::studio);
        g.setColour(th::line::normal);
        g.drawRect(getLocalBounds(), 1);
    }

    void limitAudioBufferChoices()
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        if (device == nullptr)
            return;

        juce::ComboBox* bufferBox = nullptr;
        std::function<void(juce::Component*)> findBufferBox = [&bufferBox, &findBufferBox](juce::Component* component)
        {
            if (auto* combo = dynamic_cast<juce::ComboBox*>(component))
            {
                if (combo->getText().containsIgnoreCase("samples"))
                {
                    bufferBox = combo;
                    return;
                }
            }

            for (int i = 0; i < component->getNumChildComponents() && bufferBox == nullptr; ++i)
                findBufferBox(component->getChildComponent(i));
        };
        findBufferBox(&audioSelector);

        if (bufferBox == nullptr)
            return;

        const int abletonSizes[] { 32, 64, 128, 256, 512, 1024, 2048 };
        const auto currentSize = device->getCurrentBufferSizeSamples();
        const auto deviceSizes = device->getAvailableBufferSizes();
        juce::Array<int> availableSizes;

        for (const auto size : abletonSizes)
        {
            if (deviceSizes.contains(size))
                availableSizes.add(size);
        }

        if (currentSize > 0 && ! availableSizes.contains(currentSize))
            availableSizes.add(currentSize);

        if (availableSizes.isEmpty())
            return;

        bufferBox->clear(juce::dontSendNotification);
        for (const auto size : availableSizes)
            bufferBox->addItem(juce::String(size) + " samples", size);
        bufferBox->setSelectedId(currentSize, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(24, 22);
        titleLabel.setBounds(area.removeFromTop(30));
        area.removeFromTop(18);

        constexpr int labelWidth = 154;
        constexpr int rowHeight = 30;
        constexpr int rowGap = 10;
        const auto layoutRow = [](juce::Rectangle<int> row, juce::Label& label, juce::Component& control)
        {
            label.setBounds(row.removeFromLeft(labelWidth).withTrimmedTop(5));
            row.removeFromLeft(12);
            control.setBounds(row);
        };

        layoutRow(area.removeFromTop(rowHeight), browserWidthLabel, browserWidthSlider);
        area.removeFromTop(rowGap);

        auto exportRow = area.removeFromTop(rowHeight);
        exportSampleRateLabel.setBounds(exportRow.removeFromLeft(labelWidth).withTrimmedTop(5));
        exportRow.removeFromLeft(12);
        exportSampleRateBox.setBounds(exportRow.removeFromLeft(170));
        area.removeFromTop(rowGap);

        auto warpRow = area.removeFromTop(rowHeight);
        warpRow.removeFromLeft(labelWidth + 12);
        orionWarpToggle.setBounds(warpRow);
        area.removeFromTop(18);

        auto headerRow = area.removeFromTop(28);
        midiLabel.setBounds(headerRow.removeFromLeft(labelWidth).withTrimmedTop(4));
        midiRescanButton.setBounds(headerRow.removeFromRight(92).reduced(0, 1));
        area.removeFromTop(6);

        const int midiRows = midiDeviceToggles.isEmpty() ? 1 : midiDeviceToggles.size();
        const int midiHeight = juce::jmin(midiRows * 26, juce::jmax(26, area.getHeight() - 320));
        auto midiArea = area.removeFromTop(midiHeight);
        if (midiDeviceToggles.isEmpty())
        {
            midiEmptyLabel.setBounds(midiArea.removeFromTop(26));
        }
        else
        {
            for (auto* toggle : midiDeviceToggles)
                toggle->setBounds(midiArea.removeFromTop(26));
        }
        area.removeFromTop(18);

        auto footerArea = area.removeFromBottom(52);
        saveButton.setBounds(footerArea.removeFromRight(124).withSizeKeepingCentre(124, 36));
        audioLabel.setBounds(area.removeFromTop(22));
        area.removeFromTop(6);
        audioSelector.setBounds(area);
        limitAudioBufferChoices();
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
        menu.addItem(menuProjectNew, "New");
        menu.addItem(menuProjectOpen, "Open...");
        juce::PopupMenu recentMenu;
        recentProjects.createPopupMenuItems(recentMenu, recentProjectBaseMenuId, false, true);
        menu.addSubMenu("Open Recent", recentMenu, recentMenu.getNumItems() > 0);
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
    if (menuItemID >= recentProjectBaseMenuId && menuItemID < recentProjectBaseMenuId + 100)
    {
        openRecentProject(menuItemID - recentProjectBaseMenuId);
        return;
    }

    switch (menuItemID)
    {
        case menuProjectNew:      newProjectInteractively(); break;
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

    // Keep painted labels and JUCE-native controls on the same macOS typeface. Without this,
    // browser rows use Avenir Next while TextEditor/TextButton fall back to another sans-serif.
    getLookAndFeel().setDefaultSansSerifTypefaceName("Avenir Next");
    transportButtonLookAndFeel.setDefaultSansSerifTypefaceName("Avenir Next");

    buildLabelsAndInspector();

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

    arrangementTimeline.onNormalizeClips = [this](const std::vector<ArrangementTimelineComponent::SelectedClip>& sel, bool relativeToLoudest)
    {
        normalizeClips(sel, relativeToLoudest);
    };
    arrangementTimeline.onSetClipsGainDb = [this](const std::vector<ArrangementTimelineComponent::SelectedClip>& sel, double gainDb)
    {
        setClipsGainDb(sel, gainDb);
    };
    arrangementTimeline.onMatchClipLoudness = [this](const std::vector<ArrangementTimelineComponent::SelectedClip>& sel)
    {
        matchClipLoudness(sel);
    };

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
        // The MPC panel isn't bound to a track, so arming record with it open would have no
        // target and silently fail to start. Give it a MIDI track to capture pad hits.
        if (shouldRecord && mpcSamplePanel.isVisible())
            ensureMpcRecordTrack();
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
        // INDEPENDENT toggles — previously "4-count" forcibly turned the metronome OFF, so you could
        // never have a count-in AND the click running through the whole take.
        menu.addItem(1, "Metronome (whole take)", true, withMetro);
        menu.addItem(2, "4-count before recording", true, withPrecount);
        menu.showMenuAsync(juce::PopupMenu::Options{}
                                .withTargetComponent(&transportBar)
                                .withTargetScreenArea(transportBar.localAreaToGlobal(transportBar.getRecordOptionsBounds())),
            [this, withMetro, withPrecount](int result)
            {
                if (result == 1)
                {
                    projectState.setRecordWithMetronome(! withMetro);
                    metronomeButton.setToggleState(! withMetro, juce::dontSendNotification);
                }
                else if (result == 2)
                {
                    projectState.setRecordWithCountIn(! withPrecount);
                    countInButton.setToggleState(! withPrecount, juce::dontSendNotification);
                }
                updateTransportLabels();
            });
    };
    transportBar.onMetronomeChanged = [this](bool enabled)
    {
        // Metronome and count-in are independent: turning the click on must not cancel the count-in.
        metronomeButton.setToggleState(enabled, juce::dontSendNotification);
        projectState.setRecordWithMetronome(enabled);
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
    transportBar.onMpcSample = [this]() { toggleMpcSampleFromUi(); };
    transportBar.onJam = [this]() { toggleJamSessionFromUi(); };
    addAndMakeVisible(transportBar);

    jamSession.setEmbeddedArrangementMode(true);
    jamSession.setVisible(false);
    jamSession.onClose = [this]() { toggleJamSessionFromUi(); };
    jamSession.onCreateSessionRequested = [this]() { startJamHosting(); };
    jamSession.onJoinSessionRequested   = [this]() { joinJamSession(); };
    jamSession.onLeaveSessionRequested  = [this]() { leaveJamSession(); };
    jamSession.onSendChat = [this](const juce::String& text) { sendJamChat(text); };
    jamSession.onMicEnabledChanged = [this](bool enabled)
    {
        return ! enabled || ensureAudioInputReady(true);
    };
    jamSession.onCameraEnabledChanged = [this](bool enabled)
    {
        return ! enabled || ensureCameraReady(true);
    };
    addChildComponent(jamSession);

    mpcSamplePanel.onClose = [this]
    {
        mpcSamplePanel.setVisible(false);
        resized();
        updateTransportLabels();
    };
    mpcSamplePanel.onPadTriggered = [this](int pad, int velocity)
    {
        triggerMpcPad(pad, velocity);
    };
    mpcSamplePanel.onPadSampleAssigned = [this](int pad, const juce::String& path)
    {
        assignMpcKitSample(pad, path);
    };
    mpcSamplePanel.onCommand = [this](MpcSamplePanelComponent::Command command)
    {
        handleMpcCommand(command);
    };
    mpcSamplePanel.onCommandLearnRequested = [this](MpcSamplePanelComponent::Command command)
    {
        beginMpcCommandLearn(command);
    };
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

    // Draggable browser/timeline splitter (sits above both so the gesture is reachable).
    browserResizeBar.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    browserResizeBar.setRepaintsOnMouseActivity(true);
    browserResizeBar.onDown = [this](const juce::MouseEvent& e)
    {
        isResizingBrowserPanel = true;
        browserResizeStartWidth = browserPanelWidth;
        browserResizeStartX = e.getEventRelativeTo(this).getPosition().x;
    };
    browserResizeBar.onDrag = [this](const juce::MouseEvent& e)
    {
        if (! isResizingBrowserPanel)
            return;
        const auto x = e.getEventRelativeTo(this).getPosition().x;
        browserPanelWidth = juce::jlimit(minBrowserPanelWidth, maxBrowserPanelWidth,
                                         browserResizeStartWidth + (x - browserResizeStartX));
        resized();
        repaint();
    };
    browserResizeBar.onUp = [this](const juce::MouseEvent&)
    {
        isResizingBrowserPanel = false;
    };
    addAndMakeVisible(browserResizeBar);
    addAndMakeVisible(midiEditorOverlay);
    addChildComponent(clipEditorPanel);
    addAndMakeVisible(samplerPanel);
    addChildComponent(mpcSamplePanel);
    addChildComponent(mixerPanel);
    audioFormatManager.registerBasicFormats();
    arrangementPlaybackSource = std::make_unique<ArrangementPlaybackSource>(projectState, transportEngine, audioFormatManager);
    clickTrackSource = std::make_unique<ClickTrackSource>(projectState, transportEngine,
                                                          [this]() { return metronomeButton.getToggleState(); });
    audioInputRecorder = std::make_unique<AudioInputRecorder>();
    // Open output only at launch, restoring the saved device/buffer settings. Audio input and
    // mic permission are enabled lazily when the user actually records or monitors input.
    std::unique_ptr<juce::XmlElement> savedAudio;
    if (auto settings = makeUserSettingsFile())
        savedAudio = settings->getXmlValue("audioDeviceState");

    // Strip the input device from the restored state BEFORE initialise, for two reasons:
    //  1. Opening an input at launch pops the macOS microphone-permission prompt every start
    //     (Ableton doesn't, because it doesn't open the mic until you record).
    //  2. A restored input device that differs from the output builds JUCE's AudioIODeviceCombiner,
    //     which can deadlock CoreAudio's device start on some USB gear (the hang we chased).
    // Input is opened lazily and guarded when the user actually records/monitors.
    if (savedAudio != nullptr)
    {
        savedAudio->removeAttribute("audioInputDeviceName");
        savedAudio->removeAttribute("audioDeviceInChans");
    }
    audioDeviceManager.initialise(0, 2, savedAudio.get(), true);

    // Give the parallel render pool the device's OS workgroup before playback prepares its
    // workers, so they schedule in lockstep with the CoreAudio I/O thread (fewer per-block spikes).
    if (arrangementPlaybackSource != nullptr)
        arrangementPlaybackSource->setRenderWorkgroup(audioDeviceManager.getDeviceAudioWorkgroup());

    audioDeviceManager.addChangeListener(this);   // persist device/buffer changes as they happen
    restoreUserSettings();                        // recent projects list
    refreshMidiInputDevices();
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
        // Stage 2: rebuild the arrangement's piecewise warp cache off the message thread so the marker
        // change is heard in the arrangement too (not just the clip-editor preview). Coalesced.
        if (arrangementPlaybackSource != nullptr && ! warpRebuildRunning.exchange(true))
            juce::Thread::launch([this]
            {
                if (arrangementPlaybackSource != nullptr)
                    arrangementPlaybackSource->prepareWarpCacheForCurrentTempo();
                warpRebuildRunning = false;
            });
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
    wireBrowserAndDialogs();
    wireEditors();

    resetToPlaylistView();
    setClipInspectorVisible(false);
    updateTransportLabels();
    grabKeyboardFocus();
    startTimerHz(60);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &audioDeviceManager)
    {
        saveAudioDeviceState();
        // The device (and therefore its workgroup) may have changed — refresh the render pool's
        // copy so the next prepare rebuilds workers joined to the new one.
        if (arrangementPlaybackSource != nullptr)
            arrangementPlaybackSource->setRenderWorkgroup(audioDeviceManager.getDeviceAudioWorkgroup());
    }
}

void MainComponent::saveAudioDeviceState()
{
    auto settings = makeUserSettingsFile();
    if (settings == nullptr)
        return;
    if (auto xml = audioDeviceManager.createStateXml())
        settings->setValue("audioDeviceState", xml.get());
    else
        settings->removeValue("audioDeviceState");
    settings->saveIfNeeded();
}

void MainComponent::restoreUserSettings()
{
    auto settings = makeUserSettingsFile();
    if (settings == nullptr)
        return;
    recentProjects.restoreFromString(settings->getValue("recentProjects"));
    browserPanelWidth = juce::jlimit(minBrowserPanelWidth, maxBrowserPanelWidth,
                                     settings->getIntValue("browserPanelWidth", browserPanelWidth));
    exportSampleRate = settings->getIntValue("exportSampleRate", exportSampleRate);
    setOrionWarpEnabled(settings->getBoolValue("orionWarpEnabled", isOrionWarpEnabled()));
}

void MainComponent::saveUserSettings()
{
    if (auto settings = makeUserSettingsFile())
    {
        settings->setValue("browserPanelWidth", browserPanelWidth);
        settings->setValue("exportSampleRate", exportSampleRate);
        settings->setValue("orionWarpEnabled", isOrionWarpEnabled());
        settings->saveIfNeeded();
    }
}

void MainComponent::addRecentProject(const juce::File& file)
{
    if (! file.existsAsFile())
        return;
    recentProjects.addFile(file);
    recentProjects.removeNonExistentFiles();
    if (auto settings = makeUserSettingsFile())
    {
        settings->setValue("recentProjects", recentProjects.toString());
        settings->saveIfNeeded();
    }
}

void MainComponent::openRecentProject(int recentIndex)
{
    const auto file = recentProjects.getFile(recentIndex);
    if (file.existsAsFile())
        confirmAndLoadProject(file);
    else
    {
        recentProjects.removeNonExistentFiles();   // stale entry — drop it
        statusLabel.setText("Project no longer exists: " + file.getFileName(), juce::dontSendNotification);
    }
}

MainComponent::~MainComponent()
{
    if (juce::MenuBarModel::getMacMainMenu() == this)
        juce::MenuBarModel::setMacMainMenu(nullptr);

    audioDeviceManager.removeChangeListener(this);

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
    // Stop the independent input device BEFORE the recorder it feeds is destroyed, or its callback
    // could fire on freed memory. (The recorder is no longer on audioDeviceManager.)
    independentAudioInput.stop();
    audioRecorderCallbackAttached = false;
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
    // Match resized(): keep the small left inset, but let the application surface run flush
    // to the right edge. The panels themselves are square, so no outer corner mask is needed.
    auto workArea = bounds.withTrimmedLeft(8).withTrimmedRight(0).withTrimmedTop(workspaceTopGap);
    // The visible rail includes the small outer breathing room around the nav component.
    // Reserve that same full width here so its cards are centered in the visual rail, not
    // shifted toward the browser seam.
    // Keep the painted seam identical to the component layout below:
    // left inset 8 + rail width 120 + the 2 px workspace gap.
    workArea.removeFromLeft(SidebarNavComponent::preferredWidth + 6);
    juce::Rectangle<int> browserPanelBounds;
    if (browserPanelShown())
        browserPanelBounds = workArea.removeFromLeft(currentBrowserWidth());
    auto arrangementPanel = workArea;

    g.setColour(panelColour);
    // The main workspace is a continuous DAW surface. Rounded corners belong to controls and
    // dialogs, not to the outer edges of the browser/playlist canvas.
    auto paintPanel = [&](juce::Rectangle<int> panel, bool roundLeft, bool roundRight, bool border)
    {
        if (panel.isEmpty())
            return;
        const auto r = 0.0f;
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
    // Panels abut their neighbours flush with no borders.
    if (browserPanelShown())
        paintPanel(browserPanelBounds, false, false, false);   // browser: square, flush, no border
    paintPanel(arrangementPanel, false, false, false);         // playlist: square on every edge

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
    const int activeTransportHeight = jamSessionOpen ? TransportBarComponent::jamPreferredHeight : transportShelfHeight;
    transportBar.setBounds(getLocalBounds().removeFromTop(activeTransportHeight));
    transportBar.toFront(false);
    cachedKeyCardBounds = transportBar.getKeyBounds().translated(transportBar.getX(), transportBar.getY());
    // Reduced padding for a sleeker edge-to-edge floating layout
    // Keep the horizontal inset while letting the vertical rhythm be controlled explicitly by
    // workspaceTopGap; reducing the whole rectangle here used to cancel that gap later on.
    auto bounds = getLocalBounds().withTrimmedLeft(8).withTrimmedRight(8);
    auto topStrip = bounds.removeFromTop(activeTransportHeight).reduced(18, 10);
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

    auto scanTopArea = getLocalBounds().reduced(8).removeFromTop(activeTransportHeight).reduced(18, 0);
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
    // Sit the editor exactly on the value rectangle — same box the number is painted in, so
    // there is no jump when it appears (see getTempoEditorBounds / drawReadout).
    bpmEditor.setBounds(transportBar.getTempoEditorBounds().translated(transportBar.getX(), transportBar.getY()));
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
    auto workArea = bounds.withTrimmedTop(workspaceTopGap);
    workArea.setRight(getLocalBounds().getRight());
    // The rail fills flush from the window's left edge to the browser seam, so its visible
    // extent IS the component bounds and the centred icons read as centred. `railContentInset`
    // (below, in SidebarNavComponent) is the ONLY horizontal inset — do not add another here.
    auto sidebarBounds = workArea.removeFromLeft(SidebarNavComponent::preferredWidth + 4);
    sidebarNav.setBounds(0, sidebarBounds.getY(), sidebarBounds.getRight() + 2, sidebarBounds.getHeight());
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

    auto timelineLayoutArea = playlistArea;
    if (jamSessionOpen)
    {
        constexpr int jamVideoHeight = 184;  // component inset + 156 px video strip + breathing room
        constexpr int jamChatWidth = 354;    // component inset + 326 px chat dock + gutter
        timelineLayoutArea.removeFromTop(juce::jmin(jamVideoHeight, timelineLayoutArea.getHeight()));
        if (timelineLayoutArea.getWidth() > jamChatWidth + 360)
            timelineLayoutArea.removeFromRight(jamChatWidth);
    }

    const auto samplerOpen = samplerPanel.isVisible();
    const auto clipEditorOpen = clipEditorPanel.isVisible();
    const auto stepSequencerOpen = stepSequencer.isVisible();
    const auto mpcSampleOpen = mpcSamplePanel.isVisible();
    // The MPC panel is a large full-height device view (not a compact bottom strip), so it is
    // NOT part of the shared lower-panel; it covers the whole arrangement area when open.
    const auto bottomPanelOpen = samplerOpen || clipEditorOpen || stepSequencerOpen;
    const auto closedArrangementArea = timelineLayoutArea;
    auto openArrangementArea = timelineLayoutArea;
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
        // The draggable splitter sits ON TOP of both panels, right on the actual browser/timeline seam.
        const int seamX = browserPanelBounds.getRight();
        browserResizeBar.setBounds(seamX - browserResizeHandleWidth / 2, browserPanelBounds.getY(),
                                   browserResizeHandleWidth, browserPanelBounds.getHeight());
        browserResizeBar.setVisible(browserPanelVisible);
        browserResizeBar.toFront(false);
    }
    else
    {
        browserPanel.setVisible(false);
        browserResizeBar.setVisible(false);
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
    if (mpcSampleOpen)
    {
        auto desired = mpcSamplePanel.getBounds();
        if (desired.isEmpty())
            desired = playlistArea.withSizeKeepingCentre(juce::jmin(980, playlistArea.getWidth() - 32),
                                                          juce::jmin(700, playlistArea.getHeight() - 32));

        // Preserve the user's dragged position while keeping the floating surface inside
        // the playlist workspace after a resize or browser-width change.
        const auto maxX = juce::jmax(playlistArea.getX(), playlistArea.getRight() - desired.getWidth());
        const auto maxY = juce::jmax(playlistArea.getY(), playlistArea.getBottom() - desired.getHeight());
        desired.setX(juce::jlimit(playlistArea.getX(), maxX, desired.getX()));
        desired.setY(juce::jlimit(playlistArea.getY(), maxY, desired.getY()));
        mpcSamplePanel.setBounds(desired);
    }
    else
        mpcSamplePanel.setBounds({});
    mpcSamplePanel.setVisible(mpcSampleOpen);
    if (mpcSampleOpen)
        mpcSamplePanel.toFront(false);

    if (jamSessionOpen)
    {
        jamSession.setBounds(playlistArea);
        jamSession.setVisible(true);
        jamSession.toFront(false);
    }
    else
    {
        jamSession.setBounds({});
        jamSession.setVisible(false);
    }

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
        // Independent toggles (same as the transport-bar record options).
        menu.addItem(1, "Metronome (whole take)", true, withMetro);
        menu.addItem(2, "4-count before recording", true, withPrecount);
        menu.showMenuAsync(juce::PopupMenu::Options{}
                                .withTargetComponent(&recordButton)
                                .withTargetScreenArea(recordButton.localAreaToGlobal(recordButton.getLocalBounds())),
            [this, withMetro, withPrecount](int result)
            {
                if (result == 1)
                {
                    projectState.setRecordWithMetronome(! withMetro);
                    metronomeButton.setToggleState(! withMetro, juce::dontSendNotification);
                }
                else if (result == 2)
                {
                    projectState.setRecordWithCountIn(! withPrecount);
                    countInButton.setToggleState(! withPrecount, juce::dontSendNotification);
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
        return arrangementTimeline.duplicateSelectedChords()   // chords first, then clips
            || arrangementTimeline.duplicateSelectedClip();

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

    // Browser preview has priority for the first Space press, matching Ableton: stop the
    // audition first; the next Space belongs to the project transport.
    if (previewTransportSource.isPlaying() || pendingBrowserPreviewStart)
    {
        pendingBrowserPreviewStart = false;
        browserPanel.setPreviewArmed(false);
        stopBrowserPreview(true);
        browserPanel.setPreviewPlayback(false, 0.0f);
        return true;
    }

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

    // While the transport rolls, the timeline repaints itself (playhead), so the header meters
    // animate for free. Stopped, it doesn't repaint at all — so a live input signal sat frozen
    // until some other event (a mouse move) forced a redraw. Nudge just the header column when
    // input monitoring is active so mic/line levels are visible before you hit record.
    if (audioRecorderCallbackAttached && ! transportEngine.isPlaying())
        arrangementTimeline.repaintTrackMeters();
}

void MainComponent::timerCallback()
{
    // Multiplayer Jam: diff the project against the last-synced shadow and broadcast whatever the
    // user changed. This is why no edit path in the app needs to know collab exists.
    // Costs nothing when solo (isActive() is false), and even in a session it runs at ~10 Hz rather
    // than the timer's 60 — each sync copies the project to diff it, and a tenth of a second is
    // still imperceptible next to network latency.
    // Live cursors need a faster cadence than edits to feel smooth, but far slower than the 60 Hz
    // timer — presence is ephemeral and must never crowd out real ops.
    if (collabController.isActive() && ++collabPresenceCounter >= 3)
    {
        collabPresenceCounter = 0;
        publishJamPresence();
    }

    // Shared transport: broadcast our play/stop the instant it flips.
    syncJamTransportOut();

    if (collabController.isActive() && ++collabSyncCounter >= 6)
    {
        collabSyncCounter = 0;
        collabReconciler.sync();

        // Refresh the Jam panel's health line about once a second (every 10th sync).
        if (++collabDiagnosticsCounter >= 10)
        {
            collabDiagnosticsCounter = 0;
            updateJamDiagnostics();
        }
    }

    static constexpr double mpcPadRearmDelayMs = 140.0;
    if (! mpcHardwareNoteReleaseTimes.empty())
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        for (auto it = mpcHardwareNoteReleaseTimes.begin(); it != mpcHardwareNoteReleaseTimes.end();)
        {
            if (now - it->second < mpcPadRearmDelayMs)
            {
                ++it;
                continue;
            }

            const auto key = it->first;
            const auto padIt = mpcHardwareNotePads.find(key);
            if (padIt != mpcHardwareNotePads.end())
                playMpcPad(padIt->second, 0);

            mpcHeldHardwareNoteKeys.erase(key);
            mpcHardwareNotePads.erase(key);
            it = mpcHardwareNoteReleaseTimes.erase(it);
        }
    }

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
    // State changes call refreshClipEditor() directly from their handlers. On the timer, only
    // refresh while a playhead can move; rebuilding the editor state while stopped did work
    // with no visible result.
    if (clipEditorPanel.isVisible()
        && (transportEngine.isPlaying() || clipEditorPreviewTransportSource.isPlaying()))
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
    else if (! playing && ! countingIn && recordingSession.has_value())
    {
        // Playback stopped externally (loop end / user stop) — finalise. NOT during count-in:
        // an early note (played before the downbeat) opens a session while still counting in,
        // and firing this here would finalise that one note and disarm recording, dropping the
        // whole take. During count-in the session must survive until real playback begins.
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
        // Independent: the click can run through the whole take AND you can still have a count-in.
        if (button == &metronomeButton)
            projectState.setRecordWithMetronome(metronomeButton.getToggleState());
        else
            projectState.setRecordWithCountIn(countInButton.getToggleState());
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

void MainComponent::refreshMidiInputDevices()
{
    const auto devices = juce::MidiInput::getAvailableDevices();
    const auto outputDevices = juce::MidiOutput::getAvailableDevices();
    visibleMidiInputCount = devices.size();
    const auto mpcState = mpcHardwareBridge.refreshDevices(devices, outputDevices);
    if (mpcState.inputConnected != mpcInputConnected || mpcState.inputName != mpcInputName)
    {
        mpcInputConnected = mpcState.inputConnected;
        mpcInputName = mpcState.inputName;
        statusLabel.setText(mpcInputConnected ? "MPC MIDI IN: " + mpcInputName : "MPC MIDI IN: listening",
                            juce::dontSendNotification);
    }
    mpcSamplePanel.setConnectionState(mpcState.inputConnected, mpcState.inputName);
    mpcSamplePanel.setHardwareStatus(mpcState.inputName, mpcState.outputName, mpcHardwareBridge.getLastMidiDescription());

    // Open each visible input DIRECTLY and be its ONLY client. Two things bit us here:
    //  1. On this Mac the AudioDeviceManager callback path never delivers messages.
    //  2. Registering an AudioDeviceManager callback makes it also open the CoreMIDI
    //     endpoint; several class-compliant USB devices (Keystation, MPC Sample) are
    //     single-client, so that second opener starves our openDevice and NOBODY gets
    //     messages. So: no setMidiInputDeviceEnabled, no addMidiInputDeviceCallback —
    //     just juce::MidiInput::openDevice + start(), which is the reliable path.
    for (const auto& d : devices)
    {
        // Match the last-known-good path exactly: enable in AudioDeviceManager (first-seen),
        // then be the direct openDevice client. No addMidiInputDeviceCallback (that opened a
        // second, conflicting client on single-client USB gear).
        if (! seenMidiInputDeviceIds.contains(d.identifier))
        {
            seenMidiInputDeviceIds.add(d.identifier);
            audioDeviceManager.setMidiInputDeviceEnabled(d.identifier, true);
        }

        if (directMidiInputs.count(d.identifier) > 0)
            continue;

        if (auto input = juce::MidiInput::openDevice(d.identifier, this))
        {
            input->start();
            directMidiInputs[d.identifier] = std::move(input);
        }
    }

    // Close devices that were unplugged.
    for (auto it = directMidiInputs.begin(); it != directMidiInputs.end();)
    {
        const bool stillPresent = std::any_of(devices.begin(), devices.end(),
                                              [&](const auto& d) { return d.identifier == it->first; });
        it = stillPresent ? std::next(it) : directMidiInputs.erase(it);
    }
}

void MainComponent::updateTransportLabels()
{
    const auto setLabelTextIfChanged = [](juce::Label& label, const juce::String& text)
    {
        if (label.getText() != text)
            label.setText(text, juce::dontSendNotification);
    };

    // Safety net for closing the BPM editor. onFocusLost handles clicks that land on a
    // focusable component, but clicking empty timeline space doesn't always move keyboard focus,
    // so the editor could stay open (and, now that the bar hides the value while editing, the
    // tempo would read blank). If it's open but no longer has focus, commit and close it.
    if (bpmEditor.isVisible() && ! bpmEditor.hasKeyboardFocus(true))
        endTempoEditing(true);

    if (! bpmEditor.isVisible())
    {
        setLabelTextIfChanged(bpmValueLabel, juce::String(projectState.getTempoBpm(), 2));
        bpmValueLabel.setVisible(false);
        // NB: do NOT repaint() the whole window here — this runs every 60 Hz tick and was redrawing the
        // ENTIRE UI (arrangement + all its text) continuously → ~40% CPU on the software renderer for
        // nothing. Labels repaint themselves on setText; the transport bar updates via setState below.
    }

    if (transportEngine.isCountInActive())
        setLabelTextIfChanged(meterValueLabel, "COUNT");
    else
        setLabelTextIfChanged(meterValueLabel, juce::String(projectState.getNumerator()) + "/" + juce::String(projectState.getDenominator()));

    if (transportEngine.isCountInActive())
        setLabelTextIfChanged(meterCaptionLabel, "COUNT-IN");
    else if (transportEngine.isPlaying())
        setLabelTextIfChanged(meterCaptionLabel, "RUNNING");
    else if (transportEngine.isPaused())
        setLabelTextIfChanged(meterCaptionLabel, "PAUSED");
    else
        setLabelTextIfChanged(meterCaptionLabel, "STOPPED");

    const auto clipPreviewPlaying = clipEditorPreviewTransportSource.isPlaying();
    playButton.setToggleState(transportEngine.isPlaying() || transportEngine.isCountInActive() || clipPreviewPlaying,
                              juce::dontSendNotification);
    recordButton.setToggleState(transportEngine.isRecordArmed(), juce::dontSendNotification);
    loopButton.setToggleState(transportEngine.isLoopEnabled(), juce::dontSendNotification);
    metronomeButton.setToggleState(projectState.isRecordWithMetronome(), juce::dontSendNotification);
    countInButton.setToggleState(projectState.isRecordWithCountIn(), juce::dontSendNotification);
    undoButton.setEnabled(arrangementTimeline.canUndo());
    redoButton.setEnabled(arrangementTimeline.canRedo());
    setLabelTextIfChanged(tempoLabel, "Tempo: " + juce::String(projectState.getTempoBpm(), 0) + " BPM");
    setLabelTextIfChanged(meterLabel, "Meter: " + juce::String(projectState.getNumerator()) + "/" + juce::String(projectState.getDenominator()));
    setLabelTextIfChanged(playheadLabel, "Playhead: beat " + juce::String(transportEngine.getPlayheadBeat(), 2));

    TransportBarState transportState;
    transportState.tempoBpm = projectState.getTempoBpm();
    transportState.tempoEditing = bpmEditor.isVisible();
    transportState.projectName = currentProjectFile != juce::File()
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
    transportState.mpcSampleOpen = mpcSamplePanel.isVisible();
    transportState.jamOpen = jamSessionOpen;
    transportState.midiSignalActive = ! liveMidiDisplayNotes.empty()
        || (juce::Time::getMillisecondCounterHiRes() - lastLiveMidiActivityMs) < 320.0;
    transportState.midiSignalText = transportState.midiSignalActive && lastLiveMidiSignalText.isNotEmpty()
        ? lastLiveMidiSignalText
        : "No MIDI";
    transportBar.setState(transportState);

    // menuItemsChanged() triggers an async menu-bar rebuild; calling it every 60 Hz tick churned the
    // menu model needlessly. Only rebuild when a menu-relevant state actually changed.
    const unsigned menuHash =
          (arrangementTimeline.canUndo()            ? 1u   : 0u)
        | (arrangementTimeline.canRedo()            ? 2u   : 0u)
        | (transportEngine.isPlaying()              ? 4u   : 0u)
        | (transportEngine.isCountInActive()        ? 8u   : 0u)
        | (transportEngine.isRecordArmed()          ? 16u  : 0u)
        | (transportEngine.isLoopEnabled()          ? 32u  : 0u)
        | (projectState.isRecordWithMetronome()     ? 64u  : 0u)
        | (projectState.isRecordWithCountIn()       ? 128u : 0u)
        | (mixerPanel.isVisible()                   ? 256u : 0u)
        | (clipEditorPanel.isVisible()              ? 512u : 0u)
        | (stepSequencer.isVisible()                ? 1024u: 0u)
        | (jamSessionOpen                           ? 2048u: 0u);
    if (menuHash != lastMenuStateHash)
    {
        lastMenuStateHash = menuHash;
        menuItemsChanged();
    }
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

    if (masterStripSource != nullptr)
        masterStripSource->resetStopFade();

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

bool MainComponent::ensureCameraReady(bool requestPermission)
{
    if (! juce::RuntimePermissions::isRequired(juce::RuntimePermissions::camera))
        return true;

    if (juce::RuntimePermissions::isGranted(juce::RuntimePermissions::camera))
        return true;

    if (requestPermission && ! cameraPermissionRequestInFlight)
    {
        cameraPermissionRequestInFlight = true;
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::RuntimePermissions::request(juce::RuntimePermissions::camera,
                                          [safeThis](bool granted)
                                          {
                                              if (safeThis == nullptr)
                                                  return;

                                              safeThis->cameraPermissionRequestInFlight = false;
                                              safeThis->statusLabel.setText(granted
                                                  ? "Camera permission granted. Press Camera again to start video."
                                                  : "Camera permission denied. Enable it in macOS Privacy settings.",
                                                  juce::dontSendNotification);
                                          });
    }

    return false;
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

void MainComponent::toggleMpcSampleFromUi()
{
    const auto shouldOpen = ! mpcSamplePanel.isVisible();
    if (shouldOpen)
    {
        // The MPC surface shares the lower-panel slot with the editor and sampler.
        samplerPanel.setVisible(false);
        clipEditorPanel.setVisible(false);
        stepSequencer.setVisible(false);
        stopClipEditorPreview(true);
    }

    mpcSamplePanel.setVisible(shouldOpen);
    resized();
    updateTransportLabels();
}

void MainComponent::toggleJamSessionFromUi()
{
    jamSessionOpen = ! jamSessionOpen;
    jamSession.setVisible(jamSessionOpen);
    updateTransportLabels();
    resized();
    repaint();
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
        if (path == "/Volumes" || path.startsWith("/Volumes/"))
            continue;

        const auto folder = juce::File(path);
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
    browserPanel.setLibraryRoots(sidebarBrowserFolders);   // similarity search spans all added folders
}

void MainComponent::saveSidebarBrowserFolders() const
{
    auto settings = makeUserSettingsFile();
    if (settings == nullptr)
        return;

    juce::StringArray paths;
    for (const auto& folder : sidebarBrowserFolders)
    {
        const auto path = folder.getFullPathName();
        if (path == "/Volumes" || path.startsWith("/Volumes/"))
            continue;

        paths.addIfNotAlreadyThere(path);
    }

    settings->setValue(sidebarFoldersSettingsKey, paths.joinIntoString("\n"));
    settings->saveIfNeeded();
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
            saveUserSettings();
        },
        [this](int newRate)
        {
            exportSampleRate = newRate;
            saveUserSettings();
            statusLabel.setText("Export sample rate: " + juce::String(exportSampleRate / 1000.0, 1) + " kHz",
                                juce::dontSendNotification);
        },
        [this](bool orionOn)
        {
            setOrionWarpEnabled(orionOn);
            saveUserSettings();
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
    options.dialogBackgroundColour = th::core::studio;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.componentToCentreAround = this;
    options.content->setSize(640, 720);
    if (auto* dialog = options.launchAsync())
        dialog->setAlwaysOnTop(false);
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
                editorState.waveform = it->second;
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

    auto waveform = std::make_shared<ClipEditorWaveform>();
    waveform->minValues.assign(static_cast<std::size_t>(bucketCount), 0.0f);
    waveform->maxValues.assign(static_cast<std::size_t>(bucketCount), 0.0f);

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

            auto& minValue = waveform->minValues[static_cast<std::size_t>(bucket)];
            auto& maxValue = waveform->maxValues[static_cast<std::size_t>(bucket)];
            minValue = juce::jmin(minValue, value);
            maxValue = juce::jmax(maxValue, value);
        }
        processed += toRead;
    }

    clipEditorWaveformCache.emplace(key, std::move(waveform));
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

    // Hide FIRST, then apply. applyTempoFromTransportText() calls updateTransportLabels(), which
    // itself closes the editor if it has lost focus — so if we applied while still visible, that
    // path would re-enter endTempoEditing and recurse until the stack overflowed. Hiding up front
    // makes the isVisible() guard above short-circuit any re-entry.
    bpmEditor.setVisible(false);

    if (applyChanges)
        applyTempoFromTransportText();

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
