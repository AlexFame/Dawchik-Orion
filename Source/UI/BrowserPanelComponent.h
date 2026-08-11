#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "../Analysis/AudioTagger.h"
#include "../Analysis/SampleEmbedding.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
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
    juce::StringArray tags;   // auto-derived content tags (Loop/One-shot, instrument, …) for search
    bool soundTagsRequested { false };   // by-sound analysis kicked off for this listing
};

class BrowserPanelComponent final : public juce::Component,
                                    private juce::Button::Listener,
                                    private juce::Timer
{
public:
    BrowserPanelComponent();

    std::function<void(const BrowserItem&)> onPreviewItem;
    std::function<void(const BrowserItem&)> onActivateItem;
    // Context-menu "Open in sampler": load into a sampler track WITHOUT a playlist clip.
    std::function<void(const BrowserItem&)> onOpenInSampler;
    // Enter on a file: add the sound to the playlist as a sampler track + clip.
    std::function<void(const BrowserItem&)> onAddItemToPlaylist;
    // Context menu: replace the currently-selected track's sample (or create one if none).
    std::function<void(const BrowserItem&)> onReplaceSelectedTrackSample;
    std::function<void()>                   onCloseRequested;
    // Footer transport: fired when the play/stop button in the preview bar is clicked.
    std::function<void()>                   onTogglePreviewPlayback;
    // Footer waveform: move the preview transport to a normalised position (0..1).
    std::function<void(float)>               onSeekPreview;
    // Fired when the user starts dragging an item to the playlist — caller stops the preview.
    std::function<void()>                   onDragStarted;
    // Fired when search/navigation invalidates the currently previewed item.
    std::function<void()>                   onPreviewCleared;
    // Fired when the SYNC toggle is clicked — caller should reload the preview buffer.
    std::function<void()>                   onPreviewBpmSyncToggled;
    std::function<void()>                   onPreviewKeySyncToggled;
    std::function<void(const juce::File&)>  onRootFolderChosen;

    bool isPreviewBpmSyncEnabled() const noexcept { return previewBpmSync; }
    bool isPreviewKeySyncEnabled() const noexcept { return previewKeySync; }

    std::optional<BrowserItem> getSelectedItem() const;
    // The user's added library folders — "find similar" spans ALL of these, not just the open folder.
    // Pre-indexes them in the background (Ableton-style) so search results are ready + stable.
    void setLibraryRoots(std::vector<juce::File> roots)
    {
        roots.erase(std::remove_if(roots.begin(), roots.end(), [](const juce::File& root)
        {
            const auto path = root.getFullPathName();
            return path == "/Volumes" || path.startsWith("/Volumes/");
        }), roots.end());

        juce::String signature;
        for (const auto& root : roots)
            signature << root.getFullPathName() << "\n";
        if (signature == libraryRootsSignature)
            return;

        libraryRootsSignature = signature;
        libraryRoots = std::move(roots);
        recursiveScanValid = false;   // library changed → global search cache is stale
    }
    void chooseRootFolder();
    void openFolder(const juce::File& directory);
    void showRootLocations();
    bool isShowingRootLocations() const noexcept { return showingLocationRoots; }

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
        oneShots,
        favorites
    };

    // Favorites: a persistent, cross-folder collection of sounds the user starred.
    juce::File favoritesFile() const;
    void loadFavorites();
    void saveFavorites() const;
    bool isFavorite(const juce::File& f) const;
    void toggleFavorite(const juce::File& f);
    juce::Rectangle<int> favoriteStarBounds(juce::Rectangle<int> row) const;
    std::set<juce::String> favoritesSet;

    void buttonClicked(juce::Button* button) override;
    void timerCallback() override;
    void openDirectoryItem(const BrowserItem& item);   // navigate into a folder / parent / root
    void refreshEntries();
    void showLocationRoots(bool addToHistory = true);
    void navigateTo(const juce::File& directory, bool addToHistory = true, bool customRoot = false);
    bool isCustomLibraryRoot(const juce::File& directory) const;
    BrowserLocationState getCurrentLocationState() const;
    void restoreLocationState(const BrowserLocationState& state);
    bool isCurrentLocation(const BrowserLocationState& state) const;
    void pushCurrentLocationToBackHistory();
    void goBackInBrowserHistory();
    void goForwardInBrowserHistory();
    void clearSearch();
    juce::String getLocationDisplayName() const;
    void updateNavigationButtons();
    void unlockHorizontalSwipeGesture() noexcept;
    juce::Rectangle<int> getRowBounds(int index) const noexcept;
    juce::Rectangle<int> getListViewportBounds() const noexcept;
    juce::Rectangle<int> getPreviewBarBounds() const noexcept;
    juce::Rectangle<int> getPreviewPlayButtonBounds() const noexcept;
    juce::Rectangle<int> getPreviewSyncButtonBounds() const noexcept;
    juce::Rectangle<int> getPreviewKeySyncButtonBounds() const noexcept;
    juce::Rectangle<int> getPreviewWaveformBounds() const noexcept;
    void paintPreviewBar(juce::Graphics& g);
    void paintTagsRow(juce::Graphics& g);
    juce::Rectangle<int> getTagsRowBounds() const noexcept;
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
    // Kick off (or reuse) content-based "by sound" tagging for a listed item, merging the result
    // into its tags + subtitle when it arrives. Deduped/cached by AudioTagger.
    void requestSoundTags(int itemIndex);

    // Live drag thumbnail: JUCE lets us replace the image while a drag is active, so the
    // browser can give the sample a small momentum-driven tilt instead of a static label.
    juce::DragAndDropContainer* dragVisualContainer { nullptr };
    std::optional<BrowserItem> dragVisualItem;
    std::vector<float> dragVisualPeaks;

    // "Find similar sounds" (Ableton-style): rank the current folder's files by timbral similarity to a
    // chosen query. Enter via the row context menu; exit via the header chip / search / navigation.
    void enterSimilarMode(const juce::File& query);
    void exitSimilarMode();
    void rebuildSimilarItems();          // rank cached embeddings → items (called as embeddings arrive)
    bool inSimilarMode() const noexcept { return similarQuery.has_value(); }

    AudioTagger audioTagger;
    SampleEmbedding sampleEmbedding;
    std::vector<juce::File>   libraryRoots;
    juce::String              libraryRootsSignature;
    std::optional<juce::File> similarQuery;
    std::vector<float>        similarQueryEmb;
    std::vector<juce::File>   similarFiles;              // whole-library candidate pool (audio files)
    bool                      similarDirty { false };   // re-rank pending (coalesced in timerCallback)
    bool                      similarRanked { false };  // frozen once ranked (no visible reshuffle)

    std::vector<BrowserItem> items;
    std::optional<int> selectedIndex;
    std::optional<int> hoverIndex;
    std::optional<int> dragIndex;
    juce::File currentDirectory;
    bool showingLocationRoots { true };
    bool showingMacRootOverview { false };
    bool showingUserHomeOverview { false };
    juce::String entrySignature;
    double scrollOffsetY { 0.0 };
    // Debounce for the by-sound ML tagging: don't kick off Apple SoundAnalysis for rows while the list
    // is actively scrolling (that CPU spike made scrolling feel watery during a preview).
    double lastScrollMs { 0.0 };
    float horizontalWheelAccumulator { 0.0f };
    juce::uint32 lastHorizontalWheelMs { 0 };
    bool horizontalSwipeLocked { false };
    SwipeUnlockTimer swipeUnlockTimer { *this };
    std::vector<BrowserLocationState> backHistory;
    std::vector<BrowserLocationState> forwardHistory;
    bool customRootActive { false };
    juce::Time watchedLocationTimestamp;
    std::unique_ptr<juce::FileChooser> folderChooser;
    juce::TextButton chooseFolderButton { "Add Folder" };
    juce::TextButton closeButton { juce::String::charToString(0x00D7) }; // "×"
    juce::TextEditor searchEditor;
    juce::TextButton backButton { "<" };
    juce::TextButton forwardButton { ">" };
    juce::TextButton clearSearchButton { "x" };
    juce::TextButton loopsSectionButton { "Loops" };
    juce::TextButton oneShotsSectionButton { "One-Shots" };
    juce::TextButton favoritesSectionButton { juce::String::fromUTF8("\xe2\x99\xa5 Favorites") };
    BrowserSection browserSection { BrowserSection::all };
    juce::String     searchQuery;
    // Items returned from the filesystem before applying the search filter.
    // `items` (in the .cpp) holds the post-filter view; this holds the source.
    std::vector<BrowserItem> unfilteredItems;

    // Cache of the RECURSIVE file scan for the current folder, so typing in the search box
    // filters in memory instead of re-scanning the disk on every keystroke. Rebuilt only when
    // the folder changes or its contents are modified.
    std::vector<BrowserItem> recursiveScanItems;
    juce::String recursiveScanScope;   // identifies what the cache covers (folder path, or "*library*")
    bool recursiveScanValid { false };
    // The recursive disk scan runs on this background pool so a search on a big folder never
    // freezes the UI (it used to block ~10s). While it runs, a "Searching…" row is shown.
    juce::ThreadPool scanPool { 1 };
    std::atomic<int> scanGeneration { 0 };
    juce::String scanPendingScope;
    bool recursiveScanPending { false };
    // Scan `roots` recursively (whole library when searching globally); `scopeKey` tags the cache.
    void beginRecursiveScan(std::vector<juce::File> roots, const juce::String& scopeKey);

    // Bottom preview bar state (fed by MainComponent's preview transport).
    juce::String        previewName;
    std::vector<float>  previewPeaks;          // normalised 0..1 magnitudes
    bool                previewPlaying { false };
    bool                previewArmed { false };   // quantized, waiting for the next bar
    float               previewPositionRatio { 0.0f };
    // On by default, matching Ableton, whose Raw switch (the inverse of this) is off by default:
    // previews warp to the project tempo, loop, and launch on the next bar.
    bool                previewBpmSync { true };
    bool                previewKeySync { false };
};
}  // namespace orion
