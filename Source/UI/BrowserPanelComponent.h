#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace orion
{
struct BrowserItem
{
    juce::String name;
    juce::String category;
    juce::String subtitle;
    juce::Colour colour;
    double defaultClipLengthInBeats { 4.0 };
    juce::File file;
    bool isDirectory { false };
    bool isParentLink { false };
};

class BrowserPanelComponent final : public juce::Component,
                                    private juce::Button::Listener,
                                    private juce::Timer
{
public:
    BrowserPanelComponent();

    std::function<void(const BrowserItem&)> onPreviewItem;
    std::function<void(const BrowserItem&)> onActivateItem;
    std::function<void()>                   onCloseRequested;
    // Footer transport: fired when the play/stop button in the preview bar is clicked.
    std::function<void()>                   onTogglePreviewPlayback;
    // Fired when the SYNC toggle is clicked — caller should reload the preview buffer.
    std::function<void()>                   onPreviewBpmSyncToggled;
    std::function<void(const juce::File&)>  onRootFolderChosen;

    bool isPreviewBpmSyncEnabled() const noexcept { return previewBpmSync; }

    std::optional<BrowserItem> getSelectedItem() const;
    void chooseRootFolder();
    void openFolder(const juce::File& directory);

    // Feed the bottom preview bar. Peaks are normalised 0..1 absolute magnitudes.
    void setPreviewWaveform(const juce::String& name, std::vector<float> peaks);
    void setPreviewPlayback(bool playing, float positionRatio);
    // True while the preview is quantized-armed: waiting for the next project beat
    // before it actually starts (Ableton-style launch quantize). Drives the blinking
    // play button so the user can see it's "queued" rather than silently doing nothing.
    void setPreviewArmed(bool armed);
    void clearPreview();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    class SwipeUnlockTimer final : private juce::Timer
    {
    public:
        explicit SwipeUnlockTimer(BrowserPanelComponent& ownerIn) noexcept;

        void restart();

    private:
        void timerCallback() override;

        BrowserPanelComponent& owner;
    };

    struct BrowserLocationState
    {
        juce::File directory;
        bool locationRoots { false };
        bool macRootOverview { false };
        bool userHomeOverview { false };
    };

    enum class BrowserSection
    {
        all,
        loops,
        oneShots
    };

    void buttonClicked(juce::Button* button) override;
    void timerCallback() override;
    void openDirectoryItem(const BrowserItem& item);   // navigate into a folder / parent / root
    void refreshEntries();
    void showLocationRoots(bool addToHistory = true);
    void navigateTo(const juce::File& directory, bool addToHistory = true);
    BrowserLocationState getCurrentLocationState() const;
    void restoreLocationState(const BrowserLocationState& state);
    bool isCurrentLocation(const BrowserLocationState& state) const;
    void pushCurrentLocationToBackHistory();
    void goBackInBrowserHistory();
    void goForwardInBrowserHistory();
    void unlockHorizontalSwipeGesture() noexcept;
    juce::Rectangle<int> getRowBounds(int index) const noexcept;
    juce::Rectangle<int> getListViewportBounds() const noexcept;
    juce::Rectangle<int> getPreviewBarBounds() const noexcept;
    juce::Rectangle<int> getPreviewPlayButtonBounds() const noexcept;
    juce::Rectangle<int> getPreviewSyncButtonBounds() const noexcept;
    juce::Rectangle<int> getPreviewWaveformBounds() const noexcept;
    void paintPreviewBar(juce::Graphics& g);
    void setBrowserSection(BrowserSection section);
    void updateSectionButtons();
    void clampScrollOffset() noexcept;
    std::optional<int> hitTestRow(juce::Point<int> position) const noexcept;
    bool isAudioFile(const juce::File& file) const noexcept;
    bool shouldShowDirectory(const juce::File& directory) const noexcept;
    juce::Colour colourForEntry(const juce::File& file, bool isDirectory) const noexcept;
    double defaultLengthForFile(const juce::File& file) const noexcept;
    juce::String subtitleForFile(const juce::File& file) const;
    juce::Time getWatchedLocationTimestamp() const;

    std::vector<BrowserItem> items;
    std::optional<int> selectedIndex;
    std::optional<int> hoverIndex;
    std::optional<int> dragIndex;
    juce::File currentDirectory;
    bool showingLocationRoots { true };
    bool showingMacRootOverview { false };
    bool showingUserHomeOverview { false };
    juce::String entrySignature;
    int scrollOffsetY { 0 };
    float horizontalWheelAccumulator { 0.0f };
    juce::uint32 lastHorizontalWheelMs { 0 };
    bool horizontalSwipeLocked { false };
    SwipeUnlockTimer swipeUnlockTimer { *this };
    std::vector<BrowserLocationState> backHistory;
    std::vector<BrowserLocationState> forwardHistory;
    juce::Time watchedLocationTimestamp;
    std::unique_ptr<juce::FileChooser> folderChooser;
    juce::TextButton chooseFolderButton { "Add Folder" };
    juce::TextButton closeButton { juce::String::charToString(0x00D7) }; // "×"
    juce::TextEditor searchEditor;
    juce::TextButton loopsSectionButton { "Loops" };
    juce::TextButton oneShotsSectionButton { "One-Shots" };
    BrowserSection browserSection { BrowserSection::all };
    juce::String     searchQuery;
    // Items returned from the filesystem before applying the search filter.
    // `items` (in the .cpp) holds the post-filter view; this holds the source.
    std::vector<BrowserItem> unfilteredItems;

    // Bottom preview bar state (fed by MainComponent's preview transport).
    juce::String        previewName;
    std::vector<float>  previewPeaks;          // normalised 0..1 magnitudes
    bool                previewPlaying { false };
    bool                previewArmed { false };   // quantized, waiting for the next beat
    float               previewPositionRatio { 0.0f };
    bool                previewBpmSync { false };
};
}  // namespace orion
