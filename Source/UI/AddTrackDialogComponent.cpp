#include "AddTrackDialogComponent.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>

namespace
{
const auto panelColour = juce::Colour(0xff15191f);
const auto panelStroke = juce::Colour(0xff2b3640);
const auto fieldFill   = juce::Colour(0xff0c0e10);
const auto cardIdle    = juce::Colour(0xff20262f);
const auto accent      = juce::Colour(0xffe8401f);
const auto textColour  = juce::Colours::white.withAlpha(0.94f);
const auto mutedText   = juce::Colours::white.withAlpha(0.5f);

constexpr int panelW = 540;
constexpr int panelH = 492;
constexpr int pad    = 22;
constexpr int titleH = 30;
constexpr int tabsH  = 104;
constexpr int rowH   = 30;
constexpr int rowGap = 14;
constexpr int labelW = 80;

struct TypeInfo { const char* name; const char* sub; juce::uint32 colour; };
const std::array<TypeInfo, 4> kTypes {{
    { "Audio",   "Record and edit", 0xffe8401f },
    { "MIDI",    "Compose and edit", 0xff35c9d6 },
    { "Sampler", "Drum and sample",  0xfff0a93a },
    { "Folder",  "Group tracks",     0xff3a7bd5 }
}};
}  // namespace

namespace orion
{
AddTrackDialogComponent::AddTrackDialogComponent()
{
    setVisible(false);
    setAlwaysOnTop(true);

    const auto styleEditor = [](juce::TextEditor& e, juce::Justification just)
    {
        e.setJustification(just);
        e.setColour(juce::TextEditor::backgroundColourId, fieldFill);
        e.setColour(juce::TextEditor::textColourId, textColour);
        e.setColour(juce::TextEditor::outlineColourId, panelStroke);
        e.setColour(juce::TextEditor::focusedOutlineColourId, accent);
        e.setFont(juce::FontOptions(15.0f));
    };
    styleEditor(nameBox, juce::Justification::centredLeft);
    addAndMakeVisible(nameBox);
    styleEditor(countBox, juce::Justification::centred);
    countBox.setInputRestrictions(2, "0123456789");
    addAndMakeVisible(countBox);

    outputCombo.setColour(juce::ComboBox::backgroundColourId, fieldFill);
    outputCombo.setColour(juce::ComboBox::textColourId, textColour);
    outputCombo.setColour(juce::ComboBox::outlineColourId, panelStroke);
    addAndMakeVisible(outputCombo);

    instrumentCombo.setColour(juce::ComboBox::backgroundColourId, fieldFill);
    instrumentCombo.setColour(juce::ComboBox::textColourId, textColour);
    instrumentCombo.setColour(juce::ComboBox::outlineColourId, panelStroke);
    addAndMakeVisible(instrumentCombo);

    autoColourToggle.setColour(juce::ToggleButton::textColourId, textColour);
    autoColourToggle.setColour(juce::ToggleButton::tickColourId, accent);
    autoColourToggle.setToggleState(true, juce::dontSendNotification);
    autoColourToggle.onClick = [this] { repaint(); };
    addAndMakeVisible(autoColourToggle);

    cancelButton.onClick = [this] { closeDialog(); };
    createButton.onClick = [this]
    {
        Result r;
        r.type = type;
        r.name = nameBox.getText().trim();
        r.count = juce::jlimit(1, 32, countBox.getText().getIntValue());
        r.autoColour = autoColourToggle.getToggleState();
        r.colour = chosenColour;
        r.outputBus = outputCombo.getSelectedId() - 2;
        const auto instrumentIndex = instrumentCombo.getSelectedId() - 2;
        if (instrumentIndex >= 0 && instrumentIndex < instrumentIds.size())
            r.instrumentPluginId = instrumentIds[instrumentIndex];
        const auto cb = onCreate;
        closeDialog();
        if (cb)
            cb(r);
    };
    cancelButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff262a30));
    createButton.setColour(juce::TextButton::buttonColourId, accent);
    addAndMakeVisible(cancelButton);
    addAndMakeVisible(createButton);
}

void AddTrackDialogComponent::show(int existingTrackCount, const juce::StringArray& busNames, juce::Array<juce::PluginDescription> instruments)
{
    trackCountAtOpen = existingTrackCount;
    type = TrackType::audio;

    outputCombo.clear(juce::dontSendNotification);
    outputCombo.addItem("Master", 1);
    for (int i = 0; i < busNames.size(); ++i)
        outputCombo.addItem(busNames[i], i + 2);
    outputCombo.setSelectedId(1, juce::dontSendNotification);

    instrumentCombo.clear(juce::dontSendNotification);
    instrumentIds.clear();
    instrumentCombo.addItem("No Instrument", 1);
    for (int i = 0; i < instruments.size(); ++i)
    {
        const auto& desc = instruments.getReference(i);
        auto label = desc.name;
        if (desc.manufacturerName.isNotEmpty())
            label += " - " + desc.manufacturerName;
        if (desc.pluginFormatName.isNotEmpty())
            label += " (" + desc.pluginFormatName + ")";
        instrumentCombo.addItem(label, i + 2);
        instrumentIds.add(desc.createIdentifierString());
    }
    instrumentCombo.setSelectedId(1, juce::dontSendNotification);

    countBox.setText("1", juce::dontSendNotification);
    autoColourToggle.setToggleState(true, juce::dontSendNotification);
    chosenColour = juce::Colour(kTypes[0].colour);
    applyTypeToFields();

    anim = 0.0f;
    setAlpha(0.0f);
    setVisible(true);
    toFront(true);
    resized();
    startTimerHz(60);
    nameBox.grabKeyboardFocus();
    repaint();
}

void AddTrackDialogComponent::applyTypeToFields()
{
    nameBox.setText(juce::String(kTypes[static_cast<std::size_t>(type)].name) + " " + juce::String(trackCountAtOpen + 1),
                    juce::dontSendNotification);
    if (autoColourToggle.getToggleState())
        chosenColour = juce::Colour(kTypes[static_cast<std::size_t>(type)].colour);
    instrumentCombo.setVisible(type == TrackType::midi || type == TrackType::sampler);
    resized();
    repaint();
}

void AddTrackDialogComponent::setCount(int newCount)
{
    countBox.setText(juce::String(juce::jlimit(1, 32, newCount)), juce::dontSendNotification);
}

void AddTrackDialogComponent::closeDialog()
{
    stopTimer();
    setTransform({});
    setAlpha(1.0f);
    setVisible(false);
    if (onClose)
        onClose();
}

void AddTrackDialogComponent::timerCallback()
{
    anim = juce::jmin(1.0f, anim + 0.08f);
    const auto t = 1.0f - anim;
    const auto eased = 1.0f - t * t * t;
    setAlpha(eased);
    const auto scale = 0.96f + 0.04f * eased;
    setTransform(juce::AffineTransform::scale(scale, scale, getWidth() * 0.5f, getHeight() * 0.5f));
    repaint();
    if (anim >= 1.0f)
        stopTimer();
}

void AddTrackDialogComponent::drawTabIcon(juce::Graphics& g, TrackType t, juce::Rectangle<float> a, juce::Colour c) const
{
    g.setColour(c);
    switch (t)
    {
        case TrackType::audio:
        {
            const float hs[] = { 0.4f, 0.75f, 0.55f, 1.0f, 0.7f, 0.85f, 0.45f };
            const int n = 7;
            const auto bw = a.getWidth() / (n * 1.6f);
            for (int i = 0; i < n; ++i)
            {
                const auto h = a.getHeight() * hs[i];
                const auto x = a.getX() + bw * 1.6f * i + bw * 0.3f;
                g.fillRoundedRectangle(x, a.getCentreY() - h * 0.5f, bw, h, bw * 0.4f);
            }
            break;
        }
        case TrackType::midi:
        {
            // Piano keyboard: white keys, black keys on top, last key accented in our colour.
            const int nWhite = 4;
            const auto gap = 1.0f;
            const auto ww = (a.getWidth() - gap * (nWhite - 1)) / nWhite;
            const auto wh = a.getHeight();
            for (int i = 0; i < nWhite; ++i)
            {
                const auto x = a.getX() + i * (ww + gap);
                g.setColour(i == nWhite - 1 ? c : juce::Colours::white.withAlpha(0.92f));
                g.fillRoundedRectangle(x, a.getY(), ww, wh, 2.0f);
            }
            const auto bw = ww * 0.62f;
            const auto bh = wh * 0.60f;
            const int blackAfter[] = { 0, 1 };   // C-D, D-E
            g.setColour(juce::Colour(0xff14171c));
            for (int idx : blackAfter)
            {
                const auto cx = a.getX() + (idx + 1) * (ww + gap) - gap * 0.5f;
                g.fillRoundedRectangle(cx - bw * 0.5f, a.getY(), bw, bh, 1.5f);
            }
            break;
        }
        case TrackType::sampler:
        {
            // Fit a square 3x3 grid inside the icon area (use the smaller dimension so it
            // never overflows into the label below).
            const auto side = juce::jmin(a.getWidth(), a.getHeight());
            const auto gap = side * 0.12f;
            const auto cell = (side - 2.0f * gap) / 3.0f;
            const auto ox = a.getCentreX() - side * 0.5f;
            const auto oy = a.getCentreY() - side * 0.5f;
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    g.fillRoundedRectangle(ox + col * (cell + gap), oy + r * (cell + gap), cell, cell, 2.0f);
            break;
        }
        case TrackType::folder:
        {
            // Classic folder silhouette: a tab on the top-left, then the body.
            auto f = a.reduced(a.getWidth() * 0.08f, a.getHeight() * 0.14f);
            const auto r = 2.5f;
            const auto tabW = f.getWidth() * 0.42f;
            const auto tabH = f.getHeight() * 0.22f;
            const auto bodyTop = f.getY() + tabH;

            juce::Path p;
            p.startNewSubPath(f.getX(), f.getY() + r);
            p.lineTo(f.getX(), f.getBottom() - r);
            p.quadraticTo(f.getX(), f.getBottom(), f.getX() + r, f.getBottom());
            p.lineTo(f.getRight() - r, f.getBottom());
            p.quadraticTo(f.getRight(), f.getBottom(), f.getRight(), f.getBottom() - r);
            p.lineTo(f.getRight(), bodyTop + r);
            p.quadraticTo(f.getRight(), bodyTop, f.getRight() - r, bodyTop);
            p.lineTo(f.getX() + tabW + r, bodyTop);
            p.lineTo(f.getX() + tabW - tabH * 0.4f, f.getY() + r);
            p.quadraticTo(f.getX() + tabW - tabH * 0.4f - r, f.getY(), f.getX() + tabW - tabH * 0.4f - r * 2.0f, f.getY());
            p.lineTo(f.getX() + r, f.getY());
            p.quadraticTo(f.getX(), f.getY(), f.getX(), f.getY() + r);
            p.closeSubPath();
            g.fillPath(p);
            break;
        }
    }
}

void AddTrackDialogComponent::paint(juce::Graphics& g)
{
    const auto panel = getPanelBounds();
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    g.fillRoundedRectangle(panel.toFloat().translated(0.0f, 3.0f), 14.0f);
    g.setColour(panelColour);
    g.fillRoundedRectangle(panel.toFloat(), 14.0f);
    g.setColour(panelStroke);
    g.drawRoundedRectangle(panel.toFloat(), 14.0f, 1.0f);

    auto titleRow = panel.reduced(pad).removeFromTop(titleH);
    g.setColour(textColour);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Add Track", titleRow, juce::Justification::centred, false);
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(20.0f));
    g.drawText("x", getCloseBounds(), juce::Justification::centred);

    // Tab cards.
    for (int i = 0; i < 4; ++i)
    {
        const auto tb = getTabBounds(i);
        const auto active = (static_cast<int>(type) == i);
        const auto iconColour = juce::Colour(kTypes[static_cast<std::size_t>(i)].colour);
        g.setColour(active ? cardIdle.brighter(0.12f) : cardIdle);
        g.fillRoundedRectangle(tb.toFloat(), 9.0f);
        g.setColour(active ? accent : panelStroke.withAlpha(0.6f));
        g.drawRoundedRectangle(tb.toFloat().reduced(0.5f), 9.0f, active ? 2.0f : 1.0f);

        auto cell = tb.reduced(10);
        auto iconRow = cell.removeFromTop(40);                 // fixed icon band
        auto iconArea = juce::Rectangle<float>(44.0f, 28.0f).withCentre(iconRow.getCentre().toFloat());
        drawTabIcon(g, static_cast<TrackType>(i), iconArea, iconColour);

        cell.removeFromTop(8);                                  // clear gap so text never touches the icon
        auto nameArea = cell.removeFromTop(18);
        g.setColour(active ? juce::Colours::white : textColour);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(kTypes[static_cast<std::size_t>(i)].name, nameArea, juce::Justification::centred, false);
        cell.removeFromTop(2);
        g.setColour(mutedText);
        g.setFont(juce::FontOptions(10.5f));
        g.drawText(kTypes[static_cast<std::size_t>(i)].sub, cell.removeFromTop(14), juce::Justification::centred, false);
    }

    // Field labels.
    g.setColour(mutedText);
    g.setFont(juce::FontOptions(13.5f, juce::Font::bold));
    const auto lx = panel.getX() + pad;
    g.drawText("Name",   lx, nameBox.getY(),        labelW, rowH, juce::Justification::centredLeft, false);
    g.drawText("Count",  lx, countBox.getY(),       labelW, rowH, juce::Justification::centredLeft, false);
    g.drawText("Color",  lx, getColourSwatchBounds().getY(), labelW, rowH, juce::Justification::centredLeft, false);
    if (instrumentCombo.isVisible())
        g.drawText("Instrument", lx, instrumentCombo.getY(), labelW, rowH, juce::Justification::centredLeft, false);
    g.drawText("Output", lx, outputCombo.getY(),    labelW, rowH, juce::Justification::centredLeft, false);

    // Colour swatch (always solid; Auto-Color just means it's picked automatically).
    auto sw = getColourSwatchBounds();
    g.setColour(chosenColour);
    g.fillRoundedRectangle(sw.toFloat(), 5.0f);
    g.setColour(panelStroke);
    g.drawRoundedRectangle(sw.toFloat(), 5.0f, 1.0f);

    // Count stepper arrows.
    const auto drawArrow = [&](juce::Rectangle<int> b, bool up)
    {
        g.setColour(juce::Colour(0xff262a30));
        g.fillRoundedRectangle(b.toFloat(), 3.0f);
        g.setColour(textColour.withAlpha(0.8f));
        juce::Path p;
        const auto c = b.toFloat().reduced(b.getWidth() * 0.3f, b.getHeight() * 0.32f);
        if (up) p.addTriangle(c.getX(), c.getBottom(), c.getRight(), c.getBottom(), c.getCentreX(), c.getY());
        else    p.addTriangle(c.getX(), c.getY(), c.getRight(), c.getY(), c.getCentreX(), c.getBottom());
        g.fillPath(p);
    };
    drawArrow(getCountUpBounds(), true);
    drawArrow(getCountDownBounds(), false);
}

void AddTrackDialogComponent::resized()
{
    const auto panel = getPanelBounds();
    auto inner = panel.reduced(pad);
    inner.removeFromTop(titleH);
    inner.removeFromTop(12);
    inner.removeFromTop(tabsH);       // tab cards (drawn in paint)
    inner.removeFromTop(18);

    const auto fieldX = inner.getX() + labelW;
    const auto fieldW = inner.getRight() - fieldX;

    nameBox.setBounds(inner.removeFromTop(rowH).withX(fieldX).withWidth(fieldW));
    inner.removeFromTop(rowGap);

    auto countRow = inner.removeFromTop(rowH);
    countBox.setBounds(countRow.withX(fieldX).withWidth(80));
    inner.removeFromTop(rowGap);

    auto colourRow = inner.removeFromTop(rowH);
    autoColourToggle.setBounds(colourRow.withX(fieldX + 130).withWidth(fieldW - 130));
    inner.removeFromTop(rowGap);

    if (type == TrackType::midi || type == TrackType::sampler)
    {
        instrumentCombo.setVisible(true);
        instrumentCombo.setBounds(inner.removeFromTop(rowH).withX(fieldX).withWidth(fieldW));
        inner.removeFromTop(rowGap);
    }
    else
    {
        instrumentCombo.setVisible(false);
        instrumentCombo.setBounds({});
    }

    outputCombo.setBounds(inner.removeFromTop(rowH).withX(fieldX).withWidth(fieldW));

    auto footer = panel.reduced(pad, 0).removeFromBottom(52).withTrimmedBottom(10);
    createButton.setBounds(footer.removeFromRight(112).withSizeKeepingCentre(112, 34));
    footer.removeFromRight(10);
    cancelButton.setBounds(footer.removeFromRight(100).withSizeKeepingCentre(100, 34));
}

juce::Rectangle<int> AddTrackDialogComponent::getPanelBounds() const
{
    return getLocalBounds().withSizeKeepingCentre(juce::jmin(panelW, getWidth() - 40),
                                                  juce::jmin(panelH, getHeight() - 40));
}

juce::Rectangle<int> AddTrackDialogComponent::getTabBounds(int index) const
{
    auto inner = getPanelBounds().reduced(pad);
    inner.removeFromTop(titleH + 12);
    auto row = inner.removeFromTop(tabsH);
    const auto gap = 10;
    const auto w = (row.getWidth() - gap * 3) / 4;
    return { row.getX() + index * (w + gap), row.getY(), w, row.getHeight() };
}

juce::Rectangle<int> AddTrackDialogComponent::getColourSwatchBounds() const
{
    const auto panel = getPanelBounds();
    return { panel.getX() + pad + labelW, countBox.getBottom() + rowGap, 110, rowH };
}

juce::Rectangle<int> AddTrackDialogComponent::getCountUpBounds() const
{
    return { countBox.getRight() + 8, countBox.getY(), 22, countBox.getHeight() / 2 - 1 };
}

juce::Rectangle<int> AddTrackDialogComponent::getCountDownBounds() const
{
    return { countBox.getRight() + 8, countBox.getY() + countBox.getHeight() / 2 + 1, 22, countBox.getHeight() / 2 - 1 };
}

juce::Rectangle<int> AddTrackDialogComponent::getCloseBounds() const
{
    const auto panel = getPanelBounds();
    return { panel.getRight() - pad - 24, panel.getY() + pad - 4, 24, 24 };
}

bool AddTrackDialogComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { closeDialog(); return true; }
    if (key == juce::KeyPress::returnKey) { createButton.triggerClick(); return true; }
    return false;
}

void AddTrackDialogComponent::mouseDown(const juce::MouseEvent& event)
{
    const auto pos = event.getPosition();
    if (getCloseBounds().contains(pos)) { closeDialog(); return; }

    for (int i = 0; i < 4; ++i)
        if (getTabBounds(i).contains(pos))
        {
            type = static_cast<TrackType>(i);
            applyTypeToFields();
            return;
        }

    if (getCountUpBounds().contains(pos))   { setCount(countBox.getText().getIntValue() + 1); return; }
    if (getCountDownBounds().contains(pos)) { setCount(countBox.getText().getIntValue() - 1); return; }

    if (getColourSwatchBounds().contains(pos))
    {
        auto selector = std::make_unique<juce::ColourSelector>(
            juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders | juce::ColourSelector::showColourspace);
        selector->setCurrentColour(chosenColour);
        selector->setSize(240, 260);
        selector->addChangeListener(this);
        juce::CallOutBox::launchAsynchronously(std::move(selector),
                                               localAreaToGlobal(getColourSwatchBounds()), nullptr);
        return;
    }

    if (! getPanelBounds().contains(pos))
        closeDialog();
}

void AddTrackDialogComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (auto* cs = dynamic_cast<juce::ColourSelector*>(source))
    {
        chosenColour = cs->getCurrentColour();
        autoColourToggle.setToggleState(false, juce::dontSendNotification);
        repaint();
    }
}
}  // namespace orion
