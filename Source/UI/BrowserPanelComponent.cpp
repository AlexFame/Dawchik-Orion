#include "BrowserPanelComponent.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>

namespace
{
const auto mutedText = juce::Colours::white.withAlpha(0.64f);
const auto rowBackground = juce::Colours::white.withAlpha(0.035f);
const auto rowHover = juce::Colours::white.withAlpha(0.06f);
const auto rowSelected = juce::Colours::white.withAlpha(0.09f);
const auto buttonColour = juce::Colour(0xff1b232b);
const auto buttonOutlineColour = juce::Colours::white.withAlpha(0.18f);
constexpr int rowHeight = 46;
constexpr int rowGap = 7;
constexpr int dragThresholdPx = 5;
constexpr int headerHeight = 120; // title + search + sections
constexpr int previewBarHeight = 64; // bottom preview card (waveform + play + name)
constexpr int previewSyncRowHeight = 24; // SYNC toggle row, sits below the preview card
constexpr int contentPadX = 4; // uniform horizontal inset so left/right padding stays symmetric
constexpr float horizontalSwipeThreshold = 0.14f;
constexpr int horizontalSwipeLockMs = 320;
const juce::String mountedDevicesHubName = "Mounted Devices";
const auto bpmBadgeColour = juce::Colour(0xff3f5f75);
const auto keyBadgeColour = juce::Colour(0xff5a4d68);

juce::File getMacBrowseRoot()
{
    return juce::File("/");
}

juce::File getUserHomeDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory);
}

bool isUserHomeDirectory(const juce::File& directory)
{
    return directory == getUserHomeDirectory();
}

int getContentHeight(int itemCount) noexcept
{
    return juce::jmax(0, itemCount * (rowHeight + rowGap) - rowGap);
}

double parseBpmFromFileName(const juce::File& file)
{
    auto text = file.getFileNameWithoutExtension().toLowerCase();
    const auto bpmIndex = text.indexOf("bpm");
    if (bpmIndex < 0)
    {
        if (! (text.contains("loop") || text.contains("break") || text.contains("drum") || text.contains("beat")))
            return 0.0;

        double bestBpm = 0.0;
        int bestDigitCount = 0;
        juce::String currentNumber;

        const auto textWithDelimiter = text + " ";
        for (int i = 0; i < textWithDelimiter.length(); ++i)
        {
            const auto character = textWithDelimiter[i];
            if (juce::CharacterFunctions::isDigit(character))
            {
                currentNumber += juce::String::charToString(character);
                continue;
            }

            if (currentNumber.isNotEmpty())
            {
                const auto candidateBpm = currentNumber.getDoubleValue();
                if (candidateBpm >= 40.0 && candidateBpm <= 260.0 && currentNumber.length() >= bestDigitCount)
                {
                    bestBpm = candidateBpm;
                    bestDigitCount = currentNumber.length();
                }

                currentNumber.clear();
            }
        }

        return bestBpm;
    }

    juce::String beforeBpm = text.substring(0, bpmIndex).trimEnd();
    juce::String number;
    for (int i = beforeBpm.length() - 1; i >= 0; --i)
    {
        const auto character = beforeBpm[i];
        if (juce::CharacterFunctions::isDigit(character) || character == '.')
            number = juce::String::charToString(character) + number;
        else if (number.isNotEmpty())
            break;
    }

    const auto bpm = number.getDoubleValue();
    return bpm >= 40.0 && bpm <= 260.0 ? bpm : 0.0;
}

std::optional<juce::String> parseKeyFromFileName(const juce::File& file)
{
    auto text = file.getFileNameWithoutExtension().toStdString();
    std::replace(text.begin(), text.end(), '_', ' ');
    std::replace(text.begin(), text.end(), '-', ' ');

    static const std::regex keyPattern(
        R"((^|[^A-Za-z0-9])([A-Ga-g])\s*(#|b)?\s*(min|minor|maj|major|m)(?=$|[^A-Za-z0-9]))",
        std::regex_constants::icase);

    std::smatch match;
    if (! std::regex_search(text, match, keyPattern))
        return std::nullopt;

    auto root = juce::String(match[2].str()).toUpperCase();
    const auto accidental = juce::String(match[3].str());
    if (accidental.isNotEmpty())
        root += accidental == "b" ? "b" : "#";

    const auto quality = juce::String(match[4].str()).toLowerCase();
    return root + (quality.startsWith("maj") ? " Maj" : " Min");
}

void drawBadge(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& text, juce::Colour colour)
{
    if (bounds.getWidth() <= 0 || text.isEmpty())
        return;

    g.setColour(colour.withAlpha(0.88f));
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    g.setColour(juce::Colours::white.withAlpha(0.86f));
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(text, bounds.reduced(6, 0), juce::Justification::centred, true);
}

// Draws a small glyph inside an icon box: folder for directories, an up-arrow for the
// ".." parent link, a waveform for audio files. `colour` should already be set by the caller.
void drawBrowserEntryIcon(juce::Graphics& g, juce::Rectangle<float> a, bool isDirectory, bool isParentLink)
{
    if (isParentLink)
    {
        // Up arrow.
        const auto cx = a.getCentreX(), cy = a.getCentreY();
        const auto s = juce::jmin(a.getWidth(), a.getHeight()) * 0.34f;
        juce::Path p;
        p.startNewSubPath(cx, cy - s);
        p.lineTo(cx - s, cy + s * 0.2f);
        p.lineTo(cx - s * 0.45f, cy + s * 0.2f);
        p.lineTo(cx - s * 0.45f, cy + s);
        p.lineTo(cx + s * 0.45f, cy + s);
        p.lineTo(cx + s * 0.45f, cy + s * 0.2f);
        p.lineTo(cx + s, cy + s * 0.2f);
        p.closeSubPath();
        g.fillPath(p);
        return;
    }

    if (isDirectory)
    {
        // Folder silhouette with a tab.
        auto f = a.reduced(a.getWidth() * 0.16f, a.getHeight() * 0.22f);
        const auto r = 2.0f;
        const auto tabW = f.getWidth() * 0.42f;
        const auto tabH = f.getHeight() * 0.24f;
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
        return;
    }

    // Audio file: a little symmetric waveform of vertical bars.
    const float hs[] = { 0.30f, 0.62f, 0.92f, 0.55f, 0.78f, 0.40f };
    const int n = 6;
    const auto bw = a.getWidth() / (n * 1.7f);
    for (int i = 0; i < n; ++i)
    {
        const auto h = a.getHeight() * hs[i];
        const auto x = a.getX() + bw * 1.7f * i + bw * 0.35f;
        g.fillRoundedRectangle(x, a.getCentreY() - h * 0.5f, bw, h, bw * 0.45f);
    }
}

juce::String normalisedBrowserPathText(const juce::File& file)
{
    return file.getFullPathName().toLowerCase()
               .replaceCharacter('-', ' ')
               .replaceCharacter('_', ' ')
               .replaceCharacter('/', ' ');
}

bool pathLooksLikeLoop(const juce::String& text)
{
    return text.contains("loop")
        || text.contains("loops")
        || text.contains("луп")
        || text.contains("melody")
        || text.contains("melodies")
        || text.contains("phrase")
        || text.contains("phrases")
        || text.contains("construction")
        || text.contains("midi");
}

bool pathLooksLikeOneShot(const juce::String& text)
{
    return text.contains("one shot")
        || text.contains("oneshot")
        || text.contains("one shots")
        || text.contains("oneshots")
        || text.contains("1shot")
        || text.contains("ван шот")
        || text.contains("ваншот")
        || text.contains("drum hits")
        || text.contains("hits")
        || text.contains("stab")
        || text.contains("stabs");
}

bool pathLooksLikeDrumOneShot(const juce::String& text)
{
    return text.contains("drum")
        || text.contains("kick")
        || text.contains("kicks")
        || text.contains("snare")
        || text.contains("snares")
        || text.contains("clap")
        || text.contains("claps")
        || text.contains("hat")
        || text.contains("hats")
        || text.contains("perc")
        || text.contains("percs")
        || text.contains("fx");
}

juce::String metadataTypeForFile(const juce::File& file)
{
    const auto text = normalisedBrowserPathText(file);
    const auto explicitOneShot = pathLooksLikeOneShot(text);
    const auto loop = pathLooksLikeLoop(text) || parseBpmFromFileName(file) > 0.0;

    if (explicitOneShot)
        return "One-shot";

    if (loop)
        return "Loop";

    if (pathLooksLikeDrumOneShot(text))
        return "One-shot";

    return "Audio";
}

struct BrowserSearchFilter
{
    bool wantsLoop { false };
    bool wantsOneShot { false };
    juce::StringArray terms;
};

bool isLoopItem(const orion::BrowserItem& item)
{
    if (item.isParentLink)
        return false;

    return metadataTypeForFile(item.file) == "Loop";
}

bool isOneShotItem(const orion::BrowserItem& item)
{
    if (item.isParentLink)
        return false;

    return metadataTypeForFile(item.file) == "One-shot";
}

BrowserSearchFilter parseBrowserSearchFilter(const juce::String& query)
{
    auto normalised = query.toLowerCase();
    normalised = normalised.replaceCharacter('-', ' ')
                           .replaceCharacter('_', ' ')
                           .replaceCharacter('/', ' ')
                           .replaceCharacter('.', ' ')
                           .replaceCharacter(',', ' ');

    BrowserSearchFilter filter;
    filter.wantsLoop = normalised.contains("loop")
                    || normalised.contains("луп")
                    || normalised.contains("петл");
    filter.wantsOneShot = normalised.contains("one shot")
                       || normalised.contains("oneshot")
                       || normalised.contains("1shot")
                       || normalised.contains("ван шот")
                       || normalised.contains("ваншот");

    juce::StringArray rawTerms;
    rawTerms.addTokens(normalised, " \t\r\n", "");
    rawTerms.removeEmptyStrings();
    rawTerms.trim();

    for (const auto& term : rawTerms)
    {
        if (term == "loop" || term == "loops" || term == "луп" || term == "лупы" || term == "петля" || term == "петли")
            continue;

        if (term == "one" || term == "shot" || term == "shots" || term == "oneshot" || term == "oneshots" || term == "1shot"
            || term == "ван" || term == "шот" || term == "шоты" || term == "ваншот" || term == "ваншоты")
        {
            continue;
        }

        filter.terms.addIfNotAlreadyThere(term);
    }

    return filter;
}

bool matchesBrowserSearch(const orion::BrowserItem& item, const BrowserSearchFilter& filter)
{
    if (item.isParentLink)
        return true;

    if (filter.wantsLoop && ! isLoopItem(item))
        return false;

    if (filter.wantsOneShot && ! isOneShotItem(item))
        return false;

    const auto searchable = (item.name + " " + item.subtitle + " " + item.category + " " + item.file.getFullPathName()).toLowerCase();
    for (const auto& term : filter.terms)
        if (! searchable.contains(term))
            return false;

    return true;
}

}  // namespace

namespace orion
{
BrowserPanelComponent::SwipeUnlockTimer::SwipeUnlockTimer(BrowserPanelComponent& ownerIn) noexcept
    : owner(ownerIn)
{
}

void BrowserPanelComponent::SwipeUnlockTimer::restart()
{
    startTimer(horizontalSwipeLockMs);
}

void BrowserPanelComponent::SwipeUnlockTimer::timerCallback()
{
    owner.unlockHorizontalSwipeGesture();
    stopTimer();
}

BrowserPanelComponent::BrowserPanelComponent()
{
    setWantsKeyboardFocus(true);

    chooseFolderButton.setColour(juce::TextButton::buttonColourId, buttonColour);
    chooseFolderButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff26313b));
    chooseFolderButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    chooseFolderButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    chooseFolderButton.setColour(juce::ComboBox::outlineColourId, buttonOutlineColour);
    chooseFolderButton.addListener(this);
    addAndMakeVisible(chooseFolderButton);

    // (Close button removed — toggling the browser is the toolbar's BROWSER button.)
    closeButton.setVisible(false);

    searchEditor.setTextToShowWhenEmpty("Search...", juce::Colours::white.withAlpha(0.35f));
    searchEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141c24));
    searchEditor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    searchEditor.setColour(juce::TextEditor::highlightColourId, juce::Colours::white.withAlpha(0.20f));
    searchEditor.setColour(juce::TextEditor::outlineColourId, buttonOutlineColour);
    searchEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff4a8cff));
    searchEditor.setFont(juce::FontOptions(13.0f, juce::Font::plain));
    searchEditor.setReturnKeyStartsNewLine(false);
    searchEditor.onTextChange = [this]
    {
        searchQuery = searchEditor.getText().trim();
        refreshEntries();
    };
    addAndMakeVisible(searchEditor);

    const auto styleSectionButton = [this](juce::TextButton& button)
    {
        button.setClickingTogglesState(false);
        button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff151b21));
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff26313b));
        button.setColour(juce::TextButton::textColourOffId, mutedText);
        button.setColour(juce::TextButton::textColourOnId, juce::Colours::white.withAlpha(0.94f));
        button.addListener(this);
        addAndMakeVisible(button);
    };
    styleSectionButton(loopsSectionButton);
    styleSectionButton(oneShotsSectionButton);
    updateSectionButtons();

    currentDirectory = getMacBrowseRoot();
    showLocationRoots(false);
    refreshEntries();
    startTimer(1200);
}

std::optional<BrowserItem> BrowserPanelComponent::getSelectedItem() const
{
    if (! selectedIndex.has_value())
        return std::nullopt;

    return items[static_cast<std::size_t>(*selectedIndex)];
}

void BrowserPanelComponent::chooseRootFolder()
{
    const auto startDirectory = showingLocationRoots ? getMacBrowseRoot() : currentDirectory;
    folderChooser = std::make_unique<juce::FileChooser>("Choose sample folder", startDirectory, juce::String());
    folderChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                               [this](const juce::FileChooser& chooser)
                               {
                                   const auto selected = chooser.getResult();
                                   if (selected.isDirectory())
                                   {
                                       if (onRootFolderChosen)
                                           onRootFolderChosen(selected);
                                       else
                                           openFolder(selected);
                                   }

                                   folderChooser.reset();
                               });
}

void BrowserPanelComponent::openFolder(const juce::File& directory)
{
    if (directory.isDirectory())
        navigateTo(directory);
}

void BrowserPanelComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);

    auto titleArea = bounds.removeFromTop(32);
    g.setColour(juce::Colours::white.withAlpha(0.92f));
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Browser", titleArea, juce::Justification::centredLeft);

    g.setColour(mutedText);
    g.setFont(juce::FontOptions(12.5f, juce::Font::plain));
    g.drawText("Folders and audio files for drag to playlist", bounds.removeFromTop(18), juce::Justification::centredLeft);

    const auto listViewport = getListViewportBounds();
    g.saveState();
    g.reduceClipRegion(listViewport);

    for (int index = 0; index < static_cast<int>(items.size()); ++index)
    {
        auto row = getRowBounds(index);
        if (row.isEmpty())
            continue;

        const auto selected = selectedIndex.has_value() && *selectedIndex == index;
        const auto hovered = hoverIndex.has_value() && *hoverIndex == index;
        const auto& item = items[static_cast<std::size_t>(index)];

        // Row card. Selected rows get a faint tint in the entry's own colour so the
        // selection reads as "this one" rather than a flat grey.
        g.setColour(selected ? item.colour.withAlpha(0.18f) : (hovered ? rowHover : rowBackground));
        g.fillRoundedRectangle(row.toFloat(), 10.0f);
        if (selected)
        {
            g.setColour(item.colour.withAlpha(0.55f));
            g.drawRoundedRectangle(row.toFloat().reduced(0.5f), 10.0f, 1.0f);
        }

        // Icon box on the left: tinted rounded square + folder/waveform/up-arrow glyph.
        auto iconBox = row.removeFromLeft(rowHeight).toFloat().reduced(9.0f);
        g.setColour(item.colour.withAlpha(0.20f));
        g.fillRoundedRectangle(iconBox, 7.0f);
        g.setColour(item.colour.brighter(0.45f).withAlpha(0.95f));
        drawBrowserEntryIcon(g, iconBox.reduced(5.0f), item.isDirectory, item.isParentLink);
        row.removeFromLeft(10);

        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(item.name, row.removeFromTop(19), juce::Justification::centredLeft, true);

        if (! item.isDirectory)
        {
            const auto bpm = parseBpmFromFileName(item.file);
            const auto key = parseKeyFromFileName(item.file);
            const auto roundedBpm = static_cast<int>(std::round(bpm));

            if (roundedBpm > 0)
            {
                auto badge = row.removeFromLeft(66).withHeight(17);
                drawBadge(g, badge, juce::String(roundedBpm) + " BPM", bpmBadgeColour);
                row.removeFromLeft(5);
            }

            if (key.has_value())
            {
                auto badge = row.removeFromLeft(56).withHeight(17);
                drawBadge(g, badge, *key, keyBadgeColour);
                row.removeFromLeft(7);
            }
        }

        g.setColour(mutedText);
        g.setFont(juce::FontOptions(12.0f, juce::Font::plain));
        g.drawText(item.subtitle, row, juce::Justification::centredLeft, true);
    }

    g.restoreState();

    paintPreviewBar(g);
}

void BrowserPanelComponent::paintPreviewBar(juce::Graphics& g)
{
    const auto bar = getPreviewBarBounds();
    const auto accent = juce::Colour(0xff35c9d6);

    // Card.
    g.setColour(juce::Colour(0xff10141a));
    g.fillRoundedRectangle(bar.toFloat().reduced(2.0f), 10.0f);
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawRoundedRectangle(bar.toFloat().reduced(2.0f), 10.0f, 1.0f);

    // SYNC toggle pill — sits in its own row just below the card so nothing overlaps it.
    {
        const auto syncBtn = getPreviewSyncButtonBounds();
        g.setColour(previewBpmSync ? accent.withAlpha(0.85f) : juce::Colours::white.withAlpha(0.06f));
        g.fillRoundedRectangle(syncBtn.toFloat(), 6.0f);
        g.setColour(previewBpmSync ? accent.withAlpha(0.9f) : juce::Colours::white.withAlpha(0.16f));
        g.drawRoundedRectangle(syncBtn.toFloat().reduced(0.5f), 6.0f, 1.0f);
        g.setColour(previewBpmSync ? juce::Colour(0xff10141a) : juce::Colours::white.withAlpha(0.55f));
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(previewBpmSync ? "SYNC TO PROJECT BPM  -  ON" : "SYNC TO PROJECT BPM  -  OFF",
                   syncBtn, juce::Justification::centred);
    }

    // Play / stop button.
    const auto btn = getPreviewPlayButtonBounds();
    if (previewArmed)
    {
        // Quantized & waiting for the next beat: pulse the button so it reads as "queued".
        const auto phase = std::sin(static_cast<float>(juce::Time::getMillisecondCounter() % 600) / 600.0f * juce::MathConstants<float>::twoPi);
        const auto pulse = 0.45f + 0.45f * (0.5f + 0.5f * phase);
        g.setColour(accent.withAlpha(pulse));
    }
    else
    {
        g.setColour(accent.withAlpha(previewPeaks.empty() ? 0.25f : 0.9f));
    }
    g.fillRoundedRectangle(btn.toFloat(), 8.0f);
    g.setColour(juce::Colour(0xff10141a));
    const auto gi = btn.toFloat().reduced(btn.getWidth() * 0.3f, btn.getHeight() * 0.26f);
    if (previewPlaying)
    {
        // Stop square.
        g.fillRoundedRectangle(btn.toFloat().reduced(btn.getWidth() * 0.32f), 2.0f);
    }
    else
    {
        // Play triangle.
        juce::Path tri;
        tri.startNewSubPath(gi.getX(), gi.getY());
        tri.lineTo(gi.getRight(), gi.getCentreY());
        tri.lineTo(gi.getX(), gi.getBottom());
        tri.closeSubPath();
        g.fillPath(tri);
    }

    // Waveform area.
    const auto wave = getPreviewWaveformBounds();
    if (previewPeaks.empty())
    {
        g.setColour(juce::Colours::white.withAlpha(0.30f));
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Select a sample to preview", wave, juce::Justification::centred);
        return;
    }

    // Name (top of the waveform area).
    auto waveBox = wave;
    auto nameRow = waveBox.removeFromTop(15);
    g.setColour(juce::Colours::white.withAlpha(0.80f));
    g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    g.drawText(previewName, nameRow, juce::Justification::centredLeft, true);

    // Waveform: vertical mirrored bars.
    const auto n = static_cast<int>(previewPeaks.size());
    const auto midY = waveBox.getCentreY();
    const auto halfH = waveBox.getHeight() * 0.5f - 1.0f;
    const auto playedX = waveBox.getX() + previewPositionRatio * waveBox.getWidth();
    for (int x = 0; x < waveBox.getWidth(); ++x)
    {
        const auto idx = juce::jlimit(0, n - 1, static_cast<int>(static_cast<float>(x) / waveBox.getWidth() * n));
        const auto h = juce::jmax(1.0f, previewPeaks[static_cast<std::size_t>(idx)] * halfH);
        const auto px = waveBox.getX() + x;
        g.setColour(px <= playedX ? accent.withAlpha(0.95f) : juce::Colours::white.withAlpha(0.28f));
        g.fillRect(static_cast<float>(px), midY - h, 1.0f, h * 2.0f);
    }

    // Playhead line while playing.
    if (previewPlaying)
    {
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.fillRect(playedX, static_cast<float>(waveBox.getY()), 1.0f, static_cast<float>(waveBox.getHeight()));
    }
}

void BrowserPanelComponent::setBrowserSection(BrowserSection section)
{
    if (browserSection == section)
        return;

    browserSection = section;
    updateSectionButtons();
    refreshEntries();
}

void BrowserPanelComponent::updateSectionButtons()
{
    loopsSectionButton.setToggleState(browserSection == BrowserSection::loops, juce::dontSendNotification);
    oneShotsSectionButton.setToggleState(browserSection == BrowserSection::oneShots, juce::dontSendNotification);
}

void BrowserPanelComponent::resized()
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);

    // Row 1: title row — Add Folder (right).
    auto titleRow = bounds.removeFromTop(30);
    closeButton.setBounds({});
    chooseFolderButton.setBounds(titleRow.removeFromRight(104).reduced(0, 2));

    bounds.removeFromTop(18);
    searchEditor.setBounds(bounds.removeFromTop(28).reduced(0, 2));

    bounds.removeFromTop(6);
    auto sectionRow = bounds.removeFromTop(24);
    loopsSectionButton.setBounds(sectionRow.removeFromLeft(76));
    sectionRow.removeFromLeft(6);
    oneShotsSectionButton.setBounds(sectionRow.removeFromLeft(96));
}

void BrowserPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();

    // SYNC toggle row (below the preview card).
    if (getPreviewSyncButtonBounds().contains(event.getPosition()))
    {
        previewBpmSync = !previewBpmSync;
        repaint(getPreviewSyncButtonBounds());
        if (onPreviewBpmSyncToggled)
            onPreviewBpmSyncToggled();
        return;
    }

    // Preview card play/stop button.
    if (getPreviewBarBounds().contains(event.getPosition()))
    {
        if (getPreviewPlayButtonBounds().contains(event.getPosition()) && onTogglePreviewPlayback)
            onTogglePreviewPlayback();
        return;
    }

    dragIndex = hitTestRow(event.getPosition());
    selectedIndex = dragIndex;
    repaint();

    if (! selectedIndex.has_value())
        return;

    const auto& item = items[static_cast<std::size_t>(*selectedIndex)];
    if (item.isDirectory)
    {
        openDirectoryItem(item);
        return;
    }

    if (onPreviewItem)
        onPreviewItem(item);
}

void BrowserPanelComponent::openDirectoryItem(const BrowserItem& item)
{
    if (showingLocationRoots && item.name == "Macintosh HD")
    {
        pushCurrentLocationToBackHistory();
        forwardHistory.clear();
        showingLocationRoots = false;
        showingMacRootOverview = true;
        showingUserHomeOverview = false;
        currentDirectory = getMacBrowseRoot();
        refreshEntries();
        return;
    }

    if (item.isParentLink)
    {
        const auto parentDirectory = currentDirectory.getParentDirectory();
        if (showingUserHomeOverview)
        {
            navigateTo(juce::File("/Users"));
        }
        else if (showingMacRootOverview
            || currentDirectory == juce::File("/Users")
            || currentDirectory == juce::File("/Applications")
            || currentDirectory == juce::File("/Library")
            || currentDirectory == juce::File("/System")
            || currentDirectory == juce::File("/Volumes")
            || parentDirectory == currentDirectory
            || ! parentDirectory.isDirectory())
        {
            showLocationRoots();
        }
        else
            navigateTo(parentDirectory);
    }
    else
        navigateTo(item.file);
}

void BrowserPanelComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    const auto clickedIndex = hitTestRow(event.getPosition());
    if (! clickedIndex.has_value())
        return;

    selectedIndex = clickedIndex;
    repaint();

    const auto& item = items[static_cast<std::size_t>(*selectedIndex)];
    if (item.isDirectory)
        return;

    if (onActivateItem)
        onActivateItem(item);
}

void BrowserPanelComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (! dragIndex.has_value())
        return;

    const auto& item = items[static_cast<std::size_t>(*dragIndex)];
    if (item.isDirectory)
        return;

    if ((event.getDistanceFromDragStartX() * event.getDistanceFromDragStartX())
            + (event.getDistanceFromDragStartY() * event.getDistanceFromDragStartY())
        < dragThresholdPx * dragThresholdPx)
    {
        return;
    }

    auto* dragContainer = juce::DragAndDropContainer::findParentDragContainerFor(this);
    if (dragContainer == nullptr)
        return;

    auto payload = juce::var(new juce::DynamicObject());
    auto* payloadObject = payload.getDynamicObject();
    payloadObject->setProperty("type", "browser-item");
    payloadObject->setProperty("name", item.name);
    payloadObject->setProperty("category", item.category);
    payloadObject->setProperty("subtitle", item.subtitle);
    payloadObject->setProperty("colour", static_cast<int>(item.colour.getARGB()));
    payloadObject->setProperty("lengthBeats", item.defaultClipLengthInBeats);
    payloadObject->setProperty("path", item.file.getFullPathName());

    auto dragImage = juce::Image(juce::Image::ARGB, 188, 32, true);
    juce::Graphics g(dragImage);
    g.setColour(item.colour.withAlpha(0.95f));
    g.fillRoundedRectangle(dragImage.getBounds().toFloat(), 8.0f);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(item.name, dragImage.getBounds().reduced(10, 0), juce::Justification::centredLeft, true);

    dragContainer->startDragging(payload, this, juce::ScaledImage(dragImage), true, nullptr, &event.source);
    dragIndex.reset();
}

void BrowserPanelComponent::mouseMove(const juce::MouseEvent& event)
{
    hoverIndex = hitTestRow(event.getPosition());
    repaint();
}

void BrowserPanelComponent::mouseExit(const juce::MouseEvent&)
{
    hoverIndex.reset();
    repaint();
}

void BrowserPanelComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (! getListViewportBounds().contains(event.getPosition()))
        return;

    const auto absDeltaX = std::abs(wheel.deltaX);
    const auto absDeltaY = std::abs(wheel.deltaY);
    if (absDeltaX > absDeltaY * 1.2f && absDeltaX > 0.0001f)
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (horizontalSwipeLocked)
            return;

        if (wheel.isInertial)
        {
            lastHorizontalWheelMs = now;
            return;
        }

        lastHorizontalWheelMs = now;
        horizontalWheelAccumulator += wheel.deltaX;

        if (horizontalWheelAccumulator <= -horizontalSwipeThreshold)
        {
            goForwardInBrowserHistory();
            horizontalWheelAccumulator = 0.0f;
            horizontalSwipeLocked = true;
            lastHorizontalWheelMs = now;
            swipeUnlockTimer.restart();
            return;
        }

        if (horizontalWheelAccumulator >= horizontalSwipeThreshold)
        {
            goBackInBrowserHistory();
            horizontalWheelAccumulator = 0.0f;
            horizontalSwipeLocked = true;
            lastHorizontalWheelMs = now;
            swipeUnlockTimer.restart();
            return;
        }

        return;
    }

    if (absDeltaY >= absDeltaX)
    {
        horizontalWheelAccumulator = 0.0f;
    }

    const auto dominantDelta = std::abs(wheel.deltaY) >= std::abs(wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
    if (std::abs(dominantDelta) < 0.0001f)
        return;

    scrollOffsetY -= static_cast<int>(std::round(dominantDelta * 120.0f));
    clampScrollOffset();
    hoverIndex = hitTestRow(event.getPosition());
    repaint();
}

bool BrowserPanelComponent::keyPressed(const juce::KeyPress& key)
{
    const auto count = static_cast<int>(items.size());

    // Up / Down: move the selection through files AND folders. Audio files auto-audition
    // (Ableton-style); folders just highlight. The selected row is scrolled into view.
    if ((key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey) && count > 0)
    {
        const int dir = key.getKeyCode() == juce::KeyPress::downKey ? 1 : -1;
        int next = selectedIndex.has_value() ? juce::jlimit(0, count - 1, *selectedIndex + dir)
                                             : (dir > 0 ? 0 : count - 1);
        selectedIndex = next;

        // Scroll the row fully into the viewport.
        const auto viewport = getListViewportBounds();
        const auto rowTop = next * (rowHeight + rowGap);
        if (rowTop < scrollOffsetY)
            scrollOffsetY = rowTop;
        else if (rowTop + rowHeight > scrollOffsetY + viewport.getHeight())
            scrollOffsetY = rowTop + rowHeight - viewport.getHeight();
        clampScrollOffset();

        const auto& item = items[static_cast<std::size_t>(next)];
        if (! item.isDirectory && onPreviewItem)
            onPreviewItem(item);
        repaint();
        return true;
    }

    // Left: go up one level (into the parent folder). Right: enter the selected folder.
    if (key.getKeyCode() == juce::KeyPress::leftKey)
    {
        for (const auto& it : items)
            if (it.isParentLink)
            {
                const auto parent = it;        // copy — openDirectoryItem refreshes items
                openDirectoryItem(parent);
                return true;
            }
        goBackInBrowserHistory();              // at the locations root: fall back to history
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::rightKey)
    {
        if (selectedIndex.has_value())
        {
            const auto it = items[static_cast<std::size_t>(*selectedIndex)];   // copy
            if (it.isDirectory)
                openDirectoryItem(it);
        }
        return true;
    }

    if (key.getKeyCode() != juce::KeyPress::returnKey)
        return false;

    if (! selectedIndex.has_value())
        return false;

    const auto& item = items[static_cast<std::size_t>(*selectedIndex)];
    if (item.isDirectory)
    {
        // Enter on a folder navigates into it (same as a click).
        openDirectoryItem(item);
        return true;
    }

    if (onActivateItem)
    {
        onActivateItem(item);
        return true;
    }

    return false;
}

void BrowserPanelComponent::buttonClicked(juce::Button* button)
{
    if (button == &chooseFolderButton)
        chooseRootFolder();
    else if (button == &loopsSectionButton)
        setBrowserSection(browserSection == BrowserSection::loops ? BrowserSection::all : BrowserSection::loops);
    else if (button == &oneShotsSectionButton)
        setBrowserSection(browserSection == BrowserSection::oneShots ? BrowserSection::all : BrowserSection::oneShots);
    else if (button == &closeButton)
    {
        if (onCloseRequested)
            onCloseRequested();
    }
}

void BrowserPanelComponent::timerCallback()
{
    if (! isShowing())
        return;

    const auto currentTimestamp = getWatchedLocationTimestamp();
    if (currentTimestamp != watchedLocationTimestamp)
        refreshEntries();
}

void BrowserPanelComponent::refreshEntries()
{
    std::vector<BrowserItem> refreshedItems;

    if (showingLocationRoots)
    {
        const juce::String systemName = "Macintosh HD";

        refreshedItems.push_back(BrowserItem {
            systemName,
            "System",
            "Open system disk",
            juce::Colour(0xff5b84d6),
            4.0,
            getMacBrowseRoot(),
            true,
            false
        });

        const auto volumesRoot = juce::File("/Volumes");
        if (volumesRoot.isDirectory())
        {
            juce::Array<juce::File> mountedVolumes;
            volumesRoot.findChildFiles(mountedVolumes, juce::File::findDirectories, false);
            std::sort(mountedVolumes.begin(), mountedVolumes.end(),
                      [](const juce::File& a, const juce::File& b)
                      {
                          return a.getFileName().compareNatural(b.getFileName()) < 0;
                      });

            bool hasMountedDevices = false;
            for (const auto& volume : mountedVolumes)
            {
                auto volumeName = volume.getFileName();
                if (volumeName.isEmpty())
                    continue;

                if (volumeName == systemName)
                    continue;

                hasMountedDevices = true;
            }

            if (hasMountedDevices)
            {
                refreshedItems.push_back(BrowserItem {
                    mountedDevicesHubName,
                    "Volume",
                    "Open mounted devices",
                    juce::Colour(0xff7a8ba0),
                    4.0,
                    volumesRoot,
                    true,
                    false
                });
            }
        }
    }
    else if (showingMacRootOverview)
    {
        refreshedItems.push_back(BrowserItem {
            "..",
            "Folder",
            "Back to locations",
            juce::Colour(0xff7a8ba0),
            4.0,
            juce::File(),
            true,
            true
        });

        const std::array<juce::String, 4> topLevelNames { "Applications", "Library", "System", "Users" };
        for (const auto& name : topLevelNames)
        {
            const auto directory = juce::File("/") .getChildFile(name);
            if (! directory.isDirectory())
                continue;

            refreshedItems.push_back(BrowserItem {
                name,
                "Folder",
                "Open folder",
                colourForEntry(directory, true),
                4.0,
                directory,
                true,
                false
            });
        }
    }
    else if (showingUserHomeOverview)
    {
        refreshedItems.push_back(BrowserItem {
            "..",
            "Folder",
            "Back to users",
            juce::Colour(0xff7a8ba0),
            4.0,
            juce::File("/Users"),
            true,
            true
        });

        const std::array<std::pair<juce::String, juce::String>, 5> homeFolders {
            std::pair { juce::String("Desktop"), juce::String("Desktop") },
            std::pair { juce::String("Documents"), juce::String("Documents") },
            std::pair { juce::String("Downloads"), juce::String("Downloads") },
            std::pair { juce::String("Music"), juce::String("Music") },
            std::pair { juce::String("Movies"), juce::String("Movies") }
        };

        for (const auto& [name, subtitle] : homeFolders)
        {
            refreshedItems.push_back(BrowserItem {
                name,
                "Folder",
                subtitle,
                juce::Colour(0xff5b84d6),
                4.0,
                currentDirectory.getChildFile(name),
                true,
                false
            });
        }
    }
    else if (currentDirectory == juce::File("/Volumes"))
    {
        refreshedItems.push_back(BrowserItem {
            "..",
            "Folder",
            "Back to locations",
            juce::Colour(0xff7a8ba0),
            4.0,
            juce::File(),
            true,
            true
        });

        juce::Array<juce::File> mountedVolumes;
        currentDirectory.findChildFiles(mountedVolumes, juce::File::findDirectories, false);
        std::sort(mountedVolumes.begin(), mountedVolumes.end(),
                  [](const juce::File& a, const juce::File& b)
                  {
                      return a.getFileName().compareNatural(b.getFileName()) < 0;
                  });

        const auto macBrowseRoot = getMacBrowseRoot();
        for (const auto& volume : mountedVolumes)
        {
            auto volumeName = volume.getFileName();
            if (volumeName.isEmpty() || volume == macBrowseRoot || volumeName == "Macintosh HD")
                continue;

            refreshedItems.push_back(BrowserItem {
                volumeName,
                "Volume",
                "Mounted device",
                juce::Colour(0xff7a8ba0),
                4.0,
                volume,
                true,
                false
            });
        }
    }
    else if (currentDirectory.isDirectory())
    {
        const auto parentDirectory = currentDirectory.getParentDirectory();
        if (parentDirectory != currentDirectory && parentDirectory.exists())
        {
            refreshedItems.push_back(BrowserItem {
                "..",
                "Folder",
                "Go up one level",
                juce::Colour(0xff7a8ba0),
                4.0,
                parentDirectory,
                true,
                true
            });
        }

        juce::Array<juce::File> childDirectories;
        currentDirectory.findChildFiles(childDirectories, juce::File::findDirectories, false);
        std::sort(childDirectories.begin(), childDirectories.end(),
                  [](const juce::File& a, const juce::File& b)
                  {
                      return a.getFileName().compareNatural(b.getFileName()) < 0;
                  });

        for (const auto& directory : childDirectories)
        {
            if (! shouldShowDirectory(directory))
                continue;

            refreshedItems.push_back(BrowserItem {
                directory.getFileName(),
                "Folder",
                "Open folder",
                colourForEntry(directory, true),
                4.0,
                directory,
                true,
                false
            });
        }

        juce::Array<juce::File> childFiles;
        currentDirectory.findChildFiles(childFiles, juce::File::findFiles, false);
        std::sort(childFiles.begin(), childFiles.end(),
                  [](const juce::File& a, const juce::File& b)
                  {
                      return a.getFileName().compareNatural(b.getFileName()) < 0;
                  });

        for (const auto& file : childFiles)
        {
            if (! isAudioFile(file))
                continue;

            refreshedItems.push_back(BrowserItem {
                file.getFileName(),
                file.getParentDirectory().getFileName(),
                subtitleForFile(file),
                colourForEntry(file, false),
                defaultLengthForFile(file),
                file,
                false,
                false
            });
        }
    }

    juce::String newSignature;
    for (const auto& item : refreshedItems)
        newSignature << item.name << "|" << item.subtitle << "|" << item.file.getFullPathName() << "\n";
    // Include the search query in the signature — typing in the search box doesn't
    // change the directory listing but it MUST trigger a refresh of `items`.
    newSignature << "@q=" << searchQuery;
    newSignature << "@section=" << static_cast<int>(browserSection);

    if (newSignature == entrySignature)
        return;

    entrySignature = newSignature;
    unfilteredItems = std::move(refreshedItems);

    // Apply the case-insensitive search filter. Parent-link rows (".." / "Go up")
    // always pass through so the user can keep navigating while a query is active.
    items.clear();
    items.reserve(unfilteredItems.size());
    const auto filter = parseBrowserSearchFilter(searchQuery);
    for (const auto& item : unfilteredItems)
    {
        if (item.isParentLink)
        {
            items.push_back(item);
            continue;
        }

        if (browserSection == BrowserSection::loops && ! isLoopItem(item))
            continue;

        if (browserSection == BrowserSection::oneShots && ! isOneShotItem(item))
            continue;

        if (searchQuery.isNotEmpty() && ! matchesBrowserSearch(item, filter))
            continue;

        items.push_back(item);
    }

    selectedIndex.reset();
    hoverIndex.reset();
    dragIndex.reset();
    clampScrollOffset();
    watchedLocationTimestamp = getWatchedLocationTimestamp();
    repaint();
}

void BrowserPanelComponent::navigateTo(const juce::File& directory, bool addToHistory)
{
    if (! directory.isDirectory())
        return;

    const BrowserLocationState nextState {
        directory,
        false,
        false,
        isUserHomeDirectory(directory)
    };

    if (isCurrentLocation(nextState))
        return;

    if (addToHistory)
    {
        pushCurrentLocationToBackHistory();
        forwardHistory.clear();
    }

    showingLocationRoots = false;
    showingMacRootOverview = false;
    showingUserHomeOverview = isUserHomeDirectory(directory);
    currentDirectory = directory;
    refreshEntries();
}

void BrowserPanelComponent::showLocationRoots(bool addToHistory)
{
    const BrowserLocationState nextState {
        getMacBrowseRoot(),
        true,
        false,
        false
    };

    if (isCurrentLocation(nextState))
        return;

    if (addToHistory)
    {
        pushCurrentLocationToBackHistory();
        forwardHistory.clear();
    }

    showingLocationRoots = true;
    showingMacRootOverview = false;
    showingUserHomeOverview = false;
    currentDirectory = getMacBrowseRoot();
    refreshEntries();
}

BrowserPanelComponent::BrowserLocationState BrowserPanelComponent::getCurrentLocationState() const
{
    return {
        currentDirectory,
        showingLocationRoots,
        showingMacRootOverview,
        showingUserHomeOverview
    };
}

void BrowserPanelComponent::restoreLocationState(const BrowserLocationState& state)
{
    currentDirectory = state.directory;
    showingLocationRoots = state.locationRoots;
    showingMacRootOverview = state.macRootOverview;
    showingUserHomeOverview = state.userHomeOverview;
    refreshEntries();
}

bool BrowserPanelComponent::isCurrentLocation(const BrowserLocationState& state) const
{
    return currentDirectory == state.directory
        && showingLocationRoots == state.locationRoots
        && showingMacRootOverview == state.macRootOverview
        && showingUserHomeOverview == state.userHomeOverview;
}

void BrowserPanelComponent::pushCurrentLocationToBackHistory()
{
    const auto currentState = getCurrentLocationState();
    if (backHistory.empty() || ! (backHistory.back().directory == currentState.directory
                                  && backHistory.back().locationRoots == currentState.locationRoots
                                  && backHistory.back().macRootOverview == currentState.macRootOverview
                                  && backHistory.back().userHomeOverview == currentState.userHomeOverview))
    {
        backHistory.push_back(currentState);
    }
}

void BrowserPanelComponent::goBackInBrowserHistory()
{
    if (backHistory.empty())
        return;

    forwardHistory.push_back(getCurrentLocationState());
    const auto previousState = backHistory.back();
    backHistory.pop_back();
    restoreLocationState(previousState);
}

void BrowserPanelComponent::goForwardInBrowserHistory()
{
    if (forwardHistory.empty())
        return;

    backHistory.push_back(getCurrentLocationState());
    const auto nextState = forwardHistory.back();
    forwardHistory.pop_back();
    restoreLocationState(nextState);
}

void BrowserPanelComponent::unlockHorizontalSwipeGesture() noexcept
{
    horizontalWheelAccumulator = 0.0f;
    horizontalSwipeLocked = false;
    lastHorizontalWheelMs = 0;
}

juce::Rectangle<int> BrowserPanelComponent::getRowBounds(int index) const noexcept
{
    auto listViewport = getListViewportBounds();
    const auto y = listViewport.getY() + index * (rowHeight + rowGap) - scrollOffsetY;
    return { listViewport.getX(), y, listViewport.getWidth(), rowHeight };
}

juce::Rectangle<int> BrowserPanelComponent::getListViewportBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);
    bounds.removeFromTop(headerHeight);
    bounds.removeFromBottom(previewBarHeight + previewSyncRowHeight + 8);   // card + sync row
    return bounds;
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewBarBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);
    auto region = bounds.removeFromBottom(previewBarHeight + previewSyncRowHeight);
    region.removeFromBottom(previewSyncRowHeight);   // sync row lives below the card
    return region;
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewPlayButtonBounds() const noexcept
{
    auto bar = getPreviewBarBounds().reduced(10, 10);
    return bar.removeFromLeft(bar.getHeight());   // square button on the left
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewSyncButtonBounds() const noexcept
{
    auto bounds = getLocalBounds().reduced(contentPadX, 0);
    auto row = bounds.removeFromBottom(previewSyncRowHeight);
    // Full-width pill (matches the card's left/right inset) with a little vertical breathing room.
    return row.reduced(2, 3);
}

juce::Rectangle<int> BrowserPanelComponent::getPreviewWaveformBounds() const noexcept
{
    auto bar = getPreviewBarBounds().reduced(10, 10);
    bar.removeFromLeft(bar.getHeight() + 12);      // skip play button + gap
    return bar;
}

void BrowserPanelComponent::setPreviewWaveform(const juce::String& name, std::vector<float> peaks)
{
    previewName = name;
    previewPeaks = std::move(peaks);
    previewPositionRatio = 0.0f;
    repaint(getPreviewBarBounds());
}

void BrowserPanelComponent::setPreviewPlayback(bool playing, float positionRatio)
{
    if (previewPlaying == playing && std::abs(previewPositionRatio - positionRatio) < 0.002f)
        return;
    previewPlaying = playing;
    previewPositionRatio = juce::jlimit(0.0f, 1.0f, positionRatio);
    repaint(getPreviewBarBounds());
}

void BrowserPanelComponent::setPreviewArmed(bool armed)
{
    // Always repaint while armed (even if the flag didn't change) so the pulse animates,
    // driven by the host's 60 Hz timer ticking this each frame.
    previewArmed = armed;
    repaint(getPreviewBarBounds());
}

void BrowserPanelComponent::clearPreview()
{
    previewName = {};
    previewPeaks.clear();
    previewPlaying = false;
    previewArmed = false;
    previewPositionRatio = 0.0f;
    repaint(getPreviewBarBounds());
}

void BrowserPanelComponent::clampScrollOffset() noexcept
{
    const auto listViewport = getListViewportBounds();
    const auto contentHeight = getContentHeight(static_cast<int>(items.size()));
    const auto maxScroll = juce::jmax(0, contentHeight - listViewport.getHeight());
    scrollOffsetY = juce::jlimit(0, maxScroll, scrollOffsetY);
}

std::optional<int> BrowserPanelComponent::hitTestRow(juce::Point<int> position) const noexcept
{
    for (int index = 0; index < static_cast<int>(items.size()); ++index)
    {
        if (getRowBounds(index).contains(position))
            return index;
    }

    return std::nullopt;
}

bool BrowserPanelComponent::isAudioFile(const juce::File& file) const noexcept
{
    return file.hasFileExtension("wav;wave;aif;aiff;mp3;flac;ogg;m4a");
}

bool BrowserPanelComponent::shouldShowDirectory(const juce::File& directory) const noexcept
{
    const auto name = directory.getFileName();
    if (name.isEmpty())
        return false;

    if (name.startsWithChar('.'))
        return false;

    static const std::array<juce::String, 7> hiddenNames {
        "Library",
        "Applications",
        "System",
        "private",
        "cores",
        "opt",
        "tmp"
    };

    return std::find(hiddenNames.begin(), hiddenNames.end(), name) == hiddenNames.end();
}

juce::Colour BrowserPanelComponent::colourForEntry(const juce::File& file, bool isDirectory) const noexcept
{
    if (isDirectory)
        return juce::Colour(0xff5b84d6);

    const auto lowerName = file.getFileNameWithoutExtension().toLowerCase();
    if (lowerName.contains("kick") || lowerName.contains("snare") || lowerName.contains("clap") || lowerName.contains("hat"))
        return juce::Colour(0xffd97a2b);
    if (lowerName.contains("bass") || lowerName.contains("808"))
        return juce::Colour(0xffca5d54);
    if (lowerName.contains("vox") || lowerName.contains("lead") || lowerName.contains("melody"))
        return juce::Colour(0xff5b84d6);
    if (lowerName.contains("pad") || lowerName.contains("texture") || lowerName.contains("atmo"))
        return juce::Colour(0xff7b6db5);

    return juce::Colour(0xff8aa0b7);
}

double BrowserPanelComponent::defaultLengthForFile(const juce::File& file) const noexcept
{
    if (metadataTypeForFile(file) == "Loop")
        return 8.0;

    return 4.0;
}

juce::String BrowserPanelComponent::subtitleForFile(const juce::File& file) const
{
    const auto extension = file.getFileExtension().trimCharactersAtStart(".");
    return metadataTypeForFile(file) + "  •  " + extension.toUpperCase();
}

juce::Time BrowserPanelComponent::getWatchedLocationTimestamp() const
{
    if (showingLocationRoots)
        return juce::File("/Volumes").getLastModificationTime();

    if (showingMacRootOverview)
        return juce::Time();

    if (showingUserHomeOverview)
        return juce::Time();

    if (currentDirectory.isDirectory())
        return currentDirectory.getLastModificationTime();

    return juce::Time();
}
}  // namespace orion
