#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_video/juce_video.h>

#include <array>
#include <functional>
#include <vector>

namespace orion
{
class JamSessionComponent final : public juce::Component,
                                  public juce::DragAndDropTarget,
                                  private juce::Button::Listener,
                                  private juce::TextEditor::Listener,
                                  private juce::Timer,
                                  private juce::CameraDevice::Listener
{
public:
    JamSessionComponent();

    std::function<void()> onClose;
    // Fired when the user actually wants a multiplayer session opened/joined. The host handles the
    // networking (Source/Collab); this panel only owns the presentation.
    std::function<void()> onCreateSessionRequested;
    std::function<void()> onJoinSessionRequested;
    std::function<void()> onLeaveSessionRequested;
    // Fired when the local user ends the A/V call (red hang-up). The host uses this to end the call
    // for every participant; a guest's hang-up only affects themselves.
    std::function<void()> onLocalCallEnded;
    // Fired when the local user sends a chat line; the host relays it over the network.
    std::function<void(const juce::String& text)> onSendChat;
    // Called by the host when a peer's chat line (or replayed history) arrives.
    void receiveChat(const juce::String& name, const juce::String& text);
    // End the A/V call (camera/mic/share off) but stay in the jam. Misclick-safe.
    void endCall();
    // Re-enter the A/V call after having ended it (Zoom "Join"): brings your tile back.
    void joinCall();
    // Whether this machine is currently in the video call — pushed into presence so peers know
    // whether to show our tile.
    bool isInCall() const noexcept { return callActive; }
    // Deliberately leave the whole session (ends the call, then disconnects).
    void leaveSession();

    // Real networking state, pushed in by the host. The panel no longer flips itself to "live" on
    // a click — it only goes live when a session actually connected, so what you see is the truth.
    void setSessionStatus(bool live, const juce::String& detail);

    // Live health line (link state, ops in/out, connected clients) shown in the header.
    void setDiagnostics(const juce::String& text);

    // Real session roster, pushed in by the host: the local user plus each connected peer, with the
    // colour they use for their cursor. Replaces the mockup participant list.
    struct RosterMember { juce::String name; juce::uint32 colourArgb; bool isLocal; bool inCall { true }; };
    void setRoster(const std::vector<RosterMember>& members);
    std::function<bool(bool)> onMicEnabledChanged;
    std::function<bool(bool)> onCameraEnabledChanged;
    std::function<void(bool)> onShareEnabledChanged;

    void setEmbeddedArrangementMode(bool shouldUseEmbeddedMode);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool hitTest(int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override;
    void itemDragEnter(const SourceDetails& dragSourceDetails) override;
    void itemDragExit(const SourceDetails& dragSourceDetails) override;
    void itemDropped(const SourceDetails& dragSourceDetails) override;

private:
    enum class PanelMode
    {
        chat,
        participants,
        setup
    };

    enum class ConnectionState
    {
        ready,
        live
    };

    struct Participant
    {
        juce::String name;
        juce::Colour colour;
        bool muted { false };
        bool cameraOff { false };
        bool sharing { false };
        bool inCall { true };   // whether this member currently gets a video tile (Zoom-style)

        bool operator== (const Participant& o) const
        {
            return name == o.name && colour == o.colour && muted == o.muted
                && cameraOff == o.cameraOff && sharing == o.sharing && inCall == o.inCall;
        }
        bool operator!= (const Participant& o) const { return ! (*this == o); }
    };

    struct BeatLane
    {
        juce::String name;
        juce::Colour colour;
        std::array<bool, 16> steps {};
        int ownerIndex { 0 };
    };

    void buttonClicked(juce::Button*) override;
    void textEditorReturnKeyPressed(juce::TextEditor&) override;
    void timerCallback() override;
    // Camera frames arrive here (background thread); we keep the latest and draw it ourselves,
    // scaled to COVER the tile — pixel-for-pixel fill, unlike the native viewer's letterbox.
    void imageReceived(const juce::Image& image) override;
    void drawLocalCameraFrame(juce::Graphics&, juce::Rectangle<float> tile);
    void sendChatMessage();
    void addChatMessage(const juce::String& name, const juce::String& message);
    void openFileAttachmentChooser(bool foldersOnly);
    void addFileAttachments(const juce::Array<juce::File>& files);
    void refreshControls();
    void updateCallControlsVisibility();
    // Indices into `participants` that currently get a video tile: empty when we've left the call,
    // otherwise everyone flagged inCall. Keeps the two tile-drawing paths in sync.
    std::vector<int> visibleTileIndices() const;
    // Places the "Join call" button in the join-slot tile (the first free slot after peer tiles)
    // when we've left the call; hides it otherwise. `stripInner` is the tile area (gaps included).
    void positionRejoinButton(juce::Rectangle<int> stripInner, int tileW, int tileGap);
    void addPlaylistAttachment(const juce::var& payload);
    void startCamera();
    void stopCamera();
    void attachCallControlsTo(juce::Component&);
    void layoutCallControls(juce::Rectangle<int>);
    juce::Rectangle<int> localParticipantTileBounds() const;
    juce::Rectangle<int> embeddedChatBounds() const;
    juce::Rectangle<int> embeddedVideoBounds() const;
    juce::Rectangle<int> localCameraPreviewBounds() const;
    void drawParticipantTile(juce::Graphics&, juce::Rectangle<float>, const Participant&, bool active) const;
    // Shown across the video strip after you hang up: a hint plus the "Join call" button sits here.
    void drawCallEndedPlaceholder(juce::Graphics&, juce::Rectangle<float> strip) const;
    void drawAvatar(juce::Graphics&, juce::Rectangle<float>, const Participant&) const;
    void drawChatPanel(juce::Graphics&, juce::Rectangle<float>, bool compact);
    void drawParticipantsPanel(juce::Graphics&, juce::Rectangle<float>, bool compact);
    void drawSetupPanel(juce::Graphics&, juce::Rectangle<float>, bool compact);

    // Index 0 is always the local user; entries after it are live peers pushed in from the session.
    // Seeded with just "You" — the old hardcoded Alex/Maya/Jordan/Sophie were a mockup.
    std::vector<Participant> participants {
        Participant { "You", juce::Colour(0xffd9785f), true, true, false }
    };
    std::vector<std::pair<juce::String, juce::String>> chatMessages;
    bool playlistDragOver { false };

    juce::TextButton createButton { "Create session" };
    juce::TextButton joinButton { "Join" };
    juce::TextButton exitButton { "Exit Jam" };
    juce::TextButton rejoinButton { "Join call" };   // shown in the empty video strip after you hang up
    juce::TextButton chatTab { "Chat" };
    juce::TextButton participantsTab { "Participants" };
    juce::TextButton setupTab { "Setup" };
    juce::TextButton micButton { "Mic" };
    juce::TextButton cameraButton { "Camera" };
    juce::TextButton shareButton { "Share" };
    juce::TextButton tipButton { "Tip" };
    juce::TextButton emoteButton { "Emote" };
    juce::TextButton settingsButton { "Settings" };
    juce::TextButton sendButton { "Chat" };
    juce::ComboBox audioInputBox;
    juce::ComboBox audioOutputBox;
    juce::ComboBox cameraDeviceBox;
    juce::ComboBox latencyBox;
    juce::TextEditor chatEditor;
    std::unique_ptr<juce::FileChooser> attachmentChooser;
    std::unique_ptr<juce::CameraDevice> cameraDevice;
    juce::CriticalSection frameLock;
    juce::Image latestFrame;   // most recent camera frame, drawn cover-scaled into the local tile
    PanelMode panelMode { PanelMode::chat };
    ConnectionState connectionState { ConnectionState::ready };
    juce::String inviteCode { "7K4M" };
    juce::String networkStatus;   // real connection detail shown in the header (host address, error…)
    juce::String diagnosticsText; // live op counters / link state, refreshed once a second
    bool callActive { true };   // are we in the video call? Red hang-up sets false; "Join call" true
    bool micEnabled { false };
    bool cameraEnabled { false };
    bool cameraOpening { false };
    bool sharingEnabled { false };
    bool embeddedArrangementMode { false };
    bool callControlsVisible { false };
};
} // namespace orion
