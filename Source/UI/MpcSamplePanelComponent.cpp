#include "MpcSamplePanelComponent.h"

#include "OrionTheme.h"
#include "BinaryData.h"

#include <cmath>

namespace orion
{
namespace
{
const auto coral = theme::accent::activeCoral;

// Pad-grid geometry in the render's own normalised space (0..1), measured off the baked
// image. The rows have wider gaps than the columns (labels sit between rows), so the grid is
// defined by column/row centres + a square pad size rather than a uniform gap.
constexpr float colCentreX0 = 0.313f, colStepX = 0.123f;
constexpr float rowCentreY0 = 0.503f, rowStepY = 0.126f;
constexpr float padSizeX = 0.107f, padSizeY = 0.105f;   // matches the render's pad footprint

int padIndexForGridCell(int row, int col) noexcept
{
    // MPC pads count from the bottom row upward: pad 1 is bottom-left, pad 16 is top-right.
    return (3 - row) * 4 + col;
}
}

MpcSamplePanelComponent::MpcSamplePanelComponent()
{
    setOpaque(false);
    setVisible(false);
    dragConstrainer.setMinimumOnscreenAmounts(24, 24, 24, 24);
    panelImage = juce::ImageCache::getFromMemory(BinaryData::mpc_sample_panel_png,
                                                 BinaryData::mpc_sample_panel_pngSize);
    audioFormatManager.registerBasicFormats();
    startTimerHz(30);
}

void MpcSamplePanelComponent::setConnectionState(bool isConnected, const juce::String& name)
{
    connected = isConnected;
    deviceName = name;
    repaint();
}

void MpcSamplePanelComponent::setHardwareStatus(const juce::String& midiInput,
                                                const juce::String& midiOutput,
                                                const juce::String& lastMidi)
{
    midiInputName = midiInput;
    midiOutputName = midiOutput;
    lastMidiText = lastMidi;
    repaint(mapNorm(0.330f, 0.190f, 0.310f, 0.205f).getSmallestIntegerContainer().expanded(3));
}

void MpcSamplePanelComponent::setPerformanceState(bool fullLevelEnabled, bool sixteenLevelsEnabled, bool chopEnabled, int bankIndex, int selectedPadIndex)
{
    fullLevel = fullLevelEnabled;
    sixteenLevels = sixteenLevelsEnabled;
    chopMode = chopEnabled;
    padBank = juce::jlimit(0, 3, bankIndex);
    selectedPad = juce::jlimit(0, 15, selectedPadIndex);
    repaint();
}

void MpcSamplePanelComponent::handlePadEvent(int padIndex, int velocity)
{
    setPadActivity(padIndex, velocity);
    if (velocity > 0 && onPadTriggered)
        onPadTriggered(padIndex, velocity);
}

void MpcSamplePanelComponent::setPadActivity(int padIndex, int velocity)
{
    if (padIndex < 0 || padIndex >= static_cast<int>(padVelocity.size()))
        return;
    padVelocity[static_cast<std::size_t>(padIndex)] = juce::jlimit(0, 127, velocity);
    if (velocity > 0 && chopMode && padSamples[static_cast<std::size_t>(selectedPad)].loaded)
    {
        activeChopSlice = padIndex;
        repaint();
    }
    else if (velocity > 0 && padSamples[static_cast<std::size_t>(padIndex)].loaded)
    {
        // Striking a loaded pad focuses it on the LCD (like selecting a pad on the hardware).
        selectedPad = padIndex;
        activeChopSlice = -1;
        repaint();
    }
    else
        repaint(padBounds[static_cast<std::size_t>(padIndex)].getSmallestIntegerContainer().expanded(6));
}

// Map a rect given in the render's 0..1 space to on-screen coordinates within imageArea.
juce::Rectangle<float> MpcSamplePanelComponent::mapNorm(float x, float y, float w, float h) const
{
    return { imageArea.getX() + x * imageArea.getWidth(),
             imageArea.getY() + y * imageArea.getHeight(),
             w * imageArea.getWidth(),
             h * imageArea.getHeight() };
}

void MpcSamplePanelComponent::rebuildHotspots()
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            padBounds[static_cast<std::size_t>(padIndexForGridCell(row, col))] =
                mapNorm(colCentreX0 + static_cast<float>(col) * colStepX - padSizeX * 0.5f,
                        rowCentreY0 + static_cast<float>(row) * rowStepY - padSizeY * 0.5f,
                        padSizeX, padSizeY);

    const auto button = [this](float x, float y, float w, float h)
    {
        return mapNorm(x, y, w, h);
    };

    commandBounds[commandIndex(Command::sampleMode)]   = button(0.039f, 0.345f, 0.078f, 0.043f);
    commandBounds[commandIndex(Command::seqMode)]      = button(0.134f, 0.345f, 0.078f, 0.043f);
    commandBounds[commandIndex(Command::padFx)]        = button(0.039f, 0.418f, 0.078f, 0.043f);
    commandBounds[commandIndex(Command::knobFx)]       = button(0.134f, 0.418f, 0.078f, 0.043f);
    commandBounds[commandIndex(Command::shift)]        = button(0.039f, 0.511f, 0.078f, 0.043f);
    commandBounds[commandIndex(Command::padBank)]      = button(0.134f, 0.511f, 0.078f, 0.043f);
    commandBounds[commandIndex(Command::chop)]         = button(0.784f, 0.345f, 0.079f, 0.043f);
    commandBounds[commandIndex(Command::mute)]         = button(0.884f, 0.345f, 0.079f, 0.043f);
    commandBounds[commandIndex(Command::loop)]         = button(0.784f, 0.418f, 0.079f, 0.045f);
    commandBounds[commandIndex(Command::levels16)]     = button(0.884f, 0.418f, 0.079f, 0.045f);
    commandBounds[commandIndex(Command::sampleSelect)] = button(0.784f, 0.510f, 0.079f, 0.046f);
    commandBounds[commandIndex(Command::tapTempo)]     = button(0.884f, 0.510f, 0.079f, 0.046f);
    commandBounds[commandIndex(Command::rewind)]       = button(0.784f, 0.817f, 0.079f, 0.054f);
    commandBounds[commandIndex(Command::stop)]         = button(0.784f, 0.902f, 0.079f, 0.057f);
    commandBounds[commandIndex(Command::record)]       = button(0.884f, 0.817f, 0.079f, 0.054f);
    commandBounds[commandIndex(Command::play)]         = button(0.884f, 0.902f, 0.079f, 0.057f);
    commandBounds[commandIndex(Command::undo)]         = button(0.784f, 0.732f, 0.079f, 0.044f);
    commandBounds[commandIndex(Command::redo)]         = button(0.884f, 0.732f, 0.079f, 0.044f);

    closeBounds = { static_cast<float>(getWidth() - 42), 14.0f, 26.0f, 26.0f };
}

bool MpcSamplePanelComponent::isPadPoint(juce::Point<float> point) const noexcept
{
    for (const auto& pad : padBounds)
        if (pad.contains(point))
            return true;
    return false;
}

bool MpcSamplePanelComponent::isCommandPoint(juce::Point<float> point) const noexcept
{
    for (const auto& command : commandBounds)
        if (command.expanded(4.0f, 4.0f).contains(point))
            return true;
    return false;
}

std::size_t MpcSamplePanelComponent::commandIndex(Command command) noexcept
{
    return static_cast<std::size_t>(command);
}

bool MpcSamplePanelComponent::commandUsesLocalLatch(Command command) noexcept
{
    switch (command)
    {
        case Command::seqMode:
        case Command::knobFx:
        case Command::chop:
        case Command::mute:
        case Command::loop:
        case Command::levels16:
        case Command::sampleSelect:
            return true;

        case Command::sampleMode:
        case Command::padFx:
        case Command::shift:
        case Command::padBank:
        case Command::tapTempo:
        case Command::rewind:
        case Command::stop:
        case Command::record:
        case Command::play:
        case Command::undo:
        case Command::redo:
        case Command::count:
            return false;
    }

    return false;
}

void MpcSamplePanelComponent::resized()
{
    if (panelImage.isValid())
    {
        // The panel is only the MPC body now; no surrounding card or header is drawn.
        const auto area = getLocalBounds().toFloat();
        const auto imgW = static_cast<float>(panelImage.getWidth());
        const auto imgH = static_cast<float>(panelImage.getHeight());
        const auto scale = juce::jmin(area.getWidth() / imgW, area.getHeight() / imgH);
        imageArea = juce::Rectangle<float>(imgW * scale, imgH * scale)
                        .withCentre({ area.getCentreX(), area.getCentreY() });
    }
    rebuildHotspots();
}

void MpcSamplePanelComponent::paint(juce::Graphics& g)
{
    if (panelImage.isValid())
    {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImage(panelImage, imageArea, juce::RectanglePlacement::stretchToFit);
    }

    // Live pad-hit glow over the baked pads.
    for (int i = 0; i < 16; ++i)
    {
        const auto v = padVelocity[static_cast<std::size_t>(i)];
        if (v <= 0 && i != selectedPad)
            continue;
        const auto pad = padBounds[static_cast<std::size_t>(i)];
        const auto a = v > 0 ? 0.25f + static_cast<float>(v) / 127.0f * 0.55f : 0.22f;
        g.setColour(coral.withAlpha(a * 0.5f));
        g.fillRoundedRectangle(pad, 6.0f);
        g.setColour(coral.withAlpha(a));
        g.drawRoundedRectangle(pad.reduced(0.5f), 6.0f, 2.0f);
    }

    const auto drawCommandLight = [&](Command command, juce::Colour colour, bool active)
    {
        if (! active)
            return;

        auto bounds = commandBounds[commandIndex(command)];
        const auto renderPx = imageArea.getWidth() / static_cast<float>(panelImage.getWidth());
        switch (command)
        {
            case Command::sampleMode:
            case Command::seqMode:
            case Command::shift:
            case Command::padBank:
                bounds = bounds.reduced(renderPx * 3.0f, renderPx * 3.0f);
                break;

            case Command::padFx:
            case Command::knobFx:
                bounds = bounds.withTrimmedLeft(renderPx * 1.0f)
                               .withTrimmedTop(renderPx * 2.0f)
                               .withTrimmedRight(renderPx * 4.0f)
                               .withTrimmedBottom(renderPx * 3.0f);
                break;

            default:
                break;
        }

        g.setColour(colour.withAlpha(0.38f));
        g.fillRoundedRectangle(bounds, 4.0f);
    };

    const auto drawCommandTextLight = [&](Command command, bool active)
    {
        if (! active || ! panelImage.isValid())
            return;

        auto bounds = commandBounds[commandIndex(command)];
        const auto renderPx = imageArea.getWidth() / static_cast<float>(panelImage.getWidth());
        switch (command)
        {
            case Command::sampleMode:
            case Command::seqMode:
            case Command::shift:
            case Command::padBank:
                bounds = bounds.reduced(renderPx * 3.0f, renderPx * 3.0f);
                break;

            case Command::padFx:
            case Command::knobFx:
                bounds = bounds.withTrimmedLeft(renderPx * 1.0f)
                               .withTrimmedTop(renderPx * 2.0f)
                               .withTrimmedRight(renderPx * 4.0f)
                               .withTrimmedBottom(renderPx * 3.0f);
                break;

            default:
                break;
        }

        const auto labelArea = bounds.reduced(bounds.getWidth() * 0.12f, bounds.getHeight() * 0.14f);
        const auto imgW = static_cast<float>(panelImage.getWidth());
        const auto imgH = static_cast<float>(panelImage.getHeight());
        const auto pxW = imageArea.getWidth() / imgW;
        const auto pxH = imageArea.getHeight() / imgH;

        const auto toSourceX = [&](float x)
        {
            return juce::jlimit(0, panelImage.getWidth() - 1,
                                static_cast<int>(std::floor((x - imageArea.getX()) / imageArea.getWidth() * imgW)));
        };
        const auto toSourceY = [&](float y)
        {
            return juce::jlimit(0, panelImage.getHeight() - 1,
                                static_cast<int>(std::floor((y - imageArea.getY()) / imageArea.getHeight() * imgH)));
        };

        const int sx0 = toSourceX(labelArea.getX());
        const int sy0 = toSourceY(labelArea.getY());
        const int sx1 = toSourceX(labelArea.getRight());
        const int sy1 = toSourceY(labelArea.getBottom());

        g.setColour(juce::Colours::white.withAlpha(0.82f));
        for (int sy = sy0; sy <= sy1; ++sy)
            for (int sx = sx0; sx <= sx1; ++sx)
            {
                const auto c = panelImage.getPixelAt(sx, sy);
                if (c.getAlpha() < 16 || c.getBrightness() < 0.43f || c.getSaturation() > 0.38f)
                    continue;

                const float dx = imageArea.getX() + static_cast<float>(sx) * pxW;
                const float dy = imageArea.getY() + static_cast<float>(sy) * pxH;
                g.fillRect(juce::Rectangle<float>(dx, dy, pxW + 0.35f, pxH + 0.35f));
            }
    };

    const auto chopActive = chopMode || commandLatched[commandIndex(Command::chop)];
    const auto levelsActive = sixteenLevels || commandLatched[commandIndex(Command::levels16)];
    drawCommandLight(Command::chop, theme::cool::cyan, chopActive);
    drawCommandTextLight(Command::chop, chopActive);
    drawCommandLight(Command::padBank, theme::cool::cyan, padBank > 0);
    drawCommandTextLight(Command::padBank, padBank > 0);
    drawCommandLight(Command::levels16, theme::cool::cyan, levelsActive);
    drawCommandTextLight(Command::levels16, levelsActive);
    drawCommandLight(Command::padFx, theme::cool::cyan, fullLevel);
    drawCommandTextLight(Command::padFx, fullLevel);

    for (std::size_t i = 0; i < commandLatched.size(); ++i)
        if (commandLatched[i])
        {
            const auto command = static_cast<Command>(i);
            if (command == Command::chop || command == Command::levels16)
                continue;
            drawCommandLight(static_cast<Command>(i), theme::cool::cyan, true);
            drawCommandTextLight(static_cast<Command>(i), true);
        }

    // Highlight the pad a sample is being dragged over.
    if (dragHoverPad >= 0 && dragHoverPad < 16)
    {
        const auto pad = padBounds[static_cast<std::size_t>(dragHoverPad)];
        g.setColour(theme::cool::cyan.withAlpha(0.30f));
        g.fillRoundedRectangle(pad, 6.0f);
        g.setColour(theme::cool::cyan);
        g.drawRoundedRectangle(pad.reduced(0.5f), 6.0f, 2.0f);
    }

    // LCD: live waveform of the selected pad if it holds a sample, else the hardware status.
    if (padSamples[static_cast<std::size_t>(selectedPad)].loaded)
    {
        drawScreen(g);
    }
    else
    {
        auto screen = mapNorm(0.330f, 0.190f, 0.310f, 0.205f).reduced(imageArea.getWidth() * 0.018f,
                                                                     imageArea.getHeight() * 0.018f);
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillRoundedRectangle(screen, 4.0f);
        g.setColour(theme::text::primary.withAlpha(0.86f));
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawFittedText("MPC HARDWARE", screen.removeFromTop(16.0f).toNearestInt(),
                         juce::Justification::centredLeft, 1);
        g.setFont(juce::FontOptions(10.5f));
        g.setColour(theme::cool::cyan.withAlpha(0.90f));
        g.drawFittedText("IN  " + (midiInputName.isNotEmpty() ? midiInputName : "listening"),
                         screen.removeFromTop(14.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.drawFittedText("OUT " + (midiOutputName.isNotEmpty() ? midiOutputName : "not found"),
                         screen.removeFromTop(14.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        g.setColour(theme::text::secondary.withAlpha(0.95f));
        g.drawFittedText("Drop a sample on a pad", screen.toNearestInt(), juce::Justification::topLeft, 2);
    }
}

void MpcSamplePanelComponent::mouseDown(const juce::MouseEvent& event)
{
    const auto p = event.getPosition().toFloat();
    if (closeBounds.contains(p))
    {
        if (onClose) onClose();
        return;
    }
    if (isPadPoint(p))
    {
        for (int i = 0; i < 16; ++i)
            if (padBounds[static_cast<std::size_t>(i)].contains(p))
            {
                pressedPad = i;
                handlePadEvent(i, 127);
                return;
            }
    }
    if (isCommandPoint(p))
    {
        for (std::size_t i = 0; i < commandBounds.size(); ++i)
            if (commandBounds[i].expanded(4.0f, 4.0f).contains(p))
            {
                if (event.mods.isShiftDown())
                {
                    if (onCommandLearnRequested)
                        onCommandLearnRequested(static_cast<Command>(i));
                    return;
                }

                const auto command = static_cast<Command>(i);
                if (command == Command::shift)
                    commandLatched.fill(false);
                else if (commandUsesLocalLatch(command))
                    commandLatched[i] = ! commandLatched[i];

                if (onCommand)
                    onCommand(command);
                repaint(commandBounds[i].getSmallestIntegerContainer().expanded(2));
                return;
            }
    }

    // The MPC body is the drag handle. Only pads and the close affordance above
    // consume the click; every other point moves the whole device.
    draggingPanel = true;
    dragger.startDraggingComponent(this, event);
}

void MpcSamplePanelComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (! draggingPanel)
        return;
    dragger.dragComponent(this, event, &dragConstrainer);
}

void MpcSamplePanelComponent::mouseUp(const juce::MouseEvent&)
{
    draggingPanel = false;
    if (pressedPad >= 0)
    {
        padVelocity[static_cast<std::size_t>(pressedPad)] = 0;
        if (onPadTriggered)
            onPadTriggered(pressedPad, 0);
    }
    pressedPad = -1;
    repaint();
}

void MpcSamplePanelComponent::timerCallback()
{
    bool changed = false;
    for (auto& v : padVelocity)
        if (v > 0) { v = juce::jmax(0, v - 18); changed = true; }
    if (changed) repaint();
}

// ---- Sample Mode: pad samples, waveform, drag-and-drop -----------------------------------

int MpcSamplePanelComponent::padIndexAt(juce::Point<float> point) const noexcept
{
    for (int i = 0; i < 16; ++i)
        if (padBounds[static_cast<std::size_t>(i)].contains(point))
            return i;
    return -1;
}

bool MpcSamplePanelComponent::isPadLoaded(int padIndex) const noexcept
{
    return padIndex >= 0 && padIndex < 16 && padSamples[static_cast<std::size_t>(padIndex)].loaded;
}

bool MpcSamplePanelComponent::hasAnyLoadedPad() const noexcept
{
    for (const auto& pad : padSamples)
        if (pad.loaded)
            return true;
    return false;
}

juce::String MpcSamplePanelComponent::getPadSourcePath(int padIndex) const
{
    return isPadLoaded(padIndex) ? padSamples[static_cast<std::size_t>(padIndex)].sourcePath : juce::String();
}

std::vector<float> MpcSamplePanelComponent::buildPeaks(const juce::File& file,
                                                       juce::AudioFormatManager& fm,
                                                       int buckets)
{
    std::vector<float> peaks;
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || buckets <= 0)
        return peaks;

    peaks.assign(static_cast<std::size_t>(buckets) * 2, 0.0f);
    const auto total = reader->lengthInSamples;
    const int chans = juce::jmax(1, static_cast<int>(reader->numChannels));
    const int blockMax = 1 << 15;
    juce::AudioBuffer<float> block(chans, blockMax);

    for (int b = 0; b < buckets; ++b)
    {
        const juce::int64 startS = static_cast<juce::int64>(static_cast<double>(b) / buckets * static_cast<double>(total));
        const juce::int64 endS   = static_cast<juce::int64>(static_cast<double>(b + 1) / buckets * static_cast<double>(total));
        float mn = 0.0f, mx = 0.0f;
        for (juce::int64 pos = startS; pos < endS;)
        {
            const int n = static_cast<int>(juce::jmin<juce::int64>(blockMax, endS - pos));
            block.clear();
            reader->read(&block, 0, n, pos, true, chans > 1);
            for (int c = 0; c < chans; ++c)
            {
                const float* d = block.getReadPointer(c);
                for (int i = 0; i < n; ++i) { mn = juce::jmin(mn, d[i]); mx = juce::jmax(mx, d[i]); }
            }
            pos += n;
        }
        peaks[static_cast<std::size_t>(b * 2)]     = mn;
        peaks[static_cast<std::size_t>(b * 2 + 1)] = mx;
    }
    return peaks;
}

void MpcSamplePanelComponent::loadSampleOntoPad(int padIndex, const juce::File& file)
{
    if (padIndex < 0 || padIndex >= 16 || ! file.existsAsFile())
        return;

    auto peaks = buildPeaks(file, audioFormatManager, 420);
    if (peaks.empty())
        return;   // unsupported / unreadable file — leave the pad as it was

    auto& pad = padSamples[static_cast<std::size_t>(padIndex)];
    pad.sourcePath = file.getFullPathName();
    pad.name = file.getFileNameWithoutExtension();
    pad.peaks = std::move(peaks);
    pad.loaded = true;
    selectedPad = padIndex;
    activeChopSlice = -1;
    if (onPadSampleAssigned)
        onPadSampleAssigned(padIndex, pad.sourcePath);
    repaint();
}

void MpcSamplePanelComponent::drawScreen(juce::Graphics& g)
{
    const auto& pad = padSamples[static_cast<std::size_t>(selectedPad)];

    // Sample name over the baked "B.. Sample ..." line.
    auto nameArea = mapNorm(0.335f, 0.133f, 0.240f, 0.030f);
    g.setColour(juce::Colour(0xff0b0f14));
    g.fillRect(nameArea);
    g.setColour(juce::Colour(0xffe9b64a));   // MPC amber
    g.setFont(juce::FontOptions(juce::jmax(9.0f, nameArea.getHeight() * 0.82f), juce::Font::bold));
    g.drawText("B" + juce::String(selectedPad + 1) + "  " + pad.name, nameArea,
               juce::Justification::centredLeft, true);

    // Waveform box: cover the baked placeholder, draw the real peaks.
    auto wave = mapNorm(0.332f, 0.170f, 0.305f, 0.088f);
    g.setColour(juce::Colour(0xff0b0f14));
    g.fillRect(wave);
    if (! pad.peaks.empty())
    {
        const int buckets = static_cast<int>(pad.peaks.size()) / 2;
        const float midY = wave.getCentreY();
        const float halfH = wave.getHeight() * 0.47f;
        g.setColour(juce::Colour(0xffe9b64a).withAlpha(0.28f));
        g.drawHorizontalLine(static_cast<int>(midY), wave.getX(), wave.getRight());
        g.setColour(juce::Colour(0xffe9b64a));
        for (int i = 0; i < buckets; ++i)
        {
            const float x = wave.getX() + wave.getWidth() * static_cast<float>(i)
                                              / static_cast<float>(juce::jmax(1, buckets - 1));
            const float yTop = midY - pad.peaks[static_cast<std::size_t>(i * 2 + 1)] * halfH;
            const float yBot = midY - pad.peaks[static_cast<std::size_t>(i * 2)]     * halfH;
            g.drawLine(x, yTop, x, yBot, 1.0f);
        }

        if (chopMode)
        {
            const auto safeSlice = activeChopSlice >= 0 ? juce::jlimit(0, 15, activeChopSlice) : selectedPad;
            const float sliceX0 = wave.getX() + wave.getWidth() * static_cast<float>(safeSlice) / 16.0f;
            const float sliceX1 = wave.getX() + wave.getWidth() * static_cast<float>(safeSlice + 1) / 16.0f;
            const auto activeRegion = juce::Rectangle<float>(sliceX0, wave.getY(), sliceX1 - sliceX0, wave.getHeight());

            g.setColour(theme::cool::cyan.withAlpha(0.18f));
            g.fillRect(activeRegion);
            g.setColour(theme::cool::cyan.withAlpha(0.95f));
            g.drawRect(activeRegion, 1.4f);

            g.setColour(theme::cool::cyan.withAlpha(0.86f));
            for (int slice = 1; slice < 16; ++slice)
            {
                const float x = wave.getX() + wave.getWidth() * static_cast<float>(slice) / 16.0f;
                g.drawVerticalLine(static_cast<int>(std::round(x)), wave.getY(), wave.getBottom());
            }

            auto sliceLabel = mapNorm(0.583f, 0.133f, 0.055f, 0.030f);
            g.setColour(juce::Colour(0xff0b0f14));
            g.fillRect(sliceLabel);
            g.setColour(theme::cool::cyan.withAlpha(0.95f));
            g.setFont(juce::FontOptions(juce::jmax(9.0f, sliceLabel.getHeight() * 0.82f), juce::Font::bold));
            g.drawText("S" + juce::String(safeSlice + 1).paddedLeft('0', 2),
                       sliceLabel, juce::Justification::centredRight, true);

            g.setFont(juce::FontOptions(8.5f, juce::Font::bold));
            g.drawText("SLICE " + juce::String(safeSlice + 1).paddedLeft('0', 2) + "/16",
                       wave.withTrimmedTop(wave.getHeight() - 11.0f).toNearestInt(),
                       juce::Justification::centredRight, false);
        }
    }
}

// ---- Drag-and-drop targets ----------------------------------------------------------------

namespace
{
bool looksLikeAudioFile(const juce::String& path)
{
    const auto ext = path.fromLastOccurrenceOf(".", false, true).toLowerCase();
    return ext == "wav" || ext == "aif" || ext == "aiff" || ext == "flac"
        || ext == "mp3" || ext == "ogg" || ext == "m4a" || ext == "caf";
}
}

bool MpcSamplePanelComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (looksLikeAudioFile(f))
            return true;
    return false;
}

void MpcSamplePanelComponent::fileDragEnter(const juce::StringArray&, int x, int y)
{
    dragHoverPad = padIndexAt(juce::Point<int>(x, y).toFloat());
    repaint();
}

void MpcSamplePanelComponent::fileDragMove(const juce::StringArray&, int x, int y)
{
    const int pad = padIndexAt(juce::Point<int>(x, y).toFloat());
    if (pad != dragHoverPad) { dragHoverPad = pad; repaint(); }
}

void MpcSamplePanelComponent::fileDragExit(const juce::StringArray&)
{
    dragHoverPad = -1;
    repaint();
}

void MpcSamplePanelComponent::filesDropped(const juce::StringArray& files, int x, int y)
{
    dragHoverPad = -1;
    const int pad = padIndexAt(juce::Point<int>(x, y).toFloat());
    if (pad < 0)
        return;
    for (const auto& f : files)
        if (looksLikeAudioFile(f)) { loadSampleOntoPad(pad, juce::File(f)); break; }
}

bool MpcSamplePanelComponent::isInterestedInDragSource(const SourceDetails& details)
{
    const auto* payload = details.description.getDynamicObject();
    return payload != nullptr && payload->hasProperty("path")
        && looksLikeAudioFile(payload->getProperty("path").toString());
}

void MpcSamplePanelComponent::itemDragEnter(const SourceDetails& details)
{
    dragHoverPad = padIndexAt(details.localPosition.toFloat());
    repaint();
}

void MpcSamplePanelComponent::itemDragMove(const SourceDetails& details)
{
    const int pad = padIndexAt(details.localPosition.toFloat());
    if (pad != dragHoverPad) { dragHoverPad = pad; repaint(); }
}

void MpcSamplePanelComponent::itemDragExit(const SourceDetails&)
{
    dragHoverPad = -1;
    repaint();
}

void MpcSamplePanelComponent::itemDropped(const SourceDetails& details)
{
    dragHoverPad = -1;
    const int pad = padIndexAt(details.localPosition.toFloat());
    const auto* payload = details.description.getDynamicObject();
    if (pad < 0 || payload == nullptr)
        return;
    loadSampleOntoPad(pad, juce::File(payload->getProperty("path").toString()));
}
} // namespace orion
