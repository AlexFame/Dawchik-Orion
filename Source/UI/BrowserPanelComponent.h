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

    std::optional<BrowserItem> getSelectedItem() const;
    void chooseRootFolder();

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

    void buttonClicked(juce::Button* button) override;
    void timerCallback() override;
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
};
}  // namespace orion
