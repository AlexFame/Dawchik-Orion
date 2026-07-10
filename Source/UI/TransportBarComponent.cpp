#include "TransportBarComponent.h"

#include "OrionTheme.h"

#include <cmath>

namespace orion
{
namespace
{
// Shared so paint() (background) and resized() (layout) stay in lock-step.
constexpr int kPanelWidth = 500;
constexpr int kPanelHeight = 88;
constexpr int kReadoutRowHeight = 40;   // shared readout band; the remaining band holds transport controls
constexpr int kUtilityHeight = 56;
constexpr int kBrandWidth = 272;
constexpr int kSideGroupWidth = 330;   // holds three nav items: MIXER · CLIP EDITOR · STEPS
constexpr int kCpuWidth = 92;            // CPU readout (label + bars + %)
constexpr int kMasterMeterWidth = 230;   // master meter is fixed-width, not edge-to-edge
constexpr int kOuterPadding = 24;
constexpr int kGroupGap = 24;

// Shared two-row layout so every cluster (readouts, MIXER/CLIP, MASTER OUT, CPU) puts its
// label on the same line and its content on the same line, all the same size.
constexpr int kContentBand = 34;   // centred label+value block with clear frame breathing room
constexpr int kLabelRowH = 14;
constexpr int kLabelGap = 3;
constexpr float kLabelFontSize = 12.0f;

juce::Font transportFont(float size, bool bold = false)
{
    auto font = bold
        ? juce::Font(juce::FontOptions("Avenir Next", size, juce::Font::bold))
        : juce::Font(juce::FontOptions("Avenir Next", "Medium", size));
    font.setExtraKerningFactor(bold ? 0.010f : 0.003f);
    return font;
}

juce::String formatDb(double db)
{
    return db <= -59.0 ? "-inf" : juce::String(db, 1) + " dB";
}

void styleButton(juce::TextButton& button)
{
    button.setColour(juce::TextButton::buttonColourId, theme::core::voidBlack);
    button.setColour(juce::TextButton::buttonOnColourId, theme::accent::brandCyan);
    button.setColour(juce::TextButton::textColourOffId, theme::text::secondary);
    button.setColour(juce::TextButton::textColourOnId, theme::text::inverse);
}

class IconButtonLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& buttonBackgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override
    {
        juce::ignoreUnused(buttonBackgroundColour);
        // Flat transport (per the new design): no button frame, just the icon on the bar.
        // A faint rounded highlight on hover/press gives click affordance.
        if (shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted)
        {
            const auto full = button.getLocalBounds().toFloat();
            const float side = juce::jmin(full.getWidth(), full.getHeight());
            const auto area = juce::Rectangle<float>(side, side).withCentre(full.getCentre());
            g.setColour(juce::Colours::white.withAlpha(shouldDrawButtonAsDown ? 0.10f : 0.06f));
            g.fillRoundedRectangle(area, side * 0.5f);
        }
    }

    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool,
                        bool) override
    {
        const auto role = button.getComponentID();
        const auto active = button.getToggleState();
        // prev/play read bright; stop/loop/metronome are muted until toggled on; record is red.
        auto colour = (role == "play" || role == "prev") ? theme::text::primary
                                                         : theme::text::secondary.withAlpha(0.55f);
        if (active)
            colour = theme::text::primary;
        if (role == "record")
            colour = theme::accent::recordRed;

        // Icons are drawn inside a CENTERED SQUARE, independent of the button's
        // (wide) rectangle, so shapes stay proportional and never stretch.
        const auto full = button.getLocalBounds().toFloat();
        const float side = juce::jmin(full.getWidth(), full.getHeight()) - (role == "play" ? 16.0f : 15.0f);
        const auto area = juce::Rectangle<float>(side, side).withCentre(full.getCentre());
        const auto w = area.getWidth();
        const auto h = area.getHeight();
        g.setColour(colour);

        if (role == "play")
        {
            juce::Path path;
            path.addTriangle(area.getX() + w * 0.16f, area.getY() + h * 0.06f,
                             area.getRight() - w * 0.02f, area.getCentreY(),
                             area.getX() + w * 0.16f, area.getBottom() - h * 0.06f);
            g.fillPath(path);
        }
        else if (role == "prev")
        {
            // Skip-to-start: vertical bar + left-pointing triangle.
            const auto cy = area.getCentreY();
            g.fillRoundedRectangle(juce::Rectangle<float>(area.getX() + w * 0.10f, area.getY() + h * 0.12f,
                                                          w * 0.12f, h * 0.76f), 1.5f);
            juce::Path tri;
            tri.addTriangle(area.getRight() - w * 0.04f, area.getY() + h * 0.10f,
                            area.getRight() - w * 0.04f, area.getBottom() - h * 0.10f,
                            area.getX() + w * 0.34f, cy);
            g.fillPath(tri);
        }
        else if (role == "stop")
        {
            g.fillRect(area.withSizeKeepingCentre(w * 0.58f, h * 0.58f));
        }
        else if (role == "record")
        {
            // Filled red circle inside a thin red ring (per the new design).
            g.setColour(theme::accent::recordRed);
            const auto ring = area.withSizeKeepingCentre(w * 0.86f, h * 0.86f);
            g.drawEllipse(ring, 1.3f);
            g.fillEllipse(area.withSizeKeepingCentre(w * 0.62f, h * 0.62f));
        }
        else if (role == "loop")
        {
            // Return / loop hook: an arc across the top curving down the right, with
            // an arrowhead pointing left at the bottom-left.
            const auto cx = area.getCentreX();
            const auto cy = area.getCentreY();
            juce::Path hook;
            hook.startNewSubPath(area.getX() + w * 0.18f, cy + h * 0.20f);
            hook.lineTo(area.getX() + w * 0.62f, cy + h * 0.20f);
            hook.quadraticTo(area.getRight(), cy + h * 0.20f, area.getRight(), cy - h * 0.04f);
            hook.quadraticTo(area.getRight(), area.getY() + h * 0.04f, cx, area.getY() + h * 0.04f);
            hook.lineTo(area.getX() + w * 0.18f, area.getY() + h * 0.04f);
            g.strokePath(hook, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            juce::Path arrow;
            arrow.addTriangle(area.getX() + w * 0.18f, cy + h * 0.04f,
                              area.getX() + w * 0.18f, cy + h * 0.36f,
                              area.getX() - w * 0.02f, cy + h * 0.20f);
            g.fillPath(arrow);
        }
        else if (role == "metronome")
        {
            // MPC-style metronome: a triangular body outline, a diagonal pendulum rod that
            // pokes above the apex, and a round weight (bob) high on the rod.
            const auto cx = area.getCentreX();
            const auto top = area.getY() + h * 0.16f;
            const auto bottom = area.getBottom() - h * 0.13f;
            const juce::PathStrokeType stroke(2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded);

            // Near-triangular body (very narrow top → wide base).
            juce::Path body;
            body.startNewSubPath(cx - w * 0.07f, top);
            body.lineTo(cx + w * 0.07f, top);
            body.lineTo(cx + w * 0.34f, bottom);
            body.lineTo(cx - w * 0.34f, bottom);
            body.closeSubPath();
            g.strokePath(body, stroke);

            // Base bar (stand).
            g.fillRoundedRectangle(juce::Rectangle<float>(cx - w * 0.34f, bottom - 1.3f, w * 0.68f, 2.6f), 1.0f);

            // Pendulum rod: from the base up, leaning right, tip poking just above the apex.
            const juce::Point<float> rodBottom(cx - w * 0.05f, bottom - h * 0.06f);
            const juce::Point<float> rodTop(cx + w * 0.16f, top - h * 0.06f);
            g.drawLine(rodBottom.x, rodBottom.y, rodTop.x, rodTop.y, 2.2f);

            // Weight (bob) high on the rod — a solid square like the reference.
            const juce::Point<float> bob(rodTop.x + (rodBottom.x - rodTop.x) * 0.34f,
                                         rodTop.y + (rodBottom.y - rodTop.y) * 0.34f);
            g.fillRect(juce::Rectangle<float>(w * 0.17f, w * 0.13f).withCentre(bob));
        }
    }
};

IconButtonLookAndFeel iconButtonLookAndFeel;
}  // namespace

TransportBarComponent::TransportBarComponent()
{
    prevButton.setComponentID("prev");
    playButton.setComponentID("play");
    stopButton.setComponentID("stop");
    recordButton.setComponentID("record");
    metronomeButton.setComponentID("metronome");
    loopButton.setComponentID("loop");

    for (auto* button : { &prevButton, &playButton, &stopButton, &recordButton, &metronomeButton, &loopButton })
    {
        styleButton(*button);
        button->setLookAndFeel(&iconButtonLookAndFeel);
        button->addListener(this);
        // Don't let transport buttons keep keyboard focus — otherwise after clicking one (e.g.
        // skip-to-start) it stayed focused and every Return re-triggered it, hijacking Enter.
        button->setWantsKeyboardFocus(false);
        addAndMakeVisible(*button);
    }

    for (auto* button : { &recordButton, &metronomeButton, &loopButton })
        button->setClickingTogglesState(true);

    playButton.setClickingTogglesState(false);
    prevButton.setClickingTogglesState(false);
    recordButton.addMouseListener(this, false);
}

TransportBarComponent::~TransportBarComponent()
{
    for (auto* button : { &prevButton, &playButton, &stopButton, &recordButton, &metronomeButton, &loopButton })
    {
        button->removeListener(this);
        button->setLookAndFeel(nullptr);
    }
}

void TransportBarComponent::setState(const TransportBarState& newState)
{
    // Called every 60 Hz tick. The whole bar (lots of text) was repainting each time because
    // engineLoad (CPU%) / master level jitter constantly — pegging the software renderer while idle.
    // Only repaint when a DISPLAYED value actually changes (with a tolerance on the noisy meters).
    const auto& o = state;
    const bool sameEnough =
           std::abs(newState.tempoBpm      - o.tempoBpm)      < 0.005
        && std::abs(newState.scanProgress  - o.scanProgress)  < 0.005
        && std::abs(newState.engineLoad    - o.engineLoad)    < 0.03f   // ignore CPU-meter jitter
        && std::abs(newState.masterGainDb  - o.masterGainDb)  < 0.05
        && std::abs(newState.masterLevel   - o.masterLevel)   < 0.01f
        && std::abs(newState.masterLevelDb - o.masterLevelDb) < 0.5f
        && newState.keyText == o.keyText && newState.timeSignature == o.timeSignature
        && newState.positionText == o.positionText && newState.projectName == o.projectName
        && newState.scanName == o.scanName
        && newState.playing == o.playing && newState.recording == o.recording
        && newState.loop == o.loop && newState.metronome == o.metronome && newState.countIn == o.countIn
        && newState.scanVisible == o.scanVisible
        && newState.mixerOpen == o.mixerOpen && newState.clipEditorOpen == o.clipEditorOpen
        && newState.stepSequencerOpen == o.stepSequencerOpen;

    const bool staticLayoutChanged = newState.tempoBpm != o.tempoBpm
        || newState.keyText != o.keyText || newState.timeSignature != o.timeSignature
        || newState.projectName != o.projectName || newState.scanVisible != o.scanVisible
        || newState.scanName != o.scanName || newState.scanProgress != o.scanProgress
        || newState.playing != o.playing || newState.recording != o.recording
        || newState.loop != o.loop || newState.metronome != o.metronome || newState.countIn != o.countIn
        || newState.mixerOpen != o.mixerOpen || newState.clipEditorOpen != o.clipEditorOpen
        || newState.stepSequencerOpen != o.stepSequencerOpen;
    const bool positionChanged = newState.positionText != o.positionText;
    const bool masterMeterChanged = std::abs(newState.masterGainDb - o.masterGainDb) >= 0.05
        || std::abs(newState.masterLevel - o.masterLevel) >= 0.01f
        || std::abs(newState.masterLevelDb - o.masterLevelDb) >= 0.5f;
    const bool cpuMeterChanged = std::abs(newState.engineLoad - o.engineLoad) >= 0.03f;

    state = newState;
    syncButtons();
    if (sameEnough)
        return;

    if (staticLayoutChanged)
        repaint();
    else
    {
        if (positionChanged)
            repaint(positionCardBounds);
        if (masterMeterChanged)
            repaint(masterMeterBounds);
        if (cpuMeterChanged)
            repaint(cpuMeterBounds);
    }
}

juce::Rectangle<int> TransportBarComponent::getTempoEditorBounds() const noexcept
{
    // A compact field over the tempo VALUE (which now sits below the label), sized to the
    // number and left-aligned with it — so clicking to edit doesn't blow the field up wide.
    auto column = tempoCardBounds.reduced(2, 0);
    column.removeFromTop(15);   // skip the "Tempo" label row
    return column.removeFromTop(24).withSizeKeepingCentre(juce::jmin(80, column.getWidth()), 24);
}

juce::Rectangle<int> TransportBarComponent::getKeyBounds() const noexcept
{
    return keyCardBounds;
}

juce::Rectangle<int> TransportBarComponent::getRecordOptionsBounds() const noexcept
{
    return recordButton.getBounds();
}

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(theme::core::deepSpace);

    const auto area = getLocalBounds().toFloat();
    juce::ColourGradient leftGlow(theme::cool::cyan.withAlpha(0.14f), area.getX(), area.getY(),
                                  juce::Colours::transparentBlack, area.getX() + area.getWidth() * 0.42f, area.getBottom(), false);
    g.setGradientFill(leftGlow);
    g.fillRect(area);

    juce::ColourGradient rightGlow(theme::accent::brandCyan.withAlpha(0.06f), area.getRight(), area.getY(),
                                   juce::Colours::transparentBlack, area.getX() + area.getWidth() * 0.55f, area.getBottom(), false);
    g.setGradientFill(rightGlow);
    g.fillRect(area);

    g.setColour(theme::line::subtle.withAlpha(0.62f));
    g.drawLine(0.0f, static_cast<float>(getHeight() - 1), static_cast<float>(getWidth()), static_cast<float>(getHeight() - 1), 1.0f);

    const auto panel = getLocalBounds().withSizeKeepingCentre(kPanelWidth, kPanelHeight).toFloat();
    g.setColour(theme::core::voidBlack.withAlpha(0.58f));
    g.fillRoundedRectangle(panel, theme::metrics::panelRadius);
    g.setColour(theme::line::soft.withAlpha(0.78f));
    g.drawRoundedRectangle(panel.reduced(0.8f), theme::metrics::panelRadius, 1.6f);

    // The transport buttons are the primary action group. Give them a quiet elevated well so
    // they read as one centered control block instead of six unrelated icons on the shelf.
    auto controlGroup = prevButton.getBounds();
    controlGroup = controlGroup.getUnion(stopButton.getBounds())
                             .getUnion(recordButton.getBounds())
                             .getUnion(playButton.getBounds())
                             .getUnion(loopButton.getBounds())
                             .getUnion(metronomeButton.getBounds())
                             .expanded(9, 3);
    g.setColour(theme::surface::elevated.withAlpha(0.34f));
    g.fillRoundedRectangle(controlGroup.toFloat(), theme::metrics::controlRadius);
    g.setColour(theme::line::normal.withAlpha(0.20f));
    g.drawRoundedRectangle(controlGroup.toFloat().reduced(0.5f), theme::metrics::controlRadius, 1.0f);

    // One readout column, new-design style: a small muted label on top, the value below
    // (left-aligned), with an optional dropdown chevron after the value (Key opens a picker).
    auto drawReadout = [&g](juce::Rectangle<int> bounds,
                            const juce::String& label,
                            const juce::String& value,
                            juce::Colour valueColour,
                            bool showChevron)
    {
        auto col = bounds.reduced(2, 0).withSizeKeepingCentre(bounds.getWidth() - 4, kContentBand);
        const auto labelRow = col.removeFromTop(kLabelRowH).toFloat();
        col.removeFromTop(kLabelGap);
        g.setColour(theme::text::tertiary.withAlpha(0.55f));
        g.setFont(transportFont(kLabelFontSize));
        g.drawText(label, labelRow, juce::Justification::centred);

        const auto valueRow = col.toFloat();
        const juce::Font valueFont = transportFont(21.0f, true);
        const float valueW = juce::GlyphArrangement::getStringWidth(valueFont, value);
        g.setColour(valueColour);
        g.setFont(valueFont);
        g.drawText(value, valueRow, juce::Justification::centred);

        if (showChevron)
        {
            const float chx = valueRow.getCentreX() + valueW * 0.5f + 9.0f;
            const float cy = valueRow.getCentreY();
            juce::Path chev;
            chev.startNewSubPath(chx - 4.0f, cy - 2.0f);
            chev.lineTo(chx, cy + 3.0f);
            chev.lineTo(chx + 4.0f, cy - 2.0f);
            g.setColour(theme::text::tertiary.withAlpha(0.8f));
            g.strokePath(chev, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    };

    const auto valueWhite = theme::text::primary.withAlpha(0.96f);
    drawReadout(keyCardBounds,     "Key",            state.keyText,                  valueWhite, true);
    drawReadout(timeSigCardBounds, "Time Signature", state.timeSignature,            valueWhite, false);
    drawReadout(tempoCardBounds,   "Tempo",          juce::String(state.tempoBpm, 0), valueWhite, false);
    drawReadout(positionCardBounds, "Time",          state.positionText,             valueWhite, false);

    drawBrandCluster(g, brandClusterBounds);
    drawUtilityItem(g, UtilityItem::mixer, "MIXER", getUtilityItemBounds(UtilityItem::mixer));
    drawUtilityItem(g, UtilityItem::clipEditor, "CLIP EDITOR", getUtilityItemBounds(UtilityItem::clipEditor));
    drawUtilityItem(g, UtilityItem::stepSequencer, "STEPS", getUtilityItemBounds(UtilityItem::stepSequencer));

    drawMasterMeter(g, masterMeterBounds);
    drawCpuMeter(g, cpuMeterBounds);

    if (state.scanVisible)
    {
        auto scan = getLocalBounds().reduced(24, 0).removeFromBottom(4).toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.48f));
        g.fillRoundedRectangle(scan, 2.0f);
        g.setColour(theme::warm::red);
        g.fillRoundedRectangle(scan.withWidth(scan.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, state.scanProgress))), 2.0f);
    }
}

void TransportBarComponent::resized()
{
    auto panel = getLocalBounds().withSizeKeepingCentre(kPanelWidth, kPanelHeight);
    // Side clusters share the panel's exact vertical band, so brand / mixer-clip / master / cpu
    // all sit on the same line with the same top & bottom padding as the centre panel.
    const auto clusterY = panel.getY();
    const auto clusterH = panel.getHeight();
    utilityClusterBounds = juce::Rectangle<int>(panel.getX() - kGroupGap - kSideGroupWidth,
                                                clusterY, kSideGroupWidth, clusterH);
    brandClusterBounds = juce::Rectangle<int>(kOuterPadding,
                                              clusterY,
                                              juce::jmax(190, juce::jmin(kBrandWidth, utilityClusterBounds.getX() - kOuterPadding - kGroupGap)),
                                              clusterH);

    const auto monitorX = panel.getRight() + kGroupGap;
    const auto monitorRight = getWidth() - kOuterPadding;
    monitorClusterBounds = juce::Rectangle<int>(monitorX,
                                                clusterY,
                                                juce::jmax(kSideGroupWidth, monitorRight - monitorX),
                                                clusterH);
    auto monitorContent = monitorClusterBounds.reduced(12, 7);
    // Master meter is a fixed-width readout anchored to the left of the cluster (it used to
    // stretch all the way to the window edge); CPU follows it with a bit more room.
    masterMeterBounds = monitorContent.removeFromLeft(juce::jmin(kMasterMeterWidth, monitorContent.getWidth()));
    monitorContent.removeFromLeft(16);
    cpuMeterBounds = monitorContent.removeFromLeft(juce::jmin(kCpuWidth, monitorContent.getWidth()));

    // Readouts: four equal columns (Key | Time Signature | Tempo | Time).
    auto readouts = panel.removeFromTop(kReadoutRowHeight).reduced(16, 0);
    const int colWidth = readouts.getWidth() / 4;
    keyCardBounds      = readouts.removeFromLeft(colWidth);
    timeSigCardBounds  = readouts.removeFromLeft(colWidth);
    tempoCardBounds    = readouts.removeFromLeft(colWidth);
    positionCardBounds = readouts;   // last column = whatever remains

    // Transport controls: a tight cluster of equal-size icons with a fixed gap, centred on
    // the panel axis — so spacing is perfectly even and the row is symmetric (record/play
    // straddle the centre).
    auto band = panel.reduced(16, 8);
    constexpr int iconSize = 38;
    constexpr int iconGap = 14;
    constexpr int count = 6;
    const int clusterW = count * iconSize + (count - 1) * iconGap;
    int x = band.getCentreX() - clusterW / 2;

    for (auto* button : { &prevButton, &stopButton, &recordButton, &playButton, &loopButton, &metronomeButton })
    {
        button->setBounds(x, band.getY(), iconSize, band.getHeight());
        x += iconSize + iconGap;
    }
}

void TransportBarComponent::mouseMove(const juce::MouseEvent& event)
{
    const auto item = hitTestUtilityItem(event.getPosition());
    if (hoveredUtilityItem == item)
        return;

    hoveredUtilityItem = item;
    setMouseCursor(item == UtilityItem::none ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
    repaint();
}

void TransportBarComponent::mouseExit(const juce::MouseEvent&)
{
    hoveredUtilityItem = UtilityItem::none;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

void TransportBarComponent::mouseDown(const juce::MouseEvent& event)
{
    if (const auto item = hitTestUtilityItem(event.getPosition()); item != UtilityItem::none)
    {
        if (item == UtilityItem::mixer && onMixer)
            onMixer();
        else if (item == UtilityItem::clipEditor && onClipEditor)
            onClipEditor();
        else if (item == UtilityItem::stepSequencer && onStepSequencer)
            onStepSequencer();
        return;
    }

    if (event.eventComponent == &recordButton && event.mods.isPopupMenu())
    {
        if (onRecordOptions)
            onRecordOptions();
        return;
    }

    if (tempoCardBounds.contains(event.getPosition()) && event.getNumberOfClicks() >= 2)
    {
        if (onTempoEdit)
            onTempoEdit();
        return;
    }

    if (keyCardBounds.contains(event.getPosition()))
    {
        if (onKeySelect)
            onKeySelect();
    }
}

void TransportBarComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (tempoCardBounds.contains(event.getPosition()) && onTempoEdit)
        onTempoEdit();
}

void TransportBarComponent::buttonClicked(juce::Button* button)
{
    if (syncingButtons)
        return;

    if (button == &playButton && onPlay)
        onPlay();
    else if (button == &prevButton && onSkipToStart)
        onSkipToStart();
    else if (button == &stopButton && onStop)
        onStop();
    else if (button == &recordButton && onRecordChanged)
        onRecordChanged(recordButton.getToggleState());
    else if (button == &metronomeButton && onMetronomeChanged)
        onMetronomeChanged(metronomeButton.getToggleState());
    else if (button == &loopButton && onLoopChanged)
        onLoopChanged(loopButton.getToggleState());
}

void TransportBarComponent::syncButtons()
{
    const juce::ScopedValueSetter<bool> guard(syncingButtons, true);
    playButton.setToggleState(state.playing, juce::dontSendNotification);
    recordButton.setToggleState(state.recording, juce::dontSendNotification);
    metronomeButton.setToggleState(state.metronome, juce::dontSendNotification);
    loopButton.setToggleState(state.loop, juce::dontSendNotification);
}

juce::Rectangle<int> TransportBarComponent::getUtilityItemBounds(UtilityItem item) const noexcept
{
    if (item == UtilityItem::none || utilityClusterBounds.isEmpty())
        return {};

    auto bounds = utilityClusterBounds;
    constexpr int gap = 8;
    const auto itemWidth = (bounds.getWidth() - gap * 2) / 3;
    if (item == UtilityItem::mixer)
        return bounds.removeFromLeft(itemWidth);

    bounds.removeFromLeft(itemWidth + gap);
    if (item == UtilityItem::clipEditor)
        return bounds.removeFromLeft(itemWidth);

    bounds.removeFromLeft(itemWidth + gap);
    return bounds;   // stepSequencer
}

TransportBarComponent::UtilityItem TransportBarComponent::hitTestUtilityItem(juce::Point<int> point) const noexcept
{
    for (const auto item : { UtilityItem::mixer, UtilityItem::clipEditor, UtilityItem::stepSequencer })
        if (getUtilityItemBounds(item).contains(point))
            return item;

    return UtilityItem::none;
}

void TransportBarComponent::drawUtilityItem(juce::Graphics& g, UtilityItem item, const juce::String& label, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    const auto active = (item == UtilityItem::mixer && state.mixerOpen)
                     || (item == UtilityItem::clipEditor && state.clipEditorOpen)
                     || (item == UtilityItem::stepSequencer && state.stepSequencerOpen);
    const auto hovered = hoveredUtilityItem == item;
    const auto fill = active ? theme::cool::cyan.withAlpha(0.10f)
                    : hovered ? theme::text::primary.withAlpha(0.055f)
                              : juce::Colours::transparentBlack;
    const auto colour = active ? theme::text::primary.withAlpha(0.92f)
                      : hovered ? theme::text::secondary.withAlpha(0.92f)
                                : theme::text::muted.withAlpha(0.78f);

    if (active || hovered)
    {
        g.setColour(fill);
    g.fillRoundedRectangle(bounds.toFloat().reduced(2.0f, 1.0f), theme::metrics::controlRadius);
    }

    auto content = bounds.withSizeKeepingCentre(bounds.getWidth(), kContentBand).reduced(8, 0);
    auto labelRow = content.removeFromTop(kLabelRowH);
    content.removeFromTop(kLabelGap);
    g.setColour(colour);
    g.setFont(transportFont(kLabelFontSize));
    g.drawText(label, labelRow, juce::Justification::centred);

    auto icon = content.withSizeKeepingCentre(28, juce::jmin(content.getHeight(), 20));
    drawUtilityIcon(g, item, icon.toFloat(), colour);
}

void TransportBarComponent::drawUtilityIcon(juce::Graphics& g, UtilityItem item, juce::Rectangle<float> bounds, juce::Colour colour) const
{
    g.setColour(colour);
    if (item == UtilityItem::mixer)
    {
        for (int i = 0; i < 3; ++i)
        {
            const auto index = static_cast<float>(i);
            const auto x = bounds.getX() + 5.0f + index * 8.0f;
            g.drawLine(x, bounds.getY(), x, bounds.getBottom(), 1.5f);
            g.fillEllipse(x - 3.0f, bounds.getY() + 4.0f + index * 5.0f, 6.0f, 6.0f);
        }
    }
    else if (item == UtilityItem::clipEditor)
    {
        auto body = bounds.reduced(3.0f, 5.0f);
        g.drawRoundedRectangle(body, 2.0f, 1.6f);
        g.drawLine(body.getX() + 4.0f, body.getCentreY(), body.getRight() - 4.0f, body.getCentreY(), 1.4f);
        juce::Path wave;
        wave.startNewSubPath(body.getX() + 5.0f, body.getCentreY() + 5.0f);
        wave.lineTo(body.getX() + 9.0f, body.getCentreY() + 1.0f);
        wave.lineTo(body.getX() + 13.0f, body.getCentreY() + 7.0f);
        wave.lineTo(body.getX() + 17.0f, body.getCentreY() + 2.0f);
        wave.lineTo(body.getRight() - 5.0f, body.getCentreY() + 5.0f);
        g.strokePath(wave, juce::PathStrokeType(1.5f));
    }
    else if (item == UtilityItem::stepSequencer)
    {
        // 2×4 grid of little step cells (Channel-Rack glyph).
        auto body = bounds.reduced(4.0f, 5.0f);
        const float cw = body.getWidth() / 4.0f;
        const float ch = body.getHeight() / 2.0f;
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 4; ++c)
            {
                juce::Rectangle<float> cell(body.getX() + c * cw, body.getY() + r * ch, cw, ch);
                cell = cell.reduced(1.4f);
                // A couple of cells "lit" so it reads as an active step pattern.
                const bool lit = (r == 0 && (c == 0 || c == 2)) || (r == 1 && c == 1);
                if (lit) g.fillRoundedRectangle(cell, 1.5f);
                else     g.drawRoundedRectangle(cell, 1.5f, 1.0f);
            }
    }
}

void TransportBarComponent::drawMasterMeter(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    auto content = bounds.withSizeKeepingCentre(bounds.getWidth(), kContentBand);
    auto title = content.removeFromTop(kLabelRowH);
    content.removeFromTop(kLabelGap);
    g.setColour(theme::warm::red);
    g.setFont(transportFont(kLabelFontSize));
    g.drawText("MASTER OUT", title, juce::Justification::centredLeft);

    auto row = content;
    auto meter = row.removeFromLeft(juce::jmax(96, row.getWidth() - 56)).withHeight(12).withY(row.getCentreY() - 6);
    g.setColour(theme::surface::primary.withAlpha(0.72f));
    g.fillRoundedRectangle(meter.toFloat(), 3.0f);

    if (state.masterLevel > 0.002f)
    {
        auto fill = meter.toFloat();
        fill.setWidth(fill.getWidth() * juce::jlimit(0.0f, 1.0f, state.masterLevel));
        juce::ColourGradient meterGradient(juce::Colour(0xff39d36b), fill.getX(), fill.getCentreY(),
                                           theme::warm::red, fill.getRight(), fill.getCentreY(), false);
        meterGradient.addColour(0.72, juce::Colour(0xffe7c93a));
        g.setGradientFill(meterGradient);
        g.fillRoundedRectangle(fill, 3.0f);
    }

    g.setColour(theme::text::primary.withAlpha(0.88f));
    g.setFont(transportFont(13.0f, true));
    g.drawText(formatDb(state.masterLevelDb), row.removeFromLeft(56), juce::Justification::centredRight);
}

void TransportBarComponent::drawCpuMeter(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    auto cpu = bounds.withSizeKeepingCentre(bounds.getWidth(), kContentBand);
    g.setColour(theme::text::tertiary.withAlpha(0.60f));
    g.setFont(transportFont(kLabelFontSize));
    g.drawText("CPU", cpu.removeFromTop(kLabelRowH), juce::Justification::centred);
    cpu.removeFromTop(kLabelGap);

    auto loadRow = cpu;
    auto bars = loadRow.removeFromLeft(28).withSizeKeepingCentre(24, 12);
    const auto activeBars = juce::roundToInt(juce::jlimit(0.0f, 1.0f, state.engineLoad) * 4.0f);
    for (int i = 0; i < 4; ++i)
    {
        auto bar = bars.removeFromLeft(4);
        bars.removeFromLeft(2);
        g.setColour(i < activeBars ? theme::warm::red : theme::line::subtle.withAlpha(0.42f));
        g.fillRect(bar.withTop(bar.getBottom() - 3 - i * 3));
    }

    g.setColour(theme::text::primary.withAlpha(0.88f));
    g.setFont(transportFont(12.0f, true));
    g.drawText(juce::String(juce::roundToInt(state.engineLoad * 100.0f)) + "%", loadRow, juce::Justification::centredRight);
}

void TransportBarComponent::drawBrandCluster(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty())
        return;

    const auto logoArea = bounds.removeFromLeft(62).withSizeKeepingCentre(48, 48).toFloat();
    const auto centre = logoArea.getCentre();

    for (int i = 0; i < 4; ++i)
    {
        const auto radius = 18.0f + static_cast<float>(i) * 3.0f;
        g.setColour(theme::cool::cyan.withAlpha(0.16f - static_cast<float>(i) * 0.032f));
        g.drawEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre), 2.0f);
    }

    juce::ColourGradient ring(theme::cool::cyan, logoArea.getX(), logoArea.getY(),
                              theme::cool::turquoise.withAlpha(0.58f), logoArea.getRight(), logoArea.getBottom(), false);
    g.setGradientFill(ring);
    g.drawEllipse(logoArea.reduced(6.0f), 2.8f);

    g.setColour(theme::core::voidBlack.withAlpha(0.86f));
    g.fillEllipse(logoArea.reduced(11.0f));

    g.setColour(theme::cool::cyan.withAlpha(0.95f));
    g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(centre));

    auto text = bounds.reduced(6, 0);
    text = text.withSizeKeepingCentre(text.getWidth(), 39);   // centre the two text lines vertically
    auto titleRow = text.removeFromTop(19);
    g.setColour(theme::text::primary.withAlpha(0.94f));
    g.setFont(transportFont(16.0f, true));
    g.drawText("ORION", titleRow.removeFromLeft(56), juce::Justification::centredLeft);
    g.setColour(theme::text::tertiary.withAlpha(0.70f));
    g.setFont(transportFont(10.0f, true));
    g.drawText("PROJECT", titleRow, juce::Justification::centredLeft);

    text.removeFromTop(2);
    g.setColour(theme::text::secondary.withAlpha(0.92f));
    g.setFont(transportFont(14.0f, true));
    g.drawFittedText(state.projectName.isNotEmpty() ? state.projectName : juce::String("Untitled"),
                     text.removeFromTop(18),
                     juce::Justification::centredLeft,
                     1);

}

void TransportBarComponent::drawButtonFrame(juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour colour, bool active) const
{
    g.setColour(colour);
    g.fillRoundedRectangle(bounds, theme::metrics::controlRadius);
    g.setColour((active ? theme::warm::red : theme::line::normal).withAlpha(active ? 0.68f : 0.34f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), theme::metrics::controlRadius, 1.0f);
}
}  // namespace orion
