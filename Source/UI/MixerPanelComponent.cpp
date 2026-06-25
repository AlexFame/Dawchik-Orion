#include "MixerPanelComponent.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>

#include "OrionTheme.h"

namespace
{
namespace th = orion::theme;
const auto panelBackground = th::core::deepSpace;
const auto panelStroke     = th::line::subtle;
const auto stripBackground = th::surface::primary;
const auto stripStroke     = th::line::subtle;
const auto slotBackground  = th::core::deepSpace;
const auto accentColour    = th::warm::red;
const auto cyan            = th::cool::turquoise;
const auto mutedText       = th::text::muted;

constexpr double minGainDb = -60.0;
constexpr double maxGainDb = 6.0;

constexpr int stripWidth       = 122;
constexpr int stripGap         = 6;
constexpr int masterStripWidth = 122;
constexpr int panelPadding     = 16;
constexpr int titleHeight      = 28;
constexpr int bottomBarHeight  = 40;

juce::String gainTextFromValue(double value)
{
    if (value <= minGainDb + 0.01)
        return "-inf";
    return juce::String(value, 1);
}

// Studio-One-style mixer controls: a red "pill" fader handle on a thin red track,
// and a thin cyan pan ring with a short red pointer.
// Half the fader handle height — also the track inset top/bottom so the handle stays inside
// the fader and the dB scale (which uses the same inset) lines up with the handle.
constexpr float kFaderThumbRadius = 11.0f;

struct MixerLookAndFeel : juce::LookAndFeel_V4
{
    int getSliderThumbRadius(juce::Slider& slider) override
    {
        // Fixed, predictable inset for vertical faders (JUCE's default derives it from the
        // slider WIDTH, which made the handle sit well below its value on wide strips).
        return slider.isVertical() ? static_cast<int>(kFaderThumbRadius) : juce::LookAndFeel_V4::getSliderThumbRadius(slider);
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float, float,
                          juce::Slider::SliderStyle, juce::Slider&) override
    {
        const auto cx = static_cast<float>(x) + width * 0.5f;
        const auto top = static_cast<float>(y);
        const auto bottom = static_cast<float>(y + height);

        // Track: dark groove full height, red fill from bottom up to the handle.
        const auto trackW = 4.0f;
        juce::Rectangle<float> groove(cx - trackW * 0.5f, top, trackW, bottom - top);
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(groove, trackW * 0.5f);
        g.setColour(accentColour.withAlpha(0.9f));
        g.fillRoundedRectangle(groove.withTop(sliderPos), trackW * 0.5f);

        // Handle: red rounded pill with a white centre line.
        const auto handleW = juce::jmin(static_cast<float>(width) - 2.0f, 34.0f);
        const auto handleH = 22.0f;
        juce::Rectangle<float> handle(cx - handleW * 0.5f, sliderPos - handleH * 0.5f, handleW, handleH);
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillRoundedRectangle(handle.translated(0.0f, 1.0f), 5.0f);
        g.setColour(accentColour);
        g.fillRoundedRectangle(handle, 5.0f);
        g.setColour(accentColour.darker(0.4f));
        g.drawRoundedRectangle(handle, 5.0f, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.fillRect(juce::Rectangle<float>(handle.getX() + 4.0f, handle.getCentreY() - 0.9f,
                                          handle.getWidth() - 8.0f, 1.8f));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider&) override
    {
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(width), static_cast<float>(height)).reduced(3.0f);
        const auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

        // Ring.
        juce::Path ring;
        ring.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(cyan.withAlpha(0.8f));
        g.strokePath(ring, juce::PathStrokeType(1.6f));

        // Pointer (short red line from centre).
        const juce::Point<float> tip(centre.x + radius * 0.86f * std::sin(angle),
                                     centre.y - radius * 0.86f * std::cos(angle));
        g.setColour(accentColour);
        g.drawLine({ centre, tip }, 2.6f);
        g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(centre));
    }
};
}  // namespace

namespace orion
{
MixerPanelComponent::MixerPanelComponent(ProjectState& projectState)
    : project(projectState)
{
    setVisible(false);
    mixerLnf = std::make_unique<MixerLookAndFeel>();

    const auto styleFader = [this](juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::LinearVertical);
        s.setRange(minGainDb, maxGainDb, 0.1);
        s.setValue(0.0, juce::dontSendNotification);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setColour(juce::Slider::thumbColourId, accentColour);
        s.setColour(juce::Slider::trackColourId, accentColour.withAlpha(0.85f));
        s.setColour(juce::Slider::backgroundColourId, juce::Colours::black.withAlpha(0.4f));
        s.setLookAndFeel(mixerLnf.get());
    };

    styleFader(masterVolume);
    masterVolume.onValueChange = [this]
    {
        if (onSetMasterGainDb)
            onSetMasterGainDb(masterVolume.getValue());
    };
    addChildComponent(masterVolume);

    masterPan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    masterPan.setRange(-1.0, 1.0, 0.01);
    masterPan.setValue(0.0, juce::dontSendNotification);
    masterPan.setDoubleClickReturnValue(true, 0.0);
    masterPan.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    masterPan.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                  juce::MathConstants<float>::pi * 2.75f, true);
    masterPan.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    masterPan.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::transparentBlack);
    masterPan.setColour(juce::Slider::rotarySliderOutlineColourId, cyan.withAlpha(0.75f));
    masterPan.setLookAndFeel(mixerLnf.get());
    addChildComponent(masterPan);
}

void MixerPanelComponent::open()
{
    maximized = false;
    rebuildStrips();
    if (onRequestMasterGainDb)
        masterVolume.setValue(onRequestMasterGainDb(), juce::dontSendNotification);

    setVisible(true);
    toFront(true);
    masterVolume.setVisible(true);
    masterPan.setVisible(true);
    setWantsKeyboardFocus(true);
    grabKeyboardFocus();
    startTimerHz(30);
    resized();
    repaint();
}

void MixerPanelComponent::closePanel()
{
    stopTimer();
    setVisible(false);
    if (onClose)
        onClose();
}

int MixerPanelComponent::currentStripWidth() const
{
    return stripWidth;
}

void MixerPanelComponent::rebuildStrips()
{
    for (auto& strip : strips)
    {
        if (strip == nullptr)
            continue;
        if (strip->volume != nullptr) removeChildComponent(strip->volume.get());
        if (strip->pan != nullptr)    removeChildComponent(strip->pan.get());
        if (strip->mute != nullptr)   removeChildComponent(strip->mute.get());
        if (strip->solo != nullptr)   removeChildComponent(strip->solo.get());
        if (strip->record != nullptr) removeChildComponent(strip->record.get());
    }
    strips.clear();

    const auto& tracks = project.getTracks();
    builtTrackCount = static_cast<int>(tracks.size());

    for (int i = 0; i < builtTrackCount; ++i)
    {
        auto strip = std::make_unique<ChannelStrip>();
        strip->trackIndex = i;
        const auto& track = tracks[static_cast<std::size_t>(i)];
        const int trackIndex = i;

        strip->volume = std::make_unique<juce::Slider>();
        strip->volume->setSliderStyle(juce::Slider::LinearVertical);
        strip->volume->setRange(minGainDb, maxGainDb, 0.1);
        strip->volume->setValue(track.volumeDb, juce::dontSendNotification);
        strip->volume->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        strip->volume->setColour(juce::Slider::thumbColourId, accentColour);
        strip->volume->setColour(juce::Slider::trackColourId, accentColour.withAlpha(0.85f));
        strip->volume->setColour(juce::Slider::backgroundColourId, juce::Colours::black.withAlpha(0.4f));
        strip->volume->setLookAndFeel(mixerLnf.get());
        strip->volume->onValueChange = [this, trackIndex]
        {
            auto& t = project.getTracks();
            if (trackIndex < 0 || trackIndex >= static_cast<int>(t.size())) return;
            if (auto* s = strips[static_cast<std::size_t>(trackIndex)].get(); s != nullptr && s->volume != nullptr)
                t[static_cast<std::size_t>(trackIndex)].volumeDb = s->volume->getValue();
            if (onTrackChanged) onTrackChanged();
            repaint();
        };
        addChildComponent(strip->volume.get());
        strip->volume->setVisible(true);

        strip->pan = std::make_unique<juce::Slider>();
        strip->pan->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        strip->pan->setRange(-1.0, 1.0, 0.01);
        strip->pan->setValue(track.pan, juce::dontSendNotification);
        strip->pan->setDoubleClickReturnValue(true, 0.0);
        strip->pan->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        // Centre-up pointer at pan=0; no arc fill (the fill made it look "hard left").
        strip->pan->setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                        juce::MathConstants<float>::pi * 2.75f, true);
        strip->pan->setColour(juce::Slider::thumbColourId, juce::Colours::white);
        strip->pan->setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::transparentBlack);
        strip->pan->setColour(juce::Slider::rotarySliderOutlineColourId, cyan.withAlpha(0.75f));
        strip->pan->setLookAndFeel(mixerLnf.get());
        strip->pan->onValueChange = [this, trackIndex]
        {
            auto& t = project.getTracks();
            if (trackIndex < 0 || trackIndex >= static_cast<int>(t.size())) return;
            if (auto* s = strips[static_cast<std::size_t>(trackIndex)].get(); s != nullptr && s->pan != nullptr)
                t[static_cast<std::size_t>(trackIndex)].pan = s->pan->getValue();
            if (onTrackChanged) onTrackChanged();
            repaint();
        };
        addChildComponent(strip->pan.get());
        strip->pan->setVisible(true);

        const auto makeToggle = [&](const juce::String& text, juce::Colour onColour, juce::Colour onText, bool state, auto applyFn)
        {
            auto b = std::make_unique<juce::TextButton>(text);
            b->setClickingTogglesState(true);
            b->setToggleState(state, juce::dontSendNotification);
            b->setColour(juce::TextButton::buttonOnColourId, onColour);
            b->setColour(juce::TextButton::textColourOnId, onText);
            b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff262a30));
            b->onClick = [this, trackIndex, applyFn]
            {
                auto& t = project.getTracks();
                if (trackIndex < 0 || trackIndex >= static_cast<int>(t.size())) return;
                applyFn(t[static_cast<std::size_t>(trackIndex)]);
                if (onTrackChanged) onTrackChanged();
                repaint();
            };
            addChildComponent(b.get());
            b->setVisible(true);
            return b;
        };

        strip->mute = makeToggle("M", juce::Colour(0xfff0c419), juce::Colours::black, track.muted,
            [strip = strip.get()](TrackState& t) { t.muted = strip->mute->getToggleState(); });
        strip->solo = makeToggle("S", accentColour, juce::Colours::white, track.solo,
            [strip = strip.get()](TrackState& t) { t.solo = strip->solo->getToggleState(); });
        strip->record = makeToggle("R", juce::Colour(0xffe43b3b), juce::Colours::white, track.recordArmed,
            [strip = strip.get()](TrackState& t) { t.recordArmed = strip->record->getToggleState(); });

        strips.push_back(std::move(strip));
    }

    // ---- Aux bus strips (after the tracks) ----
    const auto& buses = project.getBuses();
    builtBusCount = static_cast<int>(buses.size());
    for (int b = 0; b < builtBusCount; ++b)
    {
        auto strip = std::make_unique<ChannelStrip>();
        strip->isBus = true;
        strip->busIndex = b;
        const auto& bus = buses[static_cast<std::size_t>(b)];
        const int busIndex = b;

        strip->volume = std::make_unique<juce::Slider>();
        strip->volume->setSliderStyle(juce::Slider::LinearVertical);
        strip->volume->setRange(minGainDb, maxGainDb, 0.1);
        strip->volume->setValue(bus.volumeDb, juce::dontSendNotification);
        strip->volume->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        strip->volume->setColour(juce::Slider::thumbColourId, accentColour);
        strip->volume->setLookAndFeel(mixerLnf.get());
        {
            auto* vp = strip->volume.get();
            strip->volume->onValueChange = [this, busIndex, vp]
            {
                auto& bs = project.getBuses();
                if (busIndex >= 0 && busIndex < static_cast<int>(bs.size()))
                    bs[static_cast<std::size_t>(busIndex)].volumeDb = vp->getValue();
                if (onTrackChanged) onTrackChanged();
                repaint();
            };
        }
        addChildComponent(strip->volume.get());
        strip->volume->setVisible(true);

        strip->pan = std::make_unique<juce::Slider>();
        strip->pan->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        strip->pan->setRange(-1.0, 1.0, 0.01);
        strip->pan->setValue(bus.pan, juce::dontSendNotification);
        strip->pan->setDoubleClickReturnValue(true, 0.0);
        strip->pan->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        strip->pan->setRotaryParameters(juce::MathConstants<float>::pi * 1.25f,
                                        juce::MathConstants<float>::pi * 2.75f, true);
        strip->pan->setColour(juce::Slider::thumbColourId, juce::Colours::white);
        strip->pan->setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::transparentBlack);
        strip->pan->setColour(juce::Slider::rotarySliderOutlineColourId, cyan.withAlpha(0.75f));
        strip->pan->setLookAndFeel(mixerLnf.get());
        {
            auto* pp = strip->pan.get();
            strip->pan->onValueChange = [this, busIndex, pp]
            {
                auto& bs = project.getBuses();
                if (busIndex >= 0 && busIndex < static_cast<int>(bs.size()))
                    bs[static_cast<std::size_t>(busIndex)].pan = pp->getValue();
                if (onTrackChanged) onTrackChanged();
                repaint();
            };
        }
        addChildComponent(strip->pan.get());
        strip->pan->setVisible(true);

        strip->mute = std::make_unique<juce::TextButton>("M");
        strip->mute->setClickingTogglesState(true);
        strip->mute->setToggleState(bus.muted, juce::dontSendNotification);
        strip->mute->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xfff0c419));
        strip->mute->setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        strip->mute->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff262a30));
        {
            auto* mp = strip->mute.get();
            strip->mute->onClick = [this, busIndex, mp]
            {
                auto& bs = project.getBuses();
                if (busIndex >= 0 && busIndex < static_cast<int>(bs.size()))
                    bs[static_cast<std::size_t>(busIndex)].muted = mp->getToggleState();
                if (onTrackChanged) onTrackChanged();
                repaint();
            };
        }
        addChildComponent(strip->mute.get());
        strip->mute->setVisible(true);

        strips.push_back(std::move(strip));
    }
}

void MixerPanelComponent::refreshStrips()
{
    rebuildStrips();
    resized();
    repaint();
}

void MixerPanelComponent::syncControlsFromProject()
{
    const auto& tracks = project.getTracks();
    const auto& buses = project.getBuses();
    for (auto& strip : strips)
    {
        if (strip == nullptr) continue;

        if (strip->isBus)
        {
            if (strip->busIndex < 0 || strip->busIndex >= static_cast<int>(buses.size())) continue;
            const auto& bus = buses[static_cast<std::size_t>(strip->busIndex)];
            if (strip->volume != nullptr && ! strip->volume->isMouseButtonDown())
                strip->volume->setValue(bus.volumeDb, juce::dontSendNotification);
            if (strip->pan != nullptr && ! strip->pan->isMouseButtonDown())
                strip->pan->setValue(bus.pan, juce::dontSendNotification);
            if (strip->mute != nullptr) strip->mute->setToggleState(bus.muted, juce::dontSendNotification);
            continue;
        }

        if (strip->trackIndex < 0 || strip->trackIndex >= static_cast<int>(tracks.size()))
            continue;
        const auto& track = tracks[static_cast<std::size_t>(strip->trackIndex)];
        if (strip->volume != nullptr && ! strip->volume->isMouseButtonDown())
            strip->volume->setValue(track.volumeDb, juce::dontSendNotification);
        if (strip->pan != nullptr && ! strip->pan->isMouseButtonDown())
            strip->pan->setValue(track.pan, juce::dontSendNotification);
        if (strip->mute != nullptr)   strip->mute->setToggleState(track.muted, juce::dontSendNotification);
        if (strip->solo != nullptr)   strip->solo->setToggleState(track.solo, juce::dontSendNotification);
        if (strip->record != nullptr) strip->record->setToggleState(track.recordArmed, juce::dontSendNotification);
    }
}

MixerPanelComponent::StripLayout MixerPanelComponent::computeStripLayout(juce::Rectangle<int> card) const
{
    StripLayout L;
    L.card = card;
    auto c = card.reduced(8, 8);

    L.nameRow = c.removeFromTop(16);
    L.levelReadout = c.removeFromTop(12);
    c.removeFromTop(6);

    auto btnRow = c.removeFromTop(20);
    const auto bw = (btnRow.getWidth() - 8) / 3;
    L.mute = btnRow.removeFromLeft(bw); btnRow.removeFromLeft(4);
    L.solo = btnRow.removeFromLeft(bw); btnRow.removeFromLeft(4);
    L.record = btnRow;
    c.removeFromTop(8);

    // Logic-style: a FIXED, fairly short fader block is pinned to the bottom (with the
    // routing/pan/dB above it), and the big remaining middle goes to the Insert & Send
    // lists. COMPACT view shrinks the lists and gives the fader a touch more room.
    const auto compact = (view == MixerView::compact);
    const auto faderBlockH = compact ? 200 : 150;

    L.outDropdown = c.removeFromBottom(22);
    c.removeFromBottom(6);
    L.panCaption = c.removeFromBottom(12);
    L.panKnob = c.removeFromBottom(34);
    c.removeFromBottom(6);
    L.dbBox = c.removeFromBottom(20);
    c.removeFromBottom(6);

    auto faderBlock = c.removeFromBottom(juce::jmin(faderBlockH, c.getHeight() - 40));
    L.peakLabel = faderBlock.removeFromTop(12);
    faderBlock.removeFromTop(2);
    L.scaleArea = faderBlock.removeFromLeft(24);
    L.meter = faderBlock.removeFromRight(12);
    faderBlock.removeFromRight(6);
    L.fader = faderBlock;
    c.removeFromBottom(8);

    // Remaining middle = Insert list (larger) + Send list. Each is a label row + a tall
    // slot area that holds the chain (rows drawn in drawStrip).
    if (! compact)
    {
        auto sends = c.removeFromBottom(juce::jmax(40, c.getHeight() * 2 / 5));
        c.removeFromBottom(8);
        auto inserts = c;

        auto insLabelRow = inserts.removeFromTop(13);
        L.insertsPower = insLabelRow.removeFromRight(13);
        L.insertsLabel = insLabelRow;
        L.insertsSlot = inserts;

        auto sndLabelRow = sends.removeFromTop(13);
        L.sendsPower = sndLabelRow.removeFromRight(13);
        L.sendsLabel = sndLabelRow;
        L.sendsSlot = sends;
    }
    return L;
}

void MixerPanelComponent::drawStrip(juce::Graphics& g, const StripLayout& L, ChannelStrip* strip, bool isMaster)
{
    const auto& tracks = project.getTracks();
    const auto& buses  = project.getBuses();
    const auto isBus   = strip != nullptr && strip->isBus;

    // Resolve this strip's display data (track / bus / master).
    juce::String stripName = "MASTER";
    juce::Colour dotColour = accentColour;
    bool hasDot = false;
    float levelDb = masterLevelDb, mL = masterMeterDisplayL, mR = masterMeterDisplayR;
    double volDb = masterVolume.getValue();
    const std::vector<TrackState::InsertFx>* insertChain = nullptr;
    const std::vector<TrackState::SendFx>* sendChain = nullptr;
    int insertScroll = 0;
    juce::String outName = "Master";

    if (isMaster)
    {
        insertChain = &project.getMasterInserts();
        insertScroll = masterInsertScroll;
        outName = juce::String::fromUTF8("\xe2\x80\x94");   // master has no further output
    }

    if (! isMaster && strip != nullptr)
    {
        insertScroll = strip->insertScroll;
        levelDb = strip->levelDbDisplay;
        mL = strip->meterDisplayL;
        mR = strip->meterDisplayR;
        if (isBus && strip->busIndex >= 0 && strip->busIndex < static_cast<int>(buses.size()))
        {
            const auto& bus = buses[static_cast<std::size_t>(strip->busIndex)];
            stripName = bus.name; dotColour = bus.colour; hasDot = true;
            volDb = bus.volumeDb; insertChain = &bus.inserts;
        }
        else if (strip->trackIndex >= 0 && strip->trackIndex < static_cast<int>(tracks.size()))
        {
            const auto& t = tracks[static_cast<std::size_t>(strip->trackIndex)];
            stripName = t.name; dotColour = t.colour; hasDot = true;
            volDb = t.volumeDb; insertChain = &t.inserts; sendChain = &t.sends;
            outName = (t.outputBus >= 0 && t.outputBus < static_cast<int>(buses.size()))
                ? buses[static_cast<std::size_t>(t.outputBus)].name : juce::String("Master");
        }
    }

    // Card.
    g.setColour(isMaster ? stripBackground.brighter(0.05f) : stripBackground);
    g.fillRoundedRectangle(L.card.toFloat(), 8.0f);
    g.setColour((isMaster || isBus) ? accentColour.withAlpha(isMaster ? 0.7f : 0.45f) : stripStroke);
    g.drawRoundedRectangle(L.card.toFloat().reduced(0.5f), 8.0f, isMaster ? 1.4f : 1.0f);

    // Name + colour dot.
    auto nameRow = L.nameRow;
    if (isMaster)
    {
        g.setColour(accentColour);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText("MASTER", nameRow, juce::Justification::centred, false);
    }
    else
    {
        if (hasDot)
        {
            auto dot = nameRow.removeFromLeft(14);
            g.setColour(dotColour);
            if (isBus) g.fillRoundedRectangle(dot.withSizeKeepingCentre(8, 8).toFloat(), 2.0f);
            else       g.fillEllipse(dot.withSizeKeepingCentre(7, 7).toFloat());
        }
        g.setColour(juce::Colours::white.withAlpha(0.92f));
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText(stripName, nameRow, juce::Justification::centredLeft, true);
    }

    // Level readout (peak-hold dB).
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.drawText(levelDb <= -60.0f ? juce::String("-inf") : juce::String(levelDb, 1) + " dB",
               L.levelReadout, juce::Justification::centred, false);

    // INSERTS / SENDS: vertical lists (Logic-style). Each list shows its chain as thin
    // rows plus a "+" add row at the bottom.
    const auto drawListSection = [&](const juce::String& label, juce::Rectangle<int> labelR,
                                     juce::Rectangle<int> powerR, juce::Rectangle<int> listR,
                                     const std::vector<juce::String>& rows, int scrollPx = 0)
    {
        if (labelR.isEmpty()) return;
        g.setColour(mutedText);
        g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
        g.drawText(label, labelR, juce::Justification::centredLeft, false);
        g.setColour(mutedText.withAlpha(0.7f));
        g.drawEllipse(powerR.withSizeKeepingCentre(9, 9).toFloat(), 1.2f);

        g.setColour(slotBackground);
        g.fillRoundedRectangle(listR.toFloat(), 4.0f);
        g.setColour(stripStroke);
        g.drawRoundedRectangle(listR.toFloat(), 4.0f, 1.0f);

        const auto area = listR.reduced(3, 3);
        constexpr int rowH = 18, rowGap = 2, addH = 16;
        g.saveState();
        g.reduceClipRegion(area);
        int y = area.getY() - scrollPx;
        for (const auto& name : rows)
        {
            juce::Rectangle<int> row(area.getX(), y, area.getWidth(), rowH);
            if (row.getBottom() > area.getY() && row.getY() < area.getBottom())
            {
                g.setColour(juce::Colour(0xff2f6df0).withAlpha(0.85f));   // Logic-blue chip
                g.fillRoundedRectangle(row.toFloat(), 3.0f);
                g.setColour(juce::Colours::white.withAlpha(0.95f));
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText(name, row.reduced(5, 0), juce::Justification::centredLeft, true);
            }
            y += rowH + rowGap;
        }
        // "+" add row (scrolls with the content).
        juce::Rectangle<int> addRow(area.getX(), y, area.getWidth(), addH);
        if (addRow.getBottom() > area.getY() && addRow.getY() < area.getBottom())
        {
            g.setColour(mutedText.withAlpha(0.75f));
            g.setFont(juce::FontOptions(13.0f, juce::Font::plain));
            g.drawText("+", addRow, juce::Justification::centred, false);
        }
        g.restoreState();

        // Scroll indicator when the chain overflows the slot.
        const auto contentH = static_cast<int>(rows.size()) * (rowH + rowGap) + addH;
        if (contentH > area.getHeight())
        {
            const auto trackR = juce::Rectangle<float>(listR.getRight() - 3.0f, listR.getY() + 2.0f,
                                                       2.0f, listR.getHeight() - 4.0f);
            g.setColour(mutedText.withAlpha(0.18f));
            g.fillRoundedRectangle(trackR, 1.0f);
            const auto frac = static_cast<float>(area.getHeight()) / static_cast<float>(contentH);
            const auto thumbH = juce::jmax(10.0f, trackR.getHeight() * frac);
            const auto maxScroll = static_cast<float>(contentH - area.getHeight());
            const auto pos = maxScroll > 0.0f ? (static_cast<float>(scrollPx) / maxScroll) : 0.0f;
            const auto thumbY = trackR.getY() + pos * (trackR.getHeight() - thumbH);
            g.setColour(cyan.withAlpha(0.6f));
            g.fillRoundedRectangle(trackR.withY(thumbY).withHeight(thumbH), 1.0f);
        }
    };

    std::vector<juce::String> insertNames;
    if (insertChain != nullptr)
        for (const auto& fx : *insertChain)
            insertNames.push_back((fx.bypassed ? juce::String::fromUTF8("\xe2\x97\x8b ") : juce::String())
                                  + (fx.pluginName.isNotEmpty() ? fx.pluginName : "FX"));
    drawListSection("INSERTS", L.insertsLabel, L.insertsPower, L.insertsSlot, insertNames, insertScroll);

    std::vector<juce::String> sendNames;
    if (sendChain != nullptr)
        for (const auto& s : *sendChain)
        {
            const auto busName = (s.busIndex >= 0 && s.busIndex < static_cast<int>(buses.size()))
                ? buses[static_cast<std::size_t>(s.busIndex)].name : juce::String("Bus");
            sendNames.push_back(juce::String::fromUTF8("\xe2\x86\x92 ") + busName
                                + (s.prefader ? juce::String(" (pre)") : juce::String())
                                + "  " + juce::String(juce::roundToInt(s.level * 100.0)) + "%");
        }
    drawListSection("SENDS", L.sendsLabel, L.sendsPower, L.sendsSlot, sendNames);

    // Peak label ("---").
    g.setColour(mutedText.withAlpha(0.86f));
    g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    auto peakR = L.peakLabel;
    g.drawText("---", peakR.removeFromRight(peakR.getWidth() / 2), juce::Justification::centred, false);

    // dB scale next to the fader.
    const std::array<int, 8> ticks { 6, 0, -6, -12, -18, -24, -30, -36 };
    // Match the slider's travel: the handle centre moves between (top+radius) and
    // (bottom-radius), so inset the scale by the same amount or the labels won't line up.
    const auto faderTop = static_cast<float>(L.fader.getY()) + kFaderThumbRadius;
    const auto faderH   = juce::jmax(1.0f, static_cast<float>(L.fader.getHeight()) - 2.0f * kFaderThumbRadius);
    g.setFont(juce::FontOptions(8.5f, juce::Font::plain));
    g.setColour(mutedText.withAlpha(0.82f));
    for (const auto db : ticks)
    {
        const auto t = juce::jlimit(0.0, 1.0, (maxGainDb - db) / (maxGainDb - minGainDb));
        const auto y = faderTop + static_cast<float>(t) * faderH;
        g.drawText(juce::String(db), L.scaleArea.getX(), juce::roundToInt(y) - 6, L.scaleArea.getWidth(), 12,
                   juce::Justification::centredRight, false);
    }
    {
        const auto y = faderTop + faderH;
        g.drawText(juce::String::fromUTF8("-\xe2\x88\x9e"), L.scaleArea.getX(), juce::roundToInt(y) - 10, L.scaleArea.getWidth(), 12,
                   juce::Justification::centredRight, false);
    }

    // Stereo meter (aligned to the fader height).
    auto meterAligned = L.meter.withY(L.fader.getY()).withHeight(L.fader.getHeight());
    drawStereoMeter(g, meterAligned, mL, mR);

    // dB value box.
    g.setColour(slotBackground);
    g.fillRoundedRectangle(L.dbBox.toFloat(), 4.0f);
    g.setColour(stripStroke);
    g.drawRoundedRectangle(L.dbBox.toFloat(), 4.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.9f));
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(gainTextFromValue(volDb), L.dbBox, juce::Justification::centred, false);

    // Pan caption.
    g.setColour(mutedText.withAlpha(0.7f));
    g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    auto capRow = L.panCaption;
    const auto third = capRow.getWidth() / 3;
    g.drawText("L", capRow.removeFromLeft(third), juce::Justification::centredLeft, false);
    g.drawText("R", capRow.removeFromRight(third), juce::Justification::centredRight, false);
    g.drawText("C", capRow, juce::Justification::centred, false);

    // Output routing dropdown (click → choose Master or an aux bus, tracks only).
    g.setColour(slotBackground);
    g.fillRoundedRectangle(L.outDropdown.toFloat(), 4.0f);
    g.setColour(stripStroke);
    g.drawRoundedRectangle(L.outDropdown.toFloat(), 4.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.78f));
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    auto outR = L.outDropdown.reduced(8, 0);
    g.drawText(outName, outR.removeFromLeft(outR.getWidth() - 14), juce::Justification::centredLeft, true);
    g.drawText(juce::String::fromUTF8("\xe2\x96\xbe"), outR, juce::Justification::centredRight, false);
}

void MixerPanelComponent::paint(juce::Graphics& g)
{
    // No backdrop scrim — the mixer panel sits over the app without dimming it.
    const auto panel = getPanelBounds();
    g.setColour(panelBackground);
    g.fillRoundedRectangle(panel.toFloat(), 10.0f);
    g.setColour(panelStroke);
    g.drawRoundedRectangle(panel.toFloat(), 10.0f, 1.0f);

    auto inner = panel.reduced(panelPadding);
    auto titleRow = inner.removeFromTop(titleHeight);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("MIXER", titleRow, juce::Justification::centredLeft);

    const auto closeBounds = getCloseButtonBounds();
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(20.0f, juce::Font::plain));
    g.drawText("x", closeBounds, juce::Justification::centred);

    if (project.getTracks().empty())
    {
        g.setColour(mutedText);
        g.setFont(juce::FontOptions(14.0f, juce::Font::plain));
        g.drawText(juce::String::fromUTF8("No tracks yet \xe2\x80\x94 add a track to mix."), inner, juce::Justification::centred);
    }

    for (auto& strip : strips)
        if (strip != nullptr)
            drawStrip(g, strip->layout, strip.get(), false);

    drawStrip(g, masterLayout, nullptr, true);

    // ---- Bottom bar ----
    auto bar = panel.reduced(panelPadding, 0);
    bar = bar.removeFromBottom(bottomBarHeight);
    g.setColour(panelStroke.withAlpha(0.5f));
    g.drawHorizontalLine(bar.getY(), static_cast<float>(bar.getX()), static_cast<float>(bar.getRight()));

    // LINK (left).
    g.setColour(linkEnabled ? cyan : mutedText);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(juce::String::fromUTF8("\xf0\x9f\x94\x97 LINK"), linkButtonBounds, juce::Justification::centredLeft, false);

    // "+ BUS" button.
    g.setColour(juce::Colour(0xff262a30));
    g.fillRoundedRectangle(addBusButtonBounds.toFloat(), 5.0f);
    g.setColour(cyan.withAlpha(0.9f));
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.drawText("+ BUS", addBusButtonBounds, juce::Justification::centred, false);

    // VIEW segmented (centre).
    const auto drawSeg = [&](juce::Rectangle<int> r, const juce::String& label, bool on)
    {
        g.setColour(on ? accentColour : juce::Colour(0xff262a30));
        g.fillRoundedRectangle(r.toFloat(), 5.0f);
        g.setColour(on ? juce::Colours::white : mutedText);
        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        g.drawText(label, r, juce::Justification::centred, false);
    };
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.drawText("VIEW", viewFadersBounds.withX(viewFadersBounds.getX() - 52).withWidth(46),
               juce::Justification::centredRight, false);
    drawSeg(viewFadersBounds, "FADERS", view == MixerView::faders);
    drawSeg(viewMetersBounds, "METERS", view == MixerView::meters);
    drawSeg(viewCompactBounds, "COMPACT", view == MixerView::compact);

    // WIDTH (right).
    g.setColour(mutedText);
    g.drawText("WIDTH", juce::Rectangle<int>(bar.getRight() - 150, bar.getY(), 50, bar.getHeight()),
               juce::Justification::centredRight, false);
    auto widthKnob = juce::Rectangle<int>(bar.getRight() - 88, bar.getCentreY() - 11, 22, 22);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillEllipse(widthKnob.toFloat());
    g.setColour(cyan.withAlpha(0.85f));
    g.drawEllipse(widthKnob.toFloat().reduced(1.0f), 1.5f);
    g.setColour(juce::Colours::white.withAlpha(0.85f));
    g.drawText("100%", bar.getRight() - 56, bar.getCentreY() - 8, 52, 16, juce::Justification::centredLeft, false);

    // Drag ghost for a moving insert chip.
    if (insertDrag.dragging)
    {
        juce::Rectangle<float> chip(0, 0, 96, 18);
        chip.setCentre(insertDrag.pos.toFloat());
        g.setColour(juce::Colour(0xff2f6df0).withAlpha(0.9f));
        g.fillRoundedRectangle(chip, 3.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText(insertDrag.label, chip.reduced(5, 0), juce::Justification::centredLeft, true);
    }
}

void MixerPanelComponent::resized()
{
    const auto panel = getPanelBounds();
    auto inner = panel.reduced(panelPadding);
    inner.removeFromTop(titleHeight);
    inner.removeFromTop(6);
    inner.removeFromBottom(bottomBarHeight);

    const auto sw = currentStripWidth();
    int x = inner.getX();
    for (auto& strip : strips)
    {
        juce::Rectangle<int> col(x, inner.getY(), sw, inner.getHeight());
        if (strip != nullptr)
        {
            strip->layout = computeStripLayout(col);
            if (strip->volume != nullptr) strip->volume->setBounds(strip->layout.fader);
            if (strip->pan != nullptr)    strip->pan->setBounds(strip->layout.panKnob.withSizeKeepingCentre(40, 40));
            if (strip->mute != nullptr)   strip->mute->setBounds(strip->layout.mute);
            if (strip->solo != nullptr)   strip->solo->setBounds(strip->layout.solo);
            if (strip->record != nullptr) strip->record->setBounds(strip->layout.record);
        }
        x += sw + stripGap;
    }

    // Master strip pinned to the right edge (the space between fills as tracks are added).
    juce::Rectangle<int> masterCol(inner.getRight() - masterStripWidth, inner.getY(), masterStripWidth, inner.getHeight());
    masterLayout = computeStripLayout(masterCol);
    masterVolume.setBounds(masterLayout.fader);
    masterPan.setBounds(masterLayout.panKnob.withSizeKeepingCentre(40, 40));

    // Bottom bar hit areas.
    auto bar = panel.reduced(panelPadding, 0).removeFromBottom(bottomBarHeight);
    linkButtonBounds = bar.removeFromLeft(70).withSizeKeepingCentre(70, 20);
    bar.removeFromLeft(6);
    addBusButtonBounds = bar.removeFromLeft(60).withSizeKeepingCentre(56, 22);
    bar.removeFromRight(150);   // reserve the right side for the WIDTH control (drawn in paint)
    const int segW = 64, segH = 22, segGap = 4;
    const auto totalSeg = segW * 3 + segGap * 2;
    auto centre = bar.withSizeKeepingCentre(juce::jmin(totalSeg, juce::jmax(0, bar.getWidth())), segH);
    viewFadersBounds  = centre.removeFromLeft(segW); centre.removeFromLeft(segGap);
    viewMetersBounds  = centre.removeFromLeft(segW); centre.removeFromLeft(segGap);
    viewCompactBounds = centre.removeFromLeft(segW);
}

void MixerPanelComponent::timerCallback()
{
    if (static_cast<int>(project.getTracks().size()) != builtTrackCount
        || static_cast<int>(project.getBuses().size()) != builtBusCount)
    {
        rebuildStrips();
        resized();
    }
    else
    {
        syncControlsFromProject();
    }

    // Master meter is sourced from the host's single 60 Hz measurement (same peak-hold
    // as the tracks), so a single track reads identically on its strip and the master.
    const auto toBar = [](float lin)
    {
        const auto db = lin > 0.0f ? juce::Decibels::gainToDecibels(lin, static_cast<float>(minGainDb)) : static_cast<float>(minGainDb);
        return juce::jlimit(0.0f, 1.0f, juce::jmap(db, static_cast<float>(minGainDb), 0.0f, 0.0f, 1.0f));
    };
    if (onRequestMasterLevelStereo)
    {
        const auto s = onRequestMasterLevelStereo();
        masterMeterDisplayL = toBar(s.first);
        masterMeterDisplayR = toBar(s.second);
    }
    else if (onRequestMasterPeakStereo)
    {
        const auto s = onRequestMasterPeakStereo();
        masterMeterDisplayL = juce::jmax(toBar(s.first), masterMeterDisplayL * 0.82f);
        masterMeterDisplayR = juce::jmax(toBar(s.second), masterMeterDisplayR * 0.82f);
    }
    masterLevelDb = onRequestMasterLevelDb ? onRequestMasterLevelDb() : -100.0f;

    for (auto& strip : strips)
    {
        if (strip == nullptr) continue;
        if (strip->isBus)
        {
            if (onRequestBusLevelStereo)
            {
                const auto s = onRequestBusLevelStereo(strip->busIndex);
                strip->meterDisplayL = s.first;
                strip->meterDisplayR = s.second;
            }
            strip->levelDbDisplay = onRequestBusLevelDb ? onRequestBusLevelDb(strip->busIndex) : -100.0f;
        }
        else
        {
            if (onRequestTrackLevelStereo)
            {
                const auto s = onRequestTrackLevelStereo(strip->trackIndex);
                strip->meterDisplayL = s.first;
                strip->meterDisplayR = s.second;
            }
            else if (onRequestTrackLevel)
            {
                strip->meterDisplayL = strip->meterDisplayR = onRequestTrackLevel(strip->trackIndex);
            }
            strip->levelDbDisplay = onRequestTrackLevelDb ? onRequestTrackLevelDb(strip->trackIndex) : -100.0f;
        }
    }

    repaint();
}

void MixerPanelComponent::drawStereoMeter(juce::Graphics& g, juce::Rectangle<int> bounds, float levelL, float levelR) const
{
    const auto area = bounds.toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillRoundedRectangle(area, 2.0f);

    const auto gap = 2.0f;
    const auto barW = (area.getWidth() - gap) * 0.5f;
    auto leftBar  = area.withWidth(barW);
    auto rightBar = area.withWidth(barW).withX(area.getRight() - barW);

    const auto drawBar = [&](juce::Rectangle<float> bar, float level)
    {
        if (level <= 0.001f) return;
        juce::ColourGradient grad(juce::Colour(0xff39d36b), bar.getX(), bar.getBottom(),
                                  juce::Colour(0xffe8401f), bar.getX(), bar.getY(), false);
        grad.addColour(0.72, juce::Colour(0xffe7c93a));
        auto fill = bar.removeFromBottom(bar.getHeight() * juce::jlimit(0.0f, 1.0f, level));
        g.setGradientFill(grad);
        g.fillRoundedRectangle(fill, 1.5f);
    };
    drawBar(leftBar, levelL);
    drawBar(rightBar, levelR);
}

bool MixerPanelComponent::hitTest(int, int)
{
    return true;
}

void MixerPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    const auto pos = event.getPosition();

    if (getCloseButtonBounds().contains(pos))
    {
        closePanel();
        return;
    }

    if (linkButtonBounds.contains(pos))
    {
        linkEnabled = ! linkEnabled;
        repaint();
        return;
    }
    if (addBusButtonBounds.contains(pos)) { if (onAddBus) onAddBus(); return; }
    if (viewFadersBounds.contains(pos))  { view = MixerView::faders;  resized(); repaint(); return; }
    if (viewMetersBounds.contains(pos))  { view = MixerView::meters;  resized(); repaint(); return; }
    if (viewCompactBounds.contains(pos)) { view = MixerView::compact; resized(); repaint(); return; }

    // Name click → select that track in the arrangement; OUT box → routing menu.
    for (auto& strip : strips)
    {
        if (strip == nullptr || strip->isBus) continue;
        if (strip->layout.nameRow.contains(pos))
        {
            if (onSelectTrack) onSelectTrack(strip->trackIndex);
            return;
        }
        if (strip->layout.outDropdown.contains(pos))
        {
            if (onOutputRouteClicked) onOutputRouteClicked(strip->trackIndex);
            return;
        }
    }

    // Insert list: a press on a chip arms a possible drag (resolved in mouseUp/mouseDrag);
    // a press on the "+" / empty row is handled as a click on mouseUp.
    {
        int id = -1, row = -1;
        if (insertSlotAt(pos, id, row))
        {
            insertDrag = InsertDrag{};
            insertDrag.srcTrack = id;     // "track" here = generic insert-chain id
            insertDrag.srcIndex = row;    // already clamped (-1 = add slot)
            insertDrag.pos = pos;
            if (row >= 0)
            {
                insertDrag.armed = true;   // chip → could be a drag
                if (const auto* chain = insertChainForId(id); chain != nullptr && row < static_cast<int>(chain->size()))
                    insertDrag.label = (*chain)[static_cast<std::size_t>(row)].pluginName;
            }
            return;
        }
    }

    // Send list (track strips only): existing row arms a horizontal drag-to-set-level
    // (a plain click without dragging opens the menu in mouseUp); "+" add slot opens now.
    {
        int t = -1, row = -1;
        if (sendSlotAt(pos, t, row))
        {
            if (row >= 0)
            {
                const auto& tracks = project.getTracks();
                sendDrag = SendDrag{};
                sendDrag.armed = true;
                sendDrag.track = t;
                sendDrag.row = row;
                if (t >= 0 && t < static_cast<int>(tracks.size())
                    && row < static_cast<int>(tracks[static_cast<std::size_t>(t)].sends.size()))
                    sendDrag.startLevel = static_cast<float>(tracks[static_cast<std::size_t>(t)].sends[static_cast<std::size_t>(row)].level);
                for (auto& strip : strips)
                    if (strip != nullptr && ! strip->isBus && strip->trackIndex == t)
                        sendDrag.rowWidth = juce::jmax(1, strip->layout.sendsSlot.reduced(3, 3).getWidth());
            }
            else if (onSendClicked)
            {
                onSendClicked(t, row);
            }
            return;
        }
    }

    if (! getPanelBounds().contains(pos))
        closePanel();
}

const std::vector<TrackState::InsertFx>* MixerPanelComponent::insertChainForId(int id) const
{
    if (id == kMasterInsertKey)
        return &project.getMasterInserts();
    if (id >= 1000000)
    {
        const auto b = id - 1000000;
        const auto& buses = project.getBuses();
        return (b >= 0 && b < static_cast<int>(buses.size())) ? &buses[static_cast<std::size_t>(b)].inserts : nullptr;
    }
    const auto& tracks = project.getTracks();
    return (id >= 0 && id < static_cast<int>(tracks.size())) ? &tracks[static_cast<std::size_t>(id)].inserts : nullptr;
}

bool MixerPanelComponent::insertSlotAt(juce::Point<int> p, int& idOut, int& rowOut) const
{
    const auto hitSlot = [&](juce::Rectangle<int> slot, int id, int scrollPx) -> bool
    {
        if (slot.isEmpty() || ! slot.contains(p)) return false;
        const auto area = slot.reduced(3, 3);
        const auto* chain = insertChainForId(id);
        const auto count = chain != nullptr ? static_cast<int>(chain->size()) : 0;
        const auto row = (p.y - area.getY() + scrollPx) / 20;   // 18px row + 2px gap
        idOut = id;
        rowOut = (row >= 0 && row < count) ? row : -1;
        return true;
    };

    for (auto& strip : strips)
        if (strip != nullptr && hitSlot(strip->layout.insertsSlot, strip->insertId(), strip->insertScroll))
            return true;

    // Master strip inserts.
    return hitSlot(masterLayout.insertsSlot, kMasterInsertKey, masterInsertScroll);
}

bool MixerPanelComponent::sendSlotAt(juce::Point<int> p, int& trackOut, int& rowOut) const
{
    const auto& tracks = project.getTracks();
    for (auto& strip : strips)
    {
        if (strip == nullptr || strip->isBus || strip->layout.sendsSlot.isEmpty()
            || ! strip->layout.sendsSlot.contains(p))
            continue;
        const auto area = strip->layout.sendsSlot.reduced(3, 3);
        const auto count = (strip->trackIndex >= 0 && strip->trackIndex < static_cast<int>(tracks.size()))
            ? static_cast<int>(tracks[static_cast<std::size_t>(strip->trackIndex)].sends.size()) : 0;
        const auto row = (p.y - area.getY()) / 20;
        trackOut = strip->trackIndex;
        rowOut = (row >= 0 && row < count) ? row : -1;
        return true;
    }
    return false;
}

void MixerPanelComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (sendDrag.armed)
    {
        if (! sendDrag.dragging && event.getDistanceFromDragStart() > 4)
            sendDrag.dragging = true;
        if (sendDrag.dragging)
        {
            const auto delta = static_cast<float>(event.getDistanceFromDragStartX()) / static_cast<float>(sendDrag.rowWidth);
            const auto level = juce::jlimit(0.0f, 1.0f, sendDrag.startLevel + delta);
            if (onSendLevelChanged) onSendLevelChanged(sendDrag.track, sendDrag.row, level);
            repaint();
        }
        return;
    }

    if (insertDrag.armed)
    {
        insertDrag.pos = event.getPosition();
        if (! insertDrag.dragging && event.getDistanceFromDragStart() > 6)
        {
            insertDrag.dragging = true;
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        }
        if (insertDrag.dragging)
            repaint();
    }
}

void MixerPanelComponent::mouseUp(const juce::MouseEvent& event)
{
    const auto pos = event.getPosition();

    if (sendDrag.armed)
    {
        const bool wasDrag = sendDrag.dragging;
        const auto t = sendDrag.track, row = sendDrag.row;
        sendDrag = SendDrag{};
        // A double-click is handled by mouseDoubleClick (type a %), so don't also pop the
        // single-click send menu on the first release of that double-click.
        if (! wasDrag && event.getNumberOfClicks() < 2 && onSendClicked)
            onSendClicked(t, row);
        return;
    }

    if (insertDrag.dragging)
    {
        int t = -1, row = -1;
        // DnD currently supported between track strips only (ids < bus key base).
        if (insertSlotAt(pos, t, row) && t >= 0 && t < 1000000 && insertDrag.srcTrack < 1000000 && onInsertMoved)
        {
            const auto* chain = insertChainForId(t);
            const auto count = chain != nullptr ? static_cast<int>(chain->size()) : 0;
            const auto dest = (row >= 0 && row < count) ? row : count;   // drop on chip → before it; else end
            onInsertMoved(insertDrag.srcTrack, insertDrag.srcIndex, t, dest);
        }
        setMouseCursor(juce::MouseCursor::NormalCursor);
        insertDrag = InsertDrag{};
        repaint();
        return;
    }

    // Was a plain press (no drag): treat as a click — open the slot/add menu.
    if (insertDrag.srcTrack >= 0 && onInsertClicked)
        onInsertClicked(insertDrag.srcTrack, insertDrag.srcIndex);
    insertDrag = InsertDrag{};
}

void MixerPanelComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    const auto pos = event.getPosition();
    // Double-click a dB box → inline numeric entry.
    for (int i = 0; i < static_cast<int>(strips.size()); ++i)
        if (strips[static_cast<std::size_t>(i)] != nullptr && strips[static_cast<std::size_t>(i)]->layout.dbBox.contains(pos))
        {
            beginDbEdit(i, strips[static_cast<std::size_t>(i)]->layout.dbBox);
            return;
        }
    if (masterLayout.dbBox.contains(pos))
    {
        beginDbEdit(-1, masterLayout.dbBox);
        return;
    }

    // Double-click a send row's % → type an exact percentage.
    int st = -1, sr = -1;
    if (sendSlotAt(pos, st, sr) && sr >= 0)
    {
        for (auto& strip : strips)
            if (strip != nullptr && ! strip->isBus && strip->trackIndex == st)
            {
                const auto area = strip->layout.sendsSlot.reduced(3, 3);
                const auto rowRect = juce::Rectangle<int>(area.getX(), area.getY() + sr * 20, area.getWidth(), 18);
                beginSendEdit(st, sr, rowRect);
                break;
            }
    }
}

void MixerPanelComponent::beginSendEdit(int track, int row, juce::Rectangle<int> box)
{
    commitSendEdit();   // close any prior editor
    const auto& tracks = project.getTracks();
    if (track < 0 || track >= static_cast<int>(tracks.size())
        || row < 0 || row >= static_cast<int>(tracks[static_cast<std::size_t>(track)].sends.size()))
        return;

    sendEditTrack = track;
    sendEditRow = row;
    const auto curPct = juce::roundToInt(tracks[static_cast<std::size_t>(track)].sends[static_cast<std::size_t>(row)].level * 100.0);

    sendEditor = std::make_unique<juce::TextEditor>();
    sendEditor->setBounds(box.expanded(2, 2));
    sendEditor->setJustification(juce::Justification::centred);
    sendEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0c0e10));
    sendEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    sendEditor->setColour(juce::TextEditor::outlineColourId, cyan);
    sendEditor->setColour(juce::TextEditor::focusedOutlineColourId, cyan);
    sendEditor->setText(juce::String(curPct), false);
    sendEditor->setSelectAllWhenFocused(true);
    sendEditor->onReturnKey = [this] { commitSendEdit(); };
    sendEditor->onEscapeKey = [this] { sendEditor.reset(); sendEditTrack = sendEditRow = -1; };
    sendEditor->onFocusLost = [this] { commitSendEdit(); };
    addAndMakeVisible(sendEditor.get());
    sendEditor->grabKeyboardFocus();
}

void MixerPanelComponent::commitSendEdit()
{
    if (sendEditor == nullptr) return;
    const auto pct = juce::jlimit(0, 100, sendEditor->getText().removeCharacters("% ").getIntValue());
    const auto track = sendEditTrack, row = sendEditRow;
    sendEditor.reset();
    sendEditTrack = sendEditRow = -1;

    if (track >= 0 && row >= 0 && onSendLevelChanged)
        onSendLevelChanged(track, row, static_cast<float>(pct) / 100.0f);
    repaint();
}

void MixerPanelComponent::beginDbEdit(int target, juce::Rectangle<int> box)
{
    commitDbEdit();   // close any prior editor
    dbEditTarget = target;

    double current = 0.0;
    if (target == -1) current = masterVolume.getValue();
    else if (target >= 0 && target < static_cast<int>(strips.size()) && strips[static_cast<std::size_t>(target)]->volume != nullptr)
        current = strips[static_cast<std::size_t>(target)]->volume->getValue();

    dbEditor = std::make_unique<juce::TextEditor>();
    dbEditor->setBounds(box.expanded(2, 2));
    dbEditor->setJustification(juce::Justification::centred);
    dbEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0c0e10));
    dbEditor->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    dbEditor->setColour(juce::TextEditor::outlineColourId, cyan);
    dbEditor->setColour(juce::TextEditor::focusedOutlineColourId, cyan);
    dbEditor->setText(current <= minGainDb + 0.01 ? juce::String("-inf") : juce::String(current, 1), false);
    dbEditor->setSelectAllWhenFocused(true);
    dbEditor->onReturnKey   = [this] { commitDbEdit(); };
    dbEditor->onEscapeKey   = [this] { dbEditor.reset(); dbEditTarget = -2; };
    dbEditor->onFocusLost   = [this] { commitDbEdit(); };
    addAndMakeVisible(dbEditor.get());
    dbEditor->grabKeyboardFocus();
}

void MixerPanelComponent::commitDbEdit()
{
    if (dbEditor == nullptr) return;
    const auto text = dbEditor->getText().trim().toLowerCase();
    double db = (text == "-inf" || text == "inf" || text == "-\xe2\x88\x9e")
        ? minGainDb : text.removeCharacters("dB ").getDoubleValue();
    db = juce::jlimit(minGainDb, maxGainDb, db);

    const auto target = dbEditTarget;
    dbEditor.reset();
    dbEditTarget = -2;

    if (target == -1)
        masterVolume.setValue(db, juce::sendNotificationSync);
    else if (target >= 0 && target < static_cast<int>(strips.size()) && strips[static_cast<std::size_t>(target)]->volume != nullptr)
        strips[static_cast<std::size_t>(target)]->volume->setValue(db, juce::sendNotificationSync);
    repaint();
}

void MixerPanelComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    const auto pos = event.getPosition();
    const auto applyScroll = [&](juce::Rectangle<int> slot, int id, int& scrollRef) -> bool
    {
        if (slot.isEmpty() || ! slot.contains(pos)) return false;
        const auto* chain = insertChainForId(id);
        const auto count = chain != nullptr ? static_cast<int>(chain->size()) : 0;
        const auto areaH = slot.reduced(3, 3).getHeight();
        const auto contentH = count * 20 + 16;
        const auto maxScroll = juce::jmax(0, contentH - areaH);
        scrollRef = juce::jlimit(0, maxScroll, scrollRef - juce::roundToInt(wheel.deltaY * 60.0f));
        repaint();
        return true;
    };

    for (auto& strip : strips)
        if (strip != nullptr && applyScroll(strip->layout.insertsSlot, strip->insertId(), strip->insertScroll))
            return;
    applyScroll(masterLayout.insertsSlot, kMasterInsertKey, masterInsertScroll);
}

bool MixerPanelComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey)
    {
        maximized = ! maximized;   // Enter toggles full-screen mixer.
        resized();
        repaint();
        return true;
    }
    if (key == juce::KeyPress::escapeKey)
    {
        if (maximized) { maximized = false; resized(); repaint(); }
        else           { closePanel(); }
        return true;
    }
    return false;
}

juce::Rectangle<int> MixerPanelComponent::getPanelBounds() const
{
    if (maximized)
        return getLocalBounds().reduced(12);

    auto area = getLocalBounds().reduced(40);
    const auto stripsCount = juce::jmax(1, static_cast<int>(strips.size()));
    const auto desiredWidth = panelPadding * 2 + (stripsCount + 1) * (currentStripWidth() + stripGap) + 20;
    const auto maxWidth = juce::jmin(area.getWidth(), juce::jmax(620, desiredWidth));
    const auto maxHeight = juce::jmin(area.getHeight(), 620);
    return area.withSizeKeepingCentre(maxWidth, maxHeight);
}

juce::Rectangle<int> MixerPanelComponent::getCloseButtonBounds() const
{
    const auto panel = getPanelBounds();
    return juce::Rectangle<int>(panel.getRight() - panelPadding - 24, panel.getY() + panelPadding, 24, titleHeight);
}
}  // namespace orion
