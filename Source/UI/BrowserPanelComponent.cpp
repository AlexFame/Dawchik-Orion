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
// Extra row added below the directory path label to hold the search field, hence
// headerHeight needs more vertical room than before.
constexpr int rowGap = 7;
constexpr int dragThresholdPx = 5;
constexpr int headerHeight = 152; // title + subtitle + path + search field
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

juce::String metadataTypeForFile(const juce::File& file)
{
    const auto lowerName = file.getFileNameWithoutExtension().toLowerCase();
    if (lowerName.contains("loop") || lowerName.contains("break") || lowerName.contains("drum") || lowerName.contains("beat"))
        return "Loop";

    if (lowerName.contains("oneshot") || lowerName.contains("one shot") || lowerName.contains("stab") || lowerName.contains("hit"))
        return "One-shot";

    return "Audio";
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
                                       navigateTo(selected);

                                   folderChooser.reset();
                               });
}

void BrowserPanelComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    auto titleArea = bounds.removeFromTop(32);
    g.setColour(juce::Colours::white.withAlpha(0.92f));
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Browser", titleArea, juce::Justification::centredLeft);

    g.setColour(mutedText);
    g.setFont(juce::FontOptions(12.5f, juce::Font::plain));
    g.drawText("Folders and audio files for drag to playlist", bounds.removeFromTop(18), juce::Justification::centredLeft);

    auto pathArea = bounds.removeFromTop(52);
    g.setFont(juce::FontOptions(11.5f, juce::Font::plain));
    const auto pathLabel = showingLocationRoots ? juce::String("Locations") : currentDirectory.getFullPathName();
    auto displayPathLabel = pathLabel;
    if (showingMacRootOverview)
        displayPathLabel = "Macintosh HD";
    else if (showingUserHomeOverview)
        displayPathLabel = currentDirectory.getFileName().isNotEmpty() ? currentDirectory.getFileName() : currentDirectory.getFullPathName();
    g.drawFittedText(displayPathLabel, pathArea.withTrimmedTop(22), juce::Justification::centredLeft, 2);

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
        g.setColour(selected ? rowSelected : (hovered ? rowHover : rowBackground));
        g.fillRoundedRectangle(row.toFloat(), 10.0f);

        const auto& item = items[static_cast<std::size_t>(index)];
        g.setColour(item.colour);
        g.fillRoundedRectangle(row.removeFromLeft(5).toFloat(), 10.0f);
        row.removeFromLeft(12);

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

    const auto contentHeight = getContentHeight(static_cast<int>(items.size()));
    const auto scrollRange = contentHeight - listViewport.getHeight();
    if (scrollRange > 0)
    {
        auto scrollbarArea = listViewport;
        const auto track = scrollbarArea.removeFromRight(4).reduced(0, 4);
        const auto thumbHeight = juce::jmax(24, static_cast<int>(std::round(track.getHeight() * (listViewport.getHeight() / static_cast<double>(contentHeight)))));
        const auto thumbY = track.getY() + static_cast<int>(std::round((track.getHeight() - thumbHeight) * (scrollOffsetY / static_cast<double>(scrollRange))));

        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.fillRoundedRectangle(track.toFloat(), 2.0f);
        g.setColour(juce::Colours::white.withAlpha(0.32f));
        g.fillRoundedRectangle(juce::Rectangle<int>(track.getX(), thumbY, track.getWidth(), thumbHeight).toFloat(), 2.0f);
    }

    g.restoreState();
}

void BrowserPanelComponent::resized()
{
    auto bounds = getLocalBounds();

    // Row 1: title row — Add Folder (right).
    auto titleRow = bounds.removeFromTop(30);
    closeButton.setBounds({});
    chooseFolderButton.setBounds(titleRow.removeFromRight(104).reduced(0, 2));

    // Skip the subtitle + path block painted in paint() (18 + 52 = 70px) and place
    // the search editor in the remaining header band.
    bounds.removeFromTop(18 + 52);
    searchEditor.setBounds(bounds.removeFromTop(28).reduced(0, 2));
}

void BrowserPanelComponent::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();

    dragIndex = hitTestRow(event.getPosition());
    selectedIndex = dragIndex;
    repaint();

    if (! selectedIndex.has_value())
        return;

    const auto& item = items[static_cast<std::size_t>(*selectedIndex)];
    if (item.isDirectory)
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

        return;
    }

    if (onPreviewItem)
        onPreviewItem(item);
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
    if (key.getKeyCode() != juce::KeyPress::returnKey)
        return false;

    if (! selectedIndex.has_value())
        return false;

    const auto& item = items[static_cast<std::size_t>(*selectedIndex)];
    if (item.isDirectory)
        return false;

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

    if (newSignature == entrySignature)
        return;

    entrySignature = newSignature;
    unfilteredItems = std::move(refreshedItems);

    // Apply the case-insensitive search filter. Parent-link rows (".." / "Go up")
    // always pass through so the user can keep navigating while a query is active.
    if (searchQuery.isEmpty())
    {
        items = unfilteredItems;
    }
    else
    {
        items.clear();
        items.reserve(unfilteredItems.size());
        const auto lowerQuery = searchQuery.toLowerCase();
        for (const auto& item : unfilteredItems)
        {
            if (item.isParentLink
                || item.name.toLowerCase().contains(lowerQuery)
                || item.subtitle.toLowerCase().contains(lowerQuery))
            {
                items.push_back(item);
            }
        }
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
    auto bounds = getLocalBounds();
    bounds.removeFromTop(headerHeight);
    return bounds;
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
    const auto lowerName = file.getFileNameWithoutExtension().toLowerCase();
    if (lowerName.contains("loop") || lowerName.contains("bpm"))
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
